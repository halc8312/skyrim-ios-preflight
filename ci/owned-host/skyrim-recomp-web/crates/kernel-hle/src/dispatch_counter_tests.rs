//! Diagnostic counter equivalence, including the real Kernel::dispatch path.
use super::*;
use std::sync::atomic::Ordering;

fn entry() -> RegisteredExport {
    RegisteredExport::new(Export {
        module: ModuleId::XBOXKRNL,
        ordinal: 0x83,
        name: "authored_counter",
        handler: |_| 0,
    })
}

#[test]
fn uncalled_and_wrapped_counters_remain_distinct() {
    let e = entry();
    assert_eq!(e.call_count(), None);
    assert_eq!(e.note_call(), 1);
    assert_eq!(e.note_call(), 2);
    assert_eq!(e.call_count(), Some(2));
    // A used counter is still included if it eventually wraps to zero.
    e.calls.get().unwrap().store(0, Ordering::Relaxed);
    assert_eq!(e.call_count(), Some(0));
}

#[test]
fn concurrent_first_use_is_exact_and_has_one_first_caller() {
    let e = std::sync::Arc::new(entry());
    let gate = std::sync::Arc::new(std::sync::Barrier::new(8));
    let workers: Vec<_> = (0..8)
        .map(|_| {
            let e = e.clone();
            let gate = gate.clone();
            std::thread::spawn(move || {
                gate.wait();
                (0..2000).filter(|_| e.note_call() == 1).count()
            })
        })
        .collect();
    assert_eq!(workers.into_iter().map(|w| w.join().unwrap()).sum::<usize>(), 1);
    assert_eq!(e.call_count(), Some(16_000));
}

#[test]
fn real_dispatch_preserves_returns_unknown_policy_and_snapshots() {
    use std::process::{Command, Stdio};
    use std::time::{Duration, Instant};
    let mut child = Command::new(std::env::current_exe().unwrap())
        .args(["--ignored", "--exact", "dispatch::counter_tests::dispatch_counter_process_fixture", "--nocapture"])
        .env("SKYRT_PUBLIC_DISPATCH_COUNTER_CHILD", "1")
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .unwrap();
    let deadline = Instant::now() + Duration::from_secs(20);
    while child.try_wait().unwrap().is_none() {
        if Instant::now() >= deadline {
            child.kill().unwrap();
            let _ = child.wait();
            panic!("public dispatch fixture exceeded its process limit");
        }
        std::thread::sleep(Duration::from_millis(10));
    }
    let output = child.wait_with_output().unwrap();
    assert!(
        output.status.success(),
        "{}\n{}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
    assert!(String::from_utf8_lossy(&output.stdout).contains("1 passed"));
}

fn synthetic_runtime() -> runtime_core::Runtime {
    use xex::builder::{
        IMAGE_SCN_CNT_CODE, IMAGE_SCN_MEM_EXECUTE, IMAGE_SCN_MEM_READ, PeSection, XexBuilder, build_pe_image,
    };
    const BASE: u32 = 0x8200_0000;
    static FUNCTIONS: [u32; 1] = [BASE + 0x2000];
    unsafe extern "C-unwind" fn never_call(_: u32, _: *mut PPCContext, _: *mut u8) {
        panic!("authored counter fixture never runs guest code");
    }
    unsafe extern "C" fn init_context(_: *mut PPCContext) {}
    let image = build_pe_image(
        &[PeSection {
            name: ".text".into(),
            rva: 0x2000,
            virtual_size: 4,
            characteristics: IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ,
            data: 0x4E80_0020u32.to_be_bytes().to_vec(),
        }],
        0x1_0000,
    );
    let bytes = XexBuilder {
        image,
        load_address: BASE,
        entry_point: BASE + 0x2000,
        module_flags: 1,
        imports: vec![],
        tls: Some(xex::TlsInfo { slot_count: 64, raw_data_address: 0, data_size: 0, raw_data_size: 0 }),
        system_flags: Some(0x400),
        default_stack_size: Some(0x40000),
        execution_info: None,
        basic_compression: false,
    }
    .build();
    let module = runtime_core::GuestModule {
        image_base: BASE,
        image_size: 0x1_0000,
        code_base: BASE + 0x2000,
        code_size: 0x1000,
        functions: &FUNCTIONS,
        call: never_call,
        init_context,
    };
    runtime_core::Runtime::load(module, xex::Xex::parse(&bytes).unwrap()).unwrap()
}

#[test]
#[ignore = "only the bounded subprocess may initialize global guest translation"]
fn dispatch_counter_process_fixture() {
    assert_eq!(std::env::var("SKYRT_PUBLIC_DISPATCH_COUNTER_CHILD").as_deref(), Ok("1"));
    let k =
        Kernel::new(synthetic_runtime(), vfs::Vfs::new(), StubConfig { default_return: 42, ..Default::default() }, &[])
            .unwrap();
    assert!(k.import_stats().is_empty(), "registration alone must not count a call");
    let workers: Vec<_> = (0..4)
        .map(|_| {
            std::thread::spawn(move || {
                let mut ctx = PPCContext::new_zeroed();
                for _ in 0..500 {
                    k.dispatch(ModuleId::XBOXKRNL, 0x83, &mut ctx);
                    assert_eq!(ctx.gpr(3), runtime_core::timebase::TIMEBASE_FREQUENCY);
                }
            })
        })
        .collect();
    for worker in workers {
        worker.join().unwrap();
    }
    let mut ctx = PPCContext::new_zeroed();
    for _ in 0..3 {
        k.dispatch(ModuleId(42), 0xFFFE, &mut ctx);
        assert_eq!(ctx.gpr(3), 42, "unknown legacy policy/return is retained");
    }
    let stats = k.import_stats();
    assert_eq!(stats.len(), 2);
    assert_eq!(
        (stats[0].0, stats[0].1, stats[0].2.as_str(), stats[0].3.calls, stats[0].3.implemented),
        (ModuleId::XBOXKRNL, 0x83, "KeQueryPerformanceFrequency", 2000, true)
    );
    assert_eq!((stats[1].0, stats[1].1, stats[1].3.calls, stats[1].3.implemented), (ModuleId(42), 0xFFFE, 3, false));
    assert_eq!(k.stats.read().unwrap().len(), 1, "implemented hot path never enters the fallback table");
    assert_eq!(k.call_export("KeQueryPerformanceFrequency", &mut ctx, &[]), runtime_core::timebase::TIMEBASE_FREQUENCY);
    assert_eq!(k.import_stats()[0].3.calls, 2001);
}
