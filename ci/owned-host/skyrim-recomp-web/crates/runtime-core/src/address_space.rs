//! Guest memory plus the allocators for each region of [`crate::memory::map`].

use std::collections::{BTreeMap, HashMap};
use std::fmt;
use std::sync::Mutex;

use crate::heap::{Allocated, HeapError, PageHeap, PageState, RegionInfo, protect};
use crate::memory::{GuestMemory, MemoryError, PAGE_SIZE, map};

pub const PAGE_4K: u32 = 0x1000;
pub const PAGE_64K: u32 = 0x1_0000;
pub const PAGE_16M: u32 = 0x100_0000;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum AllocError {
    Heap(HeapError),
    Memory(MemoryError),
    /// The address is not in a region this operation applies to.
    WrongRegion(u32),
}

impl fmt::Display for AllocError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            AllocError::Heap(e) => write!(f, "{e}"),
            AllocError::Memory(e) => write!(f, "{e}"),
            AllocError::WrongRegion(a) => write!(f, "address {a:#010X} is not in the expected region"),
        }
    }
}

impl std::error::Error for AllocError {}

impl From<HeapError> for AllocError {
    fn from(e: HeapError) -> Self {
        AllocError::Heap(e)
    }
}

impl From<MemoryError> for AllocError {
    fn from(e: MemoryError) -> Self {
        AllocError::Memory(e)
    }
}

pub type AllocResult<T> = Result<T, AllocError>;

/// Which allocator owns an address.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Region {
    HostReserved,
    Virtual,
    Stack,
    System,
    Mmio,
    Xex,
    /// A physical view; the payload is the view base.
    Physical(u32),
}

pub fn region_of(addr: u32) -> Region {
    match addr {
        a if a < map::HOST_RESERVED_END => Region::HostReserved,
        a if a < map::VIRTUAL_END => Region::Virtual,
        a if a < map::STACK_END => Region::Stack,
        a if a < map::SYSTEM_END => Region::System,
        a if a < map::MMIO_END => Region::Mmio,
        a if a < map::XEX_END => Region::Xex,
        a if a < map::VIRTUAL_4K_END => Region::Virtual,
        a if a < map::PHYS_VIEW_16M => Region::Physical(map::PHYS_VIEW_64K),
        a if a < map::PHYS_VIEW_4K => Region::Physical(map::PHYS_VIEW_16M),
        _ => Region::Physical(map::PHYS_VIEW_4K),
    }
}

/// Offset between a view address and the physical address it shows. The 4 KiB
/// view is shifted by one page on hardware: 0xE0000000 + x is physical
/// x + 0x1000. Titles compute GPU addresses with this rule themselves.
pub fn physical_view_offset(view: u32) -> u32 {
    if view == map::PHYS_VIEW_4K { PAGE_4K } else { 0 }
}

/// Physical view used for a given page size (MmAllocatePhysicalMemoryEx).
pub fn physical_view_for_page_size(page_size: u32) -> u32 {
    if page_size <= PAGE_4K {
        map::PHYS_VIEW_4K
    } else if page_size <= PAGE_64K {
        map::PHYS_VIEW_64K
    } else {
        map::PHYS_VIEW_16M
    }
}

/// First-fit allocator for small kernel-owned objects. The whole system region
/// is committed up front.
struct SystemHeap {
    free: BTreeMap<u32, u32>,
    used: HashMap<u32, u32>,
}

impl SystemHeap {
    const GRANULE: u32 = 16;

    fn new() -> Self {
        let mut free = BTreeMap::new();
        free.insert(map::SYSTEM_BASE, map::SYSTEM_END - map::SYSTEM_BASE);
        SystemHeap { free, used: HashMap::new() }
    }

    fn alloc(&mut self, size: u32, align: u32) -> Option<u32> {
        let size = size.max(1).div_ceil(Self::GRANULE) * Self::GRANULE;
        let align = align.max(Self::GRANULE);
        let (block, block_size, start) = self.free.iter().find_map(|(&b, &s)| {
            let start = b.div_ceil(align) * align;
            (start as u64 + size as u64 <= b as u64 + s as u64).then_some((b, s, start))
        })?;
        self.free.remove(&block);
        if start > block {
            self.free.insert(block, start - block);
        }
        let tail = block + block_size - (start + size);
        if tail > 0 {
            self.free.insert(start + size, tail);
        }
        self.used.insert(start, size);
        Some(start)
    }

