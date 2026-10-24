//! Fixed-size host observations of frame delivery. Callers supply timestamps;
//! this module reads no clock, allocates no storage and starts no GPU work.
use std::time::Duration;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(super) struct FrameStabilitySnapshot {
    pub swap_intervals_total: u64,
    pub swap_gap_max_window_ms: u64,
    pub swap_gap_max_total_ms: u64,
    pub swap_gaps_over_100ms_total: u64,
    pub swap_gaps_over_250ms_total: u64,
    pub swap_gaps_over_1000ms_total: u64,
    pub source_samples_window: u64,
    pub source_gap_max_window_ms: u64,
    pub source_gap_max_total_ms: u64,
    pub swap_queue_wait_max_window_ms: u64,
    pub swap_queue_wait_max_total_ms: u64,
    pub present_calls_window: u64,
    pub present_call_max_window_ms: u64,
    pub present_call_max_total_ms: u64,
    pub present_failures_total: u64,
    pub present_gap_max_window_ms: u64,
    pub present_gap_max_total_ms: u64,
}

#[derive(Default)]
pub(super) struct FrameStability {
    values: FrameStabilitySnapshot,
    last_swap: Option<Duration>,
    last_source: Option<Duration>,
    last_present_call_end: Option<Duration>,
    last_successful_present: Option<Duration>,
}

fn ceil_ms(time: Duration) -> u64 {
    let fractional = u128::from(time.subsec_nanos() % 1_000_000 != 0);
    time.as_millis().saturating_add(fractional).try_into().unwrap_or(u64::MAX)
}

impl FrameStability {
    /// A native swap callback finished, including callbacks without a decodable
    /// front buffer. These are host-delivery intervals, not display refreshes.
    pub fn swap(&mut self, when: Duration) {
        if let Some(previous) = self.last_swap {
            let Some(gap) = when.checked_sub(previous) else { return };
            let v = &mut self.values;
            v.swap_intervals_total = v.swap_intervals_total.saturating_add(1);
            v.swap_gap_max_window_ms = v.swap_gap_max_window_ms.max(ceil_ms(gap));
            v.swap_gap_max_total_ms = v.swap_gap_max_total_ms.max(ceil_ms(gap));
            for (threshold, count) in [
                (100, &mut v.swap_gaps_over_100ms_total),
                (250, &mut v.swap_gaps_over_250ms_total),
                (1000, &mut v.swap_gaps_over_1000ms_total),
            ] {
                if gap > Duration::from_millis(threshold) {
                    *count = count.saturating_add(1);
                }
            }
        }
        self.last_swap = Some(when);
    }

    /// Original guest-enqueue timestamps and enqueue-to-renderer-dequeue wait.
    /// Neither source spacing nor this queue wait measures guest CPU time.
    pub fn source(&mut self, enqueued: Duration, queue_wait: Duration) {
        if let Some(previous) = self.last_source {
            let Some(gap) = enqueued.checked_sub(previous) else { return };
            self.values.source_gap_max_window_ms = self.values.source_gap_max_window_ms.max(ceil_ms(gap));
            self.values.source_gap_max_total_ms = self.values.source_gap_max_total_ms.max(ceil_ms(gap));
        }
        self.values.source_samples_window = self.values.source_samples_window.saturating_add(1);
        self.values.swap_queue_wait_max_window_ms = self.values.swap_queue_wait_max_window_ms.max(ceil_ms(queue_wait));
        self.values.swap_queue_wait_max_total_ms = self.values.swap_queue_wait_max_total_ms.max(ceil_ms(queue_wait));
        self.last_source = Some(enqueued);
    }

    /// The native presentation call returned. Its duration can include surface
    /// acquisition/driver blocking; success does not imply GPU/display completion.
    pub fn present(&mut self, started: Duration, ended: Duration, success: bool) {
        let Some(took) = ended.checked_sub(started) else { return };
        if self.last_present_call_end.is_some_and(|previous| started < previous) {
            return;
        }
        let v = &mut self.values;
        v.present_calls_window = v.present_calls_window.saturating_add(1);
        v.present_call_max_window_ms = v.present_call_max_window_ms.max(ceil_ms(took));
        v.present_call_max_total_ms = v.present_call_max_total_ms.max(ceil_ms(took));
        if success {
            if let Some(previous) = self.last_successful_present {
                let gap = ended.saturating_sub(previous);
                v.present_gap_max_window_ms = v.present_gap_max_window_ms.max(ceil_ms(gap));
                v.present_gap_max_total_ms = v.present_gap_max_total_ms.max(ceil_ms(gap));
            }
            self.last_successful_present = Some(ended);
        } else {
            v.present_failures_total = v.present_failures_total.saturating_add(1);
        }
        self.last_present_call_end = Some(ended);
    }

