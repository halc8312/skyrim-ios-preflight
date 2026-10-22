//! AOT-only synthetic texture and CPU/runtime probes. No game files are loaded.

use std::cell::Cell;
use std::mem::{align_of, offset_of, size_of};
use std::ptr;
use std::sync::Mutex;

mod atomic_probe;

use runtime_core::context::PPCContext;
use runtime_core::guest::{CallFunc, GuestModule};
use runtime_core::memory::{self, GuestMemory, MemoryError, PAGE_SIZE, map};

const WIDTH: u32 = 4;
const HEIGHT: u32 = 4;
const RGBA_BYTES: usize = WIDTH as usize * HEIGHT as usize * 4;

// Hand-authored BC1: red and blue RGB565 endpoints, then [0, 1, 2, 3] per row.
// This fixture contains no bytes extracted from a game.
const BC1_BLOCK: [u8; 8] = [0x00, 0xF8, 0x1F, 0x00, 0xE4, 0xE4, 0xE4, 0xE4];

#[unsafe(no_mangle)]
pub extern "C" fn probe_abi_version() -> u32 {
    1
}

#[unsafe(no_mangle)]
pub extern "C" fn probe_width() -> u32 {
    WIDTH
}

#[unsafe(no_mangle)]
pub extern "C" fn probe_height() -> u32 {
    HEIGHT
}

#[unsafe(no_mangle)]
pub extern "C" fn probe_rgba_byte_len() -> usize {
    RGBA_BYTES
}

/// Decodes the fixed synthetic BC1 block to row-major, straight-alpha RGBA8.
///
/// Returns 64 on success, -1 for null, -2 for insufficient capacity, or -3 if
/// the decoder panics. A failure leaves the output buffer untouched.
///
/// # Safety
/// For a non-null pointer and capacity >= 64, `out` must point to at least 64
/// writable bytes for this call. No other thread may access those bytes during
/// the call. The pointer is never retained; bytes beyond the first 64 are unused.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn probe_decode_rgba(out: *mut u8, capacity: usize) -> i32 {
    if out.is_null() {
        return -1;
    }
    if capacity < RGBA_BYTES {
        return -2;
    }

    // Decode before touching caller memory. An upstream panic cannot unwind
    // through Swift/C, and even that failure preserves the caller's buffer.
    let Ok(pixels) = std::panic::catch_unwind(|| {
        let mut pixels = [[0u8; 4]; 16];
        // Keep the probe exercising the decoder on device instead of letting
        // release/LTO replace this constant input with precomputed pixels.
        let block = std::hint::black_box(BC1_BLOCK);
        xenos_texture::bc::decode_bc1(&block, &mut pixels);
        pixels
    }) else {
        return -3;
    };

    // Arrays of [u8; 4] have no padding. The checked size is fixed, so a caller
    // reporting an excessive capacity never makes us construct a huge slice.
    unsafe {
        ptr::copy_nonoverlapping(pixels.as_ptr().cast::<u8>(), out, RGBA_BYTES);
    }
    RGBA_BYTES as i32
}

const RUNTIME_ABI: u32 = 1;
const LAYOUT_WORDS: usize = 209;
const SYNTHETIC_FUNCTIONS: [u32; 2] = [0x8200_1000, 0x8200_1004];
const CPU_MEMORY: u32 = 0x4001_4000;
// Upstream translation/dirty tables are process-global. These probes must not
// overlap each other or a running guest; they do not install a game runtime.
static RUNTIME_PROBE_LOCK: Mutex<()> = Mutex::new(());
thread_local! {
    static NESTED_CALLS: Cell<Option<u32>> = const { Cell::new(None) };
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct ProbeRuntimeResult {
    pub abi_version: u32,
    pub struct_size: u32,
    pub passed: u64,
    pub failed: u64,
    pub callback_result: u32,
    pub reserved: u32,
}

impl ProbeRuntimeResult {
    fn new() -> Self {
        Self {
            abi_version: RUNTIME_ABI,
            struct_size: size_of::<Self>() as u32,
            ..Self::default()
        }
    }
    fn check(&mut self, bit: u64, condition: bool) {
        if condition {
            self.passed |= bit;
        } else {
            self.failed |= bit;
        }
    }
}

/// Borrowed descriptor valid only for the synchronous synthetic call below.
/// This is deliberately NOT registration of arbitrary recompiled game code.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ProbeGuestDescriptor {
    pub abi_version: u32,
    pub struct_size: u32,
    pub context_size: u32,
    pub context_alignment: u32,
    pub image_base: u32,
    pub image_size: u32,
    pub code_base: u32,
    pub code_size: u32,
    pub functions: *const u32,
    pub function_count: usize,
    pub call: Option<CallFunc>,
    pub init_context: Option<unsafe extern "C" fn(*mut PPCContext)>,
}

