//! Transactional guest mirror. GPU-newer pages remain pinned until an explicit
//! version-checked CPU writeback. A CPU/GPU conflict never makes a whole stale
//! CPU page authoritative. Reused slots are ordered after all prior consumers.
use super::*;
use crate::bindings::{Entry, Group, GroupLayout, Resource};
use std::{
    collections::{BTreeMap, BTreeSet},
    sync::Arc,
};
use xenos_shader::bindings::{
    SHARED_MEMORY_BLOCK_BYTES as BLOCK, SHARED_MEMORY_BLOCKS as BLOCKS,
    SHARED_MEMORY_CHUNK_BYTES as CHUNK, SHARED_MEMORY_CHUNKS as CHUNKS,
};
pub const GUEST_PAGE_BYTES: usize = 4096;
const PAGES_PER_BLOCK: u32 = BLOCK / GUEST_PAGE_BYTES as u32;
const SLOTS_PER_CHUNK: u32 = CHUNK / BLOCK;
const _: () = assert!(
    CHUNKS == 4
        && BLOCK.is_power_of_two()
        && CHUNK.is_power_of_two()
        && BLOCK % GUEST_PAGE_BYTES as u32 == 0
        && CHUNK % BLOCK == 0
);
mod writeback;
pub use writeback::WritebackStats;

