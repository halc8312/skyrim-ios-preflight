//! Bounded diagnostic-only observations. Every shared word is atomic; readers
//! never lock, poll the GPU, inspect guest memory, or wait for a writer.

use std::sync::{Arc, OnceLock};
use std::sync::atomic::{AtomicU64, Ordering::SeqCst};
use std::time::Instant;

pub const STAGES: usize = 24;
pub const REASONS: usize = 32;
pub const LONG_US: u64 = 250_000;

/// Additional host boundaries; kept separate from the stable 24-stage IDs.
/// None of these durations is GPU execution time. Only the renderer writes them.
#[repr(usize)]
#[derive(Clone, Copy)]
pub enum Detail { MainEncoderFinish, ConstantEncoderFinish, CompletionRegistration }
pub const DETAILS: usize = 3;

/// Flush reasons occupy 0..=16; known fatal contracts occupy 24..=30.
pub(crate) fn fatal_reason_index(reason: u32) -> usize {
    match reason { 10..=16 => reason as usize + 14, _ => 31 }
}

#[repr(u64)]
#[derive(Clone, Copy)]
pub enum Stage {
    Idle, InputWait, Command, Draw, Resolve, Poll, FrameEnd, FrontBuffer,
    Configure, Acquire, PresentEncode, PresentSubmit, PresentCall, Flush,
    EncodePass, IndexUpload, Constants, TextureUpload, QueueSubmit, Cleanup,
    GpuWait, AudioDrain, AudioDeliver, Failed,
}

struct Record<const N: usize> {
    stamp: AtomicU64,
    words: [AtomicU64; N],
}
impl<const N: usize> Default for Record<N> {
    fn default() -> Self { Self { stamp: AtomicU64::new(0), words: std::array::from_fn(|_| AtomicU64::new(0)) } }
}
impl<const N: usize> Record<N> {
    fn write(&self, generation: u64, words: [u64; N]) {
        self.stamp.store(generation * 2 - 1, SeqCst);
        for (out, value) in self.words.iter().zip(words) { out.store(value, SeqCst); }
        self.stamp.store(generation * 2, SeqCst);
    }
    fn read(&self, expected: Option<u64>) -> Option<(u64, [u64; N])> {
        for _ in 0..3 {
            if let Some(record) = self.read_once(expected) { return Some(record); }
        }
        None
    }
    fn read_once(&self, expected: Option<u64>) -> Option<(u64, [u64; N])> {
        let first = self.stamp.load(SeqCst);
        if first == 0 || first & 1 != 0 || expected.is_some_and(|g| first != g * 2) { return None; }
        let words = std::array::from_fn(|i| self.words[i].load(SeqCst));
        (self.stamp.load(SeqCst) == first).then_some((first / 2, words))
    }
}

struct Live {
    published: AtomicU64,
    records: [Record<7>; 2],
}
impl Default for Live {
    fn default() -> Self { Self { published: AtomicU64::new(0), records: std::array::from_fn(|_| Record::default()) } }
}
impl Live {
    fn publish(&self, words: [u64; 7]) {
        let generation = self.published.load(SeqCst);
        if generation >= u64::MAX / 2 - 1 { return; }
        let next = generation + 1;
        self.records[(next & 1) as usize].write(next, words);
        self.published.store(next, SeqCst);
    }
    fn read(&self) -> Option<[u64; 8]> {
        for _ in 0..3 {
            let generation = self.published.load(SeqCst);
            if generation == 0 { return None; }
            if let Some((_, words)) = self.records[(generation & 1) as usize].read_once(Some(generation)) {
                let mut result = [0; 8]; result[0] = generation; result[1..].copy_from_slice(&words);
                return Some(result);
            }
        }
        None
    }
}

