//! Real CP memory paths on a self-authored image, confined to a fresh subprocess.
use super::*;
use runtime_core::alias_diagnostics::{self as diag, Counter};
use runtime_core::memory::map;

const ALIAS_CHILD_ENV: &str = "SKYRT_PUBLIC_CP_ALIAS_TEST_CHILD";
const ALIAS_CHILD_TEST: &str = "gpu::effect_order_tests::alias_diagnostics_tests::cp_alias_process_fixture";
const DATA: u32 = BASE + 0x6000;
const PAYLOAD: u32 = WRITEBACK;

#[test]
fn cp_translations_observe_aliases_without_changing_memory_effects() {
    let mut child = Command::new(std::env::current_exe().unwrap())
        .args(["--ignored", "--exact", ALIAS_CHILD_TEST, "--nocapture"])
        .env(ALIAS_CHILD_ENV, "1")
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
            panic!("isolated CP alias test exceeded its process limit");
        }
        std::thread::sleep(Duration::from_millis(10));
    }
    let output = child.wait_with_output().unwrap();
    assert!(
        output.status.success(),
        "public CP alias child failed: {}\n{}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
    assert!(String::from_utf8_lossy(&output.stdout).contains("1 passed"));
}

fn count(counter: Counter) -> u64 {
    diag::snapshot().counters[counter as usize]
}

fn one_xex_selection<T>(f: impl FnOnce() -> T) -> T {
    let before = count(Counter::SelectedXex);
    let result = f();
    assert_eq!(count(Counter::SelectedXex), before + 1);
    result
}

fn payload(k: &Kernel, words: &[u32]) -> Reader {
    for (i, &word) in words.iter().enumerate() {
        k.mem().write_u32(PAYLOAD + i as u32 * 4, word);
    }
    Reader { base: PAYLOAD, capacity: words.len() as u32, pos: 0, remaining: words.len() as u32 }
}

