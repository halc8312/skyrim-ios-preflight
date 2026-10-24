//! Opt-in numeric evidence only. No reset, mapping change, memory content, or guest execution.
use std::sync::Mutex;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};

pub const CAPACITY: usize = 64;
pub const COUNTERS: usize = 18;

#[repr(usize)]
#[derive(Clone, Copy)]
pub enum Counter {
    XexOriginQuery,
    XexOriginUncommitted,
    RendererReadPage,
    RendererWritePage,
    SelectedPhysical,
    SelectedXex,
    SelectedUnmapped,
    OverlapObservation,
    SelectionSnapshotMismatch,
    ProbeBusy,
    ProbePoisoned,
    RecordLockMiss,
    UniquePageOverflow,
    SelectionChange,
    UninterestingPageOmitted,
    RecordEviction,
    CpWriteTranslationSelectedXex,
    RendererWriteTranslationSelectedXex,
}

#[derive(Clone, Copy)]
pub enum Access {
    Read,
    Write,
}

/// Selection values: 0 unmapped, 1 physical, 2 XEX. Pages are numeric physical 4 KiB indices.
#[derive(Clone, Copy, Debug, Default)]
pub struct Record {
    pub physical_page: u32,
    pub first_selection: u8,
    pub last_selection: u8,
    pub observations: u64,
    pub changes: u64,
    pub overlap_seen: bool,
}

struct Records {
    entries: [Record; CAPACITY],
    used: usize,
}

struct State {
    enabled: AtomicBool,
    saturated: AtomicBool,
    counters: [AtomicU64; COUNTERS],
    records: Mutex<Records>,
}

impl State {
    const fn new() -> Self {
        Self {
            enabled: AtomicBool::new(false),
            saturated: AtomicBool::new(false),
            counters: [const { AtomicU64::new(0) }; COUNTERS],
            records: Mutex::new(Records {
                entries: [Record {
                    physical_page: 0,
                    first_selection: 0,
                    last_selection: 0,
                    observations: 0,
                    changes: 0,
                    overlap_seen: false,
                }; CAPACITY],
                used: 0,
            }),
        }
    }

    fn count(&self, counter: Counter) {
        let cell = &self.counters[counter as usize];
        let old = cell.try_update(Ordering::Relaxed, Ordering::Relaxed, |v| Some(v.saturating_add(1))).unwrap();
        if old == u64::MAX {
            self.saturated.store(true, Ordering::Relaxed);
        }
    }

    fn selection(&self, page: u32, selected: u8, overlap: bool, xex_relevant: bool) {
        let Ok(mut records) = self.records.try_lock() else {
            self.count(Counter::RecordLockMiss);
            return;
        };
        let index = match records.entries[..records.used].iter().position(|r| r.physical_page == page) {
            Some(i) => i,
            None if !xex_relevant => {
                self.count(Counter::UninterestingPageOmitted);
                return;
            }
            None if records.used < CAPACITY => {
                let i = records.used;
                records.entries[i] = Record {
                    physical_page: page,
                    first_selection: selected,
                    last_selection: selected,
                    ..Record::default()
                };
                records.used += 1;
                i
            }
            None => {
                // Preserve observed overlap / selection-change history over XEX-only history.
                // An eviction loses that old page's history and is explicitly counted.
                match overlap.then(|| records.entries.iter().position(|r| !r.overlap_seen && r.changes == 0)).flatten()
                {
                    Some(i) => {
                        self.count(Counter::RecordEviction);
                        records.entries[i] = Record {
                            physical_page: page,
                            first_selection: selected,
                            last_selection: selected,
                            ..Record::default()
                        };
                        i
                    }
                    None => {
                        self.count(Counter::UniquePageOverflow);
                        return;
                    }
                }
            }
        };
        let record = &mut records.entries[index];
        if record.last_selection != selected {
            self.count(Counter::SelectionChange);
            record.changes = record.changes.saturating_add(1);
        }
        if record.observations == u64::MAX || record.changes == u64::MAX {
            self.saturated.store(true, Ordering::Relaxed);
        }
        record.last_selection = selected;
        record.overlap_seen |= overlap;
        record.observations = record.observations.saturating_add(1);
    }
}

static STATE: State = State::new();

/// Enable once before guest/renderer start. There is deliberately no environment default or reset.
pub fn enable() -> bool {
    STATE.enabled.compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire).is_ok()
}

pub fn enabled() -> bool {
    STATE.enabled.load(Ordering::Relaxed)
}

pub(crate) fn count(counter: Counter) {
    if enabled() {
        STATE.count(counter);
    }
}

pub(crate) fn selection(page: u32, selected: u8, overlap: bool, xex_relevant: bool) {
    STATE.selection(page, selected, overlap, xex_relevant);
}

#[derive(Debug)]
pub struct Snapshot {
    pub enabled: bool,
    /// Independent cumulative atomic loads, NOT a coherent all-counters transaction.
    pub counters: [u64; COUNTERS],
    pub records: Vec<Record>,
    pub records_available: bool,
    pub counter_saturated: bool,
    /// Record order is observation-lock order, not a total order of concurrent memory accesses.
    pub concurrent_access_order_proven: bool,
}

pub fn snapshot() -> Snapshot {
    let entries = STATE.records.try_lock().ok().map(|r| r.entries[..r.used].to_vec());
    Snapshot {
        enabled: enabled(),
        counters: std::array::from_fn(|i| STATE.counters[i].load(Ordering::Relaxed)),
        records_available: entries.is_some(),
        records: entries.unwrap_or_default(),
        counter_saturated: STATE.saturated.load(Ordering::Relaxed),
        concurrent_access_order_proven: false,
    }
}

