//! Exact old-algorithm reference, observable access order and allocation guard.
use super::*;
use std::alloc::{GlobalAlloc, Layout, System};
use std::cell::{Cell, RefCell};
use xenos_texture::RangeKind;

thread_local! {
    static COUNTING: Cell<bool> = const { Cell::new(false) };
    static ALLOCATIONS: Cell<usize> = const { Cell::new(0) };
}
struct CountAlloc;
fn note_allocation() {
    let _ = COUNTING.try_with(|active| {
        if active.get() {
            let _ = ALLOCATIONS.try_with(|count| count.set(count.get() + 1));
        }
    });
}
unsafe impl GlobalAlloc for CountAlloc {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        note_allocation();
        unsafe { System.alloc(layout) }
    }
    unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
        note_allocation();
        unsafe { System.alloc_zeroed(layout) }
    }
    unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, size: usize) -> *mut u8 {
        note_allocation();
        unsafe { System.realloc(ptr, layout, size) }
    }
    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        unsafe { System.dealloc(ptr, layout) }
    }
}
#[global_allocator]
static ALLOCATOR: CountAlloc = CountAlloc;

fn counted<T>(f: impl FnOnce() -> T) -> (T, usize) {
    struct Reset(bool);
    impl Drop for Reset {
        fn drop(&mut self) {
            COUNTING.with(|c| c.set(self.0));
        }
    }
    ALLOCATIONS.with(|c| c.set(0));
    let reset = Reset(COUNTING.with(|c| c.replace(true)));
    let value = f();
    drop(reset);
    (value, ALLOCATIONS.with(Cell::get))
}

#[derive(Debug, PartialEq, Eq)]
enum Access {
    Version(u32, u32),
    Read(u32, usize),
}
struct Memory {
    versions: Vec<Option<u64>>,
    position: Cell<usize>,
    accesses: RefCell<Vec<Access>>,
}
impl Memory {
    fn new(versions: Vec<Option<u64>>) -> Self {
        Self { versions, position: Cell::new(0), accesses: RefCell::new(Vec::with_capacity(256)) }
    }
}
impl GuestMemory for Memory {
    fn read(&self, address: u32, out: &mut [u8]) {
        self.accesses.borrow_mut().push(Access::Read(address, out.len()));
        for (i, b) in out.iter_mut().enumerate() {
            *b = address.wrapping_add(i as u32).wrapping_mul(29) as u8;
        }
    }
    fn version(&self, address: u32, len: u32) -> Option<u64> {
        self.accesses.borrow_mut().push(Access::Version(address, len));
        let index = self.position.replace(self.position.get() + 1);
        self.versions.get(index).copied().flatten()
    }
}
fn ranges(n: usize) -> Vec<GuestRange> {
    (0..n)
        .map(|i| GuestRange {
            kind: if i == 0 { RangeKind::Base } else { RangeKind::Mip(i as u32) },
            address: 0x1000 + (i as u32) * 0x1000,
            length: [0, 1, 9, 1024][i % 4],
        })
        .collect()
}

// Verbatim previous algorithm, including fallback and hashed-byte accounting.
fn previous(memory: &dyn GuestMemory, ranges: &[GuestRange], scratch: &mut Vec<u8>, hashed: &mut u64) -> u64 {
    let versions: Option<Vec<u64>> = ranges.iter().map(|r| memory.version(r.address, r.length)).collect();
    if let Some(versions) = versions {
        return versions
            .iter()
            .zip(ranges)
            .fold(0u64, |h, (v, r)| hash::hash(h ^ u64::from(r.address), &v.to_le_bytes()));
    }
    let mut h = 0u64;
    for r in ranges {
        scratch.clear();
        scratch.resize(r.length as usize, 0);
        memory.read(r.address, scratch);
        h = hash::hash(h ^ u64::from(r.address), scratch);
        *hashed += u64::from(r.length);
    }
    h
}
fn compare(ranges: &[GuestRange], versions: Vec<Option<u64>>) {
    let before = Memory::new(versions.clone());
    let after = Memory::new(versions);
    let (mut a, mut b) = (vec![11, 22, 33], vec![11, 22, 33]);
    let (mut ah, mut bh) = (19, 19);
    assert_eq!(previous(&before, ranges, &mut a, &mut ah), hash_ranges(&after, ranges, &mut b, &mut bh));
    assert_eq!(a, b);
    assert_eq!(ah, bh);
    assert_eq!(before.accesses, after.accesses);
}

#[test]
fn tracked_hashes_match_previous_for_empty_many_and_changing_versions() {
    for n in [0, 1, 2, 8, 16, 49] {
        let ranges = ranges(n);
        for salt in [0, u64::MAX, 0x1234_5678_9876_5432] {
            compare(&ranges, (0..n).map(|i| Some(salt.wrapping_add(i as u64 * 17))).collect());
        }
    }
    let ranges = ranges(2);
    let memory = Memory::new(vec![Some(11), Some(12), Some(11), Some(13)]);
    let (mut scratch, mut hashed) = (vec![], 0);
    let before = hash_ranges(&memory, &ranges, &mut scratch, &mut hashed);
    let after = hash_ranges(&memory, &ranges, &mut scratch, &mut hashed);
    assert_ne!(before, after, "changed versions still invalidate content identity");
    assert_eq!(hashed, 0);
}

#[test]
fn first_none_preserves_version_prefix_then_reads_all_ranges_in_order() {
    let ranges = ranges(16);
    for missing in 0..ranges.len() {
        let mut versions: Vec<_> = (0..ranges.len()).map(|i| Some(i as u64 + 77)).collect();
        versions[missing] = None;
        compare(&ranges, versions.clone());
        let memory = Memory::new(versions);
        let (mut scratch, mut hashed) = (vec![], 0);
        hash_ranges(&memory, &ranges, &mut scratch, &mut hashed);
        assert_eq!(memory.position.get(), missing + 1);
        let accesses = memory.accesses.borrow();
        assert_eq!(
            &accesses[missing + 1..],
            &ranges.iter().map(|r| Access::Read(r.address, r.length as usize)).collect::<Vec<_>>()
        );
        assert_eq!(hashed, ranges.iter().map(|r| u64::from(r.length)).sum::<u64>());
    }
}

#[test]
fn tracked_path_has_zero_allocations_with_the_same_accesses_and_hash() {
    for n in [1, 8, 49] {
        let ranges = ranges(n);
        let versions = vec![Some(17); n];
        let before = Memory::new(versions.clone());
        let after = Memory::new(versions);
        let (mut a, mut b) = (vec![], vec![]);
        let (mut ah, mut bh) = (0, 0);
        let (old, old_allocations) = counted(|| previous(&before, &ranges, &mut a, &mut ah));
        let (new, new_allocations) = counted(|| hash_ranges(&after, &ranges, &mut b, &mut bh));
        assert_eq!(old, new);
        assert_eq!(before.accesses, after.accesses);
        assert_eq!((ah, bh), (0, 0));
        assert!(old_allocations > 0, "allocation guard must detect the old Vec");
        assert_eq!(new_allocations, 0, "tracked validations must not allocate");
    }
}
