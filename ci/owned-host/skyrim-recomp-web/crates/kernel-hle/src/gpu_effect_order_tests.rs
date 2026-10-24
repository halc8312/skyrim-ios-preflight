//! CPU-visible CP effects against a deliberately delayed renderer, without a GPU or guest code.
//! The kernel/memory and sync-policy globals are confined to a fresh test subprocess.

#[path = "gpu_alias_diagnostics_tests.rs"]
mod alias_diagnostics_tests;

use super::*;
use std::process::{Command, Stdio};
use std::sync::mpsc::{self, Sender};

const CHILD_ENV: &str = "SKYRT_PUBLIC_GPU_EFFECT_TEST_CHILD";
const CHILD_TEST: &str = "gpu::effect_order_tests::effect_process_fixture";
const WAIT: Duration = Duration::from_secs(2);
const BASE: u32 = 0x8200_0000;
const CODE_RVA: u32 = 0x2000;
const IMAGE_SIZE: u32 = 0x1_0000;
const WRITEBACK: u32 = 0x4014_0000;
const FENCE: u32 = WRITEBACK;
const RESOLVE: u32 = WRITEBACK + 4;
const FENCE_VALUE: u32 = 0x1234_ABCD;
const RESOLVE_VALUE: u32 = 0x7654_3210;

#[test]
fn effects_wait_for_renderer_publication_in_both_sync_modes() {
    use std::io::Read;
    fn drain<R: std::io::Read + Send + 'static>(mut pipe: R) -> std::thread::JoinHandle<Vec<u8>> {
        std::thread::spawn(move || {
            let mut saved = Vec::new();
            pipe.by_ref().take(65536).read_to_end(&mut saved).unwrap();
            std::io::copy(&mut pipe, &mut std::io::sink()).unwrap();
            saved
        })
    }
    // GuestMemory translation and SYNC_EFFECTS are process-global. Do not construct a kernel
    // or mutate either global in the ordinary, potentially parallel unit-test process.
    let mut child = Command::new(std::env::current_exe().unwrap())
        .args(["--ignored", "--exact", CHILD_TEST, "--nocapture"])
        .env(CHILD_ENV, "1")
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .unwrap();
    let stdout = drain(child.stdout.take().unwrap());
    let stderr = drain(child.stderr.take().unwrap());
    let deadline = Instant::now() + Duration::from_secs(20);
    loop {
        if child.try_wait().unwrap().is_some() {
            break;
        }
        if Instant::now() >= deadline {
            child.kill().unwrap();
            let _ = child.wait();
            panic!("isolated public GPU effect test exceeded its process limit");
        }
        // Only the subprocess watchdog polls; no sleep is an ordering oracle below.
        std::thread::sleep(Duration::from_millis(10));
    }
    let status = child.wait().unwrap();
    let stdout = stdout.join().unwrap();
    let stderr = stderr.join().unwrap();
    assert!(
        status.success(),
        "public GPU effect child failed: {}\n{}",
        String::from_utf8_lossy(&stdout),
        String::from_utf8_lossy(&stderr)
    );
    assert!(String::from_utf8_lossy(&stdout).contains("1 passed"));
}

#[test]
#[ignore = "only the isolated subprocess test may construct this synthetic kernel"]
fn effect_process_fixture() {
    assert_eq!(std::env::var(CHILD_ENV).as_deref(), Ok("1"));
    let old = SYNC_EFFECTS.load(Ordering::Relaxed);
    struct Restore(u8);
    impl Drop for Restore {
        fn drop(&mut self) {
            SYNC_EFFECTS.store(self.0, Ordering::Relaxed);
        }
    }
    let _restore = Restore(old);

    // The mobile default must not override an explicit caller policy in either direction.
    for (initial, requested, expected) in [
        (SYNC_DEFAULT_OFF, true, SYNC_DEFAULT_ON),
        (SYNC_DEFAULT_ON, false, SYNC_DEFAULT_OFF),
        (SYNC_FORCED_OFF, true, SYNC_FORCED_OFF),
        (SYNC_FORCED_ON, false, SYNC_FORCED_ON),
    ] {
        SYNC_EFFECTS.store(initial, Ordering::Relaxed);
        set_sync_default(requested);
        assert_eq!(SYNC_EFFECTS.load(Ordering::Relaxed), expected);
    }

    let k = synthetic_kernel();
    k.rt.space.alloc_virtual_fixed(WRITEBACK, 0x1000, true, 0x04).unwrap();
    k.gpu.write_register(k.mem(), reg::WRITEBACK_START, WRITEBACK);
    k.gpu.write_register(k.mem(), reg::WRITEBACK_SIZE, 0x1000);
    sync_on(k);
    sync_off(k);
    interrupted_waits_never_publish_effects(k);
}

