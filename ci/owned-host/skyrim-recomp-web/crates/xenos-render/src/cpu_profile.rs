//! Optional numeric renderer diagnostics, with no logging or file output.
//!
//! These are host elapsed times, not CPU thread time or GPU execution time. A
//! phase may include driver work or blocking; the separately reported pipeline
//! validation wait, driver poll, and submission times identify those call sites.
//! Operations are nested within draw phases and must not be added to them.
//! The Cargo feature and each Executor's runtime switch are both off by default.

use crate::renderer::Stopwatch;

pub const DRAW_STAGE_NAMES: [&str; 10] = [
    "draw_setup",
    "shaders",
    "output_state",
    "ring_space",
    "vertex_memory",
    "textures",
    "targets",
    "constants",
    "pipeline",
    "indices_record",
];

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct HostTime {
    pub calls: u64,
    pub elapsed_ns: u64,
}

/// Accumulates since profiling was enabled or the preceding snapshot was taken.
/// Taking one immediately after `Executor::end_frame` yields a frame breakdown.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct CpuProfileSnapshot {
    pub enabled: bool,
    pub draw_attempts: u64,
    pub draws_recorded: u64,
    pub draws_skipped: u64,
    pub draw_elapsed_ns: u64,
    pub draw_stages: [HostTime; DRAW_STAGE_NAMES.len()],
    /// Executor vertex/index/resolve synchronization. Texture cache source sync
    /// is inside the texture draw phase instead.
    pub shared_sync: HostTime,
    /// Requested range bytes, including repeated/overlapping ranges; not uploads.
    pub shared_sync_requested_bytes: u64,
    pub busy: HostTime,
    pub busy_true: u64,
    pub upload_queue_walk: HostTime,
    /// Remaining entries visited by the outstanding-byte sum, after retirement.
    /// Retired entries are counted separately below.
    pub upload_queue_entries_scanned: u64,
    pub upload_queue_entries_retired: u64,
    pub upload_queue_max: u64,
    pub pipeline_create: HostTime,
    /// Elapsed time inside native `block_on(pop_error_scope())`.
    pub pipeline_validation_wait: HostTime,
    /// Driver event processing, which may execute callbacks or block internally.
    pub driver_poll: HostTime,
    /// Batch encoding/upload/submission, including driver call overhead.
    pub submit: HostTime,
}

#[derive(Clone, Copy)]
pub(crate) enum Operation {
    SharedSync,
    Busy,
    UploadQueueWalk,
    PipelineCreate,
    PipelineValidationWait,
    DriverPoll,
    Submit,
}

#[derive(Default)]
pub(crate) struct CpuProfile {
    enabled: bool,
    snapshot: CpuProfileSnapshot,
    draw_start: Option<f64>,
    stage: Option<(usize, f64)>,
}

fn nanos(milliseconds: f64) -> u64 {
    (milliseconds.max(0.0) * 1_000_000.0) as u64
}

fn add(time: &mut HostTime, elapsed_ns: u64) {
    time.calls = time.calls.saturating_add(1);
    time.elapsed_ns = time.elapsed_ns.saturating_add(elapsed_ns);
}

impl CpuProfile {
    pub fn set_enabled(&mut self, enabled: bool) {
        self.enabled = enabled;
        self.snapshot = CpuProfileSnapshot::default();
        self.draw_start = None;
        self.stage = None;
    }

    pub fn take(&mut self) -> CpuProfileSnapshot {
        CpuProfileSnapshot { enabled: self.enabled, ..std::mem::take(&mut self.snapshot) }
    }

    pub fn start(&self) -> Option<Stopwatch> {
        self.enabled.then(Stopwatch::start)
    }

    pub fn operation(&mut self, operation: Operation, started: Option<Stopwatch>) {
        let Some(started) = started else { return };
        let elapsed_ns = started.elapsed().as_nanos().min(u128::from(u64::MAX)) as u64;
        let time = match operation {
            Operation::SharedSync => &mut self.snapshot.shared_sync,
            Operation::Busy => &mut self.snapshot.busy,
            Operation::UploadQueueWalk => &mut self.snapshot.upload_queue_walk,
            Operation::PipelineCreate => &mut self.snapshot.pipeline_create,
            Operation::PipelineValidationWait => &mut self.snapshot.pipeline_validation_wait,
            Operation::DriverPoll => &mut self.snapshot.driver_poll,
            Operation::Submit => &mut self.snapshot.submit,
        };
        add(time, elapsed_ns);
    }