struct Ring<const N: usize, const CAP: usize> {
    total: AtomicU64,
    entries: Box<[Record<N>; CAP]>,
}
impl<const N: usize, const CAP: usize> Default for Ring<N, CAP> {
    fn default() -> Self { Self { total: AtomicU64::new(0), entries: Box::new(std::array::from_fn(|_| Record::default())) } }
}
impl<const N: usize, const CAP: usize> Ring<N, CAP> {
    fn push(&self, words: [u64; N]) {
        // Exactly one writer per domain. Never wrap a generation into an old one.
        let previous = self.total.load(SeqCst);
        if previous >= u64::MAX / 2 - 1 { return; }
        let next = previous + 1;
        self.entries[(previous % CAP as u64) as usize].write(next, words);
        self.total.store(next, SeqCst);
    }
    fn snapshot(&self) -> (u64, u64, Vec<Vec<u64>>) {
        let total = self.total.load(SeqCst);
        let mut unavailable = 0;
        let first = total.saturating_sub(CAP as u64) + 1;
        let mut rows = Vec::with_capacity((total.min(CAP as u64)) as usize);
        for generation in first..=total {
            match self.entries[((generation - 1) % CAP as u64) as usize].read(Some(generation)) {
                Some((_, words)) => {
                    let mut row = Vec::with_capacity(N + 1);
                    row.push(generation); row.extend(words); rows.push(row);
                }
                None => unavailable += 1,
            }
        }
        (total.saturating_sub(CAP as u64), unavailable, rows)
    }
}

#[derive(Default)]
pub struct Control(OnceLock<Arc<Session>>);
impl Control {
    pub fn start(&self, session: u64, origin: Instant) -> bool {
        self.0.set(Arc::new(Session::new(session, origin))).is_ok()
    }
    pub fn session(&self) -> Option<Arc<Session>> { self.0.get().cloned() }
}

pub struct Session {
    pub id: u64,
    origin: Instant,
    live: Live,
    audio_live: Live,
    render: Ring<7, 256>,
    audio: Ring<7, 128>,
    frames: Ring<15, 512>,
    totals: [Record<5>; STAGES],
    reasons: [AtomicU64; REASONS],
    seq: AtomicU64,
    swap: AtomicU64,
    previous_frame_us: AtomicU64,
    failure: AtomicU64,
    details: [Record<5>; DETAILS],
    capacity_rejections: [AtomicU64; 4],
}
impl Session {
    pub fn new(id: u64, origin: Instant) -> Self {
        Self { id, origin, live: Live::default(), audio_live: Live::default(),
            render: Ring::default(), audio: Ring::default(), frames: Ring::default(),
            totals: std::array::from_fn(|_| Record::default()),
            reasons: std::array::from_fn(|_| AtomicU64::new(0)), seq: AtomicU64::new(0), swap: AtomicU64::new(0),
            previous_frame_us: AtomicU64::new(0), failure: AtomicU64::new(0),
            details: std::array::from_fn(|_| Record::default()),
            capacity_rejections: std::array::from_fn(|_| AtomicU64::new(0)) }
    }
    pub fn elapsed_us(&self, at: Instant) -> u64 {
        at.checked_duration_since(self.origin).map_or(0, |d| u64::try_from(d.as_micros()).unwrap_or(u64::MAX))
    }
    pub fn command(&self, seq: u64) { self.seq.store(seq, SeqCst); }
    pub fn fail(&self, reason: u32) { let _ = self.failure.compare_exchange(0, u64::from(reason), SeqCst, SeqCst); }
    pub fn reason(&self, reason: usize) {
        if let Some(counter) = self.reasons.get(reason) { saturating_add(counter, 1); }
    }
    /// Bits: block bytes, record count, changed values, invalid/overflow demand.
    /// Several limits may reject the same planned allocation.
    pub fn constant_capacity_rejected(&self, mask: u8) {
        for (bit, count) in self.capacity_rejections.iter().enumerate() {
            if mask & (1 << bit) != 0 { saturating_add(count, 1); }
        }
    }
    fn finish_stage(&self, stage: usize, elapsed: u64) {
        let Some(record) = self.totals.get(stage) else { return };
        let (generation, old) = record.read(None).unwrap_or((0, [0; 5]));
        if generation >= u64::MAX / 2 - 1 { return; }
        record.write(generation + 1, [old[0].saturating_add(1), old[1].saturating_add(elapsed), old[2].max(elapsed),
            old[3].saturating_add(u64::from(elapsed > LONG_US)), old[4].saturating_add(u64::from(elapsed > 1_000_000))]);
    }
    pub fn frame(&self, finished: Instant) {
        let previous = self.swap.load(SeqCst);
        let next = previous.saturating_add(1);
        self.swap.store(next, SeqCst);
        let mut words = [0; 15];
        let end = self.elapsed_us(finished);
        let previous_time = self.previous_frame_us.swap(end, SeqCst);
        let interval = if previous == 0 { 0 } else { end.saturating_sub(previous_time) };
        words[..4].copy_from_slice(&[end, self.seq.load(SeqCst), next, interval]);
        self.frames.push(words);
    }
    pub fn audio_begin(&self, at: Instant) {
        self.audio_live.publish([self.elapsed_us(at), 0, 0, 0, Stage::AudioDrain as u64, 0, 0]);
    }
    fn finish_audio_stage(&self, at: Instant, next: Stage, empty_periods: u64, frames: u64) {
        let start = self.audio_live.read().map_or(self.elapsed_us(at), |record| record[1]);
        let end = self.elapsed_us(at);
        let duration = end.saturating_sub(start);
        let stage = self.audio_live.read().map_or(Stage::AudioDrain as u64, |record| record[5]);
        if duration >= LONG_US || empty_periods > 0 {
            self.audio.push([start, duration, 0, 0, stage, empty_periods, frames]);
        }
        self.finish_stage(stage as usize, duration);
        self.audio_live.publish([end, 0, 0, 0, next as u64, 0, 0]);
    }
    pub fn audio_drained(&self, at: Instant, empty_periods: u64, frames: u64) {
        self.finish_audio_stage(at, Stage::AudioDeliver, empty_periods, frames);
    }
    pub fn audio_end(&self, at: Instant) { self.finish_audio_stage(at, Stage::Idle, 0, 0); }
    pub fn snapshot(&self) -> Snapshot {
        let capture_started_us = self.elapsed_us(Instant::now());
        let mut result = Snapshot { session: self.id, capture_started_us, observed_us: 0, failure: self.failure.load(SeqCst), live: self.live.read(), audio_live: self.audio_live.read(),
            render: self.render.snapshot(), audio: self.audio.snapshot(), frames: self.frames.snapshot(),
            totals: self.totals.iter().map(|record| record.read(None).map(|(_, words)| words)).collect(),
            reasons: std::array::from_fn(|i| self.reasons[i].load(SeqCst)),
            details: self.details.iter().map(|record| record.read(None).map(|(_, words)| words)).collect(),
            capacity_rejections: std::array::from_fn(|i| self.capacity_rejections[i].load(SeqCst)) };
        result.observed_us = self.elapsed_us(Instant::now());
        result
    }
}