struct StoppedBackend(GpuWaitError);

impl GpuBackend for StoppedBackend {
    fn registers(&self, _: Vec<(u32, u32)>) {}
    fn shader(&self, _: bool, _: Arc<[u32]>) {}
    fn draw(&self) {}
    fn swap(&self, _: [u32; 6], _: u32, _: u32) {}
    fn wait_idle(&self) -> Result<(), GpuWaitError> { Err(self.0) }
    fn wait_for(&self, _: u64) -> Result<(), GpuWaitError> { Err(self.0) }
    fn submitted(&self) -> Option<u64> { Some(1) }
}

fn interrupted_waits_never_publish_effects(k: &'static Kernel) {
    const COMMAND: u32 = WRITEBACK + 0x80;
    // Positive control: precisely the same MEM_WRITE packet must really store
    // into committed physical memory after publication. MEM_WRITE does not use
    // EVENT_WRITE_SHD's virtual writeback-window exception.
    clear_words(k);
    let allocation = k.rt.space.alloc_physical(0x1000, 0x1000, 0, u32::MAX, 0x1000, 0x04).unwrap();
    let physical = k.rt.space.physical_address(allocation).unwrap();
    let (entered, _receiver) = mpsc::channel();
    let backend = Arc::new(DelayedBackend::new(entered));
    backend.publish(k);
    let mut context = PPCContext::new_zeroed();
    let mut positive = processor(k, &mut context, backend);
    k.mem().write_u32(COMMAND, physical | 2);
    k.mem().write_u32(COMMAND + 4, FENCE_VALUE);
    let mut reader = Reader { base: COMMAND, capacity: 2, pos: 0, remaining: 2 };
    assert!(positive.type3(&mut reader, pm4::MEM_WRITE, 2));
    assert_eq!(k.mem().read_u32(allocation), FENCE_VALUE);
    for error in [GpuWaitError::Closed, GpuWaitError::Failed(7)] {
        for scenario in 0..4 {
            clear_words(k);
            k.mem().write_u32(allocation, 0);
            let mut ctx = PPCContext::new_zeroed();
            let mut cp = processor(k, &mut ctx, Arc::new(StoppedBackend(error)));
            let action = Deferred::Fence { addr: FENCE | 2, data: FENCE_VALUE };
            match scenario {
                0 => {
                    SYNC_EFFECTS.store(SYNC_FORCED_ON, Ordering::Relaxed);
                    assert!(!cp.defer(pm4::EVENT_WRITE_SHD, action));
                }
                1 | 2 => {
                    cp.deferred.push_back((1, action));
                    let ok = if scenario == 1 { cp.run_deferred(true) } else { cp.run_next_deferred() };
                    assert!(!ok);
                    assert_eq!(cp.deferred.len(), 1, "failed effect must not be consumed");
                }
                _ => {
                    k.mem().write_u32(COMMAND, physical | 2);
                    k.mem().write_u32(COMMAND + 4, FENCE_VALUE);
                    let mut reader = Reader { base: COMMAND, capacity: 2, pos: 0, remaining: 2 };
                    assert!(!cp.type3(&mut reader, pm4::MEM_WRITE, 2));
                }
            }
            assert_eq!(cp.backend_failure, Some(error));
            assert_eq!(k.mem().read_u32(FENCE), 0, "effect released after {error:?}");
            assert_eq!(k.mem().read_u32(allocation), 0, "physical MEM_WRITE released after {error:?}");
            cp.run(action);
            assert_eq!(k.mem().read_u32(FENCE), 0, "latched failure must reject later effects");
        }
    }
    k.rt.space.free_physical(allocation).unwrap();
}

/// A renderer with one submitted command whose guest-memory write is not yet complete.
/// A test controller releases it with an explicit happens-before edge, rather than racing
/// ordinary guest-memory accesses or relying on how quickly another thread runs.
struct DelayedBackend {
    complete: AtomicU64,
    waits: AtomicU32,
    released: Mutex<bool>,
    changed: Condvar,
    entered: Sender<()>,
}

