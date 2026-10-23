//! Bounded host audio transport, independent of guest memory and Apple APIs.
//!
//! HLE packets are 256 planar samples in FL/FR/C/LFE/RL/RR order at 48 kHz.
//! Each of eight clients has its own queue: simultaneous clients are mixed at
//! the same output time, never concatenated. One producer per queue and one
//! render consumer are supported. A concurrent producer is refused, not waited
//! for. Lifecycle/control calls may run concurrently; generation/epoch tags
//! discard old queued audio. An already executing render quantum may finish.
//!
//! Initialization allocates fixed storage. Pull uses only bounded loops, plain
//! arithmetic and atomics, with no allocation, locks, I/O or guest callbacks.
//! Counters are approximate concurrent snapshots, not evidence of audible game
//! sound. This transport does not implement XMA decoding or MMIO dispatch.

use std::mem::{align_of, size_of};
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};
use std::sync::{Mutex, OnceLock};

pub const MAX_CLIENTS: usize = 8;
pub const CHANNEL_SAMPLES: usize = 256;
pub const FRAME_SAMPLES: usize = 6 * CHANNEL_SAMPLES;
pub const SAMPLE_RATE: u32 = 48_000;
pub const CAPACITY_FRAMES: usize = 8192;
pub const MAX_PULL_FRAMES: usize = 8192;
const PACKETS: usize = CAPACITY_FRAMES / CHANNEL_SAMPLES;
const CENTER_REAR_GAIN: f32 = std::f32::consts::FRAC_1_SQRT_2;
const LFE_GAIN: f32 = 0.5;
const GAIN_RAMP_FRAMES: u32 = 128;
const DEFAULT_CONTROLS: u64 = 1.0f32.to_bits() as u64;

fn target_frames(profile: u32) -> u32 {
    match profile {
        1 => 512,
        2 => 4096,
        _ => 2048,
    }
}

fn controls(gain: f32, muted: u32, profile: u32) -> Option<u64> {
    (gain.is_finite() && (0.0..=1.0).contains(&gain) && muted <= 1 && profile <= 2)
        .then_some(gain.to_bits() as u64 | (muted as u64) << 32 | (profile as u64) << 33)
}
fn profile(bits: u64) -> u32 {
    (bits >> 33) as u32
}
fn muted(bits: u64) -> bool {
    bits & (1 << 32) != 0
}
fn gain(bits: u64) -> f32 {
    f32::from_bits(bits as u32)
}
fn active_tag(generation: u64) -> Option<u64> {
    (generation != 0 && generation <= u64::MAX / 2).then(|| generation * 2)
}

struct PacketTag {
    generation: AtomicU64,
    epoch: AtomicU64,
}

struct ClientQueue {
    samples: Box<[AtomicU32]>,
    tags: Box<[PacketTag]>,
    read: AtomicU64,
    write: AtomicU64,
    // Activated(g)=2g; Deactivated(g)=2g+1. A late activation cannot resurrect g.
    lifetime: AtomicU64,
    producer_busy: AtomicBool,
    // Written only by the single render consumer. Atomic storage also keeps
    // accidental concurrent misuse memory-safe, though it is not supported.
    seen_lifetime: AtomicU64,
    seen_epoch: AtomicU64,
    primed: AtomicBool,
}

impl ClientQueue {
    fn new() -> Result<Self, &'static str> {
        let mut samples = Vec::new();
        samples
            .try_reserve_exact(CAPACITY_FRAMES * 2)
            .map_err(|_| "audio allocation failed")?;
        samples.extend((0..CAPACITY_FRAMES * 2).map(|_| AtomicU32::new(0)));
        let mut tags = Vec::new();
        tags.try_reserve_exact(PACKETS)
            .map_err(|_| "audio allocation failed")?;
        tags.extend((0..PACKETS).map(|_| PacketTag {
            generation: AtomicU64::new(0),
            epoch: AtomicU64::new(0),
        }));
        Ok(Self {
            samples: samples.into_boxed_slice(),
            tags: tags.into_boxed_slice(),
            read: AtomicU64::new(0),
            write: AtomicU64::new(0),
            lifetime: AtomicU64::new(0),
            producer_busy: AtomicBool::new(false),
            seen_lifetime: AtomicU64::new(0),
            seen_epoch: AtomicU64::new(0),
            primed: AtomicBool::new(false),
        })
    }
}

struct ProducerGuard<'a>(&'a AtomicBool);
impl Drop for ProducerGuard<'_> {
    fn drop(&mut self) {
        self.0.store(false, Ordering::Release);
    }
}

#[derive(Default)]
struct Counters {
    submitted: AtomicU64,
    consumed: AtomicU64,
    rendered: AtomicU64,
    non_silent: AtomicU64,
    underrun: AtomicU64,
    overflow: AtomicU64,
    stale: AtomicU64,
    nonfinite: AtomicU64,
    clipped: AtomicU64,
    rejected: AtomicU64,
}

