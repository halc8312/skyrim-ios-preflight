//! Explicit CPU visibility boundary. No hash-only or whole-page CPU-wins merge.
use super::*;
use std::time::{Duration, Instant};

#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct WritebackStats {
    pub written: u32,
    pub conflicted: u32,
}

struct Unwind(Rc<Owner<Platform>>);
impl Drop for Unwind {
    fn drop(&mut self) {
        if std::thread::panicking() {
            let _ = self.0.fail_next("guest CPU writeback unwound".into());
        }
    }
}

impl GuestMemory {
    /// Read selected GPU-newer pages after verified completion, then commit each
    /// page through the adapter's atomic compare/exchange. The callback MUST:
    /// - hold the guest CPU page lock / equivalent exclusion;
    /// - compare BOTH the captured generation and bytes with `expected`;
    /// - on equality, replace bytes AND generation with `replacement`, returning
    ///   true; otherwise change nothing and return false.
    /// Every CPU store, including equal-byte/ABA writes, must advance generation.
    /// A false result leaves that page GPU-newer and pinned. It is a coherence
    /// conflict requiring adapter ordering/journaling, never permission to discard
    /// either result. Stats count commits/conflicts in THIS call; already-clean
    /// pages are skipped. Visibility is per page, not atomic across the resolve.
    ///
    /// The host must use this boundary before exposing GPU-written memory to CPU
    /// reads/writes. This API does not install CPU memory hooks by itself.
    pub fn writeback(
        &mut self,
        gpu: &mut Gpu<Platform>,
        addresses: &[u32],
        timeout: Duration,
        mut compare_exchange: impl FnMut(&GuestPage, &GuestPage) -> bool,
    ) -> Result<WritebackStats> {
        self.owner.healthy()?;
        if !Rc::ptr_eq(&self.owner, &gpu.owner) {
            return Err(Error::Rejected("guest writeback device"));
        }
        let _unwind = Unwind(self.owner.clone());
        let start = Instant::now();
        if addresses.len() > 4096 {
            return Err(Error::Rejected("guest writeback page count"));
        }
        let mut seen = BTreeSet::new();
        let mut pages = Vec::new();
        for &address in addresses {
            if address % GUEST_PAGE_BYTES as u32 != 0 || address >= 512 << 20 {
                return Err(Error::Rejected("guest writeback page address"));
            }
            if !seen.insert(address) {
                continue;
            }
            let block = self
                .state
                .blocks
                .get(&(address / BLOCK))
                .ok_or(Error::Rejected("guest writeback unmapped block"))?;
            let page = block.pages[(address % BLOCK / GUEST_PAGE_BYTES as u32) as usize]
                .as_ref()
                .ok_or(Error::Rejected("guest writeback uncaptured page"))?;
            if !page.gpu_newer {
                continue;
            }
            let next = page
                .cpu
                .generation
                .and_then(|v| v.checked_add(1))
                .ok_or(Error::Rejected("guest writeback generation exhaustion"))?;
            pages.push((page.cpu.clone(), next, block.slot));
        }
        if pages.is_empty() {
            return Ok(WritebackStats::default());
        }
        let bytes = pages.len() as u64 * GUEST_PAGE_BYTES as u64;
        let readback = gpu.create_buffer(
            "guest visibility readback",
            bytes,
            U::COPY_DST | U::MAP_READ,
        )?;
        let mut encoder = gpu.encoder()?;
        for (i, (page, _, slot)) in pages.iter().enumerate() {
            encoder.copy_buffer(
                &self.state.chunks[(slot / SLOTS_PER_CHUNK) as usize],
                u64::from(slot % SLOTS_PER_CHUNK * BLOCK + page.address % BLOCK),
                &readback,
                i as u64 * GUEST_PAGE_BYTES as u64,
                GUEST_PAGE_BYTES as u64,
            )?;
        }
        // The same queue orders this copy after every accepted guest store. A
        // failed/unknown submit cannot invoke the CPU callback or clear dirtiness.
        let serial = gpu.submit(vec![encoder.finish()?])?;
        let remaining = || {
            timeout
                .checked_sub(start.elapsed())
                .ok_or(Error::Timeout(serial))
        };
        gpu.wait(serial, remaining()?)?;
        let data = gpu.read_buffer(&readback, 0..bytes, remaining()?)?;
        self.owner.healthy()?;
        if data.len() != bytes as usize {
            return Err(Error::Rejected("guest writeback size"));
        }
        let mut stats = WritebackStats::default();
        for (i, (expected, next, _)) in pages.into_iter().enumerate() {
            let replacement = GuestPage {
                address: expected.address,
                bytes: Arc::new(
                    data[i * GUEST_PAGE_BYTES..(i + 1) * GUEST_PAGE_BYTES]
                        .try_into()
                        .unwrap(),
                ),
                generation: Some(next),
            };
            if compare_exchange(&expected, &replacement) {
                let block = Arc::make_mut(
                    self.state
                        .blocks
                        .get_mut(&(expected.address / BLOCK))
                        .unwrap(),
                );
                let lane = (expected.address % BLOCK / GUEST_PAGE_BYTES as u32) as usize;
                let revision = block.pages[lane].as_ref().unwrap().revision;
                block.pages[lane] = Some(PageState {
                    cpu: replacement,
                    gpu_newer: false,
                    revision,
                });
                stats.written += 1;
            } else {
                stats.conflicted += 1;
            }
        }
        Ok(stats)
    }
}
