//! The XMA hardware decoder.
//!
//! Titles drive it through XMA_CONTEXT_DATA blocks (64 bytes each, in
//! physical memory): they hand it input packet buffers and an output ring,
//! then kick the context. Decoding (packets, frames, loops, the output ring
//! of big-endian 16-bit PCM) is done by the xma-decoder crate (LGPL: a port
//! of FFmpeg's WMA Pro decoder with Xenia's context logic); this module keeps
//! the context array and per-context decoder state.
//!
//! MMIO stores and XMA imports enqueue on the same per-Kernel worker. Register
//! operands are logical little-endian device words; context data and PCM remain
//! big-endian. No global Kernel is installed or consulted by this device.
//!
//! Enabling is asynchronous. Guest input/output allocations and context contents
//! must remain valid until completion or synchronized disable/clear/release.
//! Those operations acquire the slot lock, wait for in-flight decoding, and
//! invalidate pending work before a slot is reused. Raw guest descriptor accesses
//! remain subject to the guest's device synchronization protocol: import locks do
//! not make unsynchronized direct loads/stores safe, and raw valid-bit edits alone
//! do not notify the blocking import's condition variable.
//!
//! The worker retains the process-lifetime Kernel returned by Kernel::new.
//! Shutdown is terminal and joins that same worker after draining accepted work;
//! callers must arrange shutdown before tearing down memory or other host state.

use std::collections::VecDeque;
use std::sync::{Condvar, Mutex};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use runtime_core::AddressSpace;
use runtime_core::address_space::{AllocError, AllocResult, PAGE_4K};
use runtime_core::heap::protect;

use crate::Kernel;

pub const CONTEXT_COUNT: u32 = 320;
pub const CONTEXT_SIZE: u32 = 64;
const CONTEXT_ARRAY_ADDRESS: u32 = 0x7FEA_1800;
pub const CURRENT_CONTEXT_INDEX: u32 = 0x7FEA_1818;
pub const NEXT_CONTEXT_INDEX: u32 = 0x7FEA_181C;
pub const CONTEXT_KICK: u32 = 0x7FEA_1940;
pub const CONTEXT_LOCK: u32 = 0x7FEA_1A40;
pub const CONTEXT_CLEAR: u32 = 0x7FEA_1A80;

/// A bit field of XMA_CONTEXT_DATA: (dword, first bit, width), bits counted
/// from the least significant bit of the big-endian dword.
type Field = (usize, u32, u32);

pub mod field {
    use super::Field;
    pub const INPUT_BUFFER_0_PACKET_COUNT: Field = (0, 0, 12);
    pub const LOOP_COUNT: Field = (0, 12, 8);
    pub const INPUT_BUFFER_0_VALID: Field = (0, 20, 1);
    pub const INPUT_BUFFER_1_VALID: Field = (0, 21, 1);
    pub const OUTPUT_BUFFER_BLOCK_COUNT: Field = (0, 22, 5);
    pub const OUTPUT_BUFFER_WRITE_OFFSET: Field = (0, 27, 5);
    pub const INPUT_BUFFER_1_PACKET_COUNT: Field = (1, 0, 12);
    pub const LOOP_SUBFRAME_END: Field = (1, 14, 3);
    pub const LOOP_SUBFRAME_SKIP: Field = (1, 17, 3);
    pub const SUBFRAME_DECODE_COUNT: Field = (1, 20, 4);
    pub const SAMPLE_RATE: Field = (1, 27, 2);
    pub const IS_STEREO: Field = (1, 29, 1);
    pub const OUTPUT_BUFFER_VALID: Field = (1, 31, 1);
    pub const INPUT_BUFFER_READ_OFFSET: Field = (2, 0, 26);
    pub const ERROR_STATUS: Field = (2, 26, 5);
    pub const LOOP_START: Field = (3, 0, 26);
    pub const LOOP_END: Field = (4, 0, 26);
    pub const PACKET_METADATA: Field = (4, 26, 5);
    pub const CURRENT_BUFFER: Field = (4, 31, 1);
    pub const INPUT_BUFFER_0_PTR: Field = (5, 0, 32);
    pub const INPUT_BUFFER_1_PTR: Field = (6, 0, 32);
    pub const OUTPUT_BUFFER_PTR: Field = (7, 0, 32);
    pub const WORK_BUFFER_PTR: Field = (8, 0, 32);
    pub const OUTPUT_BUFFER_READ_OFFSET: Field = (9, 0, 5);
}