    fn free(&mut self, addr: u32) -> Option<u32> {
        let size = self.used.remove(&addr)?;
        let mut start = addr;
        let mut len = size;
        if let Some((&prev, &prev_len)) = self.free.range(..addr).next_back()
            && prev + prev_len == addr
        {
            self.free.remove(&prev);
            start = prev;
            len += prev_len;
        }
        if let Some(&next_len) = self.free.get(&(addr + size)) {
            self.free.remove(&(addr + size));
            len += next_len;
        }
        self.free.insert(start, len);
        Some(size)
    }

    fn size_of(&self, addr: u32) -> Option<u32> {
        self.used.get(&addr).copied()
    }
}

/// Stack allocation with guard pages on both ends (layout matches Xenia).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct StackAllocation {
    pub alloc_base: u32,
    /// Lowest usable address.
    pub limit: u32,
    /// One past the highest usable address (initial r1 is derived from this).
    pub base: u32,
}

pub struct AddressSpace {
    pub mem: GuestMemory,
    virtual_heap: Mutex<PageHeap>,
    virtual_4k_heap: Mutex<PageHeap>,
    stack_heap: Mutex<PageHeap>,
    xex_heap: Mutex<PageHeap>,
    /// Indexed by physical address (0..512 MiB).
    physical: Mutex<PageHeap>,
    system: Mutex<SystemHeap>,
}

impl AddressSpace {
    pub fn new() -> AllocResult<Self> {
        let mem = GuestMemory::reserve()?;
        mem.commit(map::SYSTEM_BASE, map::SYSTEM_END - map::SYSTEM_BASE)?;
        mem.commit(map::MMIO_BASE, map::MMIO_END - map::MMIO_BASE)?;
        Ok(AddressSpace {
            mem,
            virtual_heap: Mutex::new(PageHeap::new("virtual", map::VIRTUAL_BASE, map::VIRTUAL_END - map::VIRTUAL_BASE)),
            virtual_4k_heap: Mutex::new(PageHeap::new(
                "virtual-4k",
                map::VIRTUAL_4K_BASE,
                map::VIRTUAL_4K_END - map::VIRTUAL_4K_BASE,
            )),
            stack_heap: Mutex::new(PageHeap::new("stack", map::STACK_BASE, map::STACK_END - map::STACK_BASE)),
            xex_heap: Mutex::new(PageHeap::new("xex", map::XEX_BASE, map::XEX_END - map::XEX_BASE)),
            physical: Mutex::new(PageHeap::new("physical", 0, map::PHYSICAL_SIZE)),
            system: Mutex::new(SystemHeap::new()),
        })
    }

    fn commit_ranges(&self, offset: u32, ranges: &[(u32, u32)]) -> AllocResult<()> {
        for &(addr, len) in ranges {
            self.mem.commit(offset + addr, len)?;
        }
        Ok(())
    }

    fn decommit_ranges(&self, offset: u32, ranges: &[(u32, u32)]) -> AllocResult<()> {
        #[cfg(test)]
        ordering_tests::before_cleanup(self);
        for &(addr, len) in ranges {
            self.mem.decommit(offset + addr, len)?;
        }
        Ok(())
    }

    /// Frees the committed ranges of a released allocation (they stay
    /// accessible; see [`GuestMemory::release`]).
    fn release_ranges(&self, offset: u32, ranges: &[(u32, u32)]) -> AllocResult<()> {
        #[cfg(test)]
        ordering_tests::before_cleanup(self);
        for &(addr, len) in ranges {
            self.mem.release(offset + addr, len)?;
        }
        Ok(())
    }

    fn finish(&self, offset: u32, a: Allocated) -> AllocResult<u32> {
        self.commit_ranges(offset, &a.newly_committed)?;
        Ok(a.address + offset)
    }

    // --- NtAllocateVirtualMemory heap --------------------------------------------------

    pub fn alloc_virtual(
        &self,
        size: u32,
        page_size: u32,
        commit: bool,
        protect: u32,
        top_down: bool,
    ) -> AllocResult<u32> {
        // 4 KiB-page requests prefer their own range, 64 KiB ones the main
        // heap; either spills into the other when its range is exhausted.
        let (first, second) = if page_size <= PAGE_4K {
            (&self.virtual_4k_heap, &self.virtual_heap)
        } else {
            (&self.virtual_heap, &self.virtual_4k_heap)
        };
        let r = first.lock().unwrap().alloc(size, page_size, commit, protect, top_down);
        let a = match r {
            Err(HeapError::NoSpace) => second.lock().unwrap().alloc(size, page_size, commit, protect, top_down)?,
            r => r?,
        };
        self.finish(0, a)
    }