struct AudioRing {
    clients: Box<[ClientQueue]>,
    running: AtomicBool,
    epoch: AtomicU64,
    controls: AtomicU64,
    controls_revision: AtomicU64,
    seen_controls: AtomicU64,
    render_gain: AtomicU32,
    render_step: AtomicU32,
    render_ramp_left: AtomicU32,
    counters: Counters,
}

impl AudioRing {
    fn new() -> Result<Self, &'static str> {
        let mut clients = Vec::new();
        clients
            .try_reserve_exact(MAX_CLIENTS)
            .map_err(|_| "audio allocation failed")?;
        for _ in 0..MAX_CLIENTS {
            clients.push(ClientQueue::new()?);
        }
        Ok(Self {
            clients: clients.into_boxed_slice(),
            running: AtomicBool::new(false),
            epoch: AtomicU64::new(1),
            controls: AtomicU64::new(DEFAULT_CONTROLS),
            controls_revision: AtomicU64::new(0),
            seen_controls: AtomicU64::new(DEFAULT_CONTROLS),
            render_gain: AtomicU32::new(1.0f32.to_bits()),
            render_step: AtomicU32::new(0),
            render_ramp_left: AtomicU32::new(0),
            counters: Counters::default(),
        })
    }

    fn set_running(&self, running: bool) {
        // Stop admission first. Native lifecycle calls serialize Start/Stop;
        // an already admitted producer is also checked by packet epoch at pull.
        if !running {
            self.running.store(false, Ordering::Release);
            self.epoch.fetch_add(1, Ordering::AcqRel);
        } else {
            self.running.store(true, Ordering::Release);
        }
    }

    fn set_controls(&self, level: f32, mute: u32, latency_profile: u32) -> bool {
        let Some(bits) = controls(level, mute, latency_profile) else {
            return false;
        };
        let old = self.controls.swap(bits, Ordering::AcqRel);
        if old != bits {
            self.controls_revision.fetch_add(1, Ordering::Relaxed);
            if profile(old) != latency_profile {
                self.epoch.fetch_add(1, Ordering::AcqRel);
            }
        }
        true
    }

    fn activate(&self, index: usize, generation: u64) {
        let (Some(client), Some(tag)) = (self.clients.get(index), active_tag(generation)) else {
            self.counters.rejected.fetch_add(1, Ordering::Relaxed);
            return;
        };
        client.lifetime.fetch_max(tag, Ordering::AcqRel);
    }

    fn deactivate(&self, index: usize, generation: u64) {
        let (Some(client), Some(tag)) = (self.clients.get(index), active_tag(generation)) else {
            self.counters.rejected.fetch_add(1, Ordering::Relaxed);
            return;
        };
        client.lifetime.fetch_max(tag + 1, Ordering::AcqRel);
    }

    fn submit(&self, index: usize, generation: u64, source: &[f32; FRAME_SAMPLES]) -> bool {
        let (Some(client), Some(tag)) = (self.clients.get(index), active_tag(generation)) else {
            self.counters.rejected.fetch_add(1, Ordering::Relaxed);
            return false;
        };
        // Frame(g) itself proves that g was active. It may arrive before the
        // out-of-lock Activated(g) observer; a terminal Deactivated(g) still wins.
        client.lifetime.fetch_max(tag, Ordering::AcqRel);
        let epoch = self.epoch.load(Ordering::Acquire);
        if client.lifetime.load(Ordering::Acquire) != tag || !self.running.load(Ordering::Acquire) {
            self.counters
                .stale
                .fetch_add(CHANNEL_SAMPLES as u64, Ordering::Relaxed);
            return false;
        }
        if client
            .producer_busy
            .compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed)
            .is_err()
        {
            self.counters.rejected.fetch_add(1, Ordering::Relaxed);
            return false;
        }
        let _guard = ProducerGuard(&client.producer_busy);
        let write = client.write.load(Ordering::Relaxed);
        let read = client.read.load(Ordering::Acquire);
        if write.wrapping_sub(read) > (CAPACITY_FRAMES - CHANNEL_SAMPLES) as u64 {
            self.counters
                .overflow
                .fetch_add(CHANNEL_SAMPLES as u64, Ordering::Relaxed);
            return false;
        }
        let mut bad = 0;
        let mut clipped = 0;
        for frame in 0..CHANNEL_SAMPLES {
            let mut channel = [0.0; 6];
            for (c, output) in channel.iter_mut().enumerate() {
                let sample = source[c * CHANNEL_SAMPLES + frame];
                *output = if !sample.is_finite() {
                    bad += 1;
                    0.0
                } else if !(-1.0..=1.0).contains(&sample) {
                    clipped += 1;
                    sample.clamp(-1.0, 1.0)
                } else {
                    sample
                };
            }
            // Matches the existing web host's downmix policy. LFE contribution
            // is intentional; this is not a claim of bit-exact hardware mixing.
            let shared = channel[2] * CENTER_REAR_GAIN + channel[3] * LFE_GAIN;
            let left = channel[0] + shared + channel[4] * CENTER_REAR_GAIN;
            let right = channel[1] + shared + channel[5] * CENTER_REAR_GAIN;
            let offset = (write.wrapping_add(frame as u64) as usize & (CAPACITY_FRAMES - 1)) * 2;
            client.samples[offset].store(left.to_bits(), Ordering::Relaxed);
            client.samples[offset + 1].store(right.to_bits(), Ordering::Relaxed);
        }
        let packet = &client.tags[(write as usize / CHANNEL_SAMPLES) & (PACKETS - 1)];
        packet.generation.store(generation, Ordering::Relaxed);
        packet.epoch.store(epoch, Ordering::Relaxed);
        self.counters.nonfinite.fetch_add(bad, Ordering::Relaxed);
        self.counters.clipped.fetch_add(clipped, Ordering::Relaxed);
        if client.lifetime.load(Ordering::Acquire) != tag
            || self.epoch.load(Ordering::Acquire) != epoch
            || !self.running.load(Ordering::Acquire)
        {
            self.counters
                .stale
                .fetch_add(CHANNEL_SAMPLES as u64, Ordering::Relaxed);
            return false;
        }
        client.write.store(
            write.wrapping_add(CHANNEL_SAMPLES as u64),
            Ordering::Release,
        );
        self.counters
            .submitted
            .fetch_add(CHANNEL_SAMPLES as u64, Ordering::Relaxed);
        true
    }

    fn pull(&self, left: &mut [f32], right: &mut [f32]) -> bool {
        debug_assert_eq!(left.len(), right.len());
        debug_assert!(left.len() <= MAX_PULL_FRAMES);
        left.fill(0.0);
        right.fill(0.0);
        let frames = left.len();
        let epoch = self.epoch.load(Ordering::Acquire);
        let control = self.controls.load(Ordering::Acquire);
        self.counters
            .rendered
            .fetch_add(frames as u64, Ordering::Relaxed);
        if !self.running.load(Ordering::Acquire) {
            return false;
        }
        let target = target_frames(profile(control)) as u64;
        let mut consumed = 0u64;
        let mut underrun = 0u64;
        let mut stale = 0u64;

        for client in &self.clients {
            let lifetime = client.lifetime.load(Ordering::Acquire);
            let write = client.write.load(Ordering::Acquire);
            let mut read = client.read.load(Ordering::Relaxed);
            let available = write.wrapping_sub(read);
            if available > CAPACITY_FRAMES as u64 {
                // A violated consumer-ownership contract must not create an
                // unbounded loop or index arbitrary memory.
                self.counters.rejected.fetch_add(1, Ordering::Relaxed);
                continue;
            }
            if lifetime == 0 || lifetime & 1 != 0 {
                stale += available;
                client.read.store(write, Ordering::Release);
                client.primed.store(false, Ordering::Relaxed);
                continue;
            }
            if client.seen_lifetime.load(Ordering::Relaxed) != lifetime
                || client.seen_epoch.load(Ordering::Relaxed) != epoch
            {
                client.primed.store(false, Ordering::Relaxed);
                client.seen_lifetime.store(lifetime, Ordering::Relaxed);
                client.seen_epoch.store(epoch, Ordering::Relaxed);
            }
            let generation = lifetime / 2;
            // At most CAPACITY/256 packet skips, including a partial packet.
            while read != write {
                let packet = &client.tags[(read as usize / CHANNEL_SAMPLES) & (PACKETS - 1)];
                if packet.generation.load(Ordering::Relaxed) == generation
                    && packet.epoch.load(Ordering::Relaxed) == epoch
                {
                    break;
                }
                let skip = (CHANNEL_SAMPLES as u64 - read % CHANNEL_SAMPLES as u64)
                    .min(write.wrapping_sub(read));
                read = read.wrapping_add(skip);
                stale += skip;
            }
            let mut primed = client.primed.load(Ordering::Relaxed);
            if !primed && write.wrapping_sub(read) >= target {
                primed = true;
            }
            let mut output = 0;
            if primed {
                while output < frames && read != write {
                    let packet = &client.tags[(read as usize / CHANNEL_SAMPLES) & (PACKETS - 1)];
                    if packet.generation.load(Ordering::Relaxed) != generation
                        || packet.epoch.load(Ordering::Relaxed) != epoch
                        || client.lifetime.load(Ordering::Acquire) != lifetime
                        || self.epoch.load(Ordering::Acquire) != epoch
                        || !self.running.load(Ordering::Acquire)
                    {
                        primed = false;
                        break;
                    }
                    let count = (CHANNEL_SAMPLES - read as usize % CHANNEL_SAMPLES)
                        .min(frames - output)
                        .min(write.wrapping_sub(read) as usize);
                    for frame in 0..count {
                        let offset =
                            (read.wrapping_add(frame as u64) as usize & (CAPACITY_FRAMES - 1)) * 2;
                        left[output + frame] +=
                            f32::from_bits(client.samples[offset].load(Ordering::Relaxed));
                        right[output + frame] +=
                            f32::from_bits(client.samples[offset + 1].load(Ordering::Relaxed));
                    }
                    read = read.wrapping_add(count as u64);
                    output += count;
                }
            }
            consumed += output as u64;
            underrun += (frames - output) as u64;
            if output < frames {
                primed = false;
            }
            client.primed.store(primed, Ordering::Relaxed);
            client.read.store(read, Ordering::Release);
        }

        let mut current = f32::from_bits(self.render_gain.load(Ordering::Relaxed));
        let mut step = f32::from_bits(self.render_step.load(Ordering::Relaxed));
        let mut remaining = self.render_ramp_left.load(Ordering::Relaxed);
        let desired = if muted(control) { 0.0 } else { gain(control) };
        if self.seen_controls.load(Ordering::Relaxed) != control {
            self.seen_controls.store(control, Ordering::Relaxed);
            if muted(control) {
                current = 0.0;
                step = 0.0;
                remaining = 0;
            } else {
                step = (desired - current) / GAIN_RAMP_FRAMES as f32;
                remaining = GAIN_RAMP_FRAMES;
            }
        }
        let mut non_silent = 0;
        let mut clipped = 0;
        for (l, r) in left.iter_mut().zip(right.iter_mut()) {
            if remaining != 0 {
                current += step;
                remaining -= 1;
                if remaining == 0 {
                    current = desired;
                }
            }
            let a = *l * current;
            let b = *r * current;
            clipped += (a.abs() > 1.0) as u64 + (b.abs() > 1.0) as u64;
            *l = a.clamp(-1.0, 1.0);
            *r = b.clamp(-1.0, 1.0);
            if *l != 0.0 || *r != 0.0 {
                non_silent += 1;
            }
        }
        self.render_gain.store(current.to_bits(), Ordering::Relaxed);
        self.render_step.store(step.to_bits(), Ordering::Relaxed);
        self.render_ramp_left.store(remaining, Ordering::Relaxed);
        self.counters
            .consumed
            .fetch_add(consumed, Ordering::Relaxed);
        self.counters
            .underrun
            .fetch_add(underrun, Ordering::Relaxed);
        self.counters.stale.fetch_add(stale, Ordering::Relaxed);
        self.counters
            .non_silent
            .fetch_add(non_silent, Ordering::Relaxed);
        self.counters.clipped.fetch_add(clipped, Ordering::Relaxed);
        non_silent != 0
    }

    fn status(&self) -> OHHostAudioTransportV1 {
        let control = self.controls.load(Ordering::Acquire);
        let mut status = OHHostAudioTransportV1::empty();
        status.initialized = 1;
        status.running = self.running.load(Ordering::Acquire) as u32;
        status.profile = profile(control);
        status.target_frames = target_frames(status.profile);
        status.muted = muted(control) as u32;
        status.gain_milli = (gain(control) * 1000.0).round() as u32;
        for client in &self.clients {
            let lifetime = client.lifetime.load(Ordering::Acquire);
            status.active_clients += (lifetime != 0 && lifetime & 1 == 0) as u32;
            let read = client.read.load(Ordering::Acquire);
            let write = client.write.load(Ordering::Acquire);
            status.queued_frames_max = status
                .queued_frames_max
                .max(write.wrapping_sub(read).min(CAPACITY_FRAMES as u64) as u32);
        }
        status.submitted_frames = self.counters.submitted.load(Ordering::Relaxed);
        status.consumed_frames = self.counters.consumed.load(Ordering::Relaxed);
        status.rendered_frames = self.counters.rendered.load(Ordering::Relaxed);
        status.non_silent_frames = self.counters.non_silent.load(Ordering::Relaxed);
        status.underrun_frames = self.counters.underrun.load(Ordering::Relaxed);
        status.overflow_frames = self.counters.overflow.load(Ordering::Relaxed);
        status.stale_frames = self.counters.stale.load(Ordering::Relaxed);
        status.nonfinite_samples = self.counters.nonfinite.load(Ordering::Relaxed);
        status.clipped_samples = self.counters.clipped.load(Ordering::Relaxed);
        status.rejected_events = self.counters.rejected.load(Ordering::Relaxed);
        status.transport_epoch = self.epoch.load(Ordering::Acquire);
        status.controls_revision = self.controls_revision.load(Ordering::Relaxed);
        status
    }
}

