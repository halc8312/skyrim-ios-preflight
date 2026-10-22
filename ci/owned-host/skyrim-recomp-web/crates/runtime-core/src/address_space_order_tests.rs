//! Deterministic scheduling at the allocator-state/backing-cleanup boundary.
//! A reentrant operation models the second thread only when the heap lock is
//! available; no timing, sleeps, actual concurrent memory access or game data.
use super::*;
use std::cell::RefCell;
use std::rc::Rc;
use std::sync::TryLockError;

type CleanupHook = Box<dyn FnOnce(&AddressSpace)>;
thread_local! {
    static CLEANUP_HOOK: RefCell<Option<CleanupHook>> = const { RefCell::new(None) };
}

pub(super) fn before_cleanup(space: &AddressSpace) {
    let hook = CLEANUP_HOOK.with(|cell| cell.borrow_mut().take());
    if let Some(hook) = hook { hook(space); }
}

#[derive(Clone, Copy, Debug)]
enum Case { VirtualRelease, Virtual4kRelease, VirtualDecommit, PhysicalRelease, StackRelease }
const SENTINEL: u32 = 0xA17E_1E55;
const VIRTUAL: u32 = 0x4014_0000;
const VIRTUAL4K: u32 = 0x9014_0000;

// Only the heaps exercised here need backing pages. Avoid committing the
// unrelated system/MMIO region: the existing translation tests deliberately
// use its untouched EA entries as their scratch-map fixture. Production
// allocation/free methods below still run without replacement or imitation.
fn isolated_space() -> AddressSpace {
    AddressSpace {
        mem: GuestMemory::reserve().unwrap(),
        virtual_heap: Mutex::new(PageHeap::new("test-virtual", map::VIRTUAL_BASE, map::VIRTUAL_END - map::VIRTUAL_BASE)),
        virtual_4k_heap: Mutex::new(PageHeap::new("test-virtual-4k", map::VIRTUAL_4K_BASE, map::VIRTUAL_4K_END - map::VIRTUAL_4K_BASE)),
        stack_heap: Mutex::new(PageHeap::new("test-stack", map::STACK_BASE, map::STACK_END - map::STACK_BASE)),
        xex_heap: Mutex::new(PageHeap::new("test-xex", map::XEX_BASE, map::XEX_END - map::XEX_BASE)),
        physical: Mutex::new(PageHeap::new("test-physical", 0, map::PHYSICAL_SIZE)),
        system: Mutex::new(SystemHeap::new()),
    }
}

fn allocate(space: &AddressSpace, case: Case) -> (u32, u32) {
    match case {
        Case::VirtualRelease | Case::VirtualDecommit => {
            let (base, _) = space.alloc_virtual_fixed(VIRTUAL, PAGE_4K, true, protect::READWRITE).unwrap();
            (base, base)
        }
        Case::Virtual4kRelease => {
            let (base, _) = space.alloc_virtual_fixed(VIRTUAL4K, PAGE_4K, true, protect::READWRITE).unwrap();
            (base, base)
        }
        Case::PhysicalRelease => {
            let base = space.alloc_physical(PAGE_64K, PAGE_64K, 0, u32::MAX, PAGE_64K, protect::READWRITE).unwrap();
            (base, base)
        }
        Case::StackRelease => {
            let stack = space.alloc_stack(PAGE_4K).unwrap();
            (stack.alloc_base, stack.limit)
        }
    }
}

fn heap_locked(space: &AddressSpace, case: Case) -> bool {
    let heap = match case {
        Case::VirtualRelease | Case::VirtualDecommit => &space.virtual_heap,
        Case::Virtual4kRelease => &space.virtual_4k_heap,
        Case::PhysicalRelease => &space.physical,
        Case::StackRelease => &space.stack_heap,
    };
    match heap.try_lock() {
        Ok(_) => false,
        Err(TryLockError::WouldBlock) => true,
        Err(TryLockError::Poisoned(_)) => panic!("synthetic heap was poisoned"),
    }
}

#[derive(Default)]
struct Observation { cleanup_seen: bool, locked: bool, reused: Option<(u32, u32)> }

fn check_order(case: Case) {
    let space = isolated_space();
    let original = allocate(&space, case);
    space.mem.write_u32(original.1, 0x1122_3344);
    let observation = Rc::new(RefCell::new(Observation::default()));
    let in_hook = observation.clone();
    CLEANUP_HOOK.with(|cell| {
        assert!(cell.borrow().is_none());
        *cell.borrow_mut() = Some(Box::new(move |space| {
            let locked = heap_locked(space, case);
            let reused = if locked { None } else {
                // The old implementation makes reuse possible here, before
                // A's pending zero/discard. B completes its own commit/write.
                let allocation = allocate(space, case);
                space.mem.write_u32(allocation.1, SENTINEL);
                Some(allocation)
            };
            *in_hook.borrow_mut() = Observation { cleanup_seen: true, locked, reused };
        }));
    });
    match case {
        Case::VirtualRelease | Case::Virtual4kRelease => { space.release_virtual(original.0).unwrap(); }
        Case::VirtualDecommit => { space.decommit_virtual(original.0, PAGE_4K).unwrap(); }
        Case::PhysicalRelease => { space.free_physical(original.0).unwrap(); }
        Case::StackRelease => { space.free_stack(original.0).unwrap(); }
    }
    assert!(observation.borrow().cleanup_seen);
    let reused = match observation.borrow().reused {
        Some(allocation) => allocation,
        None => {
            // With the fix B is admitted only after A's cleanup has finished.
            let allocation = allocate(&space, case);
            space.mem.write_u32(allocation.1, SENTINEL);
            allocation
        }
    };
    assert_eq!(reused, original, "fixture must reuse exactly the released/reserved range");
    assert_eq!(space.mem.read_u32(reused.1), SENTINEL, "old cleanup erased the new owner's live data: {case:?}");
    assert!(observation.borrow().locked, "cleanup must retain the matching heap lock: {case:?}");
    CLEANUP_HOOK.with(|cell| assert!(cell.borrow().is_none()));
}

#[test]
fn virtual_release_keeps_cleanup_before_reuse() { check_order(Case::VirtualRelease); }
#[test]
fn virtual_4k_release_keeps_cleanup_before_reuse() { check_order(Case::Virtual4kRelease); }
#[test]
fn reserved_decommit_keeps_cleanup_before_recommit() { check_order(Case::VirtualDecommit); }
#[test]
fn physical_release_keeps_cleanup_before_reuse() { check_order(Case::PhysicalRelease); }
#[test]
fn stack_release_keeps_cleanup_before_reuse() { check_order(Case::StackRelease); }
