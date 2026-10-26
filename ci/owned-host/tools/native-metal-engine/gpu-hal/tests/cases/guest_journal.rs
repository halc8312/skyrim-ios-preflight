//! Resolve -> immutable per-store bytes+mask -> verified visibility evidence.
use super::guest_store_cases::{environment, key, pages};
use super::resolve_cache_cases::info;
use super::{DEADLINE, finish, open, setup};
use skyrim_native_gpu::edram::targets::GuestByteWrite;

fn expected(word: u32) -> Vec<GuestByteWrite> {
    let r = info(key());
    let mut bytes = std::collections::BTreeMap::new();
    for y in 0..r.rect.height {
        for x in 0..r.rect.width {
            let at = r.copy_dest_base
                + xenos_texture::address::tiled_2d(x as i32, y as i32, r.dest_pitch_aligned, 2)
                    as u32;
            for (n, b) in word.to_le_bytes().into_iter().enumerate() {
                assert!(bytes.insert(at + n as u32, b).is_none());
            }
        }
    }
    let mut runs: Vec<GuestByteWrite> = Vec::new();
    for (address, byte) in bytes {
        if let Some(run) = runs
            .last_mut()
            .filter(|r| r.address + r.bytes.len() as u32 == address)
        {
            run.bytes.push(byte);
        } else {
            runs.push(GuestByteWrite {
                address,
                bytes: vec![byte],
            });
        }
    }
    runs
}

#[test]
fn guest_journal_is_exact_immutable_and_never_publishes_abandoned_or_failed_work() {
    let (_lock, before) = setup();
    let mut gpu = open();
    let (mut cache, mut arena, mut memory, _) = environment(&mut gpu);
    let pages = pages();
    let req = info(key());
    let mut clear = req.clone();
    clear.copy_dest_extent_length = 0;
    clear.copy_control.color_clear_enable = true;
    let (first, second) = {
        let mut f = cache
            .begin(&mut gpu)
            .unwrap()
            .with_constants(&mut arena)
            .unwrap()
            .with_guest_memory(&mut memory)
            .unwrap();
        clear.color_clear = 0xff653511;
        f.resolve_pixels(&clear).unwrap();
        f.sync_guest_pages(&pages).unwrap();
        let a = f.resolve_guest_journal(&req).unwrap().unwrap();
        assert_eq!(a.serial(), None);
        clear.color_clear = 0xff997733;
        f.resolve_pixels(&clear).unwrap();
        f.sync_guest_pages(&pages).unwrap();
        let b = f.resolve_guest_journal(&req).unwrap().unwrap();
        f.submit().unwrap();
        (a, b)
    };
    assert_eq!(
        first.read(&mut gpu, DEADLINE).unwrap(),
        expected(0xff653511)
    );
    assert_eq!(
        second.read(&mut gpu, DEADLINE).unwrap(),
        expected(0xff997733)
    );
    assert_eq!(first.serial(), second.serial());
    assert!(!memory.gpu_newer_pages().is_empty());
    let mut other = open();
    assert!(first.read(&mut other, DEADLINE).is_err());
    drop(other);
    let abandoned = {
        let mut f = cache
            .begin(&mut gpu)
            .unwrap()
            .with_constants(&mut arena)
            .unwrap()
            .with_guest_memory(&mut memory)
            .unwrap();
        f.sync_guest_pages(&pages).unwrap();
        f.resolve_guest_journal(&req).unwrap().unwrap()
    };
    assert!(abandoned.read(&mut gpu, DEADLINE).is_err());
    assert_eq!(
        first.read(&mut gpu, DEADLINE).unwrap(),
        expected(0xff653511)
    );
    gpu.fail_next_submit();
    let failed = {
        let mut f = cache
            .begin(&mut gpu)
            .unwrap()
            .with_constants(&mut arena)
            .unwrap()
            .with_guest_memory(&mut memory)
            .unwrap();
        f.sync_guest_pages(&pages).unwrap();
        let j = f.resolve_guest_journal(&req).unwrap().unwrap();
        assert!(f.submit().is_err());
        j
    };
    assert!(failed.read(&mut gpu, DEADLINE).is_err());
    drop(gpu);
    finish(before);
}
