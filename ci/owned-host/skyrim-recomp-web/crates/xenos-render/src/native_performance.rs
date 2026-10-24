//! Coarse native-renderer observations. No timers, GPU work, or I/O are started
//! here. The native loop supplies its existing five-second report measurements.
use std::sync::Mutex;
use std::time::{Duration, Instant};

use crate::executor::{ExecutorStats, OpTimes};
use crate::shared_memory::MirrorStats;
use xenos_texture_cache::CacheStats;
use super::frame_stability::FrameStabilitySnapshot;

const STALE_AFTER_MS: u64 = 10_000;

/// A fixed-size observation of one completed native report window.
///
/// `*_total` counters are cumulative for this renderer session;
/// byte gauges without that suffix describe current residency. Window times
/// are host elapsed time, may include driver blocking, and may overlap/nest.
/// They are neither CPU thread time nor GPU execution time and are not additive.
/// Executor submissions exclude the separate native presentation submissions.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct NativePerformanceSnapshot {
    /// Time from native renderer-loop initialization to this sample's window end.
    pub sample_end_elapsed_ms: u64,
    pub window_ms: u64,
    /// Age at the getter call; a stalled renderer leaves an explicitly stale sample.
    pub age_ms: u64,
    /// True when the reported `age_ms` exceeds 10,000.
    pub stale: bool,
    pub presentable_swaps_window: u64,
    pub draws_window: u64,
    pub draws_total: u64,
    pub skipped_draws_total: u64,
    pub resolves_total: u64,
    pub gpu_resolves_total: u64,
    pub submissions_total: u64,
    pub pipelines_total: u64,
    pub upload_waits_total: u64,
    pub constant_upload_bytes_total: u64,
    pub index_upload_bytes_total: u64,
    pub memory_upload_bytes_total: u64,
    pub texture_set_misses_total: u64,
    pub texture_set_stale_total: u64,
    /// Synchronization-stage binding attempts; these are not completed draws.
    pub vertex_range_eligible_bindings_total: u64,
    pub vertex_range_narrowed_bindings_total: u64,
    pub vertex_range_avoided_pages_total: u64,
    pub vertex_range_fallback_bindings_total: u64,
    pub texture_loads_total: u64,
    pub texture_reloads_total: u64,
    pub texture_hits_total: u64,
    pub texture_fallbacks_total: u64,
    pub texture_deferred_total: u64,
    pub texture_evictions_total: u64,
    pub texture_host_bytes: u64,
    pub texture_pooled_bytes: u64,
    pub mirror_slot_bytes: u64,
    pub mirror_blocks: u64,
    pub mirror_evictions_total: u64,
    pub mirror_slot_conflicts_total: u64,
    pub texture_sets_window_ms: u64,
    pub pipelines_window_ms: u64,
    pub shaders_window_ms: u64,
    pub resolves_window_ms: u64,
    pub submits_window_ms: u64,
    /// Existing native-loop busy accounting: command/presentation work including
    /// driver waits, excluding iterations that only wait for title submissions.
    pub renderer_busy_window_ms: u64,
    /// Native swap-callback completion intervals, including undecodable swaps.
    /// A gap crossing a report boundary is assigned to the window it ends in.
    pub swap_intervals_total: u64,
    pub swap_gap_max_window_ms: u64,
    pub swap_gap_max_total_ms: u64,
    pub swap_gaps_over_100ms_total: u64,
    pub swap_gaps_over_250ms_total: u64,
    pub swap_gaps_over_1000ms_total: u64,
    /// Accepted monotonic guest-enqueue spacing and enqueue-to-renderer-dequeue wait. These do not
    /// identify which guest thread ran or measure guest CPU execution time.
    pub source_samples_window: u64,
    pub source_gap_max_window_ms: u64,
    pub source_gap_max_total_ms: u64,
    pub swap_queue_wait_max_window_ms: u64,
    pub swap_queue_wait_max_total_ms: u64,
    /// Native presentation API calls; completion is not display/GPU completion.
    /// Failed calls contribute cost/count, but never a successful-present gap.
    pub present_calls_window: u64,
    pub present_call_max_window_ms: u64,
    pub present_call_max_total_ms: u64,
    pub present_failures_total: u64,
    pub present_gap_max_window_ms: u64,
    pub present_gap_max_total_ms: u64,
    /// Busy-probe counts by reason; these do not measure GPU/CPU time.
    pub gpu_frame_limit_polls_total: u64,
    pub gpu_upload_limit_polls_total: u64,
    pub gpu_resolve_pending_polls_total: u64,
    /// Longest existing pipeline-create/check measurement, truncated to ms.
    pub pipeline_call_max_total_ms: u64,
    /// Longest measured pump/observation/error-check segment, including idle
    /// queue wait and driver blocking. Excludes event processing and reporting.
    pub renderer_iteration_max_total_ms: u64,
    pub surface_configures_total: u64,
}