    pub fn busy_result(&mut self, busy: bool) {
        if self.enabled {
            self.snapshot.busy_true = self.snapshot.busy_true.saturating_add(u64::from(busy));
        }
    }

    pub fn shared_range(&mut self, bytes: u32) {
        if self.enabled {
            self.snapshot.shared_sync_requested_bytes =
                self.snapshot.shared_sync_requested_bytes.saturating_add(u64::from(bytes));
        }
    }

    pub fn upload_queue(&mut self, before: usize, remaining: usize) {
        if self.enabled {
            self.snapshot.upload_queue_entries_scanned =
                self.snapshot.upload_queue_entries_scanned.saturating_add(remaining as u64);
            self.snapshot.upload_queue_entries_retired =
                self.snapshot.upload_queue_entries_retired.saturating_add((before - remaining) as u64);
            self.snapshot.upload_queue_max = self.snapshot.upload_queue_max.max(before as u64);
        }
    }

    pub fn begin_draw(&mut self) {
        if self.enabled {
            self.begin_at(crate::renderer::now_ms());
        }
    }

    fn begin_at(&mut self, now: f64) {
        self.draw_start = Some(now);
        self.stage = Some((0, now));
        self.snapshot.draw_attempts = self.snapshot.draw_attempts.saturating_add(1);
    }

    pub fn draw_stage(&mut self, stage: usize) {
        if self.enabled {
            self.stage_at(stage, crate::renderer::now_ms());
        }
    }

    fn stage_at(&mut self, stage: usize, now: f64) {
        if let Some((previous, started)) = self.stage.take() {
            add(&mut self.snapshot.draw_stages[previous], nanos(now - started));
        }
        self.stage = (stage < DRAW_STAGE_NAMES.len()).then_some((stage, now));
    }

    pub fn end_draw(&mut self, recorded: bool) {
        if self.enabled {
            self.end_at(recorded, crate::renderer::now_ms());
        }
    }

    fn end_at(&mut self, recorded: bool, now: f64) {
        self.stage_at(DRAW_STAGE_NAMES.len(), now);
        if let Some(started) = self.draw_start.take() {
            self.snapshot.draw_elapsed_ns = self.snapshot.draw_elapsed_ns.saturating_add(nanos(now - started));
            if recorded {
                self.snapshot.draws_recorded = self.snapshot.draws_recorded.saturating_add(1);
            } else {
                self.snapshot.draws_skipped = self.snapshot.draws_skipped.saturating_add(1);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn skipped_draws_do_not_reuse_previous_stage_timestamps() {
        let mut profile = CpuProfile::default();
        profile.set_enabled(true);
        profile.begin_at(1.0);
        for stage in 1..=DRAW_STAGE_NAMES.len() {
            profile.stage_at(stage, 1.0 + stage as f64);
        }
        profile.end_at(true, 11.0);
        let complete = profile.take();
        assert_eq!((complete.draw_attempts, complete.draws_recorded, complete.draws_skipped), (1, 1, 0));
        assert!(complete.draw_stages.iter().all(|time| *time == HostTime { calls: 1, elapsed_ns: 1_000_000 }));
        profile.begin_at(20.0);
        profile.end_at(false, 22.0);
        let skipped = profile.take();
        assert_eq!((skipped.draw_attempts, skipped.draws_recorded, skipped.draws_skipped), (1, 0, 1));
        assert_eq!(skipped.draw_stages[0], HostTime { calls: 1, elapsed_ns: 2_000_000 });
        assert_eq!(skipped.draw_elapsed_ns, 2_000_000);
        assert!(skipped.draw_stages[1..].iter().all(|time| *time == HostTime::default()));
        profile.set_enabled(false);
        profile.begin_draw();
        profile.draw_stage(1);
        profile.end_draw(false);
        assert_eq!(profile.take(), CpuProfileSnapshot::default());
    }
}
