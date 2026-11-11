//! Final presentation policy, separate from guest rendering and simulation.
//!
//! A cap skips only a final surface blit/present. Every guest draw, resolve,
//! memory effect, queue completion and diagnostic dump still runs. Output
//! scaling changes drawable pixels, never the guest's render-target resolution.
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::Duration;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum PresentFilter {
    OriginalAuto = 0,
    Bilinear = 1,
    Nearest = 2,
    Bicubic = 3,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PresentationSettings {
    frame_limit: u32,
    output_scale_percent: u32,
    filter: PresentFilter,
    sharpness: u32,
}
impl Default for PresentationSettings {
    fn default() -> Self {
        Self { frame_limit: 0, output_scale_percent: 100, filter: PresentFilter::OriginalAuto, sharpness: 0 }
    }
}
impl PresentationSettings {
    pub fn from_words(words: [u32; 4]) -> Result<Self, &'static str> {
        let [frame_limit, output_scale_percent, filter, sharpness] = words;
        if ![0, 15, 20, 24, 30, 40, 60, 90, 120].contains(&frame_limit) {
            return Err("presentation frame limit is unsupported");
        }
        if !(25..=200).contains(&output_scale_percent) || sharpness > 100 {
            return Err("presentation scale or sharpness is out of range");
        }
        let filter = match filter {
            0 => PresentFilter::OriginalAuto,
            1 => PresentFilter::Bilinear,
            2 => PresentFilter::Nearest,
            3 => PresentFilter::Bicubic,
            _ => return Err("presentation filter is unsupported"),
        };
        Ok(Self { frame_limit, output_scale_percent, filter, sharpness })
    }
    pub fn words(self) -> [u32; 4] {
        [self.frame_limit, self.output_scale_percent, self.filter as u32, self.sharpness]
    }
    pub fn frame_limit(self) -> u32 {
        self.frame_limit
    }
    pub fn filter(self) -> PresentFilter {
        self.filter
    }
    pub fn sharpness(self) -> u32 {
        self.sharpness
    }
    pub(crate) fn shader_words(self) -> [u32; 4] {
        [self.filter as u32, self.sharpness, 0, 0]
    }
    fn packed(self) -> u32 {
        self.frame_limit | (self.output_scale_percent << 8) | ((self.filter as u32) << 16) | (self.sharpness << 18)
    }
    fn unpacked(word: u32) -> Self {
        Self::from_words([word & 255, (word >> 8) & 255, (word >> 16) & 3, (word >> 18) & 127])
            .expect("only validated presentation settings are stored")
    }
    /// Fit within the device limit while retaining aspect; zero means minimized.
    /// The default is byte-for-byte the supplied surface size when within limits.
    pub fn output_size(self, width: u32, height: u32, max_dimension: u32) -> (u32, u32) {
        if width == 0 || height == 0 || max_dimension == 0 {
            return (0, 0);
        }
        let w = (u64::from(width) * u64::from(self.output_scale_percent) / 100).max(1);
        let h = (u64::from(height) * u64::from(self.output_scale_percent) / 100).max(1);
        let largest = w.max(h);
        if largest > u64::from(max_dimension) {
            return (
                ((u128::from(w) * u128::from(max_dimension) / u128::from(largest)).max(1)) as u32,
                ((u128::from(h) * u128::from(max_dimension) / u128::from(largest)).max(1)) as u32,
            );
        }
        (w as u32, h as u32)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SettingsSnapshot {
    pub generation: u32,
    pub settings: PresentationSettings,
}
impl SettingsSnapshot {
    pub(crate) fn packed(self) -> u64 {
        (u64::from(self.generation) << 32) | u64::from(self.settings.packed())
    }
    pub(crate) fn unpacked(value: u64) -> Self {
        Self { generation: (value >> 32) as u32, settings: PresentationSettings::unpacked(value as u32) }
    }
}
impl Default for SettingsSnapshot {
    fn default() -> Self {
        Self { generation: 0, settings: PresentationSettings::default() }
    }
}

/// Full-tuple publication; readers cannot combine settings from different writes.
#[derive(Debug)]
pub(crate) struct SettingsControl(AtomicU64);
impl Default for SettingsControl {
    fn default() -> Self {
        Self(AtomicU64::new(SettingsSnapshot::default().packed()))
    }
}
impl SettingsControl {
    pub fn snapshot(&self) -> SettingsSnapshot {
        SettingsSnapshot::unpacked(self.0.load(Ordering::Acquire))
    }
    pub fn applied(&self, snapshot: SettingsSnapshot) {
        self.0.store(snapshot.packed(), Ordering::Release);
    }
    pub fn set(&self, settings: PresentationSettings) -> u32 {
        let mut old = self.0.load(Ordering::Acquire);
        loop {
            let previous = SettingsSnapshot::unpacked(old);
            if previous.settings == settings {
                return previous.generation;
            }
            let generation = previous.generation.wrapping_add(1).max(1);
            let new = SettingsSnapshot { generation, settings }.packed();
            match self.0.compare_exchange_weak(old, new, Ordering::AcqRel, Ordering::Acquire) {
                Ok(_) => return generation,
                Err(value) => old = value,
            }
        }
    }
}

/// Nonblocking average-cadence admission at guest swaps, not display sync or a
/// minimum interval between presents. Adjacent source frames may both qualify
/// for a noninteger rate ratio. Missed slots after a stall are discarded: there
/// is no backlog, sleeping, or guest-command dropping.
#[derive(Default)]
pub(crate) struct PresentAdmission {
    limit: u32,
    last_observed: Option<Duration>,
    // Exact next deadline in nanoseconds * limit. Divide only for comparison,
    // so a floor(n * 1s / source_rate) clock does not accumulate rounding drift.
    next: Option<u128>,
}
impl PresentAdmission {
    pub fn admit(&mut self, now: Duration, frame_limit: u32) -> bool {
        if self.limit != frame_limit || self.last_observed.is_some_and(|last| now < last) {
            self.limit = frame_limit;
            self.next = None;
        }
        self.last_observed = Some(now);
        if frame_limit == 0 {
            self.next = None;
            return true;
        }
        const PERIOD: u128 = 1_000_000_000;
        let rate = u128::from(frame_limit);
        let nanos = now.as_nanos();
        if self.next.is_some_and(|next| nanos < next / rate) {
            return false;
        }
        let next = self.next.map_or(nanos * rate + PERIOD, |next| next + PERIOD);
        // One overdue deadline can result from ordinary source-frame
        // quantization. If another slot is already due, drop the missed slots
        // and start a full period after this admission instead of catching up.
        self.next = Some(if next / rate <= nanos { nanos * rate + PERIOD } else { next });
        true
    }
}

/// How long the frame on screen should stay there before the next one may replace it, learned
/// from the title's own cadence.
///
/// The title paces itself on its 60 Hz vertical blank (Skyrim swaps every second one), but each
/// frame reaches the display a few milliseconds earlier or later than the last. Shown at the next
/// refresh, a 30 Hz title on a 120 Hz panel then alternates 25 ms and 41.7 ms frames instead of
/// holding 33.3 ms, which reads as judder although no frame was late. Giving the display a
/// minimum duration just under the title's interval turns each early frame back into an on-time
/// one. It never delays a frame that arrives on time or late.
///
/// The interval is the lower quartile of recent swap intervals, snapped down to whole vertical
/// blanks: a scene alternating between two rates is paced at the faster one, so pacing cannot
/// lower the frame rate. Irregular cadences (loading, menus that free-run) are not paced.
#[derive(Debug)]
pub(crate) struct PresentPacer {
    /// The title's vertical blank period: its swap intervals are multiples of it.
    source_period: Duration,
    intervals: [Duration; Self::WINDOW],
    filled: usize,
    cursor: usize,
    last: Option<Duration>,
    /// Earliest time the frame presented last can have reached the screen.
    shown: Duration,
    /// Swaps still to present unpaced after the title outran the pacing.
    unpaced: usize,
}
impl PresentPacer {
    const WINDOW: usize = 12;
    /// Fewer samples do not establish a cadence.
    const MINIMUM_SAMPLES: usize = 8;
    /// A gap this long is a stall or a loading screen, not a cadence sample.
    const STALL: Duration = Duration::from_millis(250);
    /// Kept clear of the refresh the frame is meant for (half a 120 Hz refresh is 4.2 ms).
    const MARGIN: Duration = Duration::from_millis(4);

    pub fn new(source_period: Duration) -> Self {
        Self {
            source_period,
            intervals: [Duration::ZERO; Self::WINDOW],
            filled: 0,
            cursor: 0,
            last: None,
            shown: Duration::ZERO,
            unpaced: 0,
        }
    }

    fn forget(&mut self) {
        self.filled = 0;
        self.cursor = 0;
    }

    /// Whole vertical blanks per swap at the title's faster recent rate, if it has one.
    fn cadence(&self) -> Option<u32> {
        if self.filled < Self::MINIMUM_SAMPLES || self.source_period.is_zero() {
            return None;
        }
        let mut sorted = self.intervals;
        let sorted = &mut sorted[..self.filled];
        sorted.sort_unstable();
        let (low, high) = (sorted[sorted.len() / 4], sorted[sorted.len() * 3 / 4]);
        // Two adjacent rates (two and three blanks) still pace; anything wider is not a cadence.
        if high.as_secs_f64() > low.as_secs_f64() * 1.75 {
            return None;
        }
        let blanks = (low.as_secs_f64() / self.source_period.as_secs_f64() + 0.3).floor();
        // Up to four blanks (15 Hz at the console's 60): slower titles gain nothing from it.
        (1.0..=4.0).contains(&blanks).then_some(blanks as u32)
    }

    /// Call once per presented swap with a monotonic time. Returns the minimum time the frame
    /// before this one should stay on screen, or `None` to present at the next refresh.
    pub fn minimum(&mut self, now: Duration) -> Option<Duration> {
        match self.last.replace(now) {
            Some(last) if now >= last && now - last < Self::STALL => {
                self.intervals[self.cursor] = now - last;
                self.cursor = (self.cursor + 1) % Self::WINDOW;
                self.filled = (self.filled + 1).min(Self::WINDOW);
            }
            // The first swap, a stall, or a clock that went backwards: start over.
            _ => {
                self.forget();
                self.unpaced = 0;
                self.shown = now;
                return None;
            }
        }
        let mut minimum = if self.unpaced > 0 {
            self.unpaced -= 1;
            None
        } else {
            self.cadence().map(|blanks| self.source_period * blanks - Self::MARGIN)
        };
        if let Some(duration) = minimum {
            // Frames queued behind the minimum mean the title now runs faster than it did when
            // the cadence was learned (a 30 Hz scene became a 60 Hz menu). Holding on would
            // throttle it to the old rate and keep measuring that rate: let go and relearn.
            let queued = (self.shown + duration).saturating_sub(now);
            if queued > duration {
                self.forget();
                self.unpaced = Self::WINDOW;
                minimum = None;
            }
        }
        self.shown = now.max(self.shown + minimum.unwrap_or(Duration::ZERO));
        minimum
    }
}

#[cfg(test)]
mod pacer_tests {
    use super::*;
    const BLANK: Duration = Duration::from_nanos(16_666_667);
    fn ms(value: f64) -> Duration {
        Duration::from_secs_f64(value / 1000.0)
    }
    /// Feeds swaps at the given intervals and returns the minimum chosen for each.
    fn run(pacer: &mut PresentPacer, start: &mut Duration, intervals: &[f64]) -> Vec<Option<Duration>> {
        intervals
            .iter()
            .map(|&interval| {
                *start += ms(interval);
                pacer.minimum(*start)
            })
            .collect()
    }

    #[test]
    fn a_steady_title_is_held_just_under_its_own_interval_once_established() {
        let mut pacer = PresentPacer::new(BLANK);
        let mut now = Duration::from_secs(1);
        assert_eq!(pacer.minimum(now), None, "the first swap has no predecessor");
        // 30 Hz with the few milliseconds of jitter that cause the judder.
        let jitter = [33.3, 29.0, 38.0, 31.0, 35.5, 33.0, 30.0, 36.5, 33.3, 28.5, 38.0, 33.3, 31.0, 35.0];
        let chosen = run(&mut pacer, &mut now, &jitter);
        assert!(chosen[..7].iter().all(Option::is_none), "no pacing before a cadence is established");
        let expected = BLANK * 2 - PresentPacer::MARGIN;
        assert!(chosen[7..].iter().all(|&d| d == Some(expected)), "{chosen:?}");
        // Below the title's interval (never delays an on-time frame) and above the refresh
        // before it on 60 Hz and 120 Hz panels alike (an early frame waits for its own slot).
        assert!(expected < BLANK * 2 && expected > BLANK * 2 - BLANK / 2);
    }

    #[test]
    fn other_console_rates_snap_to_their_own_blank_count() {
        for (interval, blanks) in [(16.7, 1), (33.3, 2), (50.0, 3), (66.7, 4)] {
            let mut pacer = PresentPacer::new(BLANK);
            let mut now = Duration::from_secs(1);
            pacer.minimum(now);
            let chosen = run(&mut pacer, &mut now, &[interval; 12]);
            assert_eq!(chosen[11], Some(BLANK * blanks - PresentPacer::MARGIN), "{interval} ms");
        }
        // Slower than four blanks, and rates between blank counts that free-run faster than the
        // next slower console rate, are left alone or paced at the faster count: never slowed.
        for (interval, expected) in [(100.0, None), (25.0, Some(BLANK - PresentPacer::MARGIN))] {
            let mut pacer = PresentPacer::new(BLANK);
            let mut now = Duration::from_secs(1);
            pacer.minimum(now);
            assert_eq!(run(&mut pacer, &mut now, &[interval; 12])[11], expected, "{interval} ms");
        }
    }

    #[test]
    fn a_scene_alternating_between_two_rates_is_paced_at_the_faster_one() {
        let mut pacer = PresentPacer::new(BLANK);
        let mut now = Duration::from_secs(1);
        pacer.minimum(now);
        let mixed = [33.3, 50.0, 33.3, 50.0, 50.0, 33.3, 33.3, 50.0, 50.0, 33.3, 50.0, 33.3];
        assert_eq!(run(&mut pacer, &mut now, &mixed)[11], Some(BLANK * 2 - PresentPacer::MARGIN));
    }

    #[test]
    fn irregular_cadences_and_stalls_are_not_paced() {
        let mut pacer = PresentPacer::new(BLANK);
        let mut now = Duration::from_secs(1);
        pacer.minimum(now);
        let loading = [16.0, 120.0, 40.0, 17.0, 90.0, 33.0, 18.0, 150.0, 16.0, 70.0, 20.0, 110.0];
        assert!(run(&mut pacer, &mut now, &loading).iter().all(Option::is_none));
        // An established cadence is forgotten across a stall and learned again from scratch.
        let mut pacer = PresentPacer::new(BLANK);
        pacer.minimum(now);
        assert!(run(&mut pacer, &mut now, &[33.3; 12])[11].is_some());
        assert_eq!(run(&mut pacer, &mut now, &[900.0]), [None]);
        let again = run(&mut pacer, &mut now, &[33.3; 9]);
        assert!(again[..7].iter().all(Option::is_none) && again[8].is_some(), "{again:?}");
        // A clock that goes backwards restarts rather than producing a huge interval.
        assert_eq!(pacer.minimum(now - ms(5.0)), None);
        assert_eq!(PresentPacer::new(Duration::ZERO).cadence(), None);
    }

    #[test]
    fn a_title_that_speeds_up_is_released_instead_of_held_at_the_old_rate() {
        let mut pacer = PresentPacer::new(BLANK);
        let mut now = Duration::from_secs(1);
        pacer.minimum(now);
        assert!(run(&mut pacer, &mut now, &[33.3; 12])[11].is_some());
        // The title switches to 60 Hz. Its frames queue behind the 30 Hz minimum for a few
        // swaps at most; then pacing lets go for a whole window and relearns the new rate.
        let fast = run(&mut pacer, &mut now, &[16.7; 40]);
        let released = fast.iter().position(Option::is_none).expect("pacing must let go");
        assert!(released <= 4, "held the old rate for {released} swaps");
        assert!(fast[released..released + PresentPacer::WINDOW].iter().all(Option::is_none));
        assert_eq!(fast[39], Some(BLANK - PresentPacer::MARGIN), "relearned at the new rate");
        // Having relearned, the faster cadence is never queued behind its own minimum again.
        assert!(fast[released + PresentPacer::WINDOW + 1..].iter().all(Option::is_some));
    }

    #[test]
    fn the_minimum_never_delays_a_frame_that_arrives_on_time() {
        // Display model: a frame is shown at the first refresh at or after both its arrival
        // and the previous frame's presentation plus the minimum.
        for refresh in [1000.0 / 60.0, 1000.0 / 120.0] {
            let mut pacer = PresentPacer::new(BLANK);
            let mut now = Duration::from_secs(1);
            pacer.minimum(now);
            let tick = |t: f64| (t / refresh).ceil() * refresh;
            let mut previous = tick(now.as_secs_f64() * 1000.0);
            // The same frames shown at the next refresh, with no minimum.
            let mut previous_unpaced = previous;
            let (mut unpaced_short, mut paced_short, mut paced_late) = (0, 0, 0);
            for step in 0..600 {
                // Every other frame misses its refresh by 2 ms; the frame after it is on time.
                // Unpaced, that alternates long and short frames on a panel with spare slots.
                let interval = 33.3333 + [2.0, -2.0][step % 2];
                now += ms(interval);
                let arrival = now.as_secs_f64() * 1000.0;
                let minimum = pacer.minimum(now).map_or(0.0, |d| d.as_secs_f64() * 1000.0);
                let unpaced = tick(arrival);
                let shown = tick(arrival.max(previous + minimum - 1e-6));
                if step > 20 {
                    unpaced_short += u32::from(unpaced - previous_unpaced < 30.0);
                    paced_short += u32::from(shown - previous < 30.0);
                    // Never later than the slot one title interval after the previous frame.
                    paced_late += u32::from(shown > unpaced.max(previous + 33.3334) + 1e-6);
                }
                previous = shown;
                previous_unpaced = unpaced;
            }
            assert_eq!((paced_short, paced_late), (0, 0), "{refresh} ms refresh");
            if refresh < 10.0 {
                assert!(unpaced_short > 0, "the model must reproduce the judder being fixed");
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn defaults_and_validation_refuse_unsupported_values() {
        assert_eq!(PresentationSettings::default().words(), [0, 100, 0, 0]);
        for fps in [0, 15, 20, 24, 30, 40, 60, 90, 120] {
            for filter in 0..=3 {
                assert!(PresentationSettings::from_words([fps, 100, filter, 0]).is_ok());
            }
        }
        for words in [
            [1, 100, 0, 0],
            [121, 100, 0, 0],
            [30, 24, 0, 0],
            [30, 201, 0, 0],
            [30, 100, 4, 0],
            [30, 100, 0, 101],
            [u32::MAX; 4],
        ] {
            assert!(PresentationSettings::from_words(words).is_err());
        }
    }
    #[test]
    fn scaling_is_output_only_bounded_and_keeps_default_exact() {
        let default = PresentationSettings::default();
        assert_eq!(default.output_size(1280, 720, 8192), (1280, 720));
        assert_eq!(default.output_size(0, 720, 8192), (0, 0));
        assert_eq!(default.output_size(1280, 720, 0), (0, 0));
        let half = PresentationSettings::from_words([30, 50, 1, 0]).unwrap();
        assert_eq!(half.output_size(1280, 720, 8192), (640, 360));
        let twice = PresentationSettings::from_words([0, 200, 3, 100]).unwrap();
        assert_eq!(twice.output_size(1280, 720, 8192), (2560, 1440));
        assert_eq!(twice.output_size(1280, 720, 2048), (2048, 1152));
        assert_eq!(twice.output_size(u32::MAX, u32::MAX, 8192), (8192, 8192));
        assert_eq!(twice.output_size(u32::MAX, u32::MAX, u32::MAX), (u32::MAX, u32::MAX));
        assert_eq!(half.output_size(1, 1, 8192), (1, 1));
    }
    #[test]
    fn coherent_control_roundtrips_and_does_not_reissue_identical_settings() {
        let control = SettingsControl::default();
        assert_eq!(control.snapshot(), SettingsSnapshot::default());
        let one = PresentationSettings::from_words([24, 75, 2, 30]).unwrap();
        assert_eq!(control.set(one), 1);
        assert_eq!(control.set(one), 1);
        assert_eq!(control.snapshot(), SettingsSnapshot { generation: 1, settings: one });
        assert_eq!(control.set(PresentationSettings::default()), 2);
        assert_eq!(control.snapshot().settings, PresentationSettings::default());
        for fps in [0, 15, 20, 24, 30, 40, 60, 90, 120] {
            for scale in [25, 100, 200] {
                for filter in 0..=3 {
                    for sharp in [0, 100] {
                        let value = PresentationSettings::from_words([fps, scale, filter, sharp]).unwrap();
                        assert_eq!(PresentationSettings::unpacked(value.packed()), value);
                    }
                }
            }
        }
    }
    #[test]
    fn simultaneous_settings_publication_never_exposes_mixed_tuples() {
        let control = std::sync::Arc::new(SettingsControl::default());
        let a = PresentationSettings::from_words([15, 25, 1, 0]).unwrap();
        let b = PresentationSettings::from_words([120, 200, 3, 100]).unwrap();
        std::thread::scope(|scope| {
            for value in [a, b] {
                let control = control.clone();
                scope.spawn(move || {
                    for _ in 0..10000 {
                        control.set(value);
                    }
                });
            }
            for _ in 0..10000 {
                let value = control.snapshot().settings;
                assert!(value == a || value == b || value == PresentationSettings::default());
            }
        });
    }
    #[test]
    fn cap_has_no_bursts_and_changes_apply_without_waiting() {
        let mut clock = PresentAdmission::default();
        assert!(clock.admit(Duration::ZERO, 30));
        assert!(!clock.admit(Duration::from_millis(33), 30));
        assert!(clock.admit(Duration::from_nanos(33_333_334), 30));
        assert!(clock.admit(Duration::from_secs(10), 30));
        assert!(!clock.admit(Duration::from_secs(10), 30));
        assert!(clock.admit(Duration::from_secs(10), 60));
        assert!(clock.admit(Duration::from_secs(10), 0));
        assert!(clock.admit(Duration::from_secs(10), 0));
        for (source_rate, cap, expected) in [(30u64, 30, 30), (60, 30, 30), (60, 60, 60)] {
            let mut clock = PresentAdmission::default();
            let admitted = (0..source_rate)
                .filter(|n| clock.admit(Duration::from_nanos(n * 1_000_000_000 / source_rate), cap))
                .count();
            assert_eq!(admitted, expected, "source {source_rate} cap {cap}");
        }
    }

    #[test]
    fn caps_preserve_average_cadence_for_noninteger_source_ratios() {
        let mut mismatches = Vec::new();
        for (source_rate, cap) in
            [(30u64, 30u32), (60, 30), (60, 60), (30, 24), (60, 24), (60, 40), (120, 90), (15, 30), (30, 0)]
        {
            let mut clock = PresentAdmission::default();
            let admitted = (0..source_rate * 8)
                .filter(|n| clock.admit(Duration::from_nanos(n * 1_000_000_000 / source_rate), cap))
                .count() as u64;
            let expected = source_rate.min(if cap == 0 { source_rate } else { u64::from(cap) }) * 8;
            if admitted != expected {
                mismatches.push((source_rate, cap, admitted, expected));
            }
        }
        assert!(mismatches.is_empty(), "source/cap/admitted/expected: {mismatches:?}");
    }

    #[test]
    fn cap_discards_stalled_slots_and_handles_clock_discontinuities() {
        let mut clock = PresentAdmission::default();
        assert!(clock.admit(Duration::ZERO, 24));
        assert!(!clock.admit(Duration::ZERO, 24));
        assert!(clock.admit(Duration::from_millis(50), 24));
        // Preserve the phase rather than requiring a fresh 41.67 ms interval.
        assert!(clock.admit(Duration::from_nanos(83_333_333), 24));
        let stalled = Duration::from_secs(10);
        assert!(clock.admit(stalled, 24));
        assert!(!clock.admit(stalled, 24));
        assert!(!clock.admit(stalled + Duration::from_nanos(41_666_665), 24));
        assert!(clock.admit(stalled + Duration::from_nanos(41_666_666), 24));
        // Clock regression starts one new schedule, then suppresses duplicates.
        assert!(clock.admit(Duration::from_secs(2), 24));
        assert!(!clock.admit(Duration::from_secs(2), 24));
        assert!(!clock.admit(Duration::from_secs(2) + Duration::from_millis(41), 24));
        assert!(clock.admit(Duration::from_secs(2), 40));
        assert!(!clock.admit(Duration::from_secs(2), 40));
        assert!(clock.admit(Duration::from_secs(2), 0));
        assert!(clock.admit(Duration::from_secs(2), 0));
        // No overflow near Duration's maximum, even when adding a deadline.
        assert!(clock.admit(Duration::MAX, 120));
        assert!(!clock.admit(Duration::MAX, 120));
        assert!(clock.admit(Duration::ZERO, 120));
        assert!(!clock.admit(Duration::ZERO, 120));
    }
}