    /// Fixed-address virtual allocation (or commit inside an existing reservation).
    /// Returns the page-aligned base and whether every page was already committed.
    pub fn alloc_virtual_fixed(&self, addr: u32, size: u32, commit: bool, protect: u32) -> AllocResult<(u32, bool)> {
        if region_of(addr) != Region::Virtual {
            return Err(AllocError::WrongRegion(addr));
        }
        let mut heap = self.virtual_heap_at(addr).lock().unwrap();
        let was_committed =
            (0..size.div_ceil(PAGE_SIZE)).all(|i| heap.state_of(addr + i * PAGE_SIZE) == Some(PageState::Committed));
        let a = heap.alloc_fixed(addr, size, commit, protect)?;
        drop(heap);
        Ok((self.finish(0, a)?, was_committed))
    }

    pub fn decommit_virtual(&self, addr: u32, size: u32) -> AllocResult<()> {
        // Do not expose Reserved pages to a recommit until their old contents
        // have been discarded. Cleanup never takes a PageHeap lock.
        self.with_page_heap(addr, |heap| {
            let ranges = heap.decommit(addr, size)?;
            self.decommit_ranges(0, &ranges)
        })?
    }

    /// Releases the allocation based at `addr`; returns its size.
    pub fn release_virtual(&self, addr: u32) -> AllocResult<u32> {
        // Keep newly free pages unavailable for reuse until cleanup finishes.
        self.with_page_heap(addr, |heap| -> AllocResult<u32> {
            let (size, ranges) = heap.release(addr)?;
            self.release_ranges(0, &ranges)?;
            Ok(size)
        })?
    }

    fn virtual_heap_at(&self, addr: u32) -> &Mutex<PageHeap> {
        if addr >= map::VIRTUAL_4K_BASE { &self.virtual_4k_heap } else { &self.virtual_heap }
    }

    fn with_page_heap<R>(&self, addr: u32, f: impl FnOnce(&mut PageHeap) -> R) -> AllocResult<R> {
        let heap = match region_of(addr) {
            Region::Virtual => self.virtual_heap_at(addr),
            Region::Stack => &self.stack_heap,
            Region::Xex => &self.xex_heap,
            _ => return Err(AllocError::WrongRegion(addr)),
        };
        Ok(f(&mut heap.lock().unwrap()))
    }

    // --- Physical memory ----------------------------------------------------------------

    /// Allocates physical memory (one copy, at [`map::PHYSICAL_BASE`] plus the
    /// physical address) and returns its address in the view for `page_size`.
    /// `min_phys`/`max_phys` bound the physical address and `align` aligns it,
    /// as in Xenia; every view shows the allocation (see
    /// [`crate::memory::fold_view`]).
    pub fn alloc_physical(
        &self,
        size: u32,
        page_size: u32,
        min_phys: u32,
        max_phys: u32,
        align: u32,
        protect: u32,
    ) -> AllocResult<u32> {
        let page_size = if page_size <= PAGE_4K {
            PAGE_4K
        } else if page_size <= PAGE_64K {
            PAGE_64K
        } else {
            PAGE_16M
        };
        let view = physical_view_for_page_size(page_size);
        let size = size.div_ceil(page_size) * page_size;
        let align = align.div_ceil(page_size).max(1) * page_size;
        // Physical addresses the view can show.
        let shift = physical_view_offset(view);
        let view_size = if view == map::PHYS_VIEW_4K { map::PHYS_VIEW_4K_SIZE } else { map::PHYSICAL_SIZE };
        let min = min_phys.max(shift);
        let max = max_phys.min(map::PHYSICAL_SIZE - 1).min(view_size - 1 + shift);
        // Bottom-up (hardware and Xenia go top-down), so linear memory only grows as far as
        // physical memory is used (see memory::linear_map).
        let a = self.physical.lock().unwrap().alloc_range(min, max, size, align, true, protect, false)?;
        self.commit_ranges(map::PHYSICAL_BASE, &a.newly_committed)?;
        Ok(view + a.address - shift)
    }