/// XMA_CONTEXT_DATA as host dwords.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct ContextData([u32; 16]);

impl ContextData {
    /// Raw descriptor snapshot. Callers must exclude concurrent device writes;
    /// imports should use Kernel::xma_context instead.
    pub fn load(k: &Kernel, ptr: u32) -> Self {
        ContextData(std::array::from_fn(|i| k.mem().read_u32(ptr + i as u32 * 4)))
    }

    /// Raw descriptor publication; callers must hold the slot lock or establish
    /// device quiescence. Imports should use Kernel::xma_update_context instead.
    pub fn store(&self, k: &Kernel, ptr: u32) {
        for (i, d) in self.0.iter().enumerate() {
            k.mem().write_u32(ptr + i as u32 * 4, *d);
        }
    }

    pub fn get(&self, (dword, shift, width): Field) -> u32 {
        let mask = if width == 32 { u32::MAX } else { (1 << width) - 1 };
        (self.0[dword] >> shift) & mask
    }

    pub fn set(&mut self, (dword, shift, width): Field, value: u32) {
        let mask = if width == 32 { u32::MAX } else { (1 << width) - 1 };
        self.0[dword] = (self.0[dword] & !(mask << shift)) | ((value & mask) << shift);
    }
}

#[derive(Default)]
struct Slot {
    allocated: bool,
    enabled: bool,
    generation: u64,
    /// Decoder state carried between kicks.
    ctx: xma_decoder::Context,
}

pub struct XmaDecoder {
    /// Guest address of the context array, fixed before guest execution.
    array: u32,
    physical: u32,
    slots: Vec<Mutex<Slot>>,
    indices: Mutex<(u32, u32)>,
    command_words: Mutex<[u32; 30]>,
    queue: Mutex<WorkQueue>,
    changed: Condvar,
    worker: Mutex<Option<JoinHandle<()>>>,
}

#[derive(Default)]
struct WorkQueue {
    pending: VecDeque<(usize, u64)>,
    in_flight: usize,
    stopped: bool,
    failed: bool,
    worker_running: bool,
    join_in_progress: bool,
    events: u64,
    #[cfg(test)]
    paused: bool,
    #[cfg(test)]
    buffer_waiters: usize,
    #[cfg(test)]
    shutdown_waiters: usize,
}

impl WorkQueue {
    fn paused(&self) -> bool {
        #[cfg(test)]
        {
            self.paused
        }
        #[cfg(not(test))]
        {
            false
        }
    }
}

impl Slot {
    fn cancel(&mut self) {
        self.enabled = false;
        self.generation = self.generation.checked_add(1).expect("XMA generation exhausted");
    }
}

impl XmaDecoder {
    pub(crate) fn new(space: &AddressSpace) -> AllocResult<Self> {
        let array =
            space.alloc_physical(CONTEXT_COUNT * CONTEXT_SIZE, PAGE_4K, 0, u32::MAX, 256, protect::READWRITE)?;
        let physical = space.physical_address(array).ok_or(AllocError::WrongRegion(array))?;
        // The SDK caches this register before its first XMACreateContext and uses
        // (context physical address - array physical base) >> 6 as the context ID.
        // XMA registers are little-endian; guest context data remains big-endian.
        space.mem.write_bytes(CONTEXT_ARRAY_ADDRESS, &physical.to_le_bytes());
        Ok(XmaDecoder {
            array,
            physical,
            slots: (0..CONTEXT_COUNT).map(|_| Mutex::new(Slot::default())).collect(),
            indices: Mutex::new((0, 1)),
            command_words: Mutex::new([0; 30]),
            queue: Mutex::new(WorkQueue::default()),
            changed: Condvar::new(),
            worker: Mutex::new(None),
        })
    }
}