const _: () = {
    assert!(size_of::<ProbeRuntimeResult>() == 32);
    assert!(size_of::<ProbeGuestDescriptor>() == 64);
};

fn pointer_check<T>(pointer: *const T) -> Result<(), i32> {
    if pointer.is_null() {
        return Err(-1);
    }
    if !(pointer as usize).is_multiple_of(align_of::<T>()) {
        return Err(-4);
    }
    Ok(())
}

/// The array contains actual Rust field offsets, not just the upstream constants.
fn context_layout() -> [u32; LAYOUT_WORDS] {
    let mut words = [0u32; LAYOUT_WORDS];
    words[..9].copy_from_slice(&[
        RUNTIME_ABI,
        size_of::<PPCContext>() as u32,
        align_of::<PPCContext>() as u32,
        offset_of!(PPCContext, lr) as u32,
        offset_of!(PPCContext, ctr) as u32,
        offset_of!(PPCContext, xer) as u32,
        offset_of!(PPCContext, reserved) as u32,
        offset_of!(PPCContext, msr) as u32,
        offset_of!(PPCContext, fpscr_csr) as u32,
    ]);
    // gpr_storage is intentionally private upstream. Derive every architectural
    // GPR slot by setting a unique marker via its real accessor, then inspecting
    // only the initialized u64 register area (never struct padding).
    let mut context = PPCContext::new_zeroed();
    const MARKER: u64 = 0xC1D2_E3F4_5566_7788;
    for n in 0..32 {
        context.set_gpr(n, MARKER);
        let slots = unsafe {
            std::slice::from_raw_parts((&*context as *const PPCContext).cast::<u64>(), 32)
        };
        words[9 + n] = (slots
            .iter()
            .position(|&value| value == MARKER)
            .expect("GPR marker")
            * 8) as u32;
        context.set_gpr(n, 0);
    }
    for n in 0..8 {
        words[41 + n] =
            (offset_of!(PPCContext, cr) + n * size_of::<runtime_core::context::CrField>()) as u32;
    }
    for n in 0..32 {
        words[49 + n] = (offset_of!(PPCContext, fpr) + n * size_of::<u64>()) as u32;
    }
    for n in 0..128 {
        words[81 + n] =
            (offset_of!(PPCContext, vr) + n * size_of::<runtime_core::context::VReg>()) as u32;
    }
    words
}

/// # Safety
/// `out` must be aligned and writable for 209 u32 values if capacity >= 209.
/// Capacity is in words. Null/short/unaligned/panic leaves output untouched.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn probe_runtime_context_layout(out: *mut u32, capacity: usize) -> i32 {
    if let Err(error) = pointer_check(out) {
        return error;
    }
    if capacity < LAYOUT_WORDS {
        return -2;
    }
    let Ok(words) = std::panic::catch_unwind(context_layout) else {
        return -3;
    };
    unsafe {
        ptr::copy_nonoverlapping(words.as_ptr(), out, LAYOUT_WORDS);
    }
    LAYOUT_WORDS as i32
}

unsafe fn checked_descriptor(pointer: *const ProbeGuestDescriptor) -> Result<GuestModule, i32> {
    pointer_check(pointer)?;
    // The caller guarantees readable descriptor storage. Structural validation
    // cannot make an invalid non-null address or function pointer safe.
    let descriptor = unsafe { pointer.read() };
    if descriptor.abi_version != RUNTIME_ABI
        || descriptor.struct_size as usize != size_of::<ProbeGuestDescriptor>()
        || descriptor.context_size as usize != size_of::<PPCContext>()
        || descriptor.context_alignment as usize != align_of::<PPCContext>()
        || descriptor.function_count != SYNTHETIC_FUNCTIONS.len()
    {
        return Err(-5);
    }
    let call = descriptor.call.ok_or(-5)?;
    let init_context = descriptor.init_context.ok_or(-5)?;
    pointer_check(descriptor.functions)?;
    let functions =
        unsafe { std::slice::from_raw_parts(descriptor.functions, descriptor.function_count) };
    if functions.windows(2).any(|pair| pair[0] >= pair[1]) || functions != SYNTHETIC_FUNCTIONS {
        return Err(-5);
    }
    let image_end = descriptor
        .image_base
        .checked_add(descriptor.image_size)
        .ok_or(-5)?;
    let code_end = descriptor
        .code_base
        .checked_add(descriptor.code_size)
        .ok_or(-5)?;
    if descriptor.image_size == 0
        || descriptor.code_size == 0
        || descriptor.image_base < map::XEX_BASE
        || image_end > map::XEX_END
        || descriptor.code_base < descriptor.image_base
        || code_end > image_end
        || !descriptor.code_base.is_multiple_of(4)
        || !descriptor.code_size.is_multiple_of(4)
        || functions.iter().any(|&entry| {
            !entry.is_multiple_of(4) || entry < descriptor.code_base || entry >= code_end
        })
    {
        return Err(-5);
    }
    Ok(GuestModule {
        image_base: descriptor.image_base,
        image_size: descriptor.image_size,
        code_base: descriptor.code_base,
        code_size: descriptor.code_size,
        // Never transmute a borrowed C array to 'static or leak an allocation.
        functions: &SYNTHETIC_FUNCTIONS,
        call,
        init_context,
    })
}