impl DelayedBackend {
    fn new(entered: Sender<()>) -> Self {
        Self {
            complete: AtomicU64::new(0),
            waits: AtomicU32::new(0),
            released: Mutex::new(false),
            changed: Condvar::new(),
            entered,
        }
    }

    fn wait(&self) {
        self.waits.fetch_add(1, Ordering::Relaxed);
        let released = self.released.lock().unwrap();
        self.entered.send(()).unwrap();
        let (released, timeout) = self.changed.wait_timeout_while(released, WAIT, |v| !*v).unwrap();
        assert!(!timeout.timed_out() && *released, "delayed renderer did not complete");
        assert_eq!(self.complete.load(Ordering::Acquire), 1);
    }

    fn publish(&self, k: &Kernel) {
        let mut released = self.released.lock().unwrap();
        assert!(!*released);
        // Model a renderer resolve's CPU-visible write, then publish completion. The CP is
        // blocked (sync mode) or on this same thread (async mode); there is no data race.
        k.mem().write_u32(RESOLVE, RESOLVE_VALUE);
        self.complete.store(1, Ordering::Release);
        *released = true;
        self.changed.notify_one();
    }
}

impl GpuBackend for DelayedBackend {
    fn registers(&self, _: Vec<(u32, u32)>) {}
    fn shader(&self, _: bool, _: Arc<[u32]>) {}
    fn draw(&self) {}
    fn swap(&self, _: [u32; 6], _: u32, _: u32) {}
    fn wait_idle(&self) -> Result<(), GpuWaitError> {
        self.wait();
        Ok(())
    }
    fn wait_for(&self, seq: u64) -> Result<(), GpuWaitError> {
        assert_eq!(seq, 1);
        self.wait();
        Ok(())
    }
    fn submitted(&self) -> Option<u64> {
        Some(1)
    }
    fn completed(&self) -> u64 {
        self.complete.load(Ordering::Acquire)
    }
}

fn clear_words(k: &Kernel) {
    k.mem().write_u32(FENCE, 0);
    k.mem().write_u32(RESOLVE, 0);
}

fn sync_on(k: &'static Kernel) {
    clear_words(k);
    SYNC_EFFECTS.store(SYNC_DEFAULT_OFF, Ordering::Relaxed);
    set_sync_default(true);
    let (entered_tx, entered_rx) = mpsc::channel();
    let backend = Arc::new(DelayedBackend::new(entered_tx));
    let worker_backend = backend.clone();
    let (done_tx, done_rx) = mpsc::channel();
    let worker = std::thread::spawn(move || {
        let mut ctx = PPCContext::new_zeroed();
        let mut cp = processor(k, &mut ctx, worker_backend);
        // Endian mode 2 yields FENCE_VALUE through GuestMemory's big-endian read API.
        assert!(cp.defer(pm4::EVENT_WRITE_SHD, Deferred::Fence { addr: FENCE | 2, data: FENCE_VALUE }));
        assert!(cp.deferred.is_empty());
        assert_eq!(cp.syncs[pm4::EVENT_WRITE_SHD as usize], 1);
        done_tx.send(()).unwrap();
    });
    entered_rx.recv_timeout(WAIT).expect("sync mode must wait for the submitted renderer work");
    assert_eq!(backend.waits.load(Ordering::Relaxed), 1);
    assert_eq!(k.mem().read_u32(FENCE), 0, "fence published before renderer completion");
    assert_eq!(k.mem().read_u32(RESOLVE), 0);
    backend.publish(k);
    done_rx.recv_timeout(WAIT).expect("sync fence did not publish after renderer completion");
    worker.join().unwrap();
    assert_eq!(k.mem().read_u32(RESOLVE), RESOLVE_VALUE);
    assert_eq!(k.mem().read_u32(FENCE), FENCE_VALUE);
}