/// Fixed C layout shared with native/OwnedHostAudio.h. See that header for units.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct OHHostAudioTransportV1 {
    pub version: u32,
    pub struct_size: u32,
    pub initialized: u32,
    pub running: u32,
    pub active_clients: u32,
    pub profile: u32,
    pub sample_rate_hz: u32,
    pub channels: u32,
    pub capacity_frames: u32,
    pub target_frames: u32,
    pub queued_frames_max: u32,
    pub muted: u32,
    pub gain_milli: u32,
    pub reserved0: u32,
    pub reserved1: u32,
    pub reserved2: u32,
    pub submitted_frames: u64,
    pub consumed_frames: u64,
    pub rendered_frames: u64,
    pub non_silent_frames: u64,
    pub underrun_frames: u64,
    pub overflow_frames: u64,
    pub stale_frames: u64,
    pub nonfinite_samples: u64,
    pub clipped_samples: u64,
    pub rejected_events: u64,
    pub transport_epoch: u64,
    pub controls_revision: u64,
}
impl OHHostAudioTransportV1 {
    fn empty() -> Self {
        Self {
            version: 1,
            struct_size: size_of::<Self>() as u32,
            sample_rate_hz: SAMPLE_RATE,
            channels: 2,
            capacity_frames: CAPACITY_FRAMES as u32,
            target_frames: target_frames(0),
            gain_milli: 1000,
            ..Self::default()
        }
    }
}