/// Pure C++ -> Rust callback used by the synthetic CPU function.
#[unsafe(no_mangle)]
pub extern "C" fn probe_runtime_callback_mix(left: u32, right: u32) -> u32 {
    NESTED_CALLS.with(|calls| {
        if let Some(count) = calls.get() {
            calls.set(Some(count.saturating_add(1)));
        }
    });
    left ^ right
}

/// Five narrow source-only checks, not Kernel boot, rendering or audio playback.
#[unsafe(no_mangle)]
pub extern "C" fn probe_runtime_component_checks() -> u32 {
    std::panic::catch_unwind(|| {
        let mut mask = 0;
        if std::hint::black_box(kernel_hle::all_exports().count()) > 0 {
            mask |= 1;
        }
        if vfs::matches_pattern(std::hint::black_box("Probe.TXT"), "p?obe.*") {
            mask |= 2;
        }
        if matches!(
            xex::Xex::parse(std::hint::black_box(b"NOT-XEX2")),
            Err(xex::Error::BadMagic)
        ) {
            mask |= 4;
        }
        if d3d_hle::nop_header(std::hint::black_box(2)) == 0xC001_1000 {
            mask |= 8;
        }
        if xma_decoder::float_to_s16(std::hint::black_box(0.5)) == 16384
            && xma_decoder::float_to_s16(-2.0) == -32767
        {
            mask |= 16;
        }
        mask
    })
    .unwrap_or(0)
}

struct ActiveCallback;
impl Drop for ActiveCallback {
    fn drop(&mut self) {
        NESTED_CALLS.with(|calls| calls.set(None));
    }
}

/// Releases committed ranges before unmapping, balancing upstream accounting.
struct ProbeMemory {
    memory: GuestMemory,
    ranges: Vec<(u32, u32)>,
}
impl ProbeMemory {
    fn reserve() -> Result<Self, i32> {
        Ok(Self {
            memory: GuestMemory::reserve().map_err(|_| -6)?,
            ranges: Vec::new(),
        })
    }
    fn commit(&mut self, address: u32, length: u32) -> Result<(), i32> {
        self.memory.commit(address, length).map_err(|_| -6)?;
        self.ranges.push((address, length));
        Ok(())
    }
}
impl Drop for ProbeMemory {
    fn drop(&mut self) {
        for &(address, length) in &self.ranges {
            let _ = self.memory.release(address, length);
        }
    }
}