/// Guest physical memory for the decoder, mapped page by page (a range may cross from physical
/// allocations into the identity-mapped XEX region).
struct PhysicalMemory<'a>(&'a Kernel);

impl PhysicalMemory<'_> {
    fn each_page(&self, addr: u32, len: usize, mut f: impl FnMut(u32, usize, usize)) -> bool {
        let mut done = 0usize;
        while done < len {
            let phys = addr.wrapping_add(done as u32);
            let n = ((PAGE_4K - phys % PAGE_4K) as usize).min(len - done);
            let Some(guest) = self.0.rt.space.translate_physical(phys) else {
                return false;
            };
            f(guest, done, n);
            done += n;
        }
        true
    }
}

impl xma_decoder::XmaMemory for PhysicalMemory<'_> {
    fn read(&self, addr: u32, buf: &mut [u8]) -> bool {
        let mem = self.0.mem();
        let len = buf.len();
        self.each_page(addr, len, |guest, offset, n| mem.read_bytes(guest, &mut buf[offset..offset + n]))
    }

    fn write(&mut self, addr: u32, data: &[u8]) -> bool {
        let mem = self.0.mem();
        self.each_page(addr, data.len(), |guest, offset, n| mem.write_bytes(guest, &data[offset..offset + n]))
    }
}

impl Kernel {
    fn xma_index(&self, ptr: u32) -> Option<usize> {
        let base = self.xma.array;
        (ptr >= base && ptr < base + CONTEXT_COUNT * CONTEXT_SIZE && (ptr - base).is_multiple_of(CONTEXT_SIZE))
            .then(|| ((ptr - base) / CONTEXT_SIZE) as usize)
    }

    /// XMACreateContext: a zeroed context, or 0 when all are in use.
    pub fn xma_allocate(&self) -> u32 {
        let base = self.xma.array;
        for (i, cell) in self.xma.slots.iter().enumerate() {
            let mut slot = cell.lock().unwrap();
            if !slot.allocated {
                slot.cancel();
                slot.allocated = true;
                slot.ctx.reset();
                let ptr = base + i as u32 * CONTEXT_SIZE;
                ContextData::default().store(self, ptr);
                return ptr;
            }
        }
        0
    }

    pub fn xma_release(&self, ptr: u32) {
        if let Some(i) = self.xma_index(ptr) {
            let mut slot = self.xma.slots[i].lock().unwrap();
            slot.cancel();
            slot.allocated = false;
            slot.ctx.reset();
            ContextData::default().store(self, ptr);
            drop(slot);
            self.xma_signal_context_change();
        }
    }

    /// The decoder's reset: inputs and output invalid, offsets rewound.
    pub fn xma_clear(&self, ptr: u32) {
        let Some(i) = self.xma_index(ptr) else { return };
        let mut slot = self.xma.slots[i].lock().unwrap();
        if !slot.allocated {
            return;
        }
        slot.cancel();
        let mut d = ContextData::load(self, ptr);
        slot.ctx.clear(&mut d.0);
        d.store(self, ptr);
        drop(slot);
        self.xma_signal_context_change();
    }

    /// Serialized guest context access for imports while the worker may run.
    pub fn xma_context(&self, ptr: u32) -> Option<ContextData> {
        let i = self.xma_index(ptr)?;
        // A decoder panic is terminal; blocking imports must fail rather than
        // unwind again while trying to inspect its poisoned context.
        let slot = self.xma.slots[i].lock().ok()?;
        slot.allocated.then(|| ContextData::load(self, ptr))
    }

    pub fn xma_update_context(&self, ptr: u32, update: impl FnOnce(&mut ContextData)) -> bool {
        let Some(i) = self.xma_index(ptr) else {
            return false;
        };
        let slot = self.xma.slots[i].lock().unwrap();
        if !slot.allocated {
            return false;
        }
        let mut d = ContextData::load(self, ptr);
        update(&mut d);
        d.store(self, ptr);
        drop(slot);
        self.xma_signal_context_change();
        true
    }