    pub fn free_physical(&self, addr: u32) -> AllocResult<u32> {
        if !matches!(region_of(addr), Region::Physical(_)) {
            return Err(AllocError::WrongRegion(addr));
        }
        let phys = self.physical_address(addr).ok_or(AllocError::WrongRegion(addr))?;
        let mut heap = self.physical.lock().unwrap();
        let (size, ranges) = heap.release(phys)?;
        self.release_ranges(map::PHYSICAL_BASE, &ranges)?;
        drop(heap); // publish the free range only after its backing cleanup
        Ok(size)
    }

    /// The guest address a physical (GPU) address is visible at: physical
    /// memory if that page is allocated, or else the identity-mapped XEX region
    /// (see [`AddressSpace::physical_address`]). Only the low 29 bits of `phys`
    /// count, as on hardware.
    pub fn translate_physical(&self, phys: u32) -> Option<u32> {
        let p = phys & (map::PHYSICAL_SIZE - 1);
        let canonical = map::PHYSICAL_BASE + p;
        if self.is_committed(canonical) {
            return Some(canonical);
        }
        let xex = map::XEX_BASE + p;
        self.is_committed(xex).then_some(xex)
    }

    /// MmGetPhysicalAddress (see [`physical_view_offset`]). XEX-region
    /// addresses are treated as identity-mapped onto physical memory like on
    /// hardware.
    pub fn physical_address(&self, addr: u32) -> Option<u32> {
        match region_of(addr) {
            Region::Physical(view) => {
                let phys = addr - view + physical_view_offset(view);
                (phys < map::PHYSICAL_SIZE).then_some(phys)
            }
            Region::Xex => Some(addr & (map::PHYSICAL_SIZE - 1)),
            _ => None,
        }
    }

    // --- Stacks -------------------------------------------------------------------------

    pub fn alloc_stack(&self, size: u32) -> AllocResult<StackAllocation> {
        let size = size.div_ceil(PAGE_4K) * PAGE_4K;
        let guard = PAGE_4K;
        let a = self.stack_heap.lock().unwrap().alloc(size + 2 * guard, PAGE_4K, true, protect::READWRITE, false)?;
        let alloc_base = self.finish(0, a)?;
        let mut heap = self.stack_heap.lock().unwrap();
        heap.protect(alloc_base, guard, protect::NOACCESS)?;
        heap.protect(alloc_base + guard + size, guard, protect::NOACCESS)?;
        drop(heap);
        // Make the guards fault on the host too, so an overflow stops at once
        // instead of silently corrupting the neighbouring stack.
        self.mem.decommit(alloc_base, guard)?;
        self.mem.decommit(alloc_base + guard + size, guard)?;
        Ok(StackAllocation { alloc_base, limit: alloc_base + guard, base: alloc_base + guard + size })
    }

    pub fn free_stack(&self, alloc_base: u32) -> AllocResult<()> {
        let mut heap = self.stack_heap.lock().unwrap();
        let (_, ranges) = heap.release(alloc_base)?;
        let result = self.decommit_ranges(0, &ranges);
        drop(heap);
        result
    }

    /// Diagnostic observation only: the HLE MmGetPhysicalAddress entry identifies the origin.
    /// No current allocator/mapping result is changed. Busy/poisoned observations are counted.
    pub fn observe_xex_physical_query(&self, addr: u32) {
        use crate::alias_diagnostics::{self as diag, Counter};
        if !diag::enabled() || region_of(addr) != Region::Xex {
            return;
        }
        diag::count(Counter::XexOriginQuery);
        if let Some((physical, xex)) = self.alias_commit_observation(addr & (map::PHYSICAL_SIZE - 1)) {
            if !xex {
                diag::count(Counter::XexOriginUncommitted);
            }
            if physical && xex {
                diag::count(Counter::OverlapObservation);
            }
        }
    }

    /// Renderer-only observed translation. The baseline result is computed FIRST and retained.
    /// A subsequent coherent pair of allocator states may differ; such disagreement is explicit.
    pub fn translate_physical_for_gpu(&self, phys: u32, access: crate::alias_diagnostics::Access) -> Option<u32> {
        use crate::alias_diagnostics::{self as diag, Access, Counter};
        let selected = self.translate_physical(phys);
        if !diag::enabled() {
            return selected;
        }
        diag::count(match access {
            Access::Read => Counter::RendererReadPage,
            Access::Write => Counter::RendererWritePage,
        });
        let write_xex_counter = matches!(access, Access::Write).then_some(Counter::RendererWriteTranslationSelectedXex);
        self.observe_physical_selection(phys, selected, write_xex_counter);
        selected
    }