#[test]
#[ignore = "only the isolated subprocess may enable process-global alias diagnostics"]
fn cp_alias_process_fixture() {
    assert_eq!(std::env::var(ALIAS_CHILD_ENV).as_deref(), Ok("1"));
    let k = synthetic_kernel();
    k.rt.space.alloc_virtual_fixed(PAYLOAD, 0x1000, true, 0x04).unwrap();
    let physical = k.rt.space.physical_address(DATA).unwrap();
    let canonical = map::PHYSICAL_BASE + physical;
    let mut ctx = PPCContext::new_zeroed();
    let (entered, _) = mpsc::channel();
    let mut cp = processor(k, &mut ctx, Arc::new(DelayedBackend::new(entered)));
    cp.backend = None;

    // Default OFF changes neither CP data/endian behavior nor any counter.
    cp.store(physical | 2, 0x1234_ABCD);
    assert_eq!(cp.load(physical | 2), 0x1234_ABCD);
    assert_eq!(k.mem().read_u32(DATA), 0x1234_ABCD);
    assert!(diag::snapshot().counters.iter().all(|&n| n == 0));
    assert!(!diag::snapshot().enabled);
    assert!(diag::enable());

    one_xex_selection(|| assert_eq!(cp.load(physical | 2), 0x1234_ABCD));
    one_xex_selection(|| cp.store(physical | 2, 0x2468_ACE0));
    assert_eq!(k.mem().read_u32(DATA), 0x2468_ACE0);
    one_xex_selection(|| assert_eq!(cp.read_physical(physical, 1), Some(vec![0x2468_ACE0])));

    k.gpu.write_register(k.mem(), reg::SCRATCH_ADDR, physical + 4);
    k.gpu.write_register(k.mem(), reg::SCRATCH_UMSK, 1);
    one_xex_selection(|| cp.write_register(reg::SCRATCH_REG0, 0x3141_5926));
    assert_eq!(k.mem().read_u32(DATA + 4), 0x3141_5926);

    let mut r = payload(k, &[0, physical + 8]);
    one_xex_selection(|| assert!(cp.type3(&mut r, pm4::EVENT_WRITE_EXT, 2)));
    assert_eq!(k.mem().read_u16(DATA + 8), 0);
    assert_eq!(k.mem().read_u16(DATA + 10), 1024);
    assert_eq!(k.mem().read_u16(DATA + 18), 1);

    one_xex_selection(|| cp.event_write_shd((physical + 32) | 2, 0x89AB_CDEF));
    assert_eq!(k.mem().read_u32(DATA + 32), 0x89AB_CDEF);
    one_xex_selection(|| cp.write_zpd_report(physical + 64));
    assert_eq!(k.mem().read_u32(DATA + 64), ZPD_SAMPLES_PER_REPORT.swap_bytes());

    // The real indirect-buffer handler reads a self-authored type-0 register packet.
    k.mem().write_u32(DATA + 128, reg::VGT_EVENT_INITIATOR);
    k.mem().write_u32(DATA + 132, 7);
    let mut r = payload(k, &[physical + 128, 2]);
    one_xex_selection(|| assert!(cp.type3(&mut r, pm4::INDIRECT_BUFFER, 2)));
    assert_eq!(k.gpu.read_register(k.mem(), reg::VGT_EVENT_INITIATOR), 7);

    assert_eq!(count(Counter::CpWriteTranslationSelectedXex), 5);
    assert_eq!(count(Counter::RendererWriteTranslationSelectedXex), 0);

    // A committed physical allocation changes selection while leaving the XEX backing alone.
    let page = physical & !0xFFFF;
    let allocation = k.rt.space.alloc_physical(0x10000, 0x10000, page, page + 0xFFFF, 0x10000, 0x04).unwrap();
    let before = count(Counter::SelectedPhysical);
    cp.store(physical | 2, 0x7654_3210);
    assert_eq!(cp.load(physical | 2), 0x7654_3210);
    assert_eq!(count(Counter::SelectedPhysical), before + 2);
    assert_eq!(k.mem().read_u32(canonical), 0x7654_3210);
    assert_eq!(k.mem().read_u32(DATA), 0x2468_ACE0);
    assert_eq!(count(Counter::CpWriteTranslationSelectedXex), 5);
    k.rt.space.free_physical(allocation).unwrap();
    one_xex_selection(|| cp.store(physical | 2, 0x5566_7788));
    assert_eq!(k.mem().read_u32(DATA), 0x5566_7788);
    assert_eq!(k.mem().read_u32(canonical), 0);
    assert_eq!(count(Counter::CpWriteTranslationSelectedXex), 6);

    let before = count(Counter::SelectedUnmapped);
    assert_eq!(cp.load(0x1900_0000), 0);
    assert_eq!(count(Counter::SelectedUnmapped), before + 1);
    assert_eq!(count(Counter::RendererReadPage), 0, "CP reads must not be mislabeled as renderer reads");
    assert_eq!(count(Counter::RendererWritePage), 0);
    assert_eq!(count(Counter::SelectionSnapshotMismatch), 0);
    assert_eq!(count(Counter::ProbeBusy), 0);
    let observed = diag::snapshot();
    assert_eq!(observed.records.len(), 1);
    let record = observed.records[0];
    assert_eq!(record.physical_page, physical / 4096);
    assert_eq!((record.first_selection, record.last_selection, record.changes), (2, 2, 2));
    assert!(record.overlap_seen);
    assert_eq!(count(Counter::OverlapObservation), 2);
    assert!(!observed.concurrent_access_order_proven);

    // The factored renderer path still retains its selected address and its distinct counters.
    use runtime_core::alias_diagnostics::Access;
    let before = count(Counter::SelectedXex);
    assert_eq!(k.rt.space.translate_physical_for_gpu(physical, Access::Read), Some(DATA));
    assert_eq!(k.rt.space.translate_physical_for_gpu(physical, Access::Write), Some(DATA));
    assert_eq!(count(Counter::SelectedXex), before + 2);
    assert_eq!(count(Counter::RendererReadPage), 1);
    assert_eq!(count(Counter::RendererWritePage), 1);
    assert_eq!(count(Counter::CpWriteTranslationSelectedXex), 6);
    assert_eq!(count(Counter::RendererWriteTranslationSelectedXex), 1);

    // A direct virtual-window write is deliberately NOT a physical-fallback observation.
    k.gpu.write_register(k.mem(), reg::WRITEBACK_START, DATA);
    k.gpu.write_register(k.mem(), reg::WRITEBACK_SIZE, 4);
    cp.event_write_shd(DATA | 2, 0x1020_3040);
    assert_eq!(k.mem().read_u32(DATA), 0x1020_3040);
    assert_eq!(count(Counter::CpWriteTranslationSelectedXex), 6);
}