    pub fn xma_initialize(&self, ptr: u32, mut data: ContextData) -> bool {
        let Some(i) = self.xma_index(ptr) else {
            return false;
        };
        let mut slot = self.xma.slots[i].lock().unwrap();
        if !slot.allocated {
            return false;
        }
        slot.cancel();
        slot.ctx.clear(&mut data.0);
        data.store(self, ptr);
        drop(slot);
        self.xma_signal_context_change();
        true
    }

    fn xma_signal_context_change(&self) {
        let mut queue = self.xma.queue.lock().unwrap();
        queue.events = queue.events.wrapping_add(1);
        self.xma.changed.notify_all();
    }

    /// Wait for both input-valid bits, including queued work with no work buffer.
    /// The event sequence closes the snapshot-to-wait race without queue -> slot
    /// nesting. Direct guest edits still require the guest's device protocol.
    pub fn xma_block_while_in_use(&self, ptr: u32) -> bool {
        loop {
            let observed = self.xma.queue.lock().unwrap().events;
            let Some(data) = self.xma_context(ptr) else {
                return false;
            };
            if data.get(field::INPUT_BUFFER_0_VALID) == 0 && data.get(field::INPUT_BUFFER_1_VALID) == 0 {
                return true;
            }
            let mut queue = self.xma.queue.lock().unwrap();
            if queue.failed || queue.stopped {
                return false;
            }
            if queue.events != observed {
                continue;
            }
            #[cfg(test)]
            {
                queue.buffer_waiters += 1;
                self.xma.changed.notify_all();
            }
            queue = self.xma.changed.wait(queue).unwrap();
            #[cfg(test)]
            {
                queue.buffer_waiters -= 1;
            }
            drop(queue);
        }
    }

