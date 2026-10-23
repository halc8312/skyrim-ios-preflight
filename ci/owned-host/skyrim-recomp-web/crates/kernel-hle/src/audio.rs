//! The XAudio render driver: titles register a client callback, and the
//! system asks each client for one frame (256 samples x 6 channels, 48 kHz)
//! every 5.33 ms while it has room in its queue. Frames drain at real-time
//! rate into an optional host output. Behavior follows Xenia's AudioSystem.

use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex, Once, OnceLock};
use std::time::{Duration, Instant};

use runtime_core::PPCContext;

use crate::Kernel;

pub const MAX_CLIENTS: usize = 8;
pub const CHANNELS: usize = 6;
pub const CHANNEL_SAMPLES: usize = 256;
pub const FRAME_SAMPLES: usize = CHANNELS * CHANNEL_SAMPLES;
pub const SAMPLE_RATE: u32 = 48_000;
/// Version of the optional host-only lifecycle observer contract below.
pub const AUDIO_OBSERVER_VERSION: u32 = 1;
/// Frames a client may have queued ahead of playback.
const MAX_QUEUED_FRAMES: u32 = 64;
/// One frame of audio: 256 samples at 48 kHz.
const FRAME_PERIOD: Duration = Duration::from_nanos(1_000_000_000 * CHANNEL_SAMPLES as u64 / SAMPLE_RATE as u64);

/// Receives every played frame: channel-major (6 blocks of 256 samples:
/// front left, front right, center, LFE, rear left, rear right).
pub type AudioOutput = Arc<dyn Fn(usize, &[f32; FRAME_SAMPLES]) + Send + Sync>;

/// Optional host transport events. The generation is assigned at registration
/// and travels with drained frames; slot reuse never relabels an old frame.
/// Events run outside `clients`' mutex, so concurrent lifecycle notifications
/// can arrive out of order. Receivers must make Deactivated(g) terminal for g
/// and ignore events older than the latest generation. Install before clients.
pub enum AudioEvent<'a> {
    Activated { client: usize, generation: u64 },
    Frame { client: usize, generation: u64, samples: &'a [f32; FRAME_SAMPLES] },
    Deactivated { client: usize, generation: u64 },
}
pub type AudioObserver = Arc<dyn for<'a> Fn(AudioEvent<'a>) + Send + Sync>;
type PlayedFrame = (usize, u64, Box<[f32; FRAME_SAMPLES]>);

/// Driver handle given to the title (Xenia's encoding).
const DRIVER_TAG: u32 = 0x4155_0000;

struct Client {
    generation: u64,
    callback: u32,
    /// Guest word holding the title's callback argument; its address is
    /// what the callback receives.
    wrapped_arg: u32,
    /// Frames submitted and not yet played.
    queued: u32,
    next_pump: Instant,
    pending: std::collections::VecDeque<Box<[f32; FRAME_SAMPLES]>>,
}

#[derive(Default)]
pub struct AudioSystem {
    clients: Mutex<[Option<Client>; MAX_CLIENTS]>,
    worker: Once,
    output: OnceLock<AudioOutput>,
    observer: OnceLock<AudioObserver>,
    next_generation: AtomicU64,
}

impl AudioSystem {
    /// Installs the host's output (once; later calls are ignored).
    pub fn set_output(&self, output: AudioOutput) {
        let _ = self.output.set(output);
    }

    pub fn client_count(&self) -> usize {
        self.clients.lock().unwrap().iter().filter(|c| c.is_some()).count()
    }

    /// Install once, before the first client has ever registered. The old
    /// index-only output remains independent for existing web hosts.
    pub fn set_observer(&self, observer: AudioObserver) -> bool {
        let clients = self.clients.lock().unwrap();
        if self.next_generation.load(Ordering::Relaxed) != 0 {
            drop(clients);
            return false;
        }
        let result = self.observer.set(observer);
        drop(clients);
        result.is_ok()
    }

    fn observe(&self, event: AudioEvent<'_>) {
        if let Some(observer) = self.observer.get() {
            observer(event);
        }
    }

    fn insert_client(&self, callback: u32, wrapped_arg: u32, now: Instant) -> Option<usize> {
        let (index, generation) = {
            let mut clients = self.clients.lock().unwrap();
            let index = clients.iter().position(Option::is_none)?;
            // Leave the high bit free for hosts' ordered active/terminal tags.
            let generation = self
                .next_generation
                .fetch_update(Ordering::Relaxed, Ordering::Relaxed, |n| n.checked_add(1).filter(|n| *n <= u64::MAX / 2))
                .ok()?
                + 1;
            clients[index] = Some(Client {
                generation,
                callback,
                wrapped_arg,
                queued: 0,
                next_pump: now,
                pending: Default::default(),
            });
            (index, generation)
        };
        self.observe(AudioEvent::Activated { client: index, generation });
        Some(index)
    }