static AUDIO: OnceLock<AudioRing> = OnceLock::new();
static INITIALIZATION: Mutex<()> = Mutex::new(());

/// Call before guest execution. Idempotent, allocates once, never called by pull.
pub fn initialize() -> Result<(), &'static str> {
    if AUDIO.get().is_some() {
        return Ok(());
    }
    let _guard = INITIALIZATION
        .lock()
        .map_err(|_| "audio initialization poisoned")?;
    if AUDIO.get().is_none() {
        AUDIO
            .set(AudioRing::new()?)
            .map_err(|_| "audio initialization raced")?;
    }
    Ok(())
}
pub fn activate_client(index: usize, generation: u64) {
    if let Some(audio) = AUDIO.get() {
        audio.activate(index, generation);
    }
}
pub fn submit_frame(index: usize, generation: u64, samples: &[f32; FRAME_SAMPLES]) {
    if let Some(audio) = AUDIO.get() {
        audio.submit(index, generation, samples);
    }
}
pub fn deactivate_client(index: usize, generation: u64) {
    if let Some(audio) = AUDIO.get() {
        audio.deactivate(index, generation);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn OHHostAudioInitializeV1() -> i32 {
    if initialize().is_ok() { 0 } else { -1 }
}
#[unsafe(no_mangle)]
pub extern "C" fn OHHostAudioSetRunningV1(running: u32) -> i32 {
    if running > 1 {
        return -1;
    }
    let Some(audio) = AUDIO.get() else {
        return -2;
    };
    audio.set_running(running == 1);
    0
}
#[unsafe(no_mangle)]
pub extern "C" fn OHHostAudioSetControlsV1(gain: f32, muted: u32, profile: u32) -> i32 {
    let Some(audio) = AUDIO.get() else {
        return -2;
    };
    if audio.set_controls(gain, muted, profile) {
        0
    } else {
        -1
    }
}

fn valid_output(left: *mut f32, right: *mut f32, frames: usize) -> bool {
    if frames == 0
        || frames > MAX_PULL_FRAMES
        || left.is_null()
        || right.is_null()
        || left.addr() % align_of::<f32>() != 0
        || right.addr() % align_of::<f32>() != 0
    {
        return false;
    }
    let bytes = frames * size_of::<f32>();
    let (Some(left_end), Some(right_end)) = (
        left.addr().checked_add(bytes),
        right.addr().checked_add(bytes),
    ) else {
        return false;
    };
    left_end <= right.addr() || right_end <= left.addr()
}

/// # Safety
/// Each pointer denotes `frames` writable floats, exclusively owned for this
/// call. The two ranges are disjoint. Exactly one render consumer calls this.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn OHHostAudioPullStereoV1(
    left: *mut f32,
    right: *mut f32,
    frames: u32,
) -> u32 {
    if !valid_output(left, right, frames as usize) {
        if let Some(audio) = AUDIO.get() {
            audio.counters.rejected.fetch_add(1, Ordering::Relaxed);
        }
        return 0;
    }
    let left = unsafe { std::slice::from_raw_parts_mut(left, frames as usize) };
    let right = unsafe { std::slice::from_raw_parts_mut(right, frames as usize) };
    if let Some(audio) = AUDIO.get() {
        audio.pull(left, right) as u32
    } else {
        left.fill(0.0);
        right.fill(0.0);
        0
    }
}

/// # Safety
/// `out` is aligned, exclusive writable storage for exactly this status type.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn OHHostAudioReadTransportV1(
    out: *mut OHHostAudioTransportV1,
    bytes: u32,
) -> i32 {
    if out.is_null()
        || out.addr() % align_of::<OHHostAudioTransportV1>() != 0
        || bytes as usize != size_of::<OHHostAudioTransportV1>()
    {
        return -1;
    }
    let status = AUDIO
        .get()
        .map_or_else(OHHostAudioTransportV1::empty, AudioRing::status);
    unsafe {
        out.write(status);
    }
    0
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::alloc::{GlobalAlloc, Layout, System};
    use std::cell::Cell;
    use std::sync::{Arc, Barrier};

    thread_local! {
        static TRACK_RENDER_ALLOC: Cell<bool> = const { Cell::new(false) };
        static RENDER_ALLOC_COUNTS: Cell<(u64,u64)> = const { Cell::new((0,0)) };
    }
    struct RenderTestAllocator;
    fn track_allocation(allocation: bool) {
        let _ = TRACK_RENDER_ALLOC.try_with(|enabled| {
            if enabled.get() {
                let _ = RENDER_ALLOC_COUNTS.try_with(|counts| {
                    let (alloc, free) = counts.get();
                    counts.set((alloc + allocation as u64, free + (!allocation) as u64));
                });
            }
        });
    }
    unsafe impl GlobalAlloc for RenderTestAllocator {
        unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
            track_allocation(true);
            unsafe { System.alloc(layout) }
        }
        unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
            track_allocation(true);
            unsafe { System.alloc_zeroed(layout) }
        }
        unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
            track_allocation(false);
            unsafe {
                System.dealloc(ptr, layout);
            }
        }
        unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, bytes: usize) -> *mut u8 {
            track_allocation(true);
            unsafe { System.realloc(ptr, layout, bytes) }
        }
    }
    #[global_allocator]
    static TEST_ALLOCATOR: RenderTestAllocator = RenderTestAllocator;

    fn ring() -> AudioRing {
        let audio = AudioRing::new().unwrap();
        assert!(audio.set_controls(1.0, 0, 1));
        audio.set_running(true);
        audio
    }
    fn frame(left: f32, right: f32) -> [f32; FRAME_SAMPLES] {
        let mut result = [0.0; FRAME_SAMPLES];
        result[..256].fill(left);
        result[256..512].fill(right);
        result
    }
    fn prime(audio: &AudioRing, index: usize, generation: u64, source: &[f32; FRAME_SAMPLES]) {
        for _ in 0..2 {
            assert!(audio.submit(index, generation, source));
        }
    }
    fn pull(audio: &AudioRing, frames: usize) -> (Vec<f32>, Vec<f32>) {
        let mut left = vec![123.0; frames];
        let mut right = vec![123.0; frames];
        audio.pull(&mut left, &mut right);
        (left, right)
    }

    #[test]
    fn planar_channel_order_and_downmix_coefficients() {
        for channel in 0..6 {
            let audio = ring();
            let mut source = [0.0; FRAME_SAMPLES];
            source[channel * 256] = 0.25;
            prime(&audio, 0, 1, &source);
            let (left, right) = pull(&audio, 256);
            let expected = match channel {
                0 => (0.25, 0.0),
                1 => (0.0, 0.25),
                2 => (0.25 * CENTER_REAR_GAIN, 0.25 * CENTER_REAR_GAIN),
                3 => (0.125, 0.125),
                4 => (0.25 * CENTER_REAR_GAIN, 0.0),
                _ => (0.0, 0.25 * CENTER_REAR_GAIN),
            };
            assert_eq!((left[0], right[0]), expected);
            assert!(left[1..].iter().chain(&right[1..]).all(|v| *v == 0.0));
        }
    }

    #[test]
    fn independent_clients_mix_at_same_time_instead_of_concatenating() {
        let audio = ring();
        prime(&audio, 0, 1, &frame(0.2, 0.0));
        prime(&audio, 1, 2, &frame(0.3, 0.4));
        let (left, right) = pull(&audio, 512);
        assert!(left.iter().all(|v| (*v - 0.5).abs() < 1e-6));
        assert!(right.iter().all(|v| *v == 0.4));
        let status = audio.status();
        assert_eq!(status.consumed_frames, 1024);
        assert_eq!(status.rendered_frames, 512);
        assert_eq!(status.non_silent_frames, 512);
        assert_eq!(status.active_clients, 2);
    }

    #[test]
    fn nonfinite_and_out_of_range_inputs_are_bounded() {
        let audio = ring();
        let mut source = frame(f32::INFINITY, f32::NAN);
        source[0] = f32::MAX;
        source[256] = -f32::MAX;
        source[512] = 1.0;
        source[768] = 1.0;
        prime(&audio, 0, 1, &source);
        let (left, right) = pull(&audio, 512);
        assert!(
            left.iter()
                .chain(&right)
                .all(|v| v.is_finite() && (-1.0..=1.0).contains(v))
        );
        assert_eq!(left[0], 1.0);
        assert!(audio.status().nonfinite_samples > 0);
        assert!(audio.status().clipped_samples > 0);
    }

    #[test]
    fn prebuffer_underflow_and_rebuffer_are_explicit() {
        let audio = ring();
        assert!(audio.submit(0, 1, &frame(0.2, 0.3)));
        assert!(pull(&audio, 256).0.iter().all(|v| *v == 0.0));
        assert_eq!(audio.status().queued_frames_max, 256);
        assert!(audio.submit(0, 1, &frame(0.2, 0.3)));
        assert_eq!(pull(&audio, 600).0[511], 0.2);
        assert_eq!(audio.status().underrun_frames, 344);
        assert!(audio.submit(0, 1, &frame(0.4, 0.0)));
        assert_eq!(pull(&audio, 1).0, [0.0]);
        assert!(audio.submit(0, 1, &frame(0.4, 0.0)));
        assert_eq!(pull(&audio, 1).0, [0.4]);
    }

    #[test]
    fn queue_full_drops_newest_without_overwriting_unread_audio() {
        let audio = ring();
        for n in 0..PACKETS {
            assert!(audio.submit(0, 1, &frame(n as f32 / 64.0, 0.0)));
        }
        assert!(!audio.submit(0, 1, &frame(-1.0, -1.0)));
        assert_eq!(audio.status().overflow_frames, 256);
        let (left, _) = pull(&audio, CAPACITY_FRAMES);
        for n in 0..PACKETS {
            assert!(
                left[n * 256..(n + 1) * 256]
                    .iter()
                    .all(|v| *v == n as f32 / 64.0)
            );
        }
        assert_eq!(audio.status().queued_frames_max, 0);
    }

    #[test]
    fn generation_tombstones_drop_late_frames_and_reordered_activation() {
        let audio = ring();
        prime(&audio, 0, 1, &frame(0.9, 0.0));
        assert_eq!(pull(&audio, 17).0, [0.9; 17]);
        audio.deactivate(0, 1);
        audio.activate(0, 1);
        assert!(!audio.submit(0, 1, &frame(0.9, 0.0)));
        assert_eq!(audio.status().active_clients, 0);
        // A new frame can precede its activation callback. It cannot resurrect g1.
        prime(&audio, 0, 2, &frame(0.2, 0.0));
        audio.deactivate(0, 1);
        audio.activate(0, 1);
        assert!(!audio.submit(0, 1, &frame(0.9, 0.0)));
        assert_eq!(pull(&audio, 512).0, [0.2; 512]);
        assert_eq!(audio.status().active_clients, 1);
        assert!(audio.status().stale_frames >= 495);
    }

    #[test]
    fn stop_and_profile_changes_discard_queued_old_epochs() {
        let audio = ring();
        prime(&audio, 0, 1, &frame(0.9, 0.0));
        audio.set_running(false);
        assert!(!audio.submit(0, 1, &frame(0.8, 0.0)));
        assert_eq!(pull(&audio, 512).0, [0.0; 512]);
        audio.set_running(true);
        prime(&audio, 0, 1, &frame(0.2, 0.0));
        assert_eq!(pull(&audio, 512).0, [0.2; 512]);
        prime(&audio, 0, 1, &frame(0.7, 0.0));
        assert!(audio.set_controls(1.0, 0, 0));
        for _ in 0..8 {
            assert!(audio.submit(0, 1, &frame(0.3, 0.0)));
        }
        assert_eq!(pull(&audio, 512).0, [0.3; 512]);
        assert_eq!(audio.status().target_frames, 2048);
    }

    #[test]
    fn mute_is_immediate_unmute_and_gain_changes_ramp() {
        let audio = ring();
        prime(&audio, 0, 1, &frame(0.8, 0.8));
        assert_eq!(pull(&audio, 1).0, [0.8]);
        assert!(audio.set_controls(1.0, 1, 1));
        assert_eq!(pull(&audio, 128).0, [0.0; 128]);
        assert!(audio.set_controls(0.5, 0, 1));
        let (left, _) = pull(&audio, 128);
        assert!(left.windows(2).all(|pair| pair[0] <= pair[1]));
        assert_eq!(left[127], 0.4);
        assert_eq!(audio.status().gain_milli, 500);
        assert_eq!(audio.status().muted, 0);
        for (level, mute, profile) in [
            (f32::NAN, 0, 0),
            (1.1, 0, 0),
            (-0.1, 0, 0),
            (0.5, 2, 0),
            (0.5, 0, 3),
        ] {
            assert!(!audio.set_controls(level, mute, profile));
        }
        assert_eq!(audio.status().gain_milli, 500);
    }

    #[test]
    fn read_and_write_positions_wrap_without_aliasing_unread_packets() {
        let audio = ring();
        let start = u64::MAX - 511;
        audio.clients[0].read.store(start, Ordering::Relaxed);
        audio.clients[0].write.store(start, Ordering::Relaxed);
        prime(&audio, 0, 1, &frame(0.4, 0.1));
        assert_eq!(audio.clients[0].write.load(Ordering::Relaxed), 0);
        assert_eq!(pull(&audio, 512).0, [0.4; 512]);
        assert_eq!(audio.clients[0].read.load(Ordering::Relaxed), 0);
        prime(&audio, 0, 1, &frame(0.2, 0.3));
        assert_eq!(pull(&audio, 512).0, [0.2; 512]);
    }

    #[test]
    fn bounded_concurrent_producer_and_consumer_preserve_packet_order() {
        let audio = Arc::new(ring());
        let barrier = Arc::new(Barrier::new(2));
        let producer = audio.clone();
        let sync = barrier.clone();
        let task = std::thread::spawn(move || {
            for round in 0..64 {
                let value = (round + 1) as f32 / 128.0;
                prime(&producer, 0, 1, &frame(value, -value));
                sync.wait();
                sync.wait();
            }
        });
        for round in 0..64 {
            barrier.wait();
            let (left, right) = pull(&audio, 512);
            let value = (round + 1) as f32 / 128.0;
            assert_eq!(left, [value; 512]);
            assert_eq!(right, [-value; 512]);
            barrier.wait();
        }
        task.join().unwrap();
        assert_eq!(audio.status().overflow_frames, 0);
        assert_eq!(audio.status().consumed_frames, 64 * 512);
    }

    #[test]
    fn concurrent_producer_misuse_and_invalid_lifetimes_are_refused() {
        assert_eq!(active_tag(0), None);
        assert_eq!(active_tag(u64::MAX / 2), Some(u64::MAX - 1));
        assert_eq!(active_tag(u64::MAX / 2 + 1), None);
        assert_eq!(active_tag(u64::MAX), None);
        let audio = ring();
        audio.clients[0]
            .producer_busy
            .store(true, Ordering::Relaxed);
        assert!(!audio.submit(0, 1, &frame(0.2, 0.0)));
        audio.clients[0]
            .producer_busy
            .store(false, Ordering::Release);
        assert!(!audio.submit(MAX_CLIENTS, 1, &frame(0.2, 0.0)));
        audio.activate(0, 0);
        audio.deactivate(0, u64::MAX);
        assert_eq!(audio.status().rejected_events, 4);
    }

    #[test]
    fn c_status_layout_and_pull_argument_validation() {
        assert_eq!(size_of::<OHHostAudioTransportV1>(), 160);
        assert_eq!(align_of::<OHHostAudioTransportV1>(), 8);
        assert_eq!(
            std::mem::offset_of!(OHHostAudioTransportV1, submitted_frames),
            64
        );
        let mut left = [99.0; 4];
        let mut right = [99.0; 4];
        assert!(valid_output(left.as_mut_ptr(), right.as_mut_ptr(), 4));
        assert!(!valid_output(left.as_mut_ptr(), left.as_mut_ptr(), 4));
        assert!(!valid_output(
            left.as_mut_ptr(),
            right.as_mut_ptr(),
            MAX_PULL_FRAMES + 1
        ));
        assert!(!valid_output(std::ptr::null_mut(), right.as_mut_ptr(), 4));
        assert!(!valid_output(
            (usize::MAX - 3) as *mut f32,
            right.as_mut_ptr(),
            4
        ));
        unsafe {
            assert_eq!(
                OHHostAudioPullStereoV1(left.as_mut_ptr(), left.as_mut_ptr(), 4),
                0
            );
        }
        assert_eq!(left, [99.0; 4]);
        let mut status = OHHostAudioTransportV1::default();
        status.version = 55;
        unsafe {
            assert_eq!(OHHostAudioReadTransportV1(&mut status, 159), -1);
        }
        assert_eq!(status.version, 55);
        assert_eq!(OHHostAudioInitializeV1(), 0);
        assert_eq!(OHHostAudioInitializeV1(), 0);
        assert_eq!(OHHostAudioSetRunningV1(0), 0);
        assert_eq!(OHHostAudioSetRunningV1(2), -1);
        unsafe {
            assert_eq!(
                OHHostAudioPullStereoV1(left.as_mut_ptr(), right.as_mut_ptr(), 4),
                0
            );
        }
        assert_eq!(left, [0.0; 4]);
        assert_eq!(right, [0.0; 4]);
        unsafe {
            assert_eq!(OHHostAudioReadTransportV1(&mut status, 160), 0);
        }
        assert_eq!(
            (status.version, status.struct_size, status.initialized),
            (1, 160, 1)
        );
    }

    #[test]
    fn render_mixing_stale_skip_and_underflow_allocate_and_free_nothing() {
        let audio = ring();
        for index in 0..MAX_CLIENTS {
            prime(&audio, index, index as u64 + 1, &frame(0.01, 0.02));
        }
        audio.deactivate(0, 1);
        prime(&audio, 0, 9, &frame(0.03, 0.04));
        let mut left = vec![0.0; MAX_PULL_FRAMES];
        let mut right = vec![0.0; MAX_PULL_FRAMES];
        RENDER_ALLOC_COUNTS.with(|counts| counts.set((0, 0)));
        TRACK_RENDER_ALLOC.with(|enabled| enabled.set(true));
        audio.pull(&mut left[..256], &mut right[..256]);
        audio.pull(&mut left, &mut right);
        audio.pull(&mut left, &mut right);
        TRACK_RENDER_ALLOC.with(|enabled| enabled.set(false));
        assert_eq!(RENDER_ALLOC_COUNTS.with(Cell::get), (0, 0));
        assert!(audio.status().consumed_frames > 0);
        assert!(audio.status().stale_frames > 0);
        assert!(audio.status().underrun_frames > 0);
    }
}