    fn xma_start_worker(&'static self) -> Result<(), String> {
        // Never entered with a slot or queue lock held.
        let mut handle = self.xma.worker.lock().unwrap();
        if self.xma.queue.lock().unwrap().stopped {
            return Err("XMA worker is shut down".into());
        }
        if handle.is_none() {
            self.xma.queue.lock().unwrap().worker_running = true;
            let spawned = runtime_core::thread::spawn_host_thread("xma".into(), move || {
                if std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| self.xma_worker_main())).is_err() {
                    let mut queue = self.xma.queue.lock().unwrap_or_else(|e| e.into_inner());
                    queue.failed = true;
                    queue.stopped = true;
                    queue.pending.clear();
                    queue.in_flight = 0;
                    queue.worker_running = false;
                    self.xma.changed.notify_all();
                }
            });
            match spawned {
                Ok(thread) => *handle = Some(thread),
                Err(e) => {
                    self.xma.queue.lock().unwrap().worker_running = false;
                    self.xma.changed.notify_all();
                    return Err(format!("starting XMA worker: {e}"));
                }
            }
        }
        Ok(())
    }

    fn xma_enable_index(&'static self, i: usize) -> Result<(), String> {
        self.xma_start_worker()?;
        let mut slot = self.xma.slots[i].lock().unwrap();
        if !slot.allocated {
            return Ok(());
        }
        let mut queue = self.xma.queue.lock().unwrap();
        if queue.stopped {
            return Err("XMA worker is shut down".into());
        }
        // Like Xenia's enabled flag, pending identical kicks coalesce. Each
        // operand is nevertheless delivered here; no register RAM is sampled.
        if !slot.enabled {
            slot.enabled = true;
            queue.pending.push_back((i, slot.generation));
            // Completion waiters share this condition variable with the worker.
            self.xma.changed.notify_all();
        }
        Ok(())
    }

    fn xma_worker_main(&'static self) {
        loop {
            let (i, generation) = {
                let mut queue = self.xma.queue.lock().unwrap();
                while (queue.pending.is_empty() || queue.paused()) && !queue.stopped {
                    queue = self.xma.changed.wait(queue).unwrap();
                }
                let Some(job) = queue.pending.pop_front() else {
                    queue.worker_running = false;
                    self.xma.changed.notify_all();
                    return;
                };
                queue.in_flight += 1;
                self.xma.changed.notify_all();
                job
            };
            {
                let mut slot = self.xma.slots[i].lock().unwrap();
                // Check generation BEFORE touching enabled: an old job cannot
                // erase a kick belonging to a reused or reinitialized context.
                if slot.generation == generation && slot.allocated && slot.enabled {
                    slot.enabled = false;
                    let ptr = self.xma.array + i as u32 * CONTEXT_SIZE;
                    let mut d = ContextData::load(self, ptr);
                    slot.ctx.work(&mut d.0, &mut PhysicalMemory(self));
                    d.store(self, ptr);
                }
            }
            let mut queue = self.xma.queue.lock().unwrap();
            queue.in_flight -= 1;
            queue.events = queue.events.wrapping_add(1);
            self.xma.changed.notify_all();
        }
    }

    /// Device-logical LE register value; the C++ bridge handles PPC byte order.
    pub fn xma_mmio_read(&self, address: u32) -> Option<u32> {
        match address {
            CONTEXT_ARRAY_ADDRESS => Some(self.xma.physical),
            CURRENT_CONTEXT_INDEX => {
                let mut indices = self.xma.indices.lock().unwrap();
                indices.0 = indices.1;
                indices.1 = (indices.1 + 1) % CONTEXT_COUNT;
                Some(indices.0)
            }
            NEXT_CONTEXT_INDEX => Some(self.xma.indices.lock().unwrap().1),
            _ => command_register(address).map(|(word, _)| self.xma.command_words.lock().unwrap()[word]),
        }
    }

    /// Receives a captured store operand, not a sampled register value.
    /// Returns false for registers outside the implemented command groups.
    pub fn xma_mmio_write(&'static self, address: u32, value: u32) -> Result<bool, String> {
        let Some((base, operation)) = [(CONTEXT_KICK, 0), (CONTEXT_LOCK, 1), (CONTEXT_CLEAR, 2)]
            .into_iter()
            .find(|(base, _)| address >= *base && address < *base + 40 && (address - *base) % 4 == 0)
        else {
            return Ok(false);
        };
        {
            let mut words = self.xma.command_words.lock().unwrap();
            words[operation * 10 + ((address - base) / 4) as usize] = value;
            // Diagnostic/backing bytes retain the LE register value. This raw
            // memory API cannot recurse through the C++ MMIO bridge.
            self.mem().write_bytes(address, &value.to_le_bytes());
        }
        let first = ((address - base) / 4 * 32) as usize;
        for bit in 0..32 {
            if value & (1u32 << bit) == 0 {
                continue;
            }
            let i = first + bit;
            if operation == 0 {
                self.xma_enable_index(i)?;
            } else if operation == 1 {
                self.xma.slots[i].lock().unwrap().cancel();
            } else {
                self.xma_clear(self.xma.array + i as u32 * CONTEXT_SIZE);
            }
        }
        Ok(true)
    }

    pub fn xma_enable(&'static self, ptr: u32) -> Result<(), String> {
        let i = self.xma_index(ptr).ok_or("invalid XMA context")? as u32;
        self.xma_mmio_write(CONTEXT_KICK + (i >> 5) * 4, 1 << (i & 31)).map(|_| ())
    }

    /// Compatibility helper for synchronous host callers, using the same worker.
    ///
    /// Returns false on an enable error, worker failure, or a five-second idle
    /// deadline. False does not establish buffer quiescence: cancel/release the
    /// context or shut down before reusing its memory. The static receiver is
    /// required because accepted work may outlive this call. Prefer xma_enable
    /// plus an explicit xma_wait_idle deadline when the error detail is needed.
    #[must_use]
    pub fn xma_work(&'static self, ptr: u32) -> bool {
        match self.xma_enable(ptr) {
            Ok(()) => self.xma_wait_idle(Duration::from_secs(5)),
            Err(error) => {
                log::error!("XMA work: {error}");
                false
            }
        }
    }

    pub fn xma_disable(&'static self, ptr: u32, wait: bool) -> bool {
        let Some(i) = self.xma_index(ptr) else {
            return false;
        };
        if self.xma_mmio_write(CONTEXT_LOCK + (i as u32 >> 5) * 4, 1 << (i & 31)).is_err() {
            return false;
        }
        if wait {
            drop(self.xma.slots[i].lock().unwrap());
            true
        } else {
            self.xma.slots[i].try_lock().is_ok()
        }
    }

    /// Host/test completion barrier; a kick itself does not promise completion.
    pub fn xma_wait_idle(&self, timeout: Duration) -> bool {
        let deadline = Instant::now() + timeout;
        let mut queue = self.xma.queue.lock().unwrap();
        while !queue.pending.is_empty() || queue.in_flight != 0 {
            let Some(left) = deadline.checked_duration_since(Instant::now()) else {
                return false;
            };
            let (next, timed) = self.xma.changed.wait_timeout(queue, left).unwrap();
            queue = next;
            if timed.timed_out() && (!queue.pending.is_empty() || queue.in_flight != 0) {
                return false;
            }
        }
        !queue.failed
    }

    /// Terminal shutdown. New kicks are refused; already queued work drains.
    pub fn xma_shutdown(&self) -> Result<(), String> {
        let handle = {
            let mut handle = self.xma.worker.lock().unwrap();
            let mut queue = self.xma.queue.lock().unwrap();
            queue.stopped = true;
            if handle.is_some() {
                queue.join_in_progress = true;
            }
            self.xma.changed.notify_all();
            handle.take()
        };
        if let Some(handle) = handle {
            let joined = handle.join();
            let mut queue = self.xma.queue.lock().unwrap();
            queue.join_in_progress = false;
            if joined.is_err() {
                queue.failed = true;
                queue.worker_running = false;
            }
            self.xma.changed.notify_all();
        }
        let mut queue = self.xma.queue.lock().unwrap();
        while queue.worker_running || queue.join_in_progress {
            #[cfg(test)]
            {
                queue.shutdown_waiters += 1;
                self.xma.changed.notify_all();
            }
            queue = self.xma.changed.wait(queue).unwrap();
            #[cfg(test)]
            {
                queue.shutdown_waiters -= 1;
            }
        }
        if queue.failed { Err("XMA worker failed".into()) } else { Ok(()) }
    }
}