    /// Reset window-only gauges, retaining predecessors across report windows.
    pub fn finish_window(&mut self) -> FrameStabilitySnapshot {
        let finished = self.values;
        self.values.swap_gap_max_window_ms = 0;
        self.values.source_samples_window = 0;
        self.values.source_gap_max_window_ms = 0;
        self.values.swap_queue_wait_max_window_ms = 0;
        self.values.present_calls_window = 0;
        self.values.present_call_max_window_ms = 0;
        self.values.present_gap_max_window_ms = 0;
        finished
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    const MS: fn(u64) -> Duration = Duration::from_millis;

    #[test]
    fn gaps_use_strict_thresholds_and_round_positive_fractions_up() {
        let mut f = FrameStability::default();
        f.swap(MS(0));
        assert_eq!(f.finish_window(), FrameStabilitySnapshot::default());
        for when in [100, 350, 1350] {
            f.swap(MS(when));
        }
        let exact = f.finish_window();
        assert_eq!((exact.swap_intervals_total, exact.swap_gap_max_window_ms), (3, 1000));
        assert_eq!(
            (exact.swap_gaps_over_100ms_total, exact.swap_gaps_over_250ms_total, exact.swap_gaps_over_1000ms_total),
            (2, 1, 0)
        );
        f.swap(MS(2350) + Duration::from_nanos(1));
        let over = f.finish_window();
        assert_eq!((over.swap_gap_max_window_ms, over.swap_gap_max_total_ms), (1001, 1001));
        assert_eq!(
            (over.swap_gaps_over_100ms_total, over.swap_gaps_over_250ms_total, over.swap_gaps_over_1000ms_total),
            (3, 2, 1)
        );
    }

    #[test]
    fn source_queue_and_native_delivery_are_separate_timelines() {
        let mut f = FrameStability::default();
        f.source(MS(10), MS(2));
        f.source(MS(43), MS(1500));
        f.swap(MS(12));
        f.swap(MS(1543));
        f.present(MS(12), MS(13), true);
        f.present(MS(1543), MS(1560), true);
        let v = f.finish_window();
        assert_eq!(
            (v.source_samples_window, v.source_gap_max_window_ms, v.swap_queue_wait_max_window_ms),
            (2, 33, 1500)
        );
        assert_eq!(v.swap_gap_max_window_ms, 1531);
        assert_eq!((v.present_calls_window, v.present_call_max_window_ms, v.present_gap_max_window_ms), (2, 17, 1547));
    }

    #[test]
    fn predecessors_and_totals_cross_report_windows_but_window_gauges_reset() {
        let mut f = FrameStability::default();
        f.source(MS(100), MS(5));
        f.swap(MS(100));
        f.present(MS(100), MS(102), true);
        f.finish_window();
        f.source(MS(2100), MS(7));
        f.swap(MS(2110));
        f.present(MS(2110), MS(2113), true);
        let v = f.finish_window();
        assert_eq!((v.source_samples_window, v.source_gap_max_window_ms), (1, 2000));
        assert_eq!((v.swap_gap_max_window_ms, v.swap_gap_max_total_ms), (2010, 2010));
        assert_eq!((v.present_calls_window, v.present_gap_max_window_ms), (1, 2011));
        let empty = f.finish_window();
        assert_eq!(
            empty,
            FrameStabilitySnapshot {
                swap_intervals_total: 1,
                swap_gap_max_total_ms: 2010,
                swap_gaps_over_100ms_total: 1,
                swap_gaps_over_250ms_total: 1,
                swap_gaps_over_1000ms_total: 1,
                source_gap_max_total_ms: 2000,
                swap_queue_wait_max_total_ms: 7,
                present_call_max_total_ms: 3,
                present_gap_max_total_ms: 2011,
                ..Default::default()
            }
        );
    }

    #[test]
    fn failed_present_counts_cost_without_claiming_a_successful_frame() {
        let mut f = FrameStability::default();
        f.present(MS(1), MS(3), true);
        f.present(MS(5), MS(1005), false);
        let failed = f.finish_window();
        assert_eq!(
            (
                failed.present_calls_window,
                failed.present_call_max_window_ms,
                failed.present_failures_total,
                failed.present_gap_max_window_ms
            ),
            (2, 1000, 1, 0)
        );
        f.present(MS(1100), MS(1102), true);
        let recovered = f.finish_window();
        assert_eq!((recovered.present_gap_max_window_ms, recovered.present_failures_total), (1099, 1));
    }

    #[test]
    fn regressed_timestamps_do_not_replace_predecessors_or_inflate_gaps() {
        let mut f = FrameStability::default();
        f.swap(MS(100));
        f.swap(MS(1));
        f.source(MS(100), MS(2));
        f.source(MS(1), Duration::MAX);
        f.present(MS(100), MS(102), true);
        f.present(MS(105), MS(104), false);
        f.present(MS(1), MS(99), false);
        f.swap(MS(133));
        f.source(MS(133), MS(4));
        f.present(MS(133), MS(135), true);
        let v = f.finish_window();
        assert_eq!((v.swap_intervals_total, v.swap_gap_max_window_ms), (1, 33));
        assert_eq!((v.source_samples_window, v.source_gap_max_window_ms, v.swap_queue_wait_max_window_ms), (2, 33, 4));
        assert_eq!((v.present_calls_window, v.present_gap_max_window_ms, v.present_failures_total), (2, 33, 0));
    }

    #[test]
    fn numeric_counters_and_duration_conversion_saturate() {
        let mut f = FrameStability::default();
        f.values.swap_intervals_total = u64::MAX;
        f.values.swap_gaps_over_100ms_total = u64::MAX;
        f.values.present_failures_total = u64::MAX;
        f.swap(Duration::ZERO);
        f.swap(Duration::MAX);
        f.source(Duration::ZERO, Duration::MAX);
        f.present(Duration::ZERO, Duration::MAX, false);
        let v = f.finish_window();
        assert_eq!(
            (v.swap_intervals_total, v.swap_gaps_over_100ms_total, v.swap_gap_max_total_ms),
            (u64::MAX, u64::MAX, u64::MAX)
        );
        assert_eq!(
            (v.swap_queue_wait_max_window_ms, v.present_call_max_window_ms, v.present_failures_total),
            (u64::MAX, u64::MAX, u64::MAX)
        );
        assert!(std::mem::size_of::<FrameStability>() <= 256);
    }
}
