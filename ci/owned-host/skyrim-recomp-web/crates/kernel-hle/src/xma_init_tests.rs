//! Eager XMA initialization and exported context lifetime on a self-authored image.
use super::*;
use crate::dispatch::{ModuleId, StubConfig};
use crate::import_policy::ImportPolicy;
use runtime_core::{GuestModule, PPCContext, Runtime};
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

const CHILD_ENV: &str = "SKYRT_PUBLIC_XMA_INIT_TEST_CHILD";
const CHILD_TEST: &str = "xma::init_tests::xma_init_process_fixture";

#[test]
fn eager_context_base_and_exported_slot_lifetime() {
    // Guest memory translation and the kernel clock updater are process-global.
    let mut child = Command::new(std::env::current_exe().unwrap())
        .args(["--ignored", "--exact", CHILD_TEST, "--nocapture"])
        .env(CHILD_ENV, "1")
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
            child.kill().unwrap();
            let _ = child.wait();
            panic!("isolated XMA initialization test exceeded its process limit");
        }
        std::thread::sleep(Duration::from_millis(10));
    }
    let output = child.wait_with_output().unwrap();
    assert!(
        output.status.success(),
        "XMA child failed: {}\n{}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
    assert!(String::from_utf8_lossy(&output.stdout).contains("1 passed"));
}

fn synthetic_runtime() -> Runtime {
    use xex::builder::{
        IMAGE_SCN_CNT_CODE, IMAGE_SCN_MEM_EXECUTE, IMAGE_SCN_MEM_READ, PeSection, XexBuilder, build_pe_image,
    };
    const BASE: u32 = 0x8200_0000;
    const CODE_RVA: u32 = 0x2000;
    const IMAGE_SIZE: u32 = 0x1_0000;
    static FUNCTIONS: [u32; 1] = [BASE + CODE_RVA];
    unsafe extern "C-unwind" fn never_call(_: u32, _: *mut PPCContext, _: *mut u8) {
        panic!("public XMA initialization test must not execute a guest function");
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
    Runtime::load(module, xex::Xex::parse(&bytes).unwrap()).unwrap()
}

fn mmio_base(k: &Kernel) -> u32 {
    let mut bytes = [0; 4];
    k.mem().read_bytes(CONTEXT_ARRAY_ADDRESS, &mut bytes);
    u32::from_le_bytes(bytes)
}

fn export(k: &'static Kernel, ctx: &mut PPCContext, ordinal: u16, arg: u32) -> u32 {
    ctx.set_gpr(3, arg as u64);
    k.dispatch(ModuleId::XBOXKRNL, ordinal, ctx);
    ctx.gpr32(3)
}

#[test]
#[ignore = "only the isolated subprocess may construct the synthetic kernel"]
fn xma_init_process_fixture() {
    assert_eq!(std::env::var(CHILD_ENV).as_deref(), Ok("1"));
    let rt = synthetic_runtime();
    // Use a nonzero, non-byte-symmetric physical base so BE/LE and pointer-vs-
    // physical-address mistakes cannot accidentally pass at the first slot.
    rt.space.alloc_physical(0x12000, PAGE_4K, 0, u32::MAX, PAGE_4K, protect::READWRITE).unwrap();
    let pages_before = rt.space.physical_pages_used();
    assert_eq!(rt.space.mem.read_u32(CONTEXT_ARRAY_ADDRESS), 0);
    let k = Kernel::new_with_import_policy(rt, vfs::Vfs::new(), StubConfig::default(), &[], ImportPolicy::StrictAll)
        .unwrap();

    // These reads happen before the first XMACreateContext, like a guest initializer.
    let cached_base = mmio_base(k);
    assert_eq!(cached_base, 0x14000);
    assert_eq!(k.mem().read_u32(CONTEXT_ARRAY_ADDRESS), cached_base.swap_bytes());
    // Kernel Variables::create also allocates one page for the video device.
    let pages_initialized = pages_before + 1 + 5;
    assert_eq!(k.rt.space.physical_pages_used(), pages_initialized);
    let canonical = runtime_core::memory::map::PHYSICAL_BASE + cached_base;
    assert_eq!(k.rt.space.allocation_size(canonical), Some(CONTEXT_COUNT * CONTEXT_SIZE));
    for page in 0..5 {
        assert!(k.rt.space.is_committed(canonical + page * PAGE_4K));
    }
    let mut contexts = vec![0xff; (CONTEXT_COUNT * CONTEXT_SIZE) as usize];
    k.mem().read_bytes(canonical, &mut contexts);
    assert!(contexts.iter().all(|&byte| byte == 0));
    println!("eager MMIO physical base {cached_base:#X} precedes every context creation; LE bytes verified");

    let out = k.rt.space.system_alloc(4, 4).unwrap();
    let mut ctx = PPCContext::new_zeroed();
    let mut pointers = Vec::new();
    for expected_id in 0..CONTEXT_COUNT {
        assert_eq!(export(k, &mut ctx, 0x224, out), crate::status::SUCCESS);
        let ptr = k.mem().read_u32(out);
        let phys = export(k, &mut ctx, 0x0BE, ptr); // MmGetPhysicalAddress
        assert_eq!(Some(phys), k.rt.space.physical_address(ptr));
        let sdk_id = (phys.wrapping_sub(cached_base) >> 6) & 0xffff;
        assert_eq!(phys, cached_base + expected_id * CONTEXT_SIZE);
        assert_eq!(sdk_id, expected_id);
        assert_eq!(mmio_base(k), cached_base);
        assert_eq!(k.rt.space.physical_pages_used(), pages_initialized);
        pointers.push(ptr);
    }
    assert_eq!(export(k, &mut ctx, 0x224, out), crate::status::NO_MEMORY);
    assert_eq!(k.mem().read_u32(out), 0);
    assert_eq!(mmio_base(k), cached_base);
    println!("all 320 exported contexts map to SDK IDs 0..319; exhaustion preserves the base");

    for id in [0usize, 159, 319] {
        k.mem().write_u32(pointers[id] + 60, 0xaabb_ccdd);
        assert_eq!(export(k, &mut ctx, 0x226, pointers[id]), 0);
        assert_eq!(k.mem().read_u32(pointers[id] + 60), 0);
        assert_eq!(mmio_base(k), cached_base);
        assert_eq!(export(k, &mut ctx, 0x224, out), crate::status::SUCCESS);
        assert_eq!(k.mem().read_u32(out), pointers[id]);
        assert_eq!(k.mem().read_u32(pointers[id] + 60), 0);
    }
    for ptr in pointers {
        assert_eq!(export(k, &mut ctx, 0x226, ptr), 0);
    }
    assert_eq!(k.rt.space.physical_pages_used(), pages_initialized);
    assert_eq!(mmio_base(k), cached_base);
    assert_eq!(export(k, &mut ctx, 0x224, out), crate::status::SUCCESS);
    assert_eq!(k.rt.space.physical_address(k.mem().read_u32(out)), Some(cached_base));
    println!("release/recreate and a full release cycle retain the same array and physical base");
}