fn sync_off(k: &'static Kernel) {
    clear_words(k);
    SYNC_EFFECTS.store(SYNC_DEFAULT_ON, Ordering::Relaxed);
    set_sync_default(false);
    let (entered_tx, entered_rx) = mpsc::channel();
    let backend = Arc::new(DelayedBackend::new(entered_tx));
    let mut ctx = PPCContext::new_zeroed();
    let mut cp = processor(k, &mut ctx, backend.clone());
    assert!(cp.defer(pm4::EVENT_WRITE_SHD, Deferred::Fence { addr: FENCE | 2, data: FENCE_VALUE }));
    assert_eq!(cp.deferred.len(), 1);
    assert_eq!(cp.deferred_count, 1);
    assert_eq!(k.mem().read_u32(FENCE), 0);
    assert!(cp.run_deferred(false));
    assert_eq!(cp.deferred.len(), 1);
    assert_eq!(k.mem().read_u32(FENCE), 0, "async fence published before renderer completion");
    assert_eq!(backend.waits.load(Ordering::Relaxed), 0);
    assert!(matches!(entered_rx.try_recv(), Err(mpsc::TryRecvError::Empty)));
    backend.publish(k);
    assert!(cp.run_deferred(false));
    assert!(cp.deferred.is_empty());
    assert_eq!(backend.waits.load(Ordering::Relaxed), 0);
    assert_eq!(k.mem().read_u32(RESOLVE), RESOLVE_VALUE);
    assert_eq!(k.mem().read_u32(FENCE), FENCE_VALUE);
}

fn processor<'a>(k: &'static Kernel, ctx: &'a mut PPCContext, backend: Arc<dyn GpuBackend>) -> CommandProcessor<'a> {
    CommandProcessor {
        k,
        mem: k.mem(),
        ctx,
        bin_mask: 0xFFFF_FFFF,
        bin_select: 0xFFFF_FFFF,
        zpd_samples: 0,
        backend: Some(backend),
        pending: Vec::new(),
        deferred: std::collections::VecDeque::new(),
        deferred_count: 0,
        syncs: [0; 128],
        sync_wait: Duration::ZERO,
        backend_failure: None,
        frame_draws: 0,
        full_frame_seen: false,
        d3d_marker: None,
        d3d_writes: Vec::new(),
        d3d_packet: (0, 0),
        d3d_last_writer: std::collections::HashMap::new(),
        d3d_draw_next: None,
        d3d_ran: std::collections::HashSet::new(),
        d3d_predicated: None,
        d3d_peek: false,
        d3d_scissor: None,
        d3d_draws: (0, 0),
    }
}

fn synthetic_kernel() -> &'static Kernel {
    use crate::dispatch::StubConfig;
    use crate::import_policy::ImportPolicy;
    use runtime_core::{GuestModule, Runtime};
    use xex::builder::{
        IMAGE_SCN_CNT_CODE, IMAGE_SCN_MEM_EXECUTE, IMAGE_SCN_MEM_READ, PeSection, XexBuilder, build_pe_image,
    };
    static FUNCTIONS: [u32; 1] = [BASE + CODE_RVA];
    unsafe extern "C-unwind" fn never_call(_: u32, _: *mut PPCContext, _: *mut u8) {
        panic!("public GPU effect test must not execute a guest function");
    }
    unsafe extern "C" fn init_context(_: *mut PPCContext) {}
    let image = build_pe_image(
        &[PeSection {
            name: ".text".into(),
            rva: CODE_RVA,
            virtual_size: 4,
            characteristics: IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ,
            data: 0x4E80_0020u32.to_be_bytes().to_vec(),
        }],
        IMAGE_SIZE,
    );
    let bytes = XexBuilder {
        image,
        load_address: BASE,
        entry_point: BASE + CODE_RVA,
        module_flags: 1,
        imports: vec![],
        tls: Some(xex::TlsInfo { slot_count: 64, raw_data_address: 0, data_size: 0, raw_data_size: 0 }),
        system_flags: Some(0x400),
        default_stack_size: Some(0x40000),
        execution_info: None,
        basic_compression: false,
    }
    .build();
    let module = GuestModule {
        image_base: BASE,
        image_size: IMAGE_SIZE,
        code_base: BASE + CODE_RVA,
        code_size: 0x1000,
        functions: &FUNCTIONS,
        call: never_call,
        init_context,
    };
    let rt = Runtime::load(module, xex::Xex::parse(&bytes).unwrap()).unwrap();
    Kernel::new_with_import_policy(rt, vfs::Vfs::new(), StubConfig::default(), &[], ImportPolicy::StrictAll).unwrap()
}