    /// CP-side observed translation, for both reads and writes. The result is retained before
    /// the later allocator observation, exactly as for renderer translations. CP observations
    /// contribute to generic selection/history and CP write-intent counters, never renderer counters.
    /// This observes the translated starting page, not every byte/page in a subsequent range or
    /// every later use of a cached translation. No ownership or stable backing lease is implied.
    pub fn translate_physical_for_cp(&self, phys: u32, access: crate::alias_diagnostics::Access) -> Option<u32> {
        let selected = self.translate_physical(phys);
        if crate::alias_diagnostics::enabled() {
            let write_xex_counter = matches!(access, crate::alias_diagnostics::Access::Write)
                .then_some(crate::alias_diagnostics::Counter::CpWriteTranslationSelectedXex);
            self.observe_physical_selection(phys, selected, write_xex_counter);
        }
        selected
    }

    fn observe_physical_selection(
        &self,
        phys: u32,
        selected: Option<u32>,
        write_xex_counter: Option<crate::alias_diagnostics::Counter>,
    ) {
        use crate::alias_diagnostics::{self as diag, Counter};
        let p = phys & (map::PHYSICAL_SIZE - 1);
        let class = match selected {
            Some(a) if a == map::PHYSICAL_BASE + p => 1,
            Some(_) => 2,
            None => 0,
        };
        diag::count(match class {
            1 => Counter::SelectedPhysical,
            2 => Counter::SelectedXex,
            _ => Counter::SelectedUnmapped,
        });
        // Classify the retained original result before optional heap/record probes. This
        // counts a translated write intent, never successful/completed memory writes.
        if class == 2 {
            if let Some(counter) = write_xex_counter {
                diag::count(counter);
            }
        }
        if let Some((physical, xex)) = self.alias_commit_observation(p) {
            let expected = if physical {
                1
            } else if xex {
                2
            } else {
                0
            };
            if class != expected {
                diag::count(Counter::SelectionSnapshotMismatch);
            }
            let overlap = physical && xex;
            if overlap {
                diag::count(Counter::OverlapObservation);
            }
            diag::selection(p / PAGE_4K, class, overlap, xex || class == 2);
        }
    }

    /// Both try-locks must succeed for a coherent point observation. No diagnostics mutex is
    /// held while either heap lock is held, and no blocking lock/order dependency is added.
    fn alias_commit_observation(&self, p: u32) -> Option<(bool, bool)> {
        use crate::alias_diagnostics::{self as diag, Counter};
        use std::sync::TryLockError;
        let miss = |error| {
            diag::count(match error {
                TryLockError::WouldBlock => Counter::ProbeBusy,
                TryLockError::Poisoned(_) => Counter::ProbePoisoned,
            });
        };
        let physical = match self.physical.try_lock() {
            Ok(h) => h,
            Err(e) => {
                miss(e);
                return None;
            }
        };
        let xex = match self.xex_heap.try_lock() {
            Ok(h) => h,
            Err(e) => {
                miss(e);
                return None;
            }
        };
        Some((
            physical.state_of(p) == Some(PageState::Committed),
            xex.state_of(map::XEX_BASE + p) == Some(PageState::Committed),
        ))
    }