fn memory_selftest() -> Result<ProbeRuntimeResult, i32> {
    let mut result = ProbeRuntimeResult::new();
    let _guard = RUNTIME_PROBE_LOCK.lock().map_err(|_| -3)?;
    let mut allocation = ProbeMemory::reserve()?;
    result.check(1 << 0, !allocation.memory.base_ptr().is_null());
    const BASE: u32 = 0x4001_0000;
    for page in 0..4 {
        allocation.commit(BASE + page * PAGE_SIZE, PAGE_SIZE)?;
        let memory = &allocation.memory;
        result.check(
            1 << 1,
            memory
                .read_vec(BASE + page * PAGE_SIZE, PAGE_SIZE)
                .iter()
                .all(|&v| v == 0),
        );
        memory.fill(BASE + page * PAGE_SIZE, PAGE_SIZE, (page + 1) as u8);
        result.check(
            1 << 4,
            (0..=page).all(|other| {
                memory
                    .read_vec(BASE + other * PAGE_SIZE, PAGE_SIZE)
                    .iter()
                    .all(|&v| v == (other + 1) as u8)
            }),
        );
    }
    let memory = &allocation.memory;
    memory.write_u32(BASE + 0x20, 0x1122_3344);
    memory.write_u64(BASE + 0x28, 0x0102_0304_0506_0708);
    result.check(
        1 << 2,
        memory.read_vec(BASE + 0x20, 4) == [0x11, 0x22, 0x33, 0x44]
            && memory.read_u32(BASE + 0x20) == 0x1122_3344
            && memory.read_u64(BASE + 0x28) == 0x0102_0304_0506_0708,
    );
    memory.fill(BASE, PAGE_SIZE, 1);
    memory
        .decommit(BASE + PAGE_SIZE, PAGE_SIZE)
        .map_err(|_| -6)?;
    result.check(
        1 << 3,
        memory
            .read_vec(BASE + PAGE_SIZE, PAGE_SIZE)
            .iter()
            .all(|&v| v == 0),
    );
    result.check(
        1 << 4,
        [0, 2, 3].into_iter().all(|other| {
            memory
                .read_vec(BASE + other * PAGE_SIZE, PAGE_SIZE)
                .iter()
                .all(|&v| v == (other + 1) as u8)
        }),
    );
    memory.fill(BASE + PAGE_SIZE, PAGE_SIZE, 0xA5);
    memory.commit(BASE + PAGE_SIZE, PAGE_SIZE).map_err(|_| -6)?;
    result.check(
        1 << 5,
        memory
            .read_vec(BASE + PAGE_SIZE, PAGE_SIZE)
            .iter()
            .all(|&v| v == 0),
    );
    memory
        .release(BASE + 2 * PAGE_SIZE, PAGE_SIZE)
        .map_err(|_| -6)?;
    result.check(
        1 << 8,
        memory
            .read_vec(BASE + 2 * PAGE_SIZE, PAGE_SIZE)
            .iter()
            .all(|&v| v == 0)
            && [0, 3].into_iter().all(|other| {
                memory
                    .read_vec(BASE + other * PAGE_SIZE, PAGE_SIZE)
                    .iter()
                    .all(|&v| v == (other + 1) as u8)
            }),
    );
    memory
        .commit(BASE + 2 * PAGE_SIZE, PAGE_SIZE)
        .map_err(|_| -6)?;
    const PHYSICAL: u32 = 0x20_000;
    allocation.commit(map::PHYSICAL_BASE + PHYSICAL, PAGE_SIZE)?;
    let memory = &allocation.memory;
    let _ = runtime_core::dirty::take(PHYSICAL / PAGE_SIZE);
    memory.write_u32(map::PHYS_VIEW_16M + PHYSICAL, 0xCAFE_BABE);
    result.check(
        1 << 6,
        memory.read_u32(map::PHYSICAL_BASE + PHYSICAL) == 0xCAFE_BABE
            && memory.read_u32(map::PHYS_VIEW_4K + PHYSICAL - PAGE_SIZE) == 0xCAFE_BABE,
    );
    result.check(
        1 << 7,
        runtime_core::dirty::take(PHYSICAL / PAGE_SIZE)
            && !runtime_core::dirty::take(PHYSICAL / PAGE_SIZE),
    );
    result.check(
        1 << 9,
        matches!(
            memory.commit(0x1000, PAGE_SIZE),
            Err(MemoryError::HostReserved { .. })
        ) && matches!(
            memory.commit(map::PHYS_VIEW_16M, PAGE_SIZE),
            Err(MemoryError::FoldedView { .. })
        ) && matches!(
            memory.commit(u32::MAX - 3, 8),
            Err(MemoryError::OutOfRange { .. })
        ),
    );
    Ok(result)
}

