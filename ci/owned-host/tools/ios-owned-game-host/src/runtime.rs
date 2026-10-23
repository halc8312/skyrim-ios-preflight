//! iOS-only integration. No guest-sys/static private image dependency. All
//! expectations come from a separately packaged binding; all lifetime owners
//! survive to process exit. The UI never hosts a guest/unwind stack frame.
use crate::{
    Check,
    audio_output,
    abi::*,
    binding::{self, TrustedBinding},
    darwin::{self, Api, Library},
    guest_fatal::{self, GuestFatal},
    lifecycle::{self, Lifecycle, Status},
    paths::{self, PinnedFile},
    progress_diagnostics,
    save_store,
};
use sha2::Digest;
use kernel_hle::{
    Kernel,
    dispatch::StubConfig,
    import_policy::{self, ImportPolicy, UnsupportedImport},
};
use runtime_core::{GuestModule, PPCContext, Runtime};
use std::{
    ffi::c_void,
    mem::{align_of, size_of},
    path::{Path, PathBuf},
    sync::{
        OnceLock,
        atomic::{AtomicBool, AtomicPtr, AtomicU32, Ordering},
    },
    time::{Duration, Instant},
};

static STATE: Lifecycle = Lifecycle::new();
static RESIDENT: OnceLock<Resident> = OnceLock::new();
static KERNEL: OnceLock<&'static Kernel> = OnceLock::new();
static CONTENT: OnceLock<PathBuf> = OnceLock::new();
// Retains the cooperative cross-process store lock until process termination.
static SAVE_SESSION: OnceLock<save_store::SaveSession> = OnceLock::new();
static STARTED: OnceLock<Instant> = OnceLock::new();
static BASE: AtomicPtr<u8> = AtomicPtr::new(std::ptr::null_mut());
static FAILURE: AtomicU32 = AtomicU32::new(0);
static MISSING_MODULE: AtomicU32 = AtomicU32::new(0);
static MISSING_ORDINAL: AtomicU32 = AtomicU32::new(0);
static EXIT_CODE: AtomicU32 = AtomicU32::new(0);
static POLICY: AtomicU32 = AtomicU32::new(0);
static PREPARE_STAGE: AtomicU32 = AtomicU32::new(0);
static IMPORTS: OnceLock<import_policy::ImportPreflight> = OnceLock::new();
static FUNCTION_COUNT: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
static STOPPING: AtomicBool = AtomicBool::new(false);
// Written only by the STOPPING first winner, before its first-failure snapshot.
static GUEST_FATAL: OnceLock<GuestFatal> = OnceLock::new();
const TOKEN: u64 = 1;
struct Resident {
    owner: Library,
    api: Api,
    binding: TrustedBinding,
    functions: Box<[u32]>,
    requirements: PGRequirementsV1,
    // Retain the exact metadata and XEX file descriptors. Bytes may be released
    // after the same owned XEX buffer has been hashed and parsed.
    _metadata: PinnedFile,
    _xex: PinnedFile,
}
fn context_layout() -> [u64; 20] {
    use runtime_core::context::offsets as o;
    [
        o::R3,
        o::R0,
        o::R1,
        o::R2,
        o::R4,
        o::R13,
        o::R31,
        o::LR,
        o::CTR,
        o::XER,
        o::RESERVED,
        o::MSR,
        o::CR0,
        o::FPSCR,
        o::F0,
        o::F1,
        o::V0,
        o::V127,
        o::SIZE,
        o::ALIGN,
    ]
    .map(|n| n as u64)
}
fn policy(n: u32) -> Check<ImportPolicy> {
    match n {
        0 => Ok(ImportPolicy::StrictAll),
        1 => Ok(ImportPolicy::CpuDiagnosticTrap),
        _ => Err("unknown import policy"),
    }
}
fn token(t: u64) -> Check<()> {
    if t == TOKEN && KERNEL.get().is_some() {
        Ok(())
    } else {
        Err("invalid resident token")
    }
}
fn resident() -> &'static Resident {
    RESIDENT.get().unwrap_or_else(|| unsafe { libc::_exit(73) })
}
fn memory(k: &Kernel) -> PGMemoryV1 {
    PGMemoryV1 {
        ea_offsets: runtime_core::memory::skyrt_ea_offset.as_ptr().cast(),
        ea_offset_count: 4096,
        written: runtime_core::dirty::skyrt_written
            .as_ptr()
            .cast_mut()
            .cast(),
        written_count: 1048576,
        guest_base: k.rt.base(),
        guest_bytes: runtime_core::memory::linear_map::SIZE,
    }
}
impl Resident {
    fn module(&'static self) -> GuestModule {
        let c = self.binding.ppc_config;
        GuestModule {
            image_base: c[0],
            image_size: c[1],
            code_base: c[2],
            code_size: c[3],
            functions: &self.functions,
            call: call_guest,
            init_context,
        }
    }
    fn bind(&self, k: &'static Kernel) -> Check<()> {
        let mut h = PGHostCallbacksV1 {
            abi_version: VERSION,
            struct_size: size_of::<PGHostCallbacksV1>() as u32,
            contracts: CONTRACTS,
            context_layout: context_layout(),
            user: (k as *const Kernel).cast_mut().cast(),
            memory: memory(k),
            timebase: Some(timebase),
            kernel_call: Some(kernel_call),
            fatal: Some(fatal),
            ..Default::default()
        };
        if unsafe {
            darwin::OHNativePrepareHostV1(
                &mut h,
                size_of::<PGHostCallbacksV1>() as u32,
                self.owner.header(),
            )
        } != 0
            || h.jump_abi != APPLE_JUMP
            || h.unwind_abi != APPLE_UNWIND
            || h.jump_pair_cookie == 0
            || h.unwind_pair_cookie == 0
            || h.jump_buffer_size != self.requirements.jump_buffer_size
            || h.jump_buffer_alignment != self.requirements.jump_buffer_alignment
        {
            return Err("SDK native jump/unwind requirements mismatch");
        }
        if unsafe { (self.api.bind)(&h, size_of::<PGHostCallbacksV1>() as u32) } != OK {
            return Err("owned guest refused host binding");
        }
        // The same live tables are republished while no guest call is active.
        // This verifies the seventh typed API's quiescent contract, without
        // relocating or copying either atomic array.
        if unsafe { (self.api.publish)(&memory(k), size_of::<PGMemoryV1>() as u32) } != OK {
            return Err("quiescent live memory publication refused");
        }
        BASE.store(k.rt.base(), Ordering::Release);
        Ok(())
    }
}
fn prepare(game: &Path, content: &Path, p: u32) -> Check<u64> {
    let import_policy = policy(p)?;
    POLICY.store(p, Ordering::Release);
    PREPARE_STAGE.store(1, Ordering::Release);
    if Kernel::try_global().is_some() {
        return Err("process already contains a Kernel");
    }
    let bundle = darwin::sdk_string(|out, cap| unsafe { darwin::OHNativeBundlePathV1(out, cap) })?;
    let sandbox =
        darwin::sdk_string(|out, cap| unsafe { darwin::OHNativeSandboxPathV1(out, cap) })?;
    let game = paths::sandbox_game(&sandbox, game)?;
    let store_root = paths::checked_child(&sandbox, Path::new("Documents/OwnedSaves"))?;
    let content = paths::fresh_content_excluding(&sandbox, &game, content, &store_root)?;
    CONTENT
        .set(content.clone())
        .map_err(|_| "content already registered")?;
    PREPARE_STAGE.store(2, Ordering::Release);
    let mut metadata = PinnedFile::open(
        &bundle,
        Path::new(paths::BINDING_RELATIVE),
        binding::MAX_BINDING_BYTES as u64,
    )?;
    let binding = binding::parse(&metadata.bytes)?;
    FUNCTION_COUNT.store(binding.function_count as u64, Ordering::Release);
    PREPARE_STAGE.store(3, Ordering::Release);
    let mut xex_file = PinnedFile::open(
        &game,
        Path::new(&binding.xex_relative_path),
        ios_signed_image_policy::MAX_IMAGE as u64,
    )?;
    binding.verify_xex_bytes(&xex_file.bytes)?;
    crate::xex_envelope::validate(&xex_file.bytes, binding.ppc_config[1])?;
    // Move the exact hashed owned buffer. Never re-open its path for parsing.
    let xex = xex::Xex::parse_owned(std::mem::take(&mut xex_file.bytes))
        .map_err(|_| "bound plaintext XEX parse refused")?;
    let identity = xex
        .execution_info()
        .ok_or("bound XEX execution identity unavailable")?;
    binding.verify_xex_identity(
        identity.title_id,
        identity.media_id,
        identity.version,
        identity.base_version,
    )?;
    if binding.ppc_config[0] != xex.image_base() || binding.ppc_config[1] != xex.image_size() {
        return Err("XEX/module image mismatch");
    }
    PREPARE_STAGE.store(4, Ordering::Release);
    let preflight =
        import_policy::preflight_imports(&xex, &[]).map_err(|_| "import preflight failed")?;
    IMPORTS
        .set(preflight)
        .map_err(|_| "preflight already registered")?;
    IMPORTS
        .get()
        .unwrap()
        .validate(import_policy)
        .map_err(|_| "strict import preflight refused missing handlers/variables")?;
    if import_policy == ImportPolicy::CpuDiagnosticTrap {
        import_policy::install_diagnostic_failure_handler(missing_import)?;
    }
    PREPARE_STAGE.store(5, Ordering::Release);
    let mut sdk_layout = [0u64; 49];
    if unsafe { darwin::OHNativeHostLayoutV1(sdk_layout.as_mut_ptr(), sdk_layout.len()) } != 49
        || sdk_layout != layout49()
    {
        return Err("SDK/Rust C ABI layout differs");
    }
    PREPARE_STAGE.store(6, Ordering::Release);
    let mut owner = Library::open(&binding)?;
    let api = owner.api()?;
    PREPARE_STAGE.store(7, Ordering::Release);
    let mut r = PGRequirementsV1::default();
    if unsafe { (api.requirements)(&mut r, size_of::<PGRequirementsV1>() as u32) } != OK
        || r.abi_version != VERSION
        || r.struct_size as usize != size_of::<PGRequirementsV1>()
        || r.pointer_bits != 64
        || r.little_endian != 1
        || r.plain_arm64 != 1
        || r.reserved != 0
        || r.context_layout != context_layout()
        || r.ea_offset_count != 4096
        || r.written_count != 1048576
        || r.jump_buffer_size == 0
        || !r.jump_buffer_alignment.is_power_of_two()
    {
        return Err("guest requirements differ from actual runtime");
    }
    let mut config = [0u64; 4];
    unsafe { (api.config)(config.as_mut_ptr()) };
    let mut c = [0u32; 4];
    for (to, from) in c.iter_mut().zip(config) {
        *to = u32::try_from(from).map_err(|_| "module config exceeds PPC32")?;
    }
    let mut count = 0;
    let table = unsafe { (api.functions)(&mut count) };
    let functions = owner.readonly_table(table, count, binding.function_count)?;
    binding.verify_module(c, &functions, r.context_layout, r.abi_version)?;
    let entry = xex.entry_point().ok_or("XEX entry absent")?;
    if functions.binary_search(&entry).is_err() {
        return Err("XEX entry not in exact function table");
    }
    owner.recheck()?;
    metadata.recheck()?;
    xex_file.recheck()?;
    owner.release_bytes();
    metadata.release_bytes();
    RESIDENT
        .set(Resident {
            owner,
            api,
            binding,
            functions,
            requirements: r,
            _metadata: metadata,
            _xex: xex_file,
        })
        .map_err(|_| "resident already registered")?;
    PREPARE_STAGE.store(8, Ordering::Release);
    let rt = Runtime::load(resident().module(), xex).map_err(|_| "bound Runtime load failed")?;
    PREPARE_STAGE.store(9, Ordering::Release);
    let k = Kernel::new_with_import_policy(
        rt,
        vfs::Vfs::with_game_root(&game),
        StubConfig::default(),
        &[],
        import_policy,
    )
    .map_err(|_| "strict Kernel preparation failed")?;
    audio_output::initialize()?;
    if !k.audio.set_observer(std::sync::Arc::new(|event| match event {
        kernel_hle::audio::AudioEvent::Activated { client, generation } => audio_output::activate_client(client, generation),
        kernel_hle::audio::AudioEvent::Frame { client, generation, samples } => audio_output::submit_frame(client, generation, samples),
        kernel_hle::audio::AudioEvent::Deactivated { client, generation } => audio_output::deactivate_client(client, generation),
    })) { return Err("audio observer must be installed before guest clients"); }
    let expected = &resident().binding;
    let save_identity = save_store::SaveIdentity {
        title_id: expected.xex_title_id,
        media_id: expected.xex_media_id,
        version: expected.xex_version,
        base_version: expected.xex_base_version,
        xex_sha256: expected.xex_sha256.clone(),
    };
    // Fresh diagnostic paths provide session uniqueness, not authentication.
    // A collision is refused by the store's exclusive session creation.
    let digest = sha2::Sha256::digest(content.as_os_str().as_encoded_bytes());
    let session_id: String = digest[..16].iter().map(|v| format!("{v:02x}")).collect();
    let save_session = save_store::prepare_session(&store_root, &game, &save_identity, &session_id)?;
    k.content.set_root(save_session.content.clone());
    SAVE_SESSION.set(save_session).map_err(|_| "save session already retained")?;
    k.install_global();
    PREPARE_STAGE.store(10, Ordering::Release);
    resident().bind(k)?;
    k.input.connect(0);
    KERNEL.set(k).map_err(|_| "Kernel already registered")?;
    POLICY.store(p, Ordering::Release);
    STATE.prepared()?;
    Ok(TOKEN)
}