pub struct Snapshot {
    pub session: u64,
    pub capture_started_us: u64,
    pub failure: u64,
    pub observed_us: u64,
    pub live: Option<[u64; 8]>,
    pub audio_live: Option<[u64; 8]>,
    pub render: (u64, u64, Vec<Vec<u64>>),
    pub audio: (u64, u64, Vec<Vec<u64>>),
    pub frames: (u64, u64, Vec<Vec<u64>>),
    pub totals: Vec<Option<[u64; 5]>>,
    pub reasons: [u64; REASONS],
    pub details: Vec<Option<[u64; 5]>>,
    pub capacity_rejections: [u64; 4],
}

/// No clock reads, allocations or atomics when explicit diagnostics are off.
pub struct DetailGuard<'a> { session: &'a Session, detail: usize, started: Instant }
impl<'a> DetailGuard<'a> {
    pub fn enter(session: &'a Option<Arc<Session>>, detail: Detail) -> Option<Self> {
        Some(Self { session: session.as_deref()?, detail: detail as usize, started: Instant::now() })
    }
}
impl Drop for DetailGuard<'_> {
    fn drop(&mut self) {
        let elapsed = u64::try_from(self.started.elapsed().as_micros()).unwrap_or(u64::MAX);
        let record = &self.session.details[self.detail];
        let (generation, old) = record.read(None).unwrap_or((0, [0; 5]));
        if generation >= u64::MAX / 2 - 1 { return; }
        record.write(generation + 1, [old[0].saturating_add(1), old[1].saturating_add(elapsed), old[2].max(elapsed),
            old[3].saturating_add(u64::from(elapsed > LONG_US)), old[4].saturating_add(u64::from(elapsed > 1_000_000))]);
    }
}