fn command_register(address: u32) -> Option<(usize, u32)> {
    [CONTEXT_KICK, CONTEXT_LOCK, CONTEXT_CLEAR].into_iter().enumerate().find_map(|(op, base)| {
        (address >= base && address < base + 40 && (address - base) % 4 == 0)
            .then(|| (op * 10 + ((address - base) / 4) as usize, base))
    })
}

#[cfg(test)]
#[path = "xma_init_tests.rs"]
mod init_tests;

#[cfg(test)]
#[path = "xma_device_tests.rs"]
mod device_tests;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bit_fields_round_trip() {
        let mut d = ContextData::default();
        d.set(field::OUTPUT_BUFFER_WRITE_OFFSET, 31);
        d.set(field::INPUT_BUFFER_0_VALID, 1);
        d.set(field::INPUT_BUFFER_0_PACKET_COUNT, 0xFFF);
        assert_eq!(d.0[0], (31 << 27) | (1 << 20) | 0xFFF);
        d.set(field::OUTPUT_BUFFER_VALID, 1);
        assert_eq!(d.0[1], 0x8000_0000);
        d.set(field::INPUT_BUFFER_0_PACKET_COUNT, 3);
        assert_eq!(d.get(field::INPUT_BUFFER_0_PACKET_COUNT), 3);
        assert_eq!(d.get(field::OUTPUT_BUFFER_WRITE_OFFSET), 31);
        d.set(field::OUTPUT_BUFFER_PTR, 0x1234_5678);
        assert_eq!(d.get(field::OUTPUT_BUFFER_PTR), 0x1234_5678);
    }
}