    /// Explicit, bounded allocator scan, intended for a startup/final diagnostics worker, not
    /// each draw. Both heap locks cover the entire scan; after return it is no ownership lease.
    pub fn alias_overlap_snapshot(&self) -> crate::alias_diagnostics::OverlapScan {
        use crate::alias_diagnostics::{self as diag, OverlapScan};
        use std::sync::TryLockError;
        let mut out = OverlapScan::default();
        if !diag::enabled() {
            return out;
        }
        let physical = match self.physical.try_lock() {
            Ok(h) => h,
            Err(e) => {
                out.status = if matches!(e, TryLockError::WouldBlock) { 2 } else { 3 };
                return out;
            }
        };
        let xex = match self.xex_heap.try_lock() {
            Ok(h) => h,
            Err(e) => {
                out.status = if matches!(e, TryLockError::WouldBlock) { 2 } else { 3 };
                return out;
            }
        };
        out.status = 1;
        let mut start = None;
        for page in 0..=(map::XEX_END - map::XEX_BASE) / PAGE_4K {
            let overlaps = page < (map::XEX_END - map::XEX_BASE) / PAGE_4K
                && physical.state_of(page * PAGE_4K) == Some(PageState::Committed)
                && xex.state_of(map::XEX_BASE + page * PAGE_4K) == Some(PageState::Committed);
            if page < (map::XEX_END - map::XEX_BASE) / PAGE_4K {
                out.scanned_pages += 1;
            }
            if overlaps {
                out.overlap_pages += 1;
                start.get_or_insert(page);
            } else if let Some(first_page) = start.take() {
                out.add_range(first_page, page - first_page);
            }
        }
        out
    }

    // --- XEX region ---------------------------------------------------------------------

    /// Reserves and commits a fixed range in the XEX region (image, header copy).
    pub fn alloc_xex(&self, addr: u32, size: u32) -> AllocResult<u32> {
        if region_of(addr) != Region::Xex {
            return Err(AllocError::WrongRegion(addr));
        }
        let a = self.xex_heap.lock().unwrap().alloc_fixed(addr, size, true, protect::READWRITE)?;
        self.finish(0, a)
    }

    /// Any free range in the XEX region.
    pub fn alloc_xex_anywhere(&self, size: u32) -> AllocResult<u32> {
        let a = self.xex_heap.lock().unwrap().alloc(size, PAGE_64K, true, protect::READWRITE, true)?;
        self.finish(0, a)
    }

    // --- System heap --------------------------------------------------------------------

    /// Zeroed kernel-owned memory.
    pub fn system_alloc(&self, size: u32, align: u32) -> AllocResult<u32> {
        let addr = self.system.lock().unwrap().alloc(size, align).ok_or(AllocError::Heap(HeapError::NoSpace))?;
        self.mem.fill(addr, size, 0);
        Ok(addr)
    }

    pub fn system_free(&self, addr: u32) -> AllocResult<u32> {
        self.system.lock().unwrap().free(addr).ok_or(AllocError::Heap(HeapError::NotAllocationBase))
    }

    pub fn system_alloc_size(&self, addr: u32) -> Option<u32> {
        self.system.lock().unwrap().size_of(addr)
    }

    // --- Queries ------------------------------------------------------------------------

    pub fn query(&self, addr: u32) -> Option<RegionInfo> {
        match region_of(addr) {
            Region::Virtual | Region::Stack | Region::Xex => {
                self.with_page_heap(addr, |h| h.query(addr)).ok().flatten()
            }
            Region::Physical(view) => {
                let phys = self.physical_address(addr)?;
                let mut info = self.physical.lock().unwrap().query(phys)?;
                // Back from physical addresses to this view's, without letting
                // a free run spill outside the view.
                let shift = physical_view_offset(view);
                let to_view = |p: u32| (view as u64 + p as u64).saturating_sub(shift as u64).max(view as u64);
                let start = to_view(info.base_address);
                let end = to_view(info.base_address + info.region_size).min(view as u64 + map::PHYSICAL_SIZE as u64);
                info.base_address = start as u32;
                info.region_size = (end - start) as u32;
                if info.state != PageState::Free {
                    info.allocation_base = to_view(info.allocation_base) as u32;
                }
                Some(info)
            }
            Region::System | Region::Mmio => Some(RegionInfo {
                base_address: addr & !(PAGE_SIZE - 1),
                allocation_base: if region_of(addr) == Region::System { map::SYSTEM_BASE } else { map::MMIO_BASE },
                allocation_protect: protect::READWRITE,
                region_size: PAGE_SIZE,
                state: PageState::Committed,
                protect: protect::READWRITE,
            }),
            Region::HostReserved => None,
        }
    }

    pub fn protect(&self, addr: u32, size: u32, new: u32) -> AllocResult<u32> {
        match region_of(addr) {
            Region::Physical(_) => {
                let phys = self.physical_address(addr).ok_or(AllocError::WrongRegion(addr))?;
                Ok(self.physical.lock().unwrap().protect(phys, size, new)?)
            }
            _ => self.with_page_heap(addr, |h| h.protect(addr, size, new))?.map_err(Into::into),
        }
    }