    fn remove_client(&self, index: usize) -> Option<Client> {
        let removed = self.clients.lock().unwrap().get_mut(index).and_then(Option::take);
        if let Some(client) = &removed {
            self.observe(AudioEvent::Deactivated { client: index, generation: client.generation });
        }
        removed
    }

    fn client_generation(&self, index: usize) -> Option<u64> {
        self.clients.lock().unwrap().get(index)?.as_ref().map(|c| c.generation)
    }

    fn queue_frame(&self, index: usize, generation: u64, frame: Box<[f32; FRAME_SAMPLES]>) -> bool {
        let mut clients = self.clients.lock().unwrap();
        if let Some(Some(c)) = clients.get_mut(index)
            && c.generation == generation
        {
            c.queued += 1;
            c.pending.push_back(frame);
            return true;
        }
        false
    }

    fn drain_period(&self, finished: &mut Vec<PlayedFrame>) {
        let mut clients = self.clients.lock().unwrap();
        for (index, client) in clients.iter_mut().enumerate() {
            if let Some(client) = client
                && let Some(frame) = client.pending.pop_front()
            {
                client.queued -= 1;
                finished.push((index, client.generation, frame));
            }
        }
    }

    fn deliver(&self, finished: &[PlayedFrame]) {
        for (client, generation, samples) in finished {
            if let Some(output) = self.output.get() {
                output(*client, samples);
            }
            self.observe(AudioEvent::Frame { client: *client, generation: *generation, samples });
        }
    }
}

pub fn driver_handle(index: usize) -> u32 {
    DRIVER_TAG | index as u32
}

pub fn client_index(driver: u32) -> Option<usize> {
    (driver & 0xFFFF_0000 == DRIVER_TAG).then_some((driver & 0xFFFF) as usize).filter(|&i| i < MAX_CLIENTS)
}

impl Kernel {
    /// XAudioRegisterRenderDriverClient. Returns the client index.
    pub fn register_audio_client(&'static self, callback: u32, callback_arg: u32) -> Option<usize> {
        let wrapped_arg = self.rt.space.system_alloc(4, 4).ok()?;
        self.mem().write_u32(wrapped_arg, callback_arg);
        let Some(index) = self.audio.insert_client(callback, wrapped_arg, Instant::now()) else {
            let _ = self.rt.space.system_free(wrapped_arg);
            return None;
        };
        log::info!("audio: client {index} registered (callback {callback:#010X}, arg {callback_arg:#010X})");
        self.audio.worker.call_once(|| self.spawn_kernel_thread("audio", audio_worker_main));
        Some(index)
    }

    pub fn unregister_audio_client(&self, index: usize) {
        let removed = self.audio.remove_client(index);
        if let Some(c) = removed {
            let _ = self.rt.space.system_free(c.wrapped_arg);
        }
    }

    /// XAudioSubmitRenderDriverFrame: copies the title's big-endian samples.
    pub fn submit_audio_frame(&self, index: usize, samples: u32) {
        let Some(generation) = self.audio.client_generation(index) else {
            return;
        };
        let mem = self.mem();
        let mut frame = Box::new([0f32; FRAME_SAMPLES]);
        for (i, s) in frame.iter_mut().enumerate() {
            *s = mem.read_f32(samples + i as u32 * 4);
        }
        self.audio.queue_frame(index, generation, frame);
    }
}