unsafe fn cpu_roundtrip(pointer: *const ProbeGuestDescriptor) -> Result<ProbeRuntimeResult, i32> {
    let module = unsafe { checked_descriptor(pointer) }?;
    let _guard = RUNTIME_PROBE_LOCK.lock().map_err(|_| -3)?;
    let mut allocation = ProbeMemory::reserve()?;
    allocation.commit(CPU_MEMORY, PAGE_SIZE)?;
    let mut context = PPCContext::new_zeroed();
    // A no-op constructor must not pass merely because Rust allocated zeros.
    for register in 0..32 {
        context.set_gpr(register, 0xBAD0_BAD0_BAD0_BAD0);
    }
    context.lr = u64::MAX;
    context.ctr = u64::MAX;
    context.msr = 0;
    unsafe {
        (module.init_context)(&mut *context);
    }
    let mut result = ProbeRuntimeResult::new();
    result.check(1 << 16, true);
    result.check(
        1 << 17,
        (0..32).all(|register| context.gpr(register) == 0)
            && context.lr == 0
            && context.ctr == 0
            && context.msr == 0x0200_A000,
    );
    result.check(
        1 << 18,
        module.has_function(SYNTHETIC_FUNCTIONS[0])
            && module.has_function(SYNTHETIC_FUNCTIONS[1])
            && !module.has_function(SYNTHETIC_FUNCTIONS[0] + 2),
    );
    context.set_gpr(3, 0x1122_3344_5566_7788);
    context.set_gpr(4, u64::from(CPU_MEMORY));
    context.set_gpr(5, 0xA1B2_C3D4);
    context.set_gpr(6, 0x5566_7788);
    allocation.memory.write_u32(CPU_MEMORY, 0x1020_3040);
    NESTED_CALLS.with(|calls| calls.set(Some(0)));
    let _active = ActiveCallback;
    unsafe {
        (module.call)(
            SYNTHETIC_FUNCTIONS[0],
            &mut *context,
            allocation.memory.base_ptr(),
        );
    }
    result.callback_result = context.gpr32(7);
    result.check(
        1 << 19,
        context.gpr(3) == (0x1122_3344_5566_7788 ^ 0xFFEE_DDCC_BBAA_0099)
            && context.gpr(7) == 0x1020_3040,
    );
    result.check(
        1 << 20,
        allocation.memory.read_u32(CPU_MEMORY) == (0xA1B2_C3D4 ^ 0x5566_7788),
    );
    result.check(1 << 21, NESTED_CALLS.with(|calls| calls.get()) == Some(1));
    Ok(result)
}

unsafe fn write_result(
    out: *mut ProbeRuntimeResult,
    capacity: usize,
    action: impl FnOnce() -> Result<ProbeRuntimeResult, i32>,
) -> i32 {
    if let Err(error) = pointer_check(out) {
        return error;
    }
    if capacity < size_of::<ProbeRuntimeResult>() {
        return -2;
    }
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(action)) {
        Ok(Ok(result)) => {
            unsafe {
                out.write(result);
            }
            if result.failed == 0 { 0 } else { -7 }
        }
        Ok(Err(error)) => error,
        Err(_) => -3,
    }
}

/// # Safety
/// Aligned `out` must be writable for one result if capacity >= its byte size.
/// Do not overlap these probes with a real guest runtime.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn probe_runtime_memory_selftest(
    out: *mut ProbeRuntimeResult,
    capacity: usize,
) -> i32 {
    unsafe { write_result(out, capacity, memory_selftest) }
}

/// # Safety
/// Descriptor and two functions must be readable, aligned, and unchanged until
/// return; output must be writable. Callbacks must be genuine matching C/C-unwind
/// functions, synchronous, non-throwing, same-thread, non-reentrant and must not
/// retain context/base. No longjmp across Rust is allowed. Callback pointer
/// validity cannot be established by a structural descriptor validator.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn probe_runtime_cpu_roundtrip(
    descriptor: *const ProbeGuestDescriptor,
    out: *mut ProbeRuntimeResult,
    capacity: usize,
) -> i32 {
    unsafe { write_result(out, capacity, || cpu_roundtrip(descriptor)) }
}

struct SyntheticExit;

/// Deliberately unwinds only through the dedicated C++ RAII probe callback.
/// Never call this through a Swift frame, a non-unwind C ABI, or a noexcept frame.
#[unsafe(no_mangle)]
pub extern "C-unwind" fn probe_runtime_raise_synthetic_exit() -> ! {
    std::panic::panic_any(SyntheticExit)
}