pub struct Guard {
    session: Arc<Session>,
    previous: [u64; 7],
    words: [u64; 7],
    stage: usize,
}
impl Guard {
    pub fn enter(session: &Option<Arc<Session>>, stage: Stage) -> Option<Self> {
        let session = session.as_ref()?.clone();
        let previous = session.live.read().map_or([0; 7], |row| row[1..].try_into().unwrap());
        let words = [session.elapsed_us(Instant::now()), 0, session.seq.load(SeqCst), session.swap.load(SeqCst), stage as u64, 0, 0];
        session.live.publish(words);
        Some(Self { session, previous, words, stage: stage as usize })
    }
}
impl Drop for Guard {
    fn drop(&mut self) {
        let elapsed = self.session.elapsed_us(Instant::now()).saturating_sub(self.words[0]);
        self.words[1] = elapsed;
        self.session.finish_stage(self.stage, elapsed);
        if elapsed >= LONG_US { self.session.render.push(self.words); }
        self.session.live.publish(self.previous);
    }
}

fn saturating_add(counter: &AtomicU64, value: u64) {
    let _ = counter.fetch_update(SeqCst, SeqCst, |old| Some(old.saturating_add(value)));
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn every_fatal_contract_has_a_distinct_in_bounds_reason() {
        let indexes: std::collections::BTreeSet<_> = (10..=16).map(fatal_reason_index).collect();
        assert_eq!(indexes.len(), 7);
        assert!(indexes.iter().all(|&index| 16 < index && index < REASONS));
        assert_eq!(fatal_reason_index(u32::MAX), 31);
    }
    #[test]
    fn storage_is_bounded_and_overwritten_events_are_explicit() {
        assert_eq!(std::mem::size_of::<Record<7>>(), 64);
        assert_eq!(std::mem::size_of::<Record<15>>(), 128);
        let allocated = std::mem::size_of::<Session>() + std::mem::size_of::<[Record<7>; 256]>()
            + std::mem::size_of::<[Record<7>; 128]>() + std::mem::size_of::<[Record<15>; 512]>();
        assert!(allocated < 128 * 1024);
        let session = Session::new(3, Instant::now());
        assert!(session.snapshot().live.is_none());
        for i in 0..300 { session.render.push([i, 1, 2, 3, 4, 5, 6]); }
        let snapshot = session.snapshot();
        assert_eq!((snapshot.render.0, snapshot.render.1, snapshot.render.2.len()), (44, 0, 256));
        assert_eq!(snapshot.render.2[0][0], 45);
    }
    #[test]
    fn partially_written_records_are_unavailable_and_nested_guards_restore_phase() {
        let session = Some(Arc::new(Session::new(7, Instant::now())));
        let outer = Guard::enter(&session, Stage::Draw).unwrap();
        { let _inner = Guard::enter(&session, Stage::QueueSubmit).unwrap(); }
        assert_eq!(session.as_ref().unwrap().snapshot().live.unwrap()[5], Stage::Draw as u64);
        drop(outer);
        let record = Record::<7>::default();
        record.stamp.store(3, SeqCst);
        assert!(record.read(None).is_none());
        record.write(3, [9; 7]);
        assert!(record.read(Some(2)).is_none());
        assert_eq!(record.read(Some(3)), Some((3, [9; 7])));
    }
    #[test]
    fn concurrent_readers_never_observe_a_mixed_tuple() {
        let live = Arc::new(Live::default());
        let writer = live.clone();
        let thread = std::thread::spawn(move || { for i in 1..10_000 { writer.publish([i; 7]); } });
        for _ in 0..10_000 {
            if let Some(row) = live.read() { assert!(row[1..].iter().all(|v| *v == row[1])); }
        }
        thread.join().unwrap();
    }
    #[test]
    fn detail_is_opt_in_and_preserves_live_stage_and_capacity_causes() {
        assert!(DetailGuard::enter(&None, Detail::MainEncoderFinish).is_none());
        let session = Some(Arc::new(Session::new(8, Instant::now())));
        let _outer = Guard::enter(&session, Stage::Flush);
        { let _detail = DetailGuard::enter(&session, Detail::MainEncoderFinish); }
        let s = session.as_ref().unwrap();
        s.constant_capacity_rejected(1 | 4);
        s.constant_capacity_rejected(4);
        let snapshot = s.snapshot();
        assert_eq!(snapshot.live.unwrap()[5], Stage::Flush as u64);
        assert_eq!(snapshot.details[0].unwrap()[0], 1);
        assert!(snapshot.details[1].is_none());
        assert_eq!(snapshot.capacity_rejections, [1, 0, 2, 0]);
        assert_eq!(snapshot.totals.len(), 24);
    }
}