/// Renderer integration only. The trusted Rust owner retains its handle and
/// CAMetalLayer strongly for the entire process; Swift receives only a token.
pub fn prepared_kernel(t: u64) -> Check<&'static Kernel> {
    token(t)?;
    if !matches!(
        STATE.state(),
        lifecycle::PREPARED | lifecycle::RENDERER_ATTACHED | lifecycle::RUNNING | lifecycle::EXITED
    ) {
        return Err("Kernel not prepared");
    }
    Ok(*KERNEL.get().ok_or("Kernel missing")?)
}
pub(crate) fn renderer_configuration_allowed(t: u64) -> Check<()> {
    token(t)?;
    if STATE.state() != lifecycle::PREPARED {
        return Err("startup graphics require prepared state before renderer attachment");
    }
    Ok(())
}
pub fn mark_renderer_attached(t: u64) -> Check<()> {
    token(t)?;
    STATE.renderer_attached()
}

unsafe extern "C" fn init_context(ctx: *mut PPCContext) {
    if BASE.load(Ordering::Acquire).is_null()
        || unsafe { (resident().api.init)(ctx.cast(), size_of::<PPCContext>() as u64) } != OK
    {
        fatal_stop(20, 0, 0);
    }
}
unsafe extern "C-unwind" fn call_guest(address: u32, ctx: *mut PPCContext, base: *mut u8) {
    if base.is_null()
        || base != BASE.load(Ordering::Acquire)
        || ctx.is_null()
        || ctx as usize % align_of::<PPCContext>() != 0
        || unsafe { (resident().api.call)(address, ctx.cast(), size_of::<PPCContext>() as u64) }
            != OK
    {
        fatal_stop(21, 0, 0);
    }
}
unsafe extern "C" fn timebase(_: *mut c_void) -> u64 {
    runtime_core::timebase::timebase()
}
unsafe extern "C" fn fatal(_: *mut c_void, category: u32, detail: u64) {
    let r = resident();
    let evidence = GuestFatal::from_callback(
        category,
        detail,
        r.binding.ppc_config[2],
        r.binding.ppc_config[3],
        &r.functions,
    );
    // A guest fatal category is not a kernel module/ordinal. Preserve its own
    // typed evidence without reinterpreting arbitrary detail as an address.
    fatal_stop_with_guest(22, 0, 0, Some(evidence));
}
unsafe extern "C-unwind" fn kernel_call(
    user: *mut c_void,
    module: u32,
    ordinal: u32,
    ctx: *mut c_void,
    base: *mut u8,
) -> i32 {
    let k = Kernel::global();
    if user != (k as *const Kernel).cast_mut().cast()
        || base != k.rt.base()
        || module > u16::MAX as u32
        || ordinal > u16::MAX as u32
        || ctx.is_null()
        || ctx as usize % align_of::<PPCContext>() != 0
    {
        fatal_stop(23, 0, 0);
    }
    unsafe { kernel_hle::dispatch::skyrt_kernel_call(module, ordinal, ctx.cast(), base) };
    1
}
fn missing_import(f: UnsupportedImport) -> ! {
    fatal_stop(24, f.module.0.into(), f.ordinal.into())
}
fn numeric_status() -> Status {
    let mut s = Status {
        version: VERSION,
        bytes: size_of::<Status>() as u32,
        state: STATE.state(),
        policy: POLICY.load(Ordering::Acquire),
        failure: FAILURE.load(Ordering::Acquire),
        missing_module: MISSING_MODULE.load(Ordering::Acquire),
        missing_ordinal: MISSING_ORDINAL.load(Ordering::Acquire),
        exit_code: EXIT_CODE.load(Ordering::Acquire),
        elapsed_ms: STARTED
            .get()
            .map_or(0, |v| v.elapsed().as_millis().min(u64::MAX as u128) as u64),
        ..Default::default()
    };
    s.function_count = FUNCTION_COUNT.load(Ordering::Acquire);
    if let Some(p) = IMPORTS.get() {
        s.function_imports = p.function_imports as u64;
        s.registered_functions = p.registered_functions as u64;
        s.variable_imports = p.variable_imports as u64;
        s.supported_variables = p.supported_variables as u64;
    }
    s
}
fn diagnostic_value() -> serde_json::Value {
    let s = numeric_status();
    // A final-record worker can overlap a fatal callback. Do not attach a later
    // callback to a status snapshot that predates failure 22.
    let g = if s.failure == 22 {
        GUEST_FATAL.get().copied().unwrap_or_default()
    } else {
        GuestFatal::default()
    };
    serde_json::json!({"version":s.version,"state":s.state,"failure":s.failure,"module":s.missing_module,"ordinal":s.missing_ordinal,
        "exit_code":s.exit_code,"elapsed_ms":s.elapsed_ms,"diagnostic_schema":guest_fatal::DIAGNOSTIC_SCHEMA,
        "guest_fatal_category":g.category,"guest_target":g.target,"guest_target_flags":g.flags,
        "ready_to_play":false,"signature_authenticated":false,"iphone_game_verified":false})
}
fn snapshot(name: &str) -> Check<()> {
    let content = CONTENT.get().ok_or("diagnostic content absent")?;
    let data = diagnostic_value();
    paths::publish_json_new(
        content,
        name,
        &serde_json::to_vec(&data).map_err(|_| "diagnostic serialization failed")?,
    )
}
fn progress_snapshot(t: u64, point: progress_diagnostics::Snapshot) -> Check<()> {
    let content = CONTENT.get().ok_or("diagnostic content absent")?;
    #[cfg(feature = "metal-renderer")]
    let renderer = {
        let mut status = crate::renderer_policy::OHRendererStatusV1::default();
        // SAFETY: one aligned local result with exactly the existing ABI size.
        let result = unsafe {
            crate::renderer::OHReadRendererStatusV1(
                t,
                &mut status,
                size_of::<crate::renderer_policy::OHRendererStatusV1>(),
            )
        };
        (result == 0).then_some(status)
    };
    #[cfg(not(feature = "metal-renderer"))]
    let renderer = {
        let _ = t;
        None
    };
    let mut data = diagnostic_value();
    point.append_numeric_fields(&mut data, renderer.as_ref())?;
    paths::publish_json_new(
        content,
        point.name,
        &serde_json::to_vec(&data).map_err(|_| "diagnostic serialization failed")?,
    )
}
fn fatal_stop(category: u32, module: u32, ordinal: u32) -> ! {
    fatal_stop_with_guest(category, module, ordinal, None)
}
fn fatal_stop_with_guest(category: u32, module: u32, ordinal: u32, guest: Option<GuestFatal>) -> ! {
    if !STOPPING.swap(true, Ordering::SeqCst) {
        if let Some(evidence) = guest {
            let _ = GUEST_FATAL.set(evidence);
        }
        MISSING_MODULE.store(module, Ordering::Release);
        MISSING_ORDINAL.store(ordinal, Ordering::Release);
        FAILURE.store(category, Ordering::Release);
        STATE.fail();
        // Evidence I/O cannot hold a failing process alive indefinitely.
        if std::thread::Builder::new()
            .name("owned-fatal-ceiling".into())
            .spawn(|| {
                std::thread::sleep(Duration::from_secs(1));
                unsafe { libc::_exit(70) }
            })
            .is_err()
        {
            unsafe { libc::_exit(70) }
        }
        let _ = snapshot("first-failure.json");
        if guest.is_some_and(|g| g.category == guest_fatal::MISSING_FUNCTION) {
            // Keep the schema2 failure evidence first. A stalled unwinder or
            // failed extra write cannot keep this process alive beyond 1 sec.
            if let (Some(r), Some(content)) = (RESIDENT.get(), CONTENT.get()) {
                let trace = r.owner.native_trace();
                let mut data = diagnostic_value();
                trace.append_numeric_fields(&mut data);
                if let Ok(bytes) = serde_json::to_vec(&data) {
                    let _ = paths::publish_json_new(content, "guest-native-trace.json", &bytes);
                }
            }
        }
        unsafe { libc::_exit(70) }
    }
    loop {
        std::thread::park_timeout(Duration::from_millis(10));
    }
}
fn start(t: u64, seconds: u32) -> Check<()> {
    token(t)?;
    lifecycle::validate_duration(seconds)?;
    let snapshots = progress_diagnostics::schedule(seconds)?;
    #[cfg(feature = "metal-renderer")]
    if !crate::renderer::is_ready(t) {
        return Err("Metal renderer not ready");
    }
    let now = Instant::now();
    STARTED.set(now).map_err(|_| "guest already started")?;
    std::thread::Builder::new()
        .name("owned-hard-deadline".into())
        .spawn(move || {
            std::thread::sleep(Duration::from_secs(seconds.into()).saturating_sub(now.elapsed()));
            // Kernel exit notification does not prove that every secondary guest
            // worker has stopped; the process ceiling applies even after main exit.
            unsafe {
                libc::_exit(if STATE.state() == lifecycle::EXITED {
                    0
                } else {
                    72
                })
            }
        })
        .map_err(|_| "hard deadline worker unavailable")?;
    // At most four independent writers. A stalled earlier checkpoint cannot
    // hold back the final writer or the unchanged hard process deadline.
    for point in snapshots {
        std::thread::Builder::new()
            .name(
                if point.name == progress_diagnostics::FINAL_NAME {
                    "owned-final-record"
                } else {
                    "owned-checkpoint"
                }
                .into(),
            )
            .spawn(move || {
                std::thread::sleep(
                    Duration::from_secs(u64::from(point.seconds)).saturating_sub(now.elapsed()),
                );
                let _ = progress_snapshot(t, point);
            })
            .map_err(|_| "diagnostic record worker unavailable")?;
    }
    let k = *KERNEL.get().ok_or("Kernel missing")?;
    // start_main_thread creates a native guest worker. No guest unwind can
    // cross this C API/Swift call; ThreadExit is caught by Kernel run_thread.
    k.start_main_thread()
        .map_err(|_| "guest main worker failed")?;
    std::thread::Builder::new()
        .name("owned-exit-monitor".into())
        .spawn(move || {
            if let Some(code) = k.wait_for_exit_timeout(Duration::from_secs(seconds.into())) {
                EXIT_CODE.store(code, Ordering::Release);
                let _ = STATE.exited();
                let _ = snapshot("guest-exit.json");
            }
        })
        .map_err(|_| "guest exit monitor unavailable")?;
    Ok(())
}
fn ffi(f: impl FnOnce() -> Check<()>) -> i32 {
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(f)) {
        Ok(Ok(())) => 0,
        _ => -3,
    }
}
/// All FFI buffers must be valid/exclusive/disjoint for the synchronous call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn OHPrepareV1(
    game: *const u8,
    game_bytes: usize,
    content: *const u8,
    content_bytes: usize,
    p: u32,
    out: *mut u64,
) -> i32 {
    if game.is_null()
        || content.is_null()
        || out.is_null()
        || out as usize % align_of::<u64>() != 0
        || !(1..=4095).contains(&game_bytes)
        || !(1..=4095).contains(&content_bytes)
        || policy(p).is_err()
    {
        return -1;
    }
    unsafe { out.write(0) };
    if STATE.begin_prepare().is_err() {
        return -2;
    }
    let code = ffi(|| {
        let g = std::str::from_utf8(unsafe { std::slice::from_raw_parts(game, game_bytes) })
            .map_err(|_| "game path UTF8")?;
        let c = std::str::from_utf8(unsafe { std::slice::from_raw_parts(content, content_bytes) })
            .map_err(|_| "content path UTF8")?;
        if g.contains('\0') || c.contains('\0') {
            return Err("NUL path refused");
        }
        let t = prepare(Path::new(g), Path::new(c), p)?;
        unsafe { out.write(t) };
        Ok(())
    });
    if code != 0 {
        FAILURE.store(
            100 + PREPARE_STAGE.load(Ordering::Acquire),
            Ordering::Release,
        );
        STATE.fail();
    }
    code
}
#[unsafe(no_mangle)]
pub extern "C" fn OHStartV1(t: u64, seconds: u32) -> i32 {
    if token(t).is_err() || lifecycle::validate_duration(seconds).is_err() {
        return -1;
    }
    if STATE.state() != lifecycle::RENDERER_ATTACHED {
        return -2;
    }
    if STATE.start(seconds).is_err() {
        return -2;
    }
    let code = ffi(|| start(t, seconds));
    if code != 0 {
        fatal_stop(25, 0, 0);
    }
    code
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn OHReadStatusV1(t: u64, out: *mut Status, bytes: usize) -> i32 {
    if out.is_null()
        || out as usize % align_of::<Status>() != 0
        || bytes != size_of::<Status>()
        || (t != 0 && token(t).is_err())
    {
        return -1;
    }
    unsafe { out.write(numeric_status()) };
    0
}
#[unsafe(no_mangle)]
pub extern "C" fn OHUpdatePadV1(
    t: u64,
    buttons: u16,
    lt: u8,
    rt: u8,
    lx: i16,
    ly: i16,
    rx: i16,
    ry: i16,
) -> i32 {
    if token(t).is_err() || buttons & 0x0800 != 0 {
        return -1;
    }
    ffi(|| {
        let k = prepared_kernel(t)?;
        k.input.update(
            0,
            kernel_hle::input::Gamepad {
                buttons,
                left_trigger: lt,
                right_trigger: rt,
                thumb_lx: lx,
                thumb_ly: ly,
                thumb_rx: rx,
                thumb_ry: ry,
            },
        );
        Ok(())
    })
}