pub(super) struct Window {
    pub end_elapsed: Duration,
    pub span: Duration,
    pub busy: Duration,
    pub draws_before: u64,
    pub presentable_swaps: u64,
    pub frame_stability: FrameStabilitySnapshot,
    pub iteration_max_ms: u64,
    pub surface_configures: u64,
}

fn milliseconds(time: Duration) -> Option<u64> {
    time.as_millis().try_into().ok()
}

impl NativePerformanceSnapshot {
    pub(super) fn capture(
        window: Window,
        stats: ExecutorStats,
        textures: CacheStats,
        mirror: MirrorStats,
        times: OpTimes,
        previous_times: OpTimes,
    ) -> Option<Self> {
        if window.span < Duration::from_millis(1) || window.end_elapsed < window.span || window.busy > window.span {
            return None;
        }
        let delta = |now: Duration, before: Duration| milliseconds(now.checked_sub(before)?);
        Some(Self {
            sample_end_elapsed_ms: milliseconds(window.end_elapsed)?,
            window_ms: milliseconds(window.span)?,
            age_ms: 0,
            stale: false,
            presentable_swaps_window: window.presentable_swaps,
            draws_window: stats.draws.checked_sub(window.draws_before)?,
            draws_total: stats.draws,
            skipped_draws_total: stats.skipped_draws,
            resolves_total: stats.resolves,
            gpu_resolves_total: stats.gpu_resolves,
            submissions_total: stats.submissions,
            pipelines_total: stats.pipelines,
            upload_waits_total: stats.upload_waits,
            constant_upload_bytes_total: stats.constant_upload_bytes,
            index_upload_bytes_total: stats.index_upload_bytes,
            memory_upload_bytes_total: stats.memory_upload_bytes,
            texture_set_misses_total: stats.texture_set_misses,
            texture_set_stale_total: stats.texture_set_stale,
            vertex_range_eligible_bindings_total: stats.vertex_range_eligible_bindings,
            vertex_range_narrowed_bindings_total: stats.vertex_range_narrowed_bindings,
            vertex_range_avoided_pages_total: stats.vertex_range_avoided_pages,
            vertex_range_fallback_bindings_total: stats.vertex_range_fallback_bindings,
            texture_loads_total: textures.loads,
            texture_reloads_total: textures.reloads,
            texture_hits_total: textures.hits,
            texture_fallbacks_total: textures.fallbacks,
            texture_deferred_total: textures.deferred,
            texture_evictions_total: textures.evicted,
            texture_host_bytes: textures.host_bytes,
            texture_pooled_bytes: textures.pooled_bytes,
            mirror_slot_bytes: mirror.slot_bytes,
            mirror_blocks: u64::from(mirror.blocks),
            mirror_evictions_total: mirror.evictions,
            mirror_slot_conflicts_total: mirror.slot_conflicts,
            texture_sets_window_ms: delta(times.texture_sets, previous_times.texture_sets)?,
            pipelines_window_ms: delta(times.pipelines, previous_times.pipelines)?,
            shaders_window_ms: delta(times.shaders, previous_times.shaders)?,
            resolves_window_ms: delta(times.resolves, previous_times.resolves)?,
            submits_window_ms: delta(times.submits, previous_times.submits)?,
            renderer_busy_window_ms: milliseconds(window.busy)?,
            swap_intervals_total: window.frame_stability.swap_intervals_total,
            swap_gap_max_window_ms: window.frame_stability.swap_gap_max_window_ms,
            swap_gap_max_total_ms: window.frame_stability.swap_gap_max_total_ms,
            swap_gaps_over_100ms_total: window.frame_stability.swap_gaps_over_100ms_total,
            swap_gaps_over_250ms_total: window.frame_stability.swap_gaps_over_250ms_total,
            swap_gaps_over_1000ms_total: window.frame_stability.swap_gaps_over_1000ms_total,
            source_samples_window: window.frame_stability.source_samples_window,
            source_gap_max_window_ms: window.frame_stability.source_gap_max_window_ms,
            source_gap_max_total_ms: window.frame_stability.source_gap_max_total_ms,
            swap_queue_wait_max_window_ms: window.frame_stability.swap_queue_wait_max_window_ms,
            swap_queue_wait_max_total_ms: window.frame_stability.swap_queue_wait_max_total_ms,
            present_calls_window: window.frame_stability.present_calls_window,
            present_call_max_window_ms: window.frame_stability.present_call_max_window_ms,
            present_call_max_total_ms: window.frame_stability.present_call_max_total_ms,
            present_failures_total: window.frame_stability.present_failures_total,
            present_gap_max_window_ms: window.frame_stability.present_gap_max_window_ms,
            present_gap_max_total_ms: window.frame_stability.present_gap_max_total_ms,
            gpu_frame_limit_polls_total: stats.gpu_frame_limit_polls,
            gpu_upload_limit_polls_total: stats.gpu_upload_limit_polls,
            gpu_resolve_pending_polls_total: stats.gpu_resolve_pending_polls,
            pipeline_call_max_total_ms: stats.pipeline_call_max_ms,
            renderer_iteration_max_total_ms: window.iteration_max_ms,
            surface_configures_total: window.surface_configures,
        })
    }
}