fn audio_worker_main(k: &'static Kernel, ctx: &mut PPCContext) {
    let start = Instant::now();
    let mut played_until = start;
    loop {
        // Play frames that are due, one per period, freeing queue slots.
        let now = Instant::now();
        let mut finished: Vec<PlayedFrame> = Vec::new();
        while played_until + FRAME_PERIOD <= now {
            played_until += FRAME_PERIOD;
            k.audio.drain_period(&mut finished);
        }
        k.audio.deliver(&finished);

        // Ask the most overdue client with room for another frame.
        let due = {
            let mut clients = k.audio.clients.lock().unwrap();
            let pick = clients
                .iter_mut()
                .enumerate()
                .filter_map(|(i, c)| c.as_mut().map(|c| (i, c)))
                .filter(|(_, c)| c.queued < MAX_QUEUED_FRAMES)
                .min_by_key(|(_, c)| c.next_pump);
            pick.and_then(|(_, c)| {
                (c.next_pump <= now).then(|| {
                    c.next_pump = c.next_pump.max(now.checked_sub(FRAME_PERIOD).unwrap_or(now)) + FRAME_PERIOD;
                    (c.callback, c.wrapped_arg)
                })
            })
        };
        match due {
            Some((callback, arg)) => {
                let saved = ctx.clone();
                ctx.set_gpr(3, arg as u64);
                if let Err(e) = k.rt.call(ctx, callback) {
                    log::error!("audio client callback {callback:#010X}: {e}");
                }
                *ctx = saved;
            }
            None => std::thread::sleep(Duration::from_millis(1)),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn driver_handles_round_trip() {
        assert_eq!(AUDIO_OBSERVER_VERSION, 1);
        assert_eq!(driver_handle(3), 0x4155_0003);
        assert_eq!(client_index(0x4155_0003), Some(3));
        assert_eq!(client_index(0x1234_0003), None);
        assert_eq!(client_index(0x4155_0009), None);
        assert_eq!(FRAME_PERIOD.as_micros(), 5333);
    }

    #[test]
    fn observer_tags_late_frames_and_runs_outside_client_mutex() {
        let audio = Arc::new(AudioSystem::default());
        let observed = Arc::new(Mutex::new(Vec::new()));
        let weak = Arc::downgrade(&audio);
        let sink = observed.clone();
        assert!(audio.set_observer(Arc::new(move |event| {
            let owner = weak.upgrade().unwrap();
            assert!(owner.clients.try_lock().is_ok());
            let event = match event {
                AudioEvent::Activated { client, generation } => (0, client, generation),
                AudioEvent::Frame { client, generation, samples } => {
                    assert_eq!(samples[0], 0.25);
                    (1, client, generation)
                }
                AudioEvent::Deactivated { client, generation } => (2, client, generation),
            };
            sink.lock().unwrap().push(event);
        })));
        assert_eq!(audio.insert_client(0x1000, 0x2000, Instant::now()), Some(0));
        assert!(!audio.set_observer(Arc::new(|_| {})));
        let mut frame = Box::new([0.0; FRAME_SAMPLES]);
        frame[0] = 0.25;
        assert!(audio.queue_frame(0, 1, frame));
        let mut finished = Vec::new();
        audio.drain_period(&mut finished);
        assert_eq!(finished[0].1, 1);
        assert!(audio.remove_client(0).is_some());
        assert_eq!(audio.insert_client(0x3000, 0x4000, Instant::now()), Some(0));
        assert!(!audio.queue_frame(0, 1, Box::new([0.5; FRAME_SAMPLES])));
        assert_eq!(audio.client_generation(0), Some(2));
        audio.deliver(&finished);
        assert_eq!(*observed.lock().unwrap(), [(0, 0, 1), (2, 0, 1), (0, 0, 2), (1, 0, 1)]);
        assert_eq!(audio.client_count(), 1);
    }

    #[test]
    fn observer_is_optional_and_legacy_period_delivery_is_unchanged() {
        let audio = AudioSystem::default();
        let delivered = Arc::new(Mutex::new(Vec::new()));
        let sink = delivered.clone();
        audio.set_output(Arc::new(move |client, frame| sink.lock().unwrap().push((client, frame[0]))));
        for index in 0..2 {
            assert_eq!(audio.insert_client(0, 0, Instant::now()), Some(index));
            let mut frame = Box::new([0.0; FRAME_SAMPLES]);
            frame[0] = index as f32;
            assert!(audio.queue_frame(index, audio.client_generation(index).unwrap(), frame));
        }
        let mut finished = Vec::new();
        audio.drain_period(&mut finished);
        audio.deliver(&finished);
        assert_eq!(*delivered.lock().unwrap(), [(0, 0.0), (1, 1.0)]);
        assert!(audio.clients.lock().unwrap().iter().flatten().all(|c| c.queued == 0));
        assert!(audio.remove_client(MAX_CLIENTS).is_none());
    }

    #[test]
    fn exhausted_generation_never_wraps_into_a_reused_identity() {
        let audio = AudioSystem::default();
        audio.next_generation.store(u64::MAX / 2, Ordering::Relaxed);
        assert_eq!(audio.insert_client(0, 0, Instant::now()), None);
        assert_eq!(audio.client_count(), 0);
    }
}
