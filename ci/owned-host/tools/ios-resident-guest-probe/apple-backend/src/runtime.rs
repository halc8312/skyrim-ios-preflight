//! Real core/Kernel path, exclusively for the three self-authored fixture
//! entries. This is not a general/private-game loader or a resettable runtime.
use crate::{
    Check,
    abi::*,
    darwin::{self, Api, Library},
    policy::validate_descriptor,
    synthetic,
};
use kernel_hle::{
    Kernel,
    dispatch::StubConfig,
    import_policy::{ImportPolicy, preflight_imports},
};
use runtime_core::{GuestModule, PPCContext, Runtime};
use std::{
    ffi::c_void,
    mem::{align_of, size_of},
    sync::{
        OnceLock,
        atomic::{AtomicBool, AtomicPtr, Ordering},
    },
    time::Duration,
};

static RESIDENT: OnceLock<Resident> = OnceLock::new();
static BASE: AtomicPtr<u8> = AtomicPtr::new(std::ptr::null_mut());
static BUSY_SEEN: AtomicBool = AtomicBool::new(false);
static STARTED: AtomicBool = AtomicBool::new(false);
struct Resident {
    owner: Library,
    api: Api,
    config: [u32; 4],
    functions: Box<[u32]>,
    requirements: PGRequirementsV1,
}
impl Resident {
    fn load(hash: &str) -> Check<Self> {
        let mut layout = [0u64; 49];
        if unsafe { darwin::RAAppleHostLayoutV1(layout.as_mut_ptr(), layout.len()) } != 49
            || layout != layout49()
        {
            return Err("actual SDK public-C-header layout differs from Rust".into());
        }
        let mut owner = Library::open(hash)?;
        let api = owner.api()?;
        let mut r = PGRequirementsV1::default();
        if unsafe { (api.requirements)(&mut r, size_of::<PGRequirementsV1>() as u32 - 1) } != -1
            || unsafe { (api.requirements)(&mut r, size_of::<PGRequirementsV1>() as u32) } != OK
        {
            return Err("fixture requirements query contract failed".into());
        }
        if r.abi_version != VERSION
            || r.struct_size as usize != size_of::<PGRequirementsV1>()
            || r.pointer_bits != 64
            || r.little_endian != 1
            || r.plain_arm64 != 1
            || r.reserved != 0
            || r.context_layout != context_layout()
            || r.ea_offset_count != 4096
            || r.written_count != 1048576
        {
            return Err("fixture requirements differ from actual Rust context/tables".into());
        }
        let mut config = [0u64; 4];
        unsafe { (api.config)(config.as_mut_ptr()) };
        let mut count = 0;
        let table = unsafe { (api.functions)(&mut count) };
        let functions = owner.readonly_table(table, count)?;
        let config = validate_descriptor(config, &functions)?;
        owner.recheck()?;
        Ok(Self {
            owner,
            api,
            config,
            functions,
            requirements: r,
        })
    }
    fn register(self) -> Check<&'static Self> {
        RESIDENT
            .set(self)
            .map_err(|_| "resident already registered")?;
        Ok(RESIDENT.get().unwrap())
    }
    fn module(&'static self) -> GuestModule {
        GuestModule {
            image_base: self.config[0],
            image_size: self.config[1],
            code_base: self.config[2],
            code_size: self.config[3],
            functions: &self.functions,
            call: call_guest,
            init_context,
        }
    }
    fn memory(kernel: &Kernel) -> PGMemoryV1 {
        PGMemoryV1 {
            ea_offsets: runtime_core::memory::skyrt_ea_offset.as_ptr().cast(),
            ea_offset_count: 4096,
            written: runtime_core::dirty::skyrt_written
                .as_ptr()
                .cast_mut()
                .cast(),
            written_count: 1048576,
            guest_base: kernel.rt.base(),
            guest_bytes: runtime_core::memory::linear_map::SIZE,
        }
    }
    fn bind(&'static self, kernel: &'static Kernel) -> Check<()> {
        if Kernel::try_global().is_none_or(|k| !std::ptr::eq(k, kernel))
            || !BASE.load(Ordering::Acquire).is_null()
        {
            return Err("global Kernel binding state rejected".into());
        }
        let mut host = PGHostCallbacksV1 {
            abi_version: VERSION,
            struct_size: size_of::<PGHostCallbacksV1>() as u32,
            contracts: CONTRACTS,
            context_layout: context_layout(),
            user: (kernel as *const Kernel).cast_mut().cast(),
            memory: Self::memory(kernel),
            timebase: Some(timebase),
            kernel_call: Some(kernel_call),
            fatal: Some(fatal),
            ..Default::default()
        };
        if unsafe {
            darwin::RAApplePrepareHostV1(
                &mut host,
                size_of::<PGHostCallbacksV1>() as u32,
                self.owner.header(),
            )
        } != 0
            || host.jump_abi != APPLE_JUMP
            || host.unwind_abi != APPLE_UNWIND
            || host.jump_buffer_size != self.requirements.jump_buffer_size
            || host.jump_buffer_alignment != self.requirements.jump_buffer_alignment
            || host.jump_buffer_size == 0
            || !host.jump_buffer_alignment.is_power_of_two()
        {
            return Err("native SDK jump/unwind/layout contract rejected".into());
        }
        if unsafe { (self.api.bind)(&host, size_of::<PGHostCallbacksV1>() as u32) } != OK {
            return Err("strict fixture host binding failed".into());
        }
        BASE.store(kernel.rt.base(), Ordering::Release);
        if unsafe { (self.api.bind)(&host, size_of::<PGHostCallbacksV1>() as u32) } != -6 {
            return Err("second binding was not refused".into());
        }
        Ok(())
    }
}
fn resident() -> &'static Resident {
    RESIDENT.get().unwrap_or_else(|| std::process::abort())
}
unsafe extern "C" fn init_context(context: *mut PPCContext) {
    if BASE.load(Ordering::Acquire).is_null()
        || unsafe { (resident().api.init)(context.cast(), size_of::<PPCContext>() as u64) } != OK
    {
        std::process::abort()
    }
}
unsafe extern "C-unwind" fn call_guest(address: u32, context: *mut PPCContext, base: *mut u8) {
    if base.is_null()
        || base != BASE.load(Ordering::Acquire)
        || unsafe { (resident().api.call)(address, context.cast(), size_of::<PPCContext>() as u64) }
            != OK
    {
        std::process::abort()
    }
}
unsafe extern "C" fn timebase(_: *mut c_void) -> u64 {
    runtime_core::timebase::timebase()
}
unsafe extern "C" fn fatal(_: *mut c_void, _: u32, _: u64) {
    std::process::abort()
}
unsafe extern "C-unwind" fn kernel_call(
    user: *mut c_void,
    module: u32,
    ordinal: u32,
    context: *mut c_void,
    base: *mut u8,
) -> i32 {
    let kernel = Kernel::global();
    if user != (kernel as *const Kernel).cast_mut().cast()
        || base != kernel.rt.base()
        || module != 0
        || !matches!(ordinal, 0x83 | 0x19)
        || context.is_null()
        || context as usize % align_of::<PPCContext>() != 0
    {
        std::process::abort()
    }
    if ordinal == 0x83 {
        if unsafe {
            (resident().api.publish)(&Resident::memory(kernel), size_of::<PGMemoryV1>() as u32)
        } != BUSY
        {
            std::process::abort()
        }
        BUSY_SEEN.store(true, Ordering::Release);
    }
    unsafe { kernel_hle::dispatch::skyrt_kernel_call(module, ordinal, context.cast(), base) };
    1
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct RAAppleResultV1 {
    pub version: u32,
    pub bytes: u32,
    pub passed: u32,
    pub failure: u32,
    pub checked_flags: u64,
    pub frequency: u64,
    pub cleanup_count: u64,
    pub dispatched_imports: u64,
    pub exit_code: u64,
    pub function_count: u64,
}
fn run(hash: &str) -> Check<RAAppleResultV1> {
    if Kernel::try_global().is_some() {
        return Err("process already owns a Kernel".into());
    }
    let guest = Resident::load(hash)?.register()?;
    let module = guest.module();
    let xex = synthetic::xex();
    let preflight = preflight_imports(&xex, &[])?;
    preflight.validate(ImportPolicy::StrictAll)?;
    if preflight.function_imports != 2
        || preflight.registered_functions != 2
        || preflight.variable_imports != 0
        || xex.image_base() != module.image_base
        || xex.image_size() != module.image_size
        || !xex
            .entry_point()
            .is_some_and(|entry| module.has_function(entry))
    {
        return Err("self-authored image/import identity mismatch".into());
    }
    // A new UUID-like nonce directory is allocated by create_dir, never reused.
    let root = std::env::temp_dir().join(format!(
        "resident-apple-public-{}-{}",
        std::process::id(),
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map_err(|_| "clock unavailable")?
            .as_nanos()
    ));
    std::fs::create_dir(&root).map_err(|_| "fresh public fixture root unavailable")?;
    for name in ["empty-game", "content"] {
        std::fs::create_dir(root.join(name))
            .map_err(|_| "fresh public fixture directory unavailable")?;
    }
    let rt = Runtime::load(module, xex).map_err(|_| "synthetic Runtime load failed")?;
    let kernel = Kernel::new_with_import_policy(
        rt,
        vfs::Vfs::with_game_root(root.join("empty-game")),
        StubConfig::default(),
        &[],
        ImportPolicy::StrictAll,
    )?;
    kernel.content.set_root(root.join("content"));
    kernel.install_global();
    guest.bind(kernel)?;
    // Native SDK setjmp/longjmp stays wholly within this C++ fixture's still-live
    // caller. It does not jump through Rust/Swift or replace ThreadExit cleanup.
    let mut context = PPCContext::new_zeroed();
    unsafe { init_context(&mut *context) };
    kernel
        .rt
        .call(&mut context, FUNCTIONS[1])
        .map_err(|_| "native jump fixture call refused")?;
    if context.gpr(3) != 0x1111 {
        return Err("actual native jump return mismatch".into());
    }
    kernel
        .rt
        .call(&mut context, FUNCTIONS[2])
        .map_err(|_| "normal fixture return refused")?;
    if context.gpr(3) != 0x2222 {
        return Err("normal fixture return mismatch".into());
    }
    let page = synthetic::FREQUENCY as usize >> 12;
    runtime_core::dirty::skyrt_written[page].store(1, Ordering::Release);
    kernel
        .start_main_thread()
        .map_err(|_| "synthetic main thread failed")?;
    let code = kernel
        .wait_for_exit_timeout(Duration::from_secs(3))
        .ok_or("synthetic main exceeded three seconds")?;
    let stats = kernel.import_stats();
    let frequency = kernel.mem().read_u32(synthetic::FREQUENCY);
    let cleanup = kernel.mem().read_u32(synthetic::CLEANUP);
    if code != 0
        || frequency != 50_000_000
        || cleanup != 1
        || stats.len() != 2
        || !BUSY_SEEN.load(Ordering::Acquire)
        || runtime_core::dirty::skyrt_written[page].load(Ordering::Acquire) != 0
    {
        return Err("ThreadExit/RAII/import/dirty outcome mismatch".into());
    }
    for ordinal in [0x83, 0x19] {
        if !stats
            .iter()
            .any(|(m, o, _, s)| m.0 == 0 && *o == ordinal && s.calls == 1 && s.implemented)
        {
            return Err("registered imports were not each dispatched exactly once".into());
        }
    }
    if unsafe { (guest.api.publish)(&Resident::memory(kernel), size_of::<PGMemoryV1>() as u32) }
        != OK
    {
        return Err("owned C++ active-call RAII did not unwind".into());
    }
    Ok(RAAppleResultV1 {
        version: VERSION,
        bytes: size_of::<RAAppleResultV1>() as u32,
        passed: 1,
        failure: 0,
        checked_flags: 0x3fff,
        frequency: frequency.into(),
        cleanup_count: cleanup.into(),
        dispatched_imports: stats.len() as u64,
        exit_code: code.into(),
        function_count: module.functions.len() as u64,
    })
}

/// Caller supplies a separately trusted post-signing full SHA, never one read
/// from the candidate image. All pointer ranges must be valid, exclusive and
/// disjoint for the duration of this synchronous call. No unwind enters Swift.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn RAAppleRunPublicProbeV1(
    hash: *const u8,
    hash_bytes: usize,
    out: *mut RAAppleResultV1,
    out_bytes: usize,
) -> i32 {
    if hash.is_null()
        || hash_bytes != 64
        || out.is_null()
        || out as usize % align_of::<RAAppleResultV1>() != 0
        || out_bytes != size_of::<RAAppleResultV1>()
    {
        return -1;
    }
    let initial = RAAppleResultV1 {
        version: VERSION,
        bytes: size_of::<RAAppleResultV1>() as u32,
        ..Default::default()
    };
    unsafe { out.write(initial) };
    if STARTED.swap(true, Ordering::AcqRel) {
        return -2;
    }
    let result = std::panic::catch_unwind(|| {
        let hash = std::str::from_utf8(unsafe { std::slice::from_raw_parts(hash, hash_bytes) })
            .map_err(|_| "trusted hash is not ASCII")?;
        run(hash)
    });
    match result {
        Ok(Ok(result)) => {
            unsafe { out.write(result) };
            0
        }
        _ => {
            unsafe { (*out).failure = 1 };
            -3
        }
    }
}
