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
