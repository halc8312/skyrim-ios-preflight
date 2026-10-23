//! Deterministic device-core tests. The only pause gate is cfg(test) queue state.
use super::*;
use crate::dispatch::StubConfig;

#[test]
fn device_commands_and_lifetimes() {
    use std::process::{Command, Stdio};
    let mut child = Command::new(std::env::current_exe().unwrap())
        .args(["--ignored", "--exact", "xma::device_tests::device_process_fixture", "--nocapture"])
        .env("SKYRT_PUBLIC_XMA_DEVICE_CHILD", "1")
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .unwrap();
    let deadline = Instant::now() + Duration::from_secs(20);
    while child.try_wait().unwrap().is_none() {
        if Instant::now() >= deadline {
            child.kill().unwrap();
            let _ = child.wait();
            panic!("isolated XMA device fixture exceeded its process limit");
        }
        std::thread::sleep(Duration::from_millis(10));
    }
    let output = child.wait_with_output().unwrap();
    let stdout = String::from_utf8_lossy(&output.stdout);
    assert!(output.status.success(), "XMA device child failed: {stdout}\n{}", String::from_utf8_lossy(&output.stderr));
    assert!(stdout.contains("1 passed"));
    print!("{stdout}");
}

#[test]
#[ignore = "only the isolated subprocess may construct the synthetic kernel"]
fn device_process_fixture() {
    assert_eq!(std::env::var("SKYRT_PUBLIC_XMA_DEVICE_CHILD").as_deref(), Ok("1"));
    let k = Kernel::new(super::init_tests::synthetic_runtime(), vfs::Vfs::new(), StubConfig::default(), &[]).unwrap();
    assert!(Kernel::try_global().is_none());
    let alloc = |size| k.rt.space.alloc_physical(size, PAGE_4K, 0, u32::MAX, PAGE_4K, protect::READWRITE).unwrap();
    let input = alloc(2048);
    let output = alloc(9 * 256);
    k.mem().fill(input, 2048, 0);
    k.mem().write_bytes(input, &[0, 0, 1, 0xFF]);
    k.mem().fill(output, 9 * 256, 0xA5);
    // Exclude failed physical reads as an alternative reason for buffer advance.
    let mut observed = [0; 2048];
    assert!(xma_decoder::XmaMemory::read(
        &PhysicalMemory(k),
        k.rt.space.physical_address(input).unwrap(),
        &mut observed,
    ));
    assert_eq!(observed.as_slice(), k.mem().read_vec(input, 2048));
    let configure = |ptr| {
        let mut d = ContextData::default();
        d.set(field::INPUT_BUFFER_0_PTR, k.rt.space.physical_address(input).unwrap());
        d.set(field::INPUT_BUFFER_0_PACKET_COUNT, 1);
        d.set(field::OUTPUT_BUFFER_PTR, k.rt.space.physical_address(output).unwrap());
        d.set(field::OUTPUT_BUFFER_BLOCK_COUNT, 8);
        d.set(field::SUBFRAME_DECODE_COUNT, 1);
        assert!(k.xma_initialize(ptr, d));
        assert!(k.xma_update_context(ptr, |d| {
            d.set(field::INPUT_BUFFER_0_VALID, 1);
            d.set(field::OUTPUT_BUFFER_VALID, 1);
        }));
    };
    let pause = |paused| {
        k.xma.queue.lock().unwrap().paused = paused;
        k.xma.changed.notify_all();
    };
    let drain = || assert!(k.xma_wait_idle(Duration::from_secs(5)));
    let wait_state = |predicate: fn(&WorkQueue) -> bool| {
        let queue = k.xma.queue.lock().unwrap();
        let (queue, timeout) =
            k.xma.changed.wait_timeout_while(queue, Duration::from_secs(5), |q| !predicate(q)).unwrap();
        assert!(!timeout.timed_out() || predicate(&queue), "worker test state did not arrive");
    };
    let consumed = |ptr| {
        let d = k.xma_context(ptr).unwrap();
        assert_eq!(d.get(field::INPUT_BUFFER_0_VALID), 0);
        assert_eq!(d.get(field::CURRENT_BUFFER), 1);
    };
    let contexts: Vec<_> = (0..CONTEXT_COUNT).map(|_| k.xma_allocate()).collect();
    assert!(contexts.iter().all(|&p| p != 0));
    assert_eq!(k.xma_allocate(), 0);
    let physical = k.xma_mmio_read(CONTEXT_ARRAY_ADDRESS).unwrap();
    assert_eq!(k.xma_mmio_read(NEXT_CONTEXT_INDEX), Some(1));
    assert_eq!(k.xma_mmio_read(NEXT_CONTEXT_INDEX), Some(1));
    for n in 1..=CONTEXT_COUNT {
        assert_eq!(k.xma_mmio_read(CURRENT_CONTEXT_INDEX), Some(n % CONTEXT_COUNT));
    }
    assert_eq!(k.xma_mmio_read(CONTEXT_ARRAY_ADDRESS), Some(physical));
    assert_eq!(k.xma_mmio_read(CURRENT_CONTEXT_INDEX + 1), None);
    assert!(!k.xma_mmio_write(CONTEXT_KICK + 40, 1).unwrap());
    assert!(!k.xma_mmio_write(CONTEXT_KICK + 1, 1).unwrap());
    assert!(k.xma_mmio_write(CONTEXT_KICK, 0).unwrap());
    assert!(k.xma.worker.lock().unwrap().is_none());

    // Every bit of all ten groups, including31/32/319. Pending duplicate
    // kicks coalesce under enabled-bit semantics, with no sampled registers.
    pause(true);
    for &ptr in &contexts {
        configure(ptr);
    }
    for group in 0..10 {
        assert!(k.xma_mmio_write(CONTEXT_KICK + group * 4, u32::MAX).unwrap());
    }
    assert!(k.xma_mmio_write(CONTEXT_KICK, 1).unwrap());
    assert_eq!(k.xma.queue.lock().unwrap().pending.len(), 320);
    assert_eq!(k.xma_mmio_read(CONTEXT_KICK), Some(1));
    assert_eq!(k.mem().read_vec(CONTEXT_KICK, 4), 1u32.to_le_bytes());
    // Changing diagnostic backing words cannot erase captured commands.
    for group in 0..10 {
        k.mem().write_bytes(CONTEXT_KICK + group * 4, &[0; 4]);
    }
    pause(false);
    drain();
    for &ptr in &contexts {
        consumed(ptr);
    }
    assert!(k.mem().read_vec(output, 9 * 256).iter().all(|&b| b == 0xA5));

    // Every lock and clear group uses the same captured mask/index mapping.
    pause(true);
    for &ptr in &contexts {
        configure(ptr);
    }
    for group in 0..10 {
        k.xma_mmio_write(CONTEXT_KICK + group * 4, u32::MAX).unwrap();
        k.xma_mmio_write(CONTEXT_LOCK + group * 4, u32::MAX).unwrap();
    }
    pause(false);
    drain();
    for &ptr in &contexts {
        assert_eq!(k.xma_context(ptr).unwrap().get(field::INPUT_BUFFER_0_VALID), 1);
    }
    pause(true);
    for group in 0..10 {
        k.xma_mmio_write(CONTEXT_KICK + group * 4, u32::MAX).unwrap();
        k.xma_mmio_write(CONTEXT_CLEAR + group * 4, u32::MAX).unwrap();
    }
    pause(false);
    drain();
    for &ptr in &contexts {
        let data = k.xma_context(ptr).unwrap();
        assert_eq!(data.get(field::INPUT_BUFFER_0_VALID), 0);
        assert_eq!(data.get(field::OUTPUT_BUFFER_VALID), 0);
    }

    // Identical value after rearming must dispatch anew on each call.
    for _ in 0..4 {
        configure(contexts[0]);
        k.xma_mmio_write(CONTEXT_KICK, 1).unwrap();
        drain();
        consumed(contexts[0]);
    }

    // The legacy convenience name still uses this worker and reports a bounded
    // failure while paused; false is not permission to reuse the input buffer.
    configure(contexts[0]);
    pause(true);
    assert!(!k.xma_work(contexts[0]));
    assert_eq!(k.xma.queue.lock().unwrap().pending.len(), 1);
    assert_eq!(k.xma_context(contexts[0]).unwrap().get(field::INPUT_BUFFER_0_VALID), 1);
    pause(false);
    drain();
    consumed(contexts[0]);
    configure(contexts[0]);
    assert!(k.xma_work(contexts[0]));
    consumed(contexts[0]);
    assert!(!k.xma_work(contexts[0] + 1));

    // Lock cancels a pending generation without consuming the input.
    pause(true);
    configure(contexts[0]);
    k.xma_mmio_write(CONTEXT_KICK, 1).unwrap();
    k.xma_mmio_write(CONTEXT_LOCK, 1).unwrap();
    pause(false);
    drain();
    assert_eq!(k.xma_context(contexts[0]).unwrap().get(field::INPUT_BUFFER_0_VALID), 1);

    // Queue stale work, clear, then release/reallocate the SAME slot. The old
    // jobs must not clear the new enabled bit or publish into the new lifetime.
    pause(true);
    k.xma_mmio_write(CONTEXT_KICK, 1).unwrap();
    k.xma_mmio_write(CONTEXT_CLEAR, 1).unwrap();
    assert_eq!(k.xma_context(contexts[0]).unwrap().get(field::INPUT_BUFFER_0_VALID), 0);
    configure(contexts[0]);
    k.xma_mmio_write(CONTEXT_KICK, 1).unwrap();
    k.xma_release(contexts[0]);
    let reused = k.xma_allocate();
    assert_eq!(reused, contexts[0]);
    configure(reused);
    k.xma_mmio_write(CONTEXT_KICK, 1).unwrap();
    assert_eq!(k.xma.queue.lock().unwrap().pending.len(), 3);
    pause(false);
    drain();
    consumed(reused);

    // The real blocking import cannot treat WORK_BUFFER_PTR == 0 as idle
    // while work is queued. Wait until the import is actually on the CV.
    pause(true);
    configure(reused);
    assert_eq!(k.xma_context(reused).unwrap().get(field::WORK_BUFFER_PTR), 0);
    k.xma_enable(reused).unwrap();
    let blocked = std::thread::spawn(move || {
        let mut guest = runtime_core::PPCContext::new_zeroed();
        guest.set_gpr(3, reused as u64);
        k.dispatch(crate::dispatch::ModuleId::XBOXKRNL, 0x235, &mut guest);
        guest.gpr32(3)
    });
    wait_state(|q| q.buffer_waiters != 0);
    assert!(!blocked.is_finished());
    pause(false);
    assert_eq!(blocked.join().unwrap(), 0);
    drain();
    consumed(reused);

    // Shutdown drains pending work, joins with no device locks held, and is
    // terminal. The cfg(test) pause cannot make shutdown wait indefinitely.
    pause(true);
    configure(reused);
    k.xma_enable(reused).unwrap();
    let held_slot = k.xma.slots[0].lock().unwrap();
    let first_shutdown = std::thread::spawn(move || k.xma_shutdown());
    wait_state(|q| q.stopped && q.in_flight == 1);
    let second_shutdown = std::thread::spawn(move || k.xma_shutdown());
    wait_state(|q| q.shutdown_waiters != 0);
    assert!(!first_shutdown.is_finished());
    assert!(!second_shutdown.is_finished());
    drop(held_slot);
    first_shutdown.join().unwrap().unwrap();
    second_shutdown.join().unwrap().unwrap();
    consumed(reused);
    assert!(k.xma_enable(reused).is_err());
    assert!(!k.xma_work(reused));
    k.xma_shutdown().unwrap();
    // Bounded terminal-state check, not a worker-recovery test: a poisoned slot
    // maps the real blocking import to E_FALSE instead of a second panic.
    let poisoned = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        let _slot = k.xma.slots[0].lock().unwrap();
        panic!("authored terminal slot poison");
    }));
    assert!(poisoned.is_err());
    let mut guest = runtime_core::PPCContext::new_zeroed();
    guest.set_gpr(3, reused as u64);
    k.dispatch(crate::dispatch::ModuleId::XBOXKRNL, 0x235, &mut guest);
    assert_eq!(guest.gpr32(3), 1);
    assert!(Kernel::try_global().is_none());
    println!(
        "XMA_DEVICE all320_ids=PASS repeated_operands=PASS stale_generations=PASS blocking_import=PASS concurrent_shutdown=PASS"
    );
}