    pub fn allocation_size(&self, addr: u32) -> Option<u32> {
        match region_of(addr) {
            Region::Physical(_) => self.physical.lock().unwrap().allocation_size(self.physical_address(addr)?),
            Region::System => self.system_alloc_size(addr),
            _ => self.with_page_heap(addr, |h| h.allocation_size(addr)).ok().flatten(),
        }
    }

    pub fn is_committed(&self, addr: u32) -> bool {
        // The page's state only: `query` measures the whole run of like pages, which can be
        // most of a heap (the command processor asks for every buffer it reads).
        let state = match region_of(addr) {
            Region::Virtual | Region::Stack | Region::Xex => {
                self.with_page_heap(addr, |h| h.state_of(addr)).ok().flatten()
            }
            Region::Physical(_) => self.physical_address(addr).and_then(|p| self.physical.lock().unwrap().state_of(p)),
            Region::System | Region::Mmio => Some(PageState::Committed),
            Region::HostReserved => None,
        };
        state == Some(PageState::Committed)
    }

    /// Committed physical pages (for MmQueryStatistics).
    pub fn physical_pages_used(&self) -> u32 {
        self.physical.lock().unwrap().usage().1
    }

    /// Reserved bytes in the virtual heap.
    pub fn virtual_bytes_reserved(&self) -> u32 {
        (self.virtual_heap.lock().unwrap().usage().0 + self.virtual_4k_heap.lock().unwrap().usage().0) * PAGE_SIZE
    }
}

#[cfg(test)]
#[path = "address_space_order_tests.rs"]
mod ordering_tests;

#[cfg(all(test, target_os = "linux"))]
mod alias_write_heap_tests {
    use super::*;
    use crate::alias_diagnostics::{self as diag, Counter};
    const CHILD: &str = "address_space::alias_write_heap_tests::retained_selection_busy_heap_fixture";
    #[test]
    fn write_counter_survives_later_busy_heap_probe() {
        use std::process::{Command, Stdio};
        use std::time::{Duration, Instant};
        let mut child = Command::new(std::env::current_exe().unwrap())
            .args(["--ignored", "--exact", CHILD, "--nocapture"])
            .env("SKYRT_PUBLIC_ALIAS_WRITE_HEAP_CHILD", "1")
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .spawn()
            .unwrap();
        let deadline = Instant::now() + Duration::from_secs(20);
        loop {
            if child.try_wait().unwrap().is_some() {
                break;
            }
            if Instant::now() >= deadline {
                let _ = child.kill();
                let _ = child.wait();
                panic!("public heap fixture deadline");
            }
            std::thread::sleep(Duration::from_millis(10));
        }
        let output = child.wait_with_output().unwrap();
        assert!(output.status.success(), "public heap fixture failed: {}", String::from_utf8_lossy(&output.stderr));
        assert!(String::from_utf8_lossy(&output.stdout).contains("1 passed"));
    }
    #[test]
    #[ignore = "fresh subprocess only: guest-memory translations and diagnostics are process-global"]
    fn retained_selection_busy_heap_fixture() {
        assert_eq!(std::env::var("SKYRT_PUBLIC_ALIAS_WRITE_HEAP_CHILD").as_deref(), Ok("1"));
        let space = AddressSpace::new().unwrap();
        space.alloc_xex(map::XEX_BASE, PAGE_4K).unwrap();
        let selected = space.translate_physical(0);
        assert_eq!(selected, Some(map::XEX_BASE));
        assert!(diag::enable());
        let guard = space.physical.lock().unwrap();
        // The actual selection was retained before the deliberate later observation failure.
        space.observe_physical_selection(0, selected, Some(Counter::CpWriteTranslationSelectedXex));
        let s = diag::snapshot();
        assert_eq!(s.counters[Counter::CpWriteTranslationSelectedXex as usize], 1);
        assert_eq!(s.counters[Counter::ProbeBusy as usize], 1);
        assert!(s.records.is_empty());
        drop(guard);
        // No write tag: the same XEX selection for a read cannot increment either write counter.
        space.observe_physical_selection(0, selected, None);
        let s = diag::snapshot();
        assert_eq!(s.counters[Counter::CpWriteTranslationSelectedXex as usize], 1);
        assert_eq!(s.counters[Counter::RendererWriteTranslationSelectedXex as usize], 0);
    }
}