/// Scan status: 0 disabled, 1 complete/coherent, 2 busy (no scan), 3 poisoned (no scan).
#[derive(Clone, Copy, Debug, Default)]
pub struct Range {
    pub first_page: u32,
    pub pages: u32,
}

#[derive(Debug)]
pub struct OverlapScan {
    pub status: u8,
    pub scanned_pages: u32,
    pub overlap_pages: u32,
    pub range_count: u32,
    pub ranges: [Range; 16],
    pub stored_ranges: usize,
    pub truncated: bool,
}

impl Default for OverlapScan {
    fn default() -> Self {
        Self {
            status: 0,
            scanned_pages: 0,
            overlap_pages: 0,
            range_count: 0,
            ranges: [Range::default(); 16],
            stored_ranges: 0,
            truncated: false,
        }
    }
}

impl OverlapScan {
    pub(crate) fn add_range(&mut self, first_page: u32, pages: u32) {
        self.range_count += 1;
        if self.stored_ranges < self.ranges.len() {
            self.ranges[self.stored_ranges] = Range { first_page, pages };
            self.stored_ranges += 1;
        } else {
            self.truncated = true;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bounded_records_keep_changes_and_report_overflow() {
        let state = State::new();
        state.selection(3, 2, false, true);
        state.selection(3, 1, true, true);
        state.selection(3, 2, false, true);
        for page in 4..4 + CAPACITY as u32 {
            state.selection(page, 0, false, true);
        }
        let records = state.records.lock().unwrap();
        assert_eq!(records.used, CAPACITY);
        assert_eq!(records.entries[0].changes, 2);
        assert!(records.entries[0].overlap_seen);
        assert_eq!(state.counters[Counter::SelectionChange as usize].load(Ordering::Relaxed), 2);
        assert_eq!(state.counters[Counter::UniquePageOverflow as usize].load(Ordering::Relaxed), 1);
    }

    #[test]
    fn saturation_and_busy_recording_are_explicit() {
        let state = State::new();
        state.counters[Counter::ProbeBusy as usize].store(u64::MAX, Ordering::Relaxed);
        state.count(Counter::ProbeBusy);
        assert_eq!(state.counters[Counter::ProbeBusy as usize].load(Ordering::Relaxed), u64::MAX);
        assert!(state.saturated.load(Ordering::Relaxed));
        let _guard = state.records.lock().unwrap();
        state.selection(0, 0, false, true);
        assert_eq!(state.counters[Counter::RecordLockMiss as usize].load(Ordering::Relaxed), 1);
    }

    #[test]
    fn overlap_ranges_report_truncation_without_losing_total() {
        let mut scan = OverlapScan::default();
        for i in 0..17 {
            scan.add_range(i * 2, 1);
        }
        assert_eq!(scan.range_count, 17);
        assert_eq!(scan.stored_ranges, 16);
        assert_eq!(scan.ranges[15].first_page, 30);
        assert!(scan.truncated);
    }

    #[test]
    fn ordinary_pages_cannot_displace_overlap_or_change_evidence() {
        let state = State::new();
        for page in 0..CAPACITY as u32 {
            state.selection(page, 2, false, true);
        }
        state.selection(0, 1, true, true);
        state.selection(100, 1, false, false);
        assert_eq!(state.counters[Counter::UninterestingPageOmitted as usize].load(Ordering::Relaxed), 1);
        state.selection(101, 1, true, true);
        assert_eq!(state.counters[Counter::RecordEviction as usize].load(Ordering::Relaxed), 1);
        let records = state.records.lock().unwrap();
        assert!(records.entries.iter().any(|r| r.physical_page == 0 && r.changes == 1));
        assert!(records.entries.iter().any(|r| r.physical_page == 101 && r.overlap_seen));
        assert!(!records.entries.iter().any(|r| r.physical_page == 100));
    }
}

#[cfg(test)]
mod write_counter_tests {
    use super::*;
    #[test]
    fn original_counter_indices_and_locked_record_independence() {
        let original = [
            Counter::XexOriginQuery,
            Counter::XexOriginUncommitted,
            Counter::RendererReadPage,
            Counter::RendererWritePage,
            Counter::SelectedPhysical,
            Counter::SelectedXex,
            Counter::SelectedUnmapped,
            Counter::OverlapObservation,
            Counter::SelectionSnapshotMismatch,
            Counter::ProbeBusy,
            Counter::ProbePoisoned,
            Counter::RecordLockMiss,
            Counter::UniquePageOverflow,
            Counter::SelectionChange,
            Counter::UninterestingPageOmitted,
            Counter::RecordEviction,
        ];
        for (i, counter) in original.into_iter().enumerate() {
            assert_eq!(counter as usize, i);
        }
        assert_eq!(Counter::CpWriteTranslationSelectedXex as usize, 16);
        assert_eq!(Counter::RendererWriteTranslationSelectedXex as usize, 17);
        let state = State::new();
        let _records = state.records.lock().unwrap();
        state.count(Counter::CpWriteTranslationSelectedXex);
        state.selection(7, 2, false, true);
        assert_eq!(state.counters[16].load(Ordering::Relaxed), 1);
        assert_eq!(state.counters[Counter::RecordLockMiss as usize].load(Ordering::Relaxed), 1);
        state.counters[17].store(u64::MAX, Ordering::Relaxed);
        state.count(Counter::RendererWriteTranslationSelectedXex);
        assert_eq!(state.counters[17].load(Ordering::Relaxed), u64::MAX);
        assert!(state.saturated.load(Ordering::Relaxed));
    }
}