/// # Safety
/// Counter must be aligned, readable/writable, exclusive and initially zero.
/// Call must be a genuine same-thread C-unwind callback. Its only permitted
/// unwind is the dedicated Rust payload; it must not throw a C++ exception,
/// catch/replace the Rust panic, longjmp, or call through Swift/noexcept frames.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn probe_runtime_catch_cpp_unwind(
    call: Option<unsafe extern "C-unwind" fn(*mut u32)>,
    counter: *mut u32,
) -> i32 {
    if let Err(error) = pointer_check(counter) {
        return error;
    }
    let Some(call) = call else {
        return -5;
    };
    if unsafe { counter.read() } != 0 {
        return -5;
    }
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| unsafe { call(counter) })) {
        Err(payload) if payload.is::<SyntheticExit>() && unsafe { counter.read() } == 1 => 0,
        _ => -8,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const EXPECTED_ROW: [u8; 16] = [
        255, 0, 0, 255, // red endpoint
        0, 0, 255, 255, // blue endpoint
        170, 0, 85, 255, // two thirds red, one third blue
        85, 0, 170, 255, // one third red, two thirds blue
    ];

    #[test]
    fn metadata_matches_the_c_buffer_contract() {
        assert_eq!(probe_abi_version(), 1);
        assert_eq!((probe_width(), probe_height()), (4, 4));
        assert_eq!(probe_rgba_byte_len(), 64);
        assert_eq!(std::mem::size_of::<[[u8; 4]; 16]>(), RGBA_BYTES);
    }

    #[test]
    fn decodes_every_expected_pixel_through_the_ffi() {
        let mut out = [0xA5u8; RGBA_BYTES];
        assert_eq!(
            unsafe { probe_decode_rgba(out.as_mut_ptr(), out.len()) },
            64
        );
        for row in out.chunks_exact(16) {
            assert_eq!(row, EXPECTED_ROW);
        }
    }

    #[test]
    fn rejects_every_short_capacity_without_writing() {
        for capacity in 0..RGBA_BYTES {
            let mut out = [0xA5u8; RGBA_BYTES];
            assert_eq!(unsafe { probe_decode_rgba(out.as_mut_ptr(), capacity) }, -2);
            assert_eq!(out, [0xA5; RGBA_BYTES]);
        }
    }

    #[test]
    fn rejects_null_without_dereferencing_it() {
        for capacity in [0, RGBA_BYTES - 1, RGBA_BYTES, usize::MAX] {
            assert_eq!(unsafe { probe_decode_rgba(ptr::null_mut(), capacity) }, -1);
        }
    }

    #[test]
    fn preserves_guard_bytes_and_unused_capacity() {
        let mut guarded = [0xA5u8; RGBA_BYTES + 16];
        let out = unsafe { guarded.as_mut_ptr().add(8) };
        assert_eq!(unsafe { probe_decode_rgba(out, RGBA_BYTES + 8) }, 64);
        assert_eq!(&guarded[..8], &[0xA5; 8]);
        assert_eq!(&guarded[8 + RGBA_BYTES..], &[0xA5; 8]);
        for row in guarded[8..8 + RGBA_BYTES].chunks_exact(16) {
            assert_eq!(row, EXPECTED_ROW);
        }
    }

    #[test]
    fn excessive_reported_capacity_still_writes_only_the_fixed_image() {
        let mut out = [0xA5u8; RGBA_BYTES];
        assert_eq!(
            unsafe { probe_decode_rgba(out.as_mut_ptr(), usize::MAX) },
            64
        );
        for row in out.chunks_exact(16) {
            assert_eq!(row, EXPECTED_ROW);
        }
    }

    unsafe extern "C" fn synthetic_init(context: *mut PPCContext) {
        unsafe {
            context.write(*PPCContext::new_zeroed());
        }
    }

    unsafe extern "C-unwind" fn synthetic_call(
        address: u32,
        context: *mut PPCContext,
        base: *mut u8,
    ) {
        assert_eq!(address, SYNTHETIC_FUNCTIONS[0]);
        let context = unsafe { &mut *context };
        let guest = context.gpr32(4);
        let host = unsafe { base.add(memory::linear(guest) as usize).cast::<u32>() };
        let before = u32::from_be(unsafe { host.read_unaligned() });
        let value = probe_runtime_callback_mix(context.gpr32(5), context.gpr32(6));
        unsafe {
            host.write_unaligned(value.to_be());
        }
        context.set_gpr(7, u64::from(before));
        context.set_gpr(3, context.gpr(3) ^ 0xFFEE_DDCC_BBAA_0099);
    }

    fn descriptor() -> ProbeGuestDescriptor {
        ProbeGuestDescriptor {
            abi_version: RUNTIME_ABI,
            struct_size: size_of::<ProbeGuestDescriptor>() as u32,
            context_size: size_of::<PPCContext>() as u32,
            context_alignment: align_of::<PPCContext>() as u32,
            image_base: 0x8200_0000,
            image_size: 0x2000,
            code_base: 0x8200_1000,
            code_size: 0x1000,
            functions: SYNTHETIC_FUNCTIONS.as_ptr(),
            function_count: 2,
            call: Some(synthetic_call),
            init_context: Some(synthetic_init),
        }
    }

    #[test]
    fn reports_every_actual_context_register_offset() {
        let mut out = [u32::MAX; LAYOUT_WORDS + 1];
        assert_eq!(
            unsafe { probe_runtime_context_layout(out.as_mut_ptr(), out.len()) },
            209
        );
        assert_eq!(&out[..9], &[1, 2688, 64, 256, 264, 272, 280, 288, 324]);
        assert_eq!(&out[9..13], &[8, 16, 24, 0]);
        for n in 4..32 {
            assert_eq!(out[9 + n], n as u32 * 8);
        }
        for n in 0..8 {
            assert_eq!(out[41 + n], 292 + n as u32 * 4);
        }
        for n in 0..32 {
            assert_eq!(out[49 + n], 328 + n as u32 * 8);
        }
        for n in 0..128 {
            assert_eq!(out[81 + n], 592 + n as u32 * 16);
        }
        assert_eq!(out[LAYOUT_WORDS], u32::MAX);
    }

    #[test]
    fn rejects_layout_null_short_and_unaligned_without_writing() {
        let mut out = [0xA5u32; LAYOUT_WORDS];
        assert_eq!(
            unsafe { probe_runtime_context_layout(ptr::null_mut(), usize::MAX) },
            -1
        );
        assert_eq!(
            unsafe { probe_runtime_context_layout(out.as_mut_ptr(), LAYOUT_WORDS - 1) },
            -2
        );
        let unaligned = unsafe { out.as_mut_ptr().cast::<u8>().add(1).cast::<u32>() };
        assert_eq!(
            unsafe { probe_runtime_context_layout(unaligned, LAYOUT_WORDS) },
            -4
        );
        assert_eq!(out, [0xA5; LAYOUT_WORDS]);
    }

    #[test]
    fn descriptor_rejects_bad_versions_sizes_ranges_and_callbacks() {
        let original = descriptor();
        let modifications: [fn(&mut ProbeGuestDescriptor); 13] = [
            |d| d.abi_version += 1,
            |d| d.struct_size -= 1,
            |d| d.context_size -= 1,
            |d| d.context_alignment /= 2,
            |d| d.call = None,
            |d| d.init_context = None,
            |d| d.function_count = usize::MAX,
            |d| d.code_base += 1,
            |d| d.code_size = 0,
            |d| d.image_base = u32::MAX - 1,
            |d| d.image_size = 0,
            |d| d.code_size = u32::MAX,
            |d| d.image_base = 0,
        ];
        for modify in modifications {
            let mut invalid = original;
            modify(&mut invalid);
            assert!(matches!(unsafe { checked_descriptor(&invalid) }, Err(-5)));
        }
        assert!(unsafe { checked_descriptor(&original) }.is_ok());
    }

    #[test]
    fn descriptor_rejects_null_unaligned_unsorted_duplicate_and_foreign_functions() {
        let mut d = descriptor();
        assert!(matches!(
            unsafe { checked_descriptor(ptr::null()) },
            Err(-1)
        ));
        let unaligned = unsafe {
            (&d as *const ProbeGuestDescriptor)
                .cast::<u8>()
                .add(1)
                .cast()
        };
        assert!(matches!(unsafe { checked_descriptor(unaligned) }, Err(-4)));
        d.functions = ptr::null();
        assert!(matches!(unsafe { checked_descriptor(&d) }, Err(-1)));
        d.functions = unsafe { SYNTHETIC_FUNCTIONS.as_ptr().cast::<u8>().add(1).cast() };
        assert!(matches!(unsafe { checked_descriptor(&d) }, Err(-4)));
        for invalid in [
            [0x8200_1004, 0x8200_1000],
            [0x8200_1000; 2],
            [0x8200_1000, 0x8200_1008],
        ] {
            d.functions = invalid.as_ptr();
            assert!(matches!(unsafe { checked_descriptor(&d) }, Err(-5)));
        }
    }

    #[test]
    fn memory_probe_exercises_guest_pages_endian_aliases_and_dirty_tracking() {
        let mut result = ProbeRuntimeResult::default();
        assert_eq!(unsafe { probe_runtime_memory_selftest(&mut result, 32) }, 0);
        assert_eq!((result.abi_version, result.struct_size), (1, 32));
        assert_eq!((result.passed, result.failed), (0x3FF, 0));
    }

    #[test]
    fn guest_module_dispatch_returns_through_a_nested_rust_callback() {
        let mut result = ProbeRuntimeResult::default();
        assert_eq!(
            unsafe { probe_runtime_cpu_roundtrip(&descriptor(), &mut result, 32) },
            0
        );
        assert_eq!((result.passed, result.failed), (0x3F_0000, 0));
        assert_eq!(result.callback_result, 0x1020_3040);
        assert_eq!(NESTED_CALLS.with(Cell::get), None);
    }

    #[test]
    fn no_op_cpu_callback_reports_failed_checks_instead_of_success() {
        unsafe extern "C-unwind" fn no_op(_: u32, _: *mut PPCContext, _: *mut u8) {}
        let mut d = descriptor();
        d.call = Some(no_op);
        let mut result = ProbeRuntimeResult::default();
        assert_eq!(
            unsafe { probe_runtime_cpu_roundtrip(&d, &mut result, 32) },
            -7
        );
        assert_eq!(result.passed, 0x7_0000);
        assert_eq!(result.failed, 0x38_0000);
    }

    #[test]
    fn no_op_context_constructor_cannot_pass_the_init_check() {
        unsafe extern "C" fn no_op(_: *mut PPCContext) {}
        let mut d = descriptor();
        d.init_context = Some(no_op);
        let mut result = ProbeRuntimeResult::default();
        assert_eq!(
            unsafe { probe_runtime_cpu_roundtrip(&d, &mut result, 32) },
            -7
        );
        assert_eq!(result.failed, 1 << 17);
    }

    #[test]
    fn result_contract_rejects_invalid_outputs_without_running_callback() {
        let mut result = ProbeRuntimeResult {
            abi_version: 0xA5,
            ..ProbeRuntimeResult::default()
        };
        let mut invalid = descriptor();
        invalid.abi_version += 1;
        assert_eq!(
            unsafe { probe_runtime_cpu_roundtrip(&invalid, &mut result, 32) },
            -5
        );
        assert_eq!(result.abi_version, 0xA5);
        assert_eq!(
            unsafe { probe_runtime_cpu_roundtrip(ptr::null(), &mut result, 31) },
            -2
        );
        assert_eq!(
            unsafe { probe_runtime_memory_selftest(ptr::null_mut(), 32) },
            -1
        );
        let unaligned = unsafe {
            (&mut result as *mut ProbeRuntimeResult)
                .cast::<u8>()
                .add(1)
                .cast()
        };
        assert_eq!(unsafe { probe_runtime_memory_selftest(unaligned, 32) }, -4);
        assert_eq!(result.abi_version, 0xA5);
    }

    #[test]
    fn public_components_pass_narrow_synthetic_checks() {
        assert_eq!(probe_runtime_component_checks(), 31);
    }

    #[test]
    fn dedicated_exit_catches_after_a_callback_cleanup() {
        struct Guard(*mut u32);
        impl Drop for Guard {
            fn drop(&mut self) {
                unsafe {
                    *self.0 += 1;
                }
            }
        }
        unsafe extern "C-unwind" fn call(counter: *mut u32) {
            let _guard = Guard(counter);
            probe_runtime_raise_synthetic_exit();
        }
        let mut counter = 0;
        assert_eq!(
            unsafe { probe_runtime_catch_cpp_unwind(Some(call), &mut counter) },
            0
        );
        assert_eq!(counter, 1);
    }

    #[test]
    fn dedicated_exit_rejects_a_normal_return_and_an_unrelated_panic() {
        unsafe extern "C-unwind" fn normal(_: *mut u32) {}
        unsafe extern "C-unwind" fn unrelated(_: *mut u32) {
            std::panic::panic_any("not SyntheticExit");
        }
        for call in [normal as unsafe extern "C-unwind" fn(*mut u32), unrelated] {
            let mut counter = 0;
            assert_eq!(
                unsafe { probe_runtime_catch_cpp_unwind(Some(call), &mut counter) },
                -8
            );
        }
    }

    #[test]
    fn dedicated_exit_rejects_invalid_counter_and_missing_callback() {
        let mut counter = 0;
        assert_eq!(
            unsafe { probe_runtime_catch_cpp_unwind(None, &mut counter) },
            -5
        );
        assert_eq!(
            unsafe { probe_runtime_catch_cpp_unwind(None, ptr::null_mut()) },
            -1
        );
        let unaligned = unsafe { (&mut counter as *mut u32).cast::<u8>().add(1).cast() };
        assert_eq!(
            unsafe { probe_runtime_catch_cpp_unwind(None, unaligned) },
            -4
        );
    }
}
