//! One linear heap construction instead of rescanning the cache per victim.
use std::cmp::Ordering;
use std::collections::BinaryHeap;

// Small evictions keep the old allocation-free search. Bound the total direct
// scan work before using the heap for a pressure burst, not the victim count.
pub(super) fn direct_scan_fits(scanned: usize, remaining: usize) -> bool {
    scanned == 0 || scanned.saturating_add(remaining) <= 8192
}

struct Candidate<K> {
    key: K,
    used: u64,
    ordinal: usize,
}

// Reverse age/order: BinaryHeap pops the oldest entry and preserves the input
// order for ties. Keys need no ordering, and never participate in comparisons.
impl<K> PartialEq for Candidate<K> {
    fn eq(&self, other: &Self) -> bool {
        (self.used, self.ordinal) == (other.used, other.ordinal)
    }
}
impl<K> Eq for Candidate<K> {}
impl<K> PartialOrd for Candidate<K> {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}
impl<K> Ord for Candidate<K> {
    fn cmp(&self, other: &Self) -> Ordering {
        other.used.cmp(&self.used).then_with(|| other.ordinal.cmp(&self.ordinal))
    }
}

pub(super) struct OldestFirst<K>(BinaryHeap<Candidate<K>>);

impl<K> Iterator for OldestFirst<K> {
    type Item = K;

    fn next(&mut self) -> Option<K> {
        self.0.pop().map(|entry| entry.key)
    }
}

pub(super) fn oldest_first<K>(entries: impl Iterator<Item = (K, u64)>) -> OldestFirst<K> {
    let heap: BinaryHeap<_> =
        entries.enumerate().map(|(ordinal, (key, used))| Candidate { key, used, ordinal }).collect();
    OldestFirst(heap)
}

#[cfg(test)]
mod tests {
    use super::*;
    use rustc_hash::FxHashMap;

    fn reference(mut entries: FxHashMap<u32, u64>) -> Vec<u32> {
        let mut result = Vec::new();
        while let Some(key) = entries.iter().min_by_key(|(_, used)| **used).map(|(key, _)| *key) {
            result.push(key);
            entries.remove(&key);
        }
        result
    }

    #[test]
    fn repeated_minimum_reference_including_ties_and_extreme_ages() {
        for count in [0, 1, 2, 7, 31, 128, 513] {
            for seed in 0..64u64 {
                let mut random = seed + 1;
                let entries: FxHashMap<u32, u64> = (0..count)
                    .map(|key| {
                        random = random.wrapping_mul(6364136223846793005).wrapping_add(1);
                        let age = match seed % 4 {
                            0 => random % 7,
                            1 => random,
                            2 => u64::MAX - random % 7,
                            _ => 0,
                        };
                        (key, age)
                    })
                    .collect();
                let actual: Vec<_> = oldest_first(entries.iter().map(|(key, used)| (*key, *used))).collect();
                assert_eq!(actual, reference(entries), "count={count}, seed={seed}");
            }
        }
    }

    #[test]
    fn omitted_protected_entries_and_partial_consumption_keep_lru_order() {
        let entries: FxHashMap<u32, u64> = (0..100).map(|key| (key, u64::from(key / 3))).collect();
        let mut old = entries.clone();
        let mut expected = Vec::new();
        while let Some(key) =
            old.iter().filter(|(key, _)| **key % 7 != 0).min_by_key(|(_, used)| **used).map(|(key, _)| *key)
        {
            expected.push(key);
            old.remove(&key);
        }
        let actual: Vec<_> =
            oldest_first(entries.iter().filter(|(key, _)| **key % 7 != 0).map(|(key, used)| (*key, *used)))
                .take(19)
                .collect();
        assert_eq!(actual, expected[..19]);
    }

    #[test]
    #[ignore = "explicit quiet-host victim-selection benchmark, not a GPU/FPS benchmark"]
    fn benchmark_pressure_victim_selection() {
        use std::hint::black_box;
        use std::time::Instant;
        assert_eq!(std::env::var("PUBLIC_EVICTION_BENCH").as_deref(), Ok("1"));
        for (count, victims) in [(64, 16), (512, 128), (4096, 1), (4096, 2), (4096, 1024), (16384, 4096)] {
            let template: FxHashMap<u32, u64> = (0..count).map(|key| (key, u64::from(key))).collect();
            let expected = reference(template.clone());
            for pair in 0..7 {
                let mut elapsed = [0u128; 2];
                for binned in [pair % 2 == 0, pair % 2 != 0] {
                    let mut entries = template.clone();
                    let start = Instant::now();
                    let mut selected = Vec::with_capacity(victims);
                    // Include the same bounded direct-search work as production.
                    if binned && victims > 1 {
                        let mut scanned = 0;
                        while selected.len() < victims && direct_scan_fits(scanned, entries.len()) {
                            scanned += entries.len();
                            let key = *entries.iter().min_by_key(|(_, used)| **used).unwrap().0;
                            entries.remove(&key);
                            selected.push(key);
                        }
                        if selected.len() < victims {
                            let direct = selected.len();
                            for key in
                                oldest_first(entries.iter().map(|(key, used)| (*key, *used))).take(victims - direct)
                            {
                                selected.push(key);
                            }
                            for key in &selected[direct..] {
                                entries.remove(key);
                            }
                        }
                    } else {
                        while selected.len() < victims {
                            let key = *entries.iter().min_by_key(|(_, used)| **used).unwrap().0;
                            entries.remove(&key);
                            selected.push(key);
                        }
                    }
                    elapsed[usize::from(binned)] = start.elapsed().as_nanos();
                    assert_eq!(selected, expected[..victims]);
                    black_box(entries);
                }
                println!(
                    "EVICTION_SELECTION count={count} victims={victims} pair={pair} old_ns={} candidate_ns={}",
                    elapsed[0], elapsed[1]
                );
            }
        }
    }
}