#[derive(Clone)]
pub struct GuestPage {
    /// A page-aligned physical offset, without virtual alias bits.
    pub address: u32,
    /// Captured bytes, retained across any preparation/submission retry. The
    /// game adapter must capture coherently, not identify one version and then
    /// read different bytes. No live mutable guest pointer is stored here.
    pub bytes: Arc<[u8; GUEST_PAGE_BYTES]>,
    /// Coherently captured, monotonically increasing per-page CPU write version.
    /// Every write (including equal-byte/ABA writes) must advance it. None is
    /// supported only while the page has no GPU-newer result; it cannot be a
    /// GPU store destination or resynchronize a GPU-newer page.
    pub generation: Option<u64>,
}
#[derive(Clone)]
struct PageState {
    cpu: GuestPage,
    gpu_newer: bool,
    // Identity of the mirrored CONTENT, distinct from the CPU write generation.
    // A resolve changes it even before submission; abandonment discards it.
    revision: u64,
}
#[derive(Clone)]
struct BlockState {
    slot: u32,
    used: u64,
    pages: [Option<PageState>; PAGES_PER_BLOCK as usize],
}
impl BlockState {
    fn gpu_newer(&self) -> bool {
        self.pages.iter().flatten().any(|p| p.gpu_newer)
    }
}
#[derive(Clone)]
struct State {
    blocks: BTreeMap<u32, Arc<BlockState>>,
    owners: Vec<Option<u32>>,
    chunks: [Buffer<Platform>; CHUNKS as usize],
    created: u32,
    group: Group<Platform>,
    clock: u64,
}
pub struct GuestMemory {
    owner: Rc<Owner<Platform>>,
    layout: GroupLayout<Platform>,
    table: Buffer<Platform>,
    state: State,
    last_submission: Serial,
}
impl GuestMemory {
    /// Authored GPU oracle only: return bytes, never a mutable buffer handle.
    #[cfg(feature = "fault-injection")]
    pub fn test_read_page(
        &self,
        gpu: &mut Gpu<Platform>,
        address: u32,
        timeout: std::time::Duration,
    ) -> Result<Vec<u8>> {
        self.owner.healthy()?;
        if !Rc::ptr_eq(&self.owner, &gpu.owner) || address % GUEST_PAGE_BYTES as u32 != 0 {
            return Err(Error::Rejected("guest oracle page"));
        }
        let record = self
            .state
            .blocks
            .get(&(address / BLOCK))
            .ok_or(Error::Rejected("guest oracle unmapped block"))?;
        gpu.wait(self.last_submission, timeout)?;
        let offset = u64::from(record.slot % SLOTS_PER_CHUNK * BLOCK + address % BLOCK);
        let readback = gpu.create_buffer(
            "guest page oracle readback",
            GUEST_PAGE_BYTES as u64,
            U::COPY_DST | U::MAP_READ,
        )?;
        let mut encoder = gpu.encoder()?;
        encoder.copy_buffer(
            &self.state.chunks[(record.slot / SLOTS_PER_CHUNK) as usize],
            offset,
            &readback,
            0,
            GUEST_PAGE_BYTES as u64,
        )?;
        let serial = gpu.submit(vec![encoder.finish()?])?;
        gpu.wait(serial, timeout)?;
        gpu.read_buffer(&readback, 0..GUEST_PAGE_BYTES as u64, timeout)
    }
    pub fn layout(&self) -> &GroupLayout<Platform> {
        &self.layout
    }
    pub fn mapped_blocks(&self) -> usize {
        self.state.blocks.len()
    }
    pub fn last_submission(&self) -> Serial {
        self.last_submission
    }
    /// These pages cannot be evicted or overwritten by CPU snapshots. The
    /// adapter must write them back before CPU reads/writes require visibility.
    pub fn gpu_newer_pages(&self) -> Vec<u32> {
        self.state
            .blocks
            .values()
            .flat_map(|b| b.pages.iter().flatten())
            .filter(|p| p.gpu_newer)
            .map(|p| p.cpu.address)
            .collect()
    }
    pub(super) fn begin(&mut self, owner: &Rc<Owner<Platform>>) -> Result<MemoryFrame<'_>> {
        if !Rc::ptr_eq(&self.owner, owner) {
            return Err(Error::Rejected("guest memory device"));
        }
        // One shallow metadata copy per transaction, not per draw or page.
        // The captured page bytes and untouched block records stay shared.
        let state = self.state.clone();
        Ok(MemoryFrame {
            original: self,
            state,
            capture: None,
        })
    }
}
fn group(
    gpu: &Gpu<Platform>,
    layout: &GroupLayout<Platform>,
    chunks: &[Buffer<Platform>; 4],
    table: &Buffer<Platform>,
) -> Result<Group<Platform>> {
    let resources = [&chunks[0], &chunks[1], &chunks[2], &chunks[3], table];
    gpu.bind_group(
        layout,
        &resources
            .map(|b| Entry {
                binding: 0,
                resource: Resource::Buffer {
                    buffer: b,
                    offset: 0,
                    size: b.size(),
                },
            })
            .into_iter()
            .enumerate()
            .map(|(i, mut e)| {
                e.binding = i as u32;
                e
            })
            .collect::<Vec<_>>(),
    )
}
impl Gpu<Platform> {
    pub fn guest_memory(&mut self, max_chunks: u32) -> Result<GuestMemory> {
        if !(1..=CHUNKS).contains(&max_chunks) {
            return Err(Error::Rejected("guest mirror chunk budget"));
        }
        if u64::from(CHUNK) > self.owner.limits.max_storage_buffer_binding_size {
            return Err(Error::Rejected(
                "guest mirror chunk exceeds storage binding limit",
            ));
        }
        let both = types::ShaderStages::VERTEX | types::ShaderStages::FRAGMENT;
        let entries = [0, 1, 2, 3, 4].map(|binding| types::BindGroupLayoutEntry {
            binding,
            visibility: both,
            ty: types::BindingType::Buffer {
                ty: types::BufferBindingType::Storage { read_only: true },
                has_dynamic_offset: false,
                min_binding_size: NonZeroU64::new(4),
            },
            count: None,
        });
        let layout = self.group_layout(&entries)?;
        let mut chunks = Vec::new();
        for _ in 0..CHUNKS {
            chunks.push(self.create_buffer(
                "guest mirror empty chunk",
                4,
                U::STORAGE_READ_ONLY | U::STORAGE_READ_WRITE | U::COPY_DST,
            )?);
        }
        let chunks: [Buffer<Platform>; 4] = chunks
            .try_into()
            .map_err(|_| Error::Rejected("guest chunk count"))?;
        let table = self.create_buffer(
            "guest mirror page table",
            u64::from(BLOCKS) * 4,
            U::STORAGE_READ_ONLY | U::COPY_DST,
        )?;
        let group = group(self, &layout, &chunks, &table)?;
        Ok(GuestMemory {
            owner: self.owner.clone(),
            layout,
            table,
            state: State {
                blocks: BTreeMap::new(),
                owners: vec![None; (max_chunks * SLOTS_PER_CHUNK) as usize],
                chunks,
                created: 0,
                group,
                clock: 0,
            },
            last_submission: 0,
        })
    }
}
pub(super) struct MemoryFrame<'a> {
    original: &'a mut GuestMemory,
    state: State,
    capture: Option<BTreeSet<u32>>,
}
pub(super) enum Bytes {
    Page(Arc<[u8; GUEST_PAGE_BYTES]>),
    Table([u8; 4]),
}
impl Bytes {
    pub(super) fn as_slice(&self) -> &[u8] {
        match self {
            Self::Page(p) => p.as_slice(),
            Self::Table(v) => v,
        }
    }
}
pub(super) struct Write {
    pub buffer: Buffer<Platform>,
    pub offset: u64,
    pub bytes: Bytes,
}
pub(super) struct PlannedSync {
    blocks: BTreeMap<u32, Option<Arc<BlockState>>>,
    owners: BTreeMap<u32, u32>,
    chunks: [Buffer<Platform>; 4],
    created: u32,
    group: Group<Platform>,
    clock: u64,
    capture: BTreeSet<u32>,
    pub clears: Vec<(Buffer<Platform>, Range<u64>)>,
    pub writes: Vec<Write>,
    pub bytes: u64,
    pub changed_pages: u64,
    pub evictions: u64,
}
impl MemoryFrame<'_> {
    pub(super) fn cpu_page(&self, address: u32) -> Result<Arc<[u8; GUEST_PAGE_BYTES]>> {
        if !self.captured(address) {
            return Err(Error::Rejected("guest CPU texture page uncaptured"));
        }
        let page = self
            .state
            .blocks
            .get(&(address / BLOCK))
            .and_then(|b| b.pages[(address % BLOCK / GUEST_PAGE_BYTES as u32) as usize].as_ref())
            .ok_or(Error::Rejected("guest CPU texture page absent"))?;
        if page.gpu_newer {
            return Err(Error::Rejected(
                "guest CPU texture fallback would read stale bytes",
            ));
        }
        Ok(page.cpu.bytes.clone())
    }
    pub(super) fn validate_versions(&self, versions: &[(u32, u64)]) -> Result<()> {
        for &(address, revision) in versions {
            let page = self.state.blocks.get(&(address / BLOCK)).and_then(|b| {
                b.pages[(address % BLOCK / GUEST_PAGE_BYTES as u32) as usize].as_ref()
            });
            if !self.captured(address) || page.is_none_or(|p| p.revision != revision) {
                return Err(Error::Rejected(
                    "guest texture generation is stale or uncaptured",
                ));
            }
        }
        Ok(())
    }
    pub(super) fn has_capture(&self) -> bool {
        self.capture.is_some()
    }
    pub(super) fn captured(&self, address: u32) -> bool {
        self.capture.as_ref().is_some_and(|c| c.contains(&address))
    }
    pub(super) fn consume_capture(&mut self) {
        self.capture = None;
    }
    pub(super) fn group(&self) -> Group<Platform> {
        self.state.group.clone()
    }
    pub(super) fn prepare(
        &self,
        gpu: &mut Gpu<Platform>,
        pages: &[GuestPage],
    ) -> Result<PlannedSync> {
        if pages.len() > 4096 {
            return Err(Error::Rejected("guest page snapshot count"));
        }
        let mut requested = BTreeSet::new();
        let mut unique = BTreeMap::new();
        for p in pages {
            if p.address % GUEST_PAGE_BYTES as u32 != 0 || u64::from(p.address) >= 512 << 20 {
                return Err(Error::Rejected("guest physical page address"));
            }
            requested.insert(p.address / BLOCK);
            if let Some(previous) = unique.insert(p.address, p) {
                if previous.bytes != p.bytes || previous.generation != p.generation {
                    return Err(Error::Rejected("conflicting guest page snapshots"));
                }
            }
        }
        if requested.len() > self.state.owners.len() {
            return Err(Error::Rejected("single draw exceeds guest mirror capacity"));
        }
        let clock = self
            .state
            .clock
            .checked_add(1)
            .ok_or(Error::Rejected("guest mirror clock exhaustion"))?;
        let mut plan = PlannedSync {
            blocks: BTreeMap::new(),
            owners: BTreeMap::new(),
            chunks: self.state.chunks.clone(),
            created: self.state.created,
            group: self.state.group.clone(),
            clock,
            capture: unique.keys().copied().collect(),
            clears: Vec::new(),
            writes: Vec::new(),
            bytes: 0,
            changed_pages: 0,
            evictions: 0,
        };
        for &block in &requested {
            let current = self.state.blocks.get(&block);
            let mut record = if let Some(current) = current {
                (**current).clone()
            } else {
                let slot = (0..self.state.owners.len() as u32)
                    .find(|slot| {
                        !plan.owners.contains_key(slot)
                            && self.state.owners[*slot as usize].is_none()
                    })
                    .or_else(|| {
                        (0..self.state.owners.len() as u32)
                            .filter_map(|slot| {
                                if plan.owners.contains_key(&slot) {
                                    return None;
                                }
                                let victim = self.state.owners[slot as usize]?;
                                if requested.contains(&victim)
                                    || self.state.blocks[&victim].gpu_newer()
                                {
                                    return None;
                                }
                                Some((self.state.blocks[&victim].used, slot))
                            })
                            .min()
                            .map(|(_, slot)| slot)
                    })
                    .ok_or(Error::Rejected("guest mirror slots unavailable"))?;
                if let Some(victim) = self.state.owners[slot as usize] {
                    plan.blocks.insert(victim, None);
                    plan.writes.push(Write {
                        buffer: self.original.table.clone(),
                        offset: u64::from(victim) * 4,
                        bytes: Bytes::Table(0u32.to_le_bytes()),
                    });
                    plan.evictions += 1;
                }
                let chunk = slot / SLOTS_PER_CHUNK;
                while plan.created <= chunk {
                    let index = plan.created as usize;
                    // Keep allocation AND initialization inside this plan.
                    // create_buffer would retain its prefix clear even when
                    // staging admission rejects the entire page preparation.
                    // The raw handle cannot escape: the full clear is encoded
                    // before publishing the group or copying any page below.
                    plan.chunks[index] = gpu.raw_buffer(
                        "guest mirror resident chunk",
                        u64::from(CHUNK),
                        U::STORAGE_READ_ONLY | U::STORAGE_READ_WRITE | U::COPY_DST | U::COPY_SRC,
                        false,
                    )?;
                    plan.clears
                        .push((plan.chunks[index].clone(), 0..u64::from(CHUNK)));
                    plan.created += 1;
                }
                let offset = u64::from(slot % SLOTS_PER_CHUNK) * u64::from(BLOCK);
                // A reused slot must not expose another guest block's uncopied
                // pages. Never-used slots are already covered by the full
                // chunk clear, so they require no redundant per-slot clear.
                if self.state.owners[slot as usize].is_some() {
                    plan.clears.push((
                        plan.chunks[chunk as usize].clone(),
                        offset..offset + u64::from(BLOCK),
                    ));
                }
                plan.owners.insert(slot, block);
                plan.writes.push(Write {
                    buffer: self.original.table.clone(),
                    offset: u64::from(block) * 4,
                    bytes: Bytes::Table((slot + 1).to_le_bytes()),
                });
                BlockState {
                    slot,
                    used: clock,
                    pages: std::array::from_fn(|_| None),
                }
            };
            record.used = clock;
            // Each requested block is cloned once, never every resident page.
            for (&address, page) in unique.range(block * BLOCK..(block + 1) * BLOCK) {
                let lane = (address % BLOCK / GUEST_PAGE_BYTES as u32) as usize;
                let previous = record.pages[lane].as_ref();
                let same = previous.is_some_and(|previous| {
                    Arc::ptr_eq(&previous.cpu.bytes, &page.bytes)
                        || previous.cpu.bytes == page.bytes
                });
                if let Some(previous) = previous {
                    if let (Some(old), Some(new)) = (previous.cpu.generation, page.generation) {
                        if new < old || (new == old && !same) {
                            return Err(Error::Rejected("invalid guest capture generation"));
                        }
                    }
                    if previous.gpu_newer {
                        if !same || page.generation != previous.cpu.generation {
                            return Err(Error::Rejected("guest CPU/GPU coherence conflict"));
                        }
                        // Identical CPU baseline: the GPU result remains current.
                        continue;
                    }
                    if same && previous.cpu.generation == page.generation {
                        continue;
                    }
                }
                record.pages[lane] = Some(PageState {
                    cpu: (*page).clone(),
                    gpu_newer: false,
                    revision: if same {
                        previous.unwrap().revision
                    } else {
                        id()?
                    },
                });
                if same {
                    continue;
                }
                let chunk = record.slot / SLOTS_PER_CHUNK;
                let offset = u64::from(record.slot % SLOTS_PER_CHUNK) * u64::from(BLOCK)
                    + u64::from(address % BLOCK);
                plan.writes.push(Write {
                    buffer: plan.chunks[chunk as usize].clone(),
                    offset,
                    bytes: Bytes::Page(page.bytes.clone()),
                });
                plan.changed_pages += 1;
            }
            plan.blocks.insert(block, Some(Arc::new(record)));
        }
        if plan.created != self.state.created {
            plan.group = group(
                gpu,
                &self.original.layout,
                &plan.chunks,
                &self.original.table,
            )?;
        }
        plan.bytes = plan
            .writes
            .iter()
            .map(|w| w.bytes.as_slice().len() as u64)
            .sum();
        Ok(plan)
    }
    pub(super) fn apply(&mut self, plan: PlannedSync) {
        for (block, state) in plan.blocks {
            match state {
                Some(state) => {
                    self.state.blocks.insert(block, state);
                }
                None => {
                    self.state.blocks.remove(&block);
                }
            }
        }
        for (slot, block) in plan.owners {
            self.state.owners[slot as usize] = Some(block);
        }
        self.state.chunks = plan.chunks;
        self.state.created = plan.created;
        self.state.group = plan.group;
        self.state.clock = plan.clock;
        self.capture = Some(plan.capture);
    }
    pub(super) fn commit(self, serial: Serial) {
        self.original.state = self.state;
        self.original.last_submission = serial;
    }
    pub(super) fn validate_store(&self, range: &Range<u32>) -> Result<()> {
        if range.is_empty() || range.end > 512 << 20 {
            return Err(Error::Rejected("guest store extent"));
        }
        let first = range.start / GUEST_PAGE_BYTES as u32;
        let last = (range.end - 1) / GUEST_PAGE_BYTES as u32;
        if last - first >= 4096 {
            return Err(Error::Rejected("guest store snapshot count"));
        }
        for n in first..=last {
            let address = n * GUEST_PAGE_BYTES as u32;
            let page = self.state.blocks.get(&(address / BLOCK)).and_then(|b| {
                b.pages[(address % BLOCK / GUEST_PAGE_BYTES as u32) as usize].as_ref()
            });
            if !self.captured(address)
                || page.is_none_or(|p| {
                    p.cpu.generation.is_none() || p.cpu.generation == Some(u64::MAX)
                })
            {
                return Err(Error::Rejected(
                    "guest store requires fresh tracked destination capture",
                ));
            }
        }
        Ok(())
    }
    pub(super) fn store_buffers(&self) -> (&Buffer<Platform>, &[Buffer<Platform>; 4]) {
        (&self.original.table, &self.state.chunks)
    }
    pub(super) fn mark_gpu_written(&mut self, range: &Range<u32>) -> Result<()> {
        // Called only after successful encoding and validate_store. Metadata is
        // speculative until the enclosing frame's submission is accepted.
        let revision = id()?;
        for address in (range.start / GUEST_PAGE_BYTES as u32
            ..=(range.end - 1) / GUEST_PAGE_BYTES as u32)
            .map(|p| p * GUEST_PAGE_BYTES as u32)
        {
            let block = Arc::make_mut(self.state.blocks.get_mut(&(address / BLOCK)).unwrap());
            let page = block.pages[(address % BLOCK / GUEST_PAGE_BYTES as u32) as usize]
                .as_mut()
                .unwrap();
            page.gpu_newer = true;
            page.revision = revision;
        }
        self.consume_capture();
        Ok(())
    }

    /// Snapshot the exact private GPU sources and their content revisions. All
    /// pages must belong to the currently armed capture, including GPU-newer
    /// pages whose CPU baseline was validated by prepare(). No raw mirror
    /// buffer or mutable descriptor is exposed to callers outside this owner.
    pub(super) fn capture_range(&self, range: Range<u32>) -> Result<CapturedRange> {
        if range.is_empty() || range.end > 512 << 20 || range.start % 4 != 0 || range.end % 4 != 0 {
            return Err(Error::Rejected("guest texture physical extent"));
        }
        let mut result = CapturedRange {
            versions: Vec::new(),
            copies: Vec::new(),
        };
        let mut address = range.start;
        while address < range.end {
            let page = address / GUEST_PAGE_BYTES as u32 * GUEST_PAGE_BYTES as u32;
            if !self.captured(page) {
                return Err(Error::Rejected("guest texture reads outside its capture"));
            }
            let block = self
                .state
                .blocks
                .get(&(page / BLOCK))
                .ok_or(Error::Rejected("guest texture unmapped block"))?;
            let record = block.pages[(page % BLOCK / GUEST_PAGE_BYTES as u32) as usize]
                .as_ref()
                .ok_or(Error::Rejected("guest texture unmapped page"))?;
            result.versions.push((page, record.revision));
            let bytes = (range.end - address)
                .min(GUEST_PAGE_BYTES as u32 - address % GUEST_PAGE_BYTES as u32);
            let offset = u64::from(block.slot % SLOTS_PER_CHUNK * BLOCK + address % BLOCK);
            let buffer = self.state.chunks[(block.slot / SLOTS_PER_CHUNK) as usize].clone();
            if let Some(last) = result.copies.last_mut()
                && last.0.id() == buffer.id()
                && last.1 + last.2 == offset
            {
                last.2 += u64::from(bytes);
            } else {
                result.copies.push((buffer, offset, u64::from(bytes)));
            }
            address += bytes;
        }
        Ok(result)
    }
}

pub(super) struct CapturedRange {
    pub versions: Vec<(u32, u64)>,
    pub copies: Vec<(Buffer<Platform>, u64, u64)>,
}