#[derive(Clone, Copy)]
struct Published {
    observed_at: Instant,
    snapshot: NativePerformanceSnapshot,
}

#[derive(Default)]
pub(super) struct PerformanceStore(Mutex<Option<Published>>);

impl PerformanceStore {
    /// Contention/poison/refused measurements preserve the preceding sample.
    /// Collect cache gauges only after admission; the reader also never waits.
    pub fn try_publish(
        &self,
        observed_at: Instant,
        capture: impl FnOnce() -> Option<NativePerformanceSnapshot>,
    ) -> bool {
        let Ok(mut slot) = self.0.try_lock() else { return false };
        if slot.is_some_and(|old| old.observed_at >= observed_at) {
            return false;
        }
        let Some(snapshot) = capture() else { return false };
        *slot = Some(Published { observed_at, snapshot });
        true
    }

    pub fn snapshot_at(&self, now: Instant) -> Option<NativePerformanceSnapshot> {
        let published = (*self.0.try_lock().ok()?)?;
        // The lock guard above is gone before calculating age or returning data.
        let age_ms = milliseconds(now.checked_duration_since(published.observed_at)?)?;
        Some(NativePerformanceSnapshot { age_ms, stale: age_ms > STALE_AFTER_MS, ..published.snapshot })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::{Arc, Barrier, mpsc};

    fn sample(draws: u64) -> NativePerformanceSnapshot {
        NativePerformanceSnapshot::capture(
            Window {
                end_elapsed: Duration::from_secs(12),
                span: Duration::from_secs(5),
                busy: Duration::from_millis(4321),
                draws_before: 0,
                presentable_swaps: 9,
                frame_stability: FrameStabilitySnapshot::default(),
                iteration_max_ms: 0,
                surface_configures: 0,
            },
            ExecutorStats { draws, constant_upload_bytes: draws * 3, ..Default::default() },
            CacheStats::default(),
            MirrorStats::default(),
            OpTimes::default(),
            OpTimes::default(),
        )
        .unwrap()
    }

    #[test]
    fn captures_totals_gauges_and_existing_window_times_without_resetting_them() {
        let stats = ExecutorStats {
            draws: 150,
            skipped_draws: 2,
            resolves: 3,
            gpu_resolves: 4,
            submissions: 5,
            pipelines: 6,
            upload_waits: 7,
            constant_upload_bytes: 8,
            index_upload_bytes: 9,
            memory_upload_bytes: 10,
            texture_set_misses: 11,
            texture_set_stale: 12,
            vertex_range_eligible_bindings: 29,
            vertex_range_narrowed_bindings: 28,
            vertex_range_avoided_pages: 27,
            vertex_range_fallback_bindings: 26,
            gpu_frame_limit_polls: 110, gpu_upload_limit_polls: 220,
            gpu_resolve_pending_polls: 330, pipeline_call_max_ms: 440,
            ..Default::default()
        };
        let textures = CacheStats {
            loads: 13,
            reloads: 14,
            hits: 15,
            fallbacks: 16,
            deferred: 17,
            evicted: 18,
            host_bytes: 19,
            pooled_bytes: 20,
            ..Default::default()
        };
        let mirror = MirrorStats { slot_bytes: 21, blocks: 22, evictions: 23, slot_conflicts: 24 };
        let times = OpTimes {
            texture_sets: Duration::from_millis(1100),
            pipelines: Duration::from_millis(1200),
            shaders: Duration::from_millis(1300),
            resolves: Duration::from_millis(1400),
            submits: Duration::from_millis(1500),
        };
        let before = OpTimes {
            texture_sets: Duration::from_secs(1),
            pipelines: Duration::from_secs(1),
            shaders: Duration::from_secs(1),
            resolves: Duration::from_secs(1),
            submits: Duration::from_secs(1),
        };
        let result = NativePerformanceSnapshot::capture(
            Window {
                end_elapsed: Duration::from_secs(12),
                span: Duration::from_secs(5),
                busy: Duration::from_micros(4321999),
                draws_before: 100,
                presentable_swaps: 9,
                frame_stability: FrameStabilitySnapshot {
                    swap_intervals_total: 50, swap_gap_max_window_ms: 1200,
                    swap_gap_max_total_ms: 2000, swap_gaps_over_100ms_total: 7,
                    swap_gaps_over_250ms_total: 3, swap_gaps_over_1000ms_total: 2,
                    source_samples_window: 9, source_gap_max_window_ms: 100,
                    source_gap_max_total_ms: 200, swap_queue_wait_max_window_ms: 300,
                    swap_queue_wait_max_total_ms: 700, present_calls_window: 5,
                    present_call_max_window_ms: 21, present_call_max_total_ms: 88,
                    present_failures_total: 1, present_gap_max_window_ms: 1205,
                    present_gap_max_total_ms: 2500,
                },
                iteration_max_ms: 1000,
                surface_configures: 2,
            },
            stats,
            textures,
            mirror,
            times,
            before,
        )
        .unwrap();
        assert_eq!(
            (result.sample_end_elapsed_ms, result.window_ms, result.age_ms, result.stale),
            (12000, 5000, 0, false)
        );
        assert_eq!((result.presentable_swaps_window, result.draws_window, result.draws_total), (9, 50, 150));
        assert_eq!(
            [
                result.skipped_draws_total,
                result.resolves_total,
                result.gpu_resolves_total,
                result.submissions_total,
                result.pipelines_total,
                result.upload_waits_total,
                result.constant_upload_bytes_total,
                result.index_upload_bytes_total,
                result.memory_upload_bytes_total,
                result.texture_set_misses_total,
                result.texture_set_stale_total,
                result.texture_loads_total,
                result.texture_reloads_total,
                result.texture_hits_total,
                result.texture_fallbacks_total,
                result.texture_deferred_total,
                result.texture_evictions_total,
                result.texture_host_bytes,
                result.texture_pooled_bytes,
                result.mirror_slot_bytes,
                result.mirror_blocks,
                result.mirror_evictions_total,
                result.mirror_slot_conflicts_total,
            ],
            std::array::from_fn::<_, 23, _>(|i| i as u64 + 2)
        );
        assert_eq!(
            [
                result.texture_sets_window_ms,
                result.pipelines_window_ms,
                result.shaders_window_ms,
                result.resolves_window_ms,
                result.submits_window_ms,
                result.renderer_busy_window_ms
            ],
            [100, 200, 300, 400, 500, 4321]
        );
        assert_eq!(stats.draws, 150);
        assert_eq!([result.vertex_range_eligible_bindings_total, result.vertex_range_narrowed_bindings_total,
            result.vertex_range_avoided_pages_total, result.vertex_range_fallback_bindings_total], [29, 28, 27, 26]);
        assert_eq!(times.submits, Duration::from_millis(1500));
        assert_eq!([
            result.swap_intervals_total, result.swap_gap_max_window_ms, result.swap_gap_max_total_ms,
            result.swap_gaps_over_100ms_total, result.swap_gaps_over_250ms_total, result.swap_gaps_over_1000ms_total,
            result.source_samples_window, result.source_gap_max_window_ms, result.swap_queue_wait_max_window_ms,
            result.present_calls_window, result.present_call_max_window_ms, result.present_failures_total,
            result.present_gap_max_window_ms, result.source_gap_max_total_ms,
            result.swap_queue_wait_max_total_ms, result.present_call_max_total_ms, result.present_gap_max_total_ms,
        ], [50, 1200, 2000, 7, 3, 2, 9, 100, 300, 5, 21, 1, 1205, 200, 700, 88, 2500]);
        assert_eq!([result.gpu_frame_limit_polls_total, result.gpu_upload_limit_polls_total,
            result.gpu_resolve_pending_polls_total, result.pipeline_call_max_total_ms,
            result.renderer_iteration_max_total_ms, result.surface_configures_total], [110, 220, 330, 440, 1000, 2]);
        assert!(std::mem::size_of::<NativePerformanceSnapshot>() <= 512);
    }

    #[test]
    fn invalid_windows_and_counter_resets_refuse_instead_of_underflowing() {
        let valid = || Window {
            end_elapsed: Duration::from_secs(10),
            span: Duration::from_secs(5),
            busy: Duration::from_secs(1),
            draws_before: 0,
            presentable_swaps: 0,
            frame_stability: FrameStabilitySnapshot::default(),
            iteration_max_ms: 0,
            surface_configures: 0,
        };
        let capture = |window, now, before| {
            NativePerformanceSnapshot::capture(
                window,
                ExecutorStats::default(),
                CacheStats::default(),
                MirrorStats::default(),
                now,
                before,
            )
        };
        let mut window = valid();
        window.span = Duration::ZERO;
        assert!(capture(window, OpTimes::default(), OpTimes::default()).is_none());
        let mut window = valid();
        window.end_elapsed = Duration::from_secs(4);
        assert!(capture(window, OpTimes::default(), OpTimes::default()).is_none());
        let mut window = valid();
        window.busy = Duration::from_secs(6);
        assert!(capture(window, OpTimes::default(), OpTimes::default()).is_none());
        let mut window = valid();
        window.draws_before = 1;
        assert!(capture(window, OpTimes::default(), OpTimes::default()).is_none());
        let mut window = valid();
        window.end_elapsed = Duration::from_secs(u64::MAX);
        assert!(capture(window, OpTimes::default(), OpTimes::default()).is_none());
        for index in 0..5 {
            let mut before = OpTimes::default();
            let field = match index {
                0 => &mut before.texture_sets,
                1 => &mut before.pipelines,
                2 => &mut before.shaders,
                3 => &mut before.resolves,
                _ => &mut before.submits,
            };
            *field = Duration::from_nanos(1);
            assert!(capture(valid(), OpTimes::default(), before).is_none());
        }
        assert!(capture(valid(), OpTimes::default(), OpTimes::default()).is_some());
    }

    #[test]
    fn missing_age_staleness_and_refused_publications_are_explicit() {
        let store = PerformanceStore::default();
        let now = Instant::now();
        assert!(store.snapshot_at(now).is_none());
        assert!(store.try_publish(now, || Some(sample(7))));
        assert_eq!(store.snapshot_at(now).unwrap().draws_total, 7);
        assert!(store.snapshot_at(now - Duration::from_nanos(1)).is_none());
        let boundary = store.snapshot_at(now + Duration::from_secs(10)).unwrap();
        assert_eq!((boundary.age_ms, boundary.stale), (10000, false));
        let stale = store.snapshot_at(now + Duration::from_millis(10001)).unwrap();
        assert_eq!((stale.age_ms, stale.stale), (10001, true));
        assert!(!store.try_publish(now, || panic!("out-of-order publication must not collect")));
        assert!(!store.try_publish(now + Duration::from_secs(1), || None));
        assert_eq!(store.snapshot_at(now + Duration::from_secs(1)).unwrap().draws_total, 7);
    }

    #[test]
    fn contended_publication_and_getter_do_not_wait_or_collect() {
        let store = Arc::new(PerformanceStore::default());
        let now = Instant::now();
        assert!(store.try_publish(now, || Some(sample(3))));
        let (ready_tx, ready_rx) = mpsc::sync_channel(1);
        let (release_tx, release_rx) = mpsc::sync_channel(1);
        let other = store.clone();
        let holder = std::thread::spawn(move || {
            let _guard = other.0.lock().unwrap();
            ready_tx.send(()).unwrap();
            // Bounded even if a regression replaces try_lock with a blocking lock.
            let _ = release_rx.recv_timeout(Duration::from_secs(2));
        });
        ready_rx.recv_timeout(Duration::from_secs(2)).unwrap();
        assert!(store.snapshot_at(now).is_none());
        assert!(!store.try_publish(now + Duration::from_secs(1), || panic!("contended writer must not collect")));
        release_tx.send(()).unwrap();
        holder.join().unwrap();
        assert_eq!(store.snapshot_at(now).unwrap().draws_total, 3);
    }

    #[test]
    fn concurrent_readers_never_observe_torn_numeric_samples_and_poison_refuses() {
        let store = Arc::new(PerformanceStore::default());
        let start = Instant::now();
        assert!(store.try_publish(start, || Some(sample(1))));
        let barrier = Arc::new(Barrier::new(2));
        let other = store.clone();
        let other_barrier = barrier.clone();
        let writer = std::thread::spawn(move || {
            other_barrier.wait();
            for value in 2..1002 {
                other.try_publish(start + Duration::from_nanos(value), || Some(sample(value)));
            }
        });
        barrier.wait();
        for _ in 0..2000 {
            if let Some(snapshot) = store.snapshot_at(start + Duration::from_secs(1)) {
                assert_eq!(snapshot.constant_upload_bytes_total, snapshot.draws_total * 3);
                assert_eq!(snapshot.draws_window, snapshot.draws_total);
            }
        }
        writer.join().unwrap();
        assert!(store.try_publish(start + Duration::from_secs(2), || Some(sample(1002))));
        assert_eq!(store.snapshot_at(start + Duration::from_secs(2)).unwrap().draws_total, 1002);
        let _ = std::panic::catch_unwind(|| {
            let _guard = store.0.lock().unwrap();
            panic!("authored poison");
        });
        assert!(store.snapshot_at(start + Duration::from_secs(2)).is_none());
        assert!(!store.try_publish(start + Duration::from_secs(3), || panic!("poisoned writer must not collect")));
    }
}
