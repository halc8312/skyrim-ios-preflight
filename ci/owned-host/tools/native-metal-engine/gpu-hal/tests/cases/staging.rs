use super::{DEADLINE, U, finish, open_config, setup};
use skyrim_native_gpu::Config;

#[test]
fn complete_initialized_uploads_preserve_every_byte_generation_and_failure_atomicity() {
    let (_lock, before) = setup();
    let mut config = Config::default();
    config.max_pending_upload_bytes = 2048;
    config.retained_staging_bytes = 4096;
    config.retained_staging_buffers = 8;
    let mut gpu = open_config(config);
    for bytes in [&[][..], &[1, 2, 3][..]] {
        assert!(
            gpu.create_buffer_init("bad size", bytes, U::COPY_DST)
                .is_err()
        );
    }
    assert!(
        gpu.create_buffer_init("no permission", &[1; 4], U::COPY_SRC)
            .is_err()
    );
    assert!(
        gpu.create_buffer_init(
            "invalid mapping",
            &[1; 4],
            U::MAP_READ | U::UNIFORM | U::COPY_DST
        )
        .is_err()
    );
    let out = gpu
        .create_buffer("init snapshots", 1096, U::MAP_READ | U::COPY_DST)
        .unwrap();
    let mut e = gpu.encoder().unwrap();
    let mut expected = Vec::new();
    let mut offset = 0;
    for size in [4, 64, 1028] {
        let mut data: Vec<_> = (0..size).map(|i| (i * 37 + size) as u8).collect();
        expected.extend_from_slice(&data);
        let b = gpu
            .create_buffer_init("immutable generation", &data, U::COPY_SRC | U::COPY_DST)
            .unwrap();
        data.fill(0xEE); // Queued upload owns its snapshot, not this CPU slice.
        e.copy_buffer(&b, 0, &out, offset, size as u64).unwrap();
        offset += size as u64;
        drop(b);
    }
    let live = gpu.live_buffer_count();
    assert!(
        gpu.create_buffer_init("over budget", &[7; 2048], U::COPY_SRC | U::COPY_DST)
            .is_err()
    );
    assert_eq!(gpu.live_buffer_count(), live);
    let s = gpu.submit(vec![e.finish().unwrap()]).unwrap();
    gpu.wait(s, DEADLINE).unwrap();
    assert_eq!(gpu.read_buffer(&out, 0..1096, DEADLINE).unwrap(), expected);
    // Reusing a larger retired staging slot must not leak its tail to a new
    // exact-size destination; ordinary partial writes still see zero elsewhere.
    let small = gpu
        .create_buffer_init("small init", &[0x3C; 4], U::COPY_SRC | U::COPY_DST)
        .unwrap();
    let zero = gpu
        .create_buffer("partial still zeroed", 64, U::COPY_SRC | U::COPY_DST)
        .unwrap();
    gpu.write_buffer(&zero, 12, &[0xA5; 4]).unwrap();
    let mut e = gpu.encoder().unwrap();
    e.copy_buffer(&small, 0, &out, 0, 4).unwrap();
    e.copy_buffer(&zero, 0, &out, 4, 64).unwrap();
    gpu.submit(vec![e.finish().unwrap()]).unwrap();
    let got = gpu.read_buffer(&out, 0..68, DEADLINE).unwrap();
    let mut wanted = vec![0; 68];
    wanted[..4].fill(0x3C);
    wanted[16..20].fill(0xA5);
    assert_eq!(got, wanted);
    drop(gpu);
    finish(before);
}

#[test]
fn upload_pool_waits_for_observed_retirement_and_never_copies_old_bucket_tail() {
    let (_lock, before) = setup();
    let mut config = Config::default();
    config.retained_staging_bytes = 4096;
    config.retained_staging_buffers = 1;
    let mut gpu = open_config(config);
    let dst = gpu
        .create_buffer("upload destination", 512, U::COPY_SRC | U::COPY_DST)
        .unwrap();
    let out = gpu
        .create_buffer("ordered upload snapshots", 1536, U::COPY_DST | U::MAP_READ)
        .unwrap();
    gpu.delay_fence_observation(Some(0));
    gpu.write_buffer(&dst, 0, &[0xA5; 512]).unwrap();
    let mut first = gpu.encoder().unwrap();
    first.copy_buffer(&dst, 0, &out, 0, 512).unwrap();
    gpu.submit(vec![first.finish().unwrap()]).unwrap();
    gpu.write_buffer(&dst, 0, &[0x3C; 512]).unwrap();
    let mut second = gpu.encoder().unwrap();
    second.copy_buffer(&dst, 0, &out, 512, 512).unwrap();
    let serial = gpu.submit(vec![second.finish().unwrap()]).unwrap();
    gpu.poll().unwrap();
    let stats = gpu.staging_stats();
    assert_eq!(
        (stats.created, stats.reused, stats.retained_buffers),
        (2, 0, 0)
    );
    gpu.delay_fence_observation(None);
    gpu.wait(serial, DEADLINE).unwrap();
    gpu.poll().unwrap();
    assert_eq!(gpu.staging_stats().retained_buffers, 1);
    // The sole cached lease is the first (A5) allocation. A short upload must
    // not copy its old A5 tail over the destination's current 3C tail.
    gpu.write_buffer(&dst, 0, &[0x7E; 256]).unwrap();
    let mut third = gpu.encoder().unwrap();
    third.copy_buffer(&dst, 0, &out, 1024, 512).unwrap();
    gpu.submit(vec![third.finish().unwrap()]).unwrap();
    let bytes = gpu.read_buffer(&out, 0..1536, DEADLINE).unwrap();
    assert_eq!(&bytes[..512], &[0xA5; 512]);
    assert_eq!(&bytes[512..1024], &[0x3C; 512]);
    assert_eq!(&bytes[1024..1280], &[0x7E; 256]);
    assert_eq!(&bytes[1280..], &[0x3C; 256]);
    let stats = gpu.staging_stats();
    assert_eq!(
        (stats.created, stats.reused, stats.retained_buffers),
        (2, 1, 1)
    );
    gpu.trim_staging_pool();
    drop(dst);
    drop(out);
    gpu.poll().unwrap();
    assert_eq!(gpu.live_buffer_count(), 0);
    assert_eq!(gpu.native_counters().buffers.read(), 0);
    drop(gpu);
    finish(before);
}

#[test]
fn warmed_upload_pool_has_bounded_native_allocations_and_releases_idle_budget() {
    let (_lock, before) = setup();
    let mut config = Config::default();
    config.max_staging_buffer_bytes = 128;
    config.retained_staging_bytes = 128;
    config.retained_staging_buffers = 2;
    let mut gpu = open_config(config);
    let dst = gpu
        .create_buffer("small destination", 128, U::COPY_SRC | U::COPY_DST)
        .unwrap();
    let out = gpu
        .create_buffer("small snapshots", 128, U::COPY_DST | U::MAP_READ)
        .unwrap();
    for round in 0..30u8 {
        let size = if round < 2 { 64 } else { 128 };
        // Metal execution completion can precede HAL fence observation.
        // Force that distinction instead of depending on the host timing.
        #[cfg(target_vendor = "apple")]
        gpu.delay_fence_observation(Some(gpu.verified_serial()));
        gpu.write_buffer(&dst, 0, &vec![round; size]).unwrap();
        let mut e = gpu.encoder().unwrap();
        e.copy_buffer(&dst, 0, &out, 0, size as u64).unwrap();
        gpu.submit(vec![e.finish().unwrap()]).unwrap();
        assert_eq!(
            gpu.read_buffer(&out, 0..size as u64, DEADLINE).unwrap(),
            vec![round; size]
        );
        #[cfg(target_vendor = "apple")]
        {
            assert!(gpu.in_flight_count() > 0);
            assert_eq!(gpu.staging_stats().retained_buffers, 0);
            gpu.delay_fence_observation(None);
        }
        // Readback proves the bytes, not that upload memory is reusable.
        // Keep the exact allocation/reuse assertions after bounded retirement.
        let deadline = std::time::Instant::now() + DEADLINE;
        while gpu.in_flight_count() != 0 && std::time::Instant::now() < deadline {
            gpu.poll().unwrap();
            if gpu.in_flight_count() != 0 {
                std::thread::sleep(std::time::Duration::from_millis(1));
            }
        }
        assert_eq!(gpu.in_flight_count(), 0, "upload retirement deadline");
        let stats = gpu.staging_stats();
        assert_eq!(stats.created, if round < 2 { 1 } else { 2 });
        assert_eq!(stats.retained_buffers, 1);
        assert_eq!(stats.retained_bytes, size as u64);
        assert_eq!(gpu.native_counters().buffers.read(), 3);
    }
    gpu.trim_staging_pool();
    assert_eq!(gpu.native_counters().buffers.read(), 2);
    assert_eq!(gpu.staging_stats().resident_bytes, 0);
    drop(gpu);
    finish(before);
}

#[test]
fn upload_pool_never_recycles_after_unknown_submit_failure_or_while_pending() {
    let (_lock, before) = setup();
    let mut config = Config::default();
    config.max_staging_buffer_bytes = 128;
    config.retained_staging_bytes = 128;
    config.drop_timeout = std::time::Duration::from_millis(1);
    let mut gpu = open_config(config);
    let dst = gpu
        .create_buffer("pending destination", 128, U::COPY_DST)
        .unwrap();
    gpu.write_buffer(&dst, 0, &[1; 128]).unwrap();
    assert!(gpu.write_buffer(&dst, 0, &[2; 4]).is_err());
    assert_eq!(gpu.staging_stats().pending_leases, 1);
    gpu.trim_staging_pool();
    assert_eq!(gpu.staging_stats().resident_bytes, 128);
    gpu.fail_next_submit();
    assert!(gpu.submit(vec![]).is_err());
    assert_eq!(gpu.staging_stats().retained_buffers, 0);
    assert_eq!(gpu.in_flight_count(), 1);
    let count = gpu.native_counters().buffers.read();
    gpu.trim_staging_pool();
    assert_eq!(gpu.native_counters().buffers.read(), count);
    assert!(gpu.write_buffer(&dst, 0, &[3; 4]).is_err());
    drop(gpu);
    finish(before);
}

#[test]
fn upload_buckets_never_reject_an_exact_fit_or_pin_a_large_idle_lease_for_a_tiny_write() {
    let (_lock, before) = setup();
    for pooled in [false, true] {
        let mut cfg = Config::default();
        cfg.max_staging_buffer_bytes = 150;
        cfg.retained_staging_bytes = if pooled { 150 } else { 0 };
        cfg.retained_staging_buffers = 2;
        let mut gpu = open_config(cfg);
        let dst = gpu
            .create_buffer("exact-fit destination", 120, U::COPY_SRC | U::COPY_DST)
            .unwrap();
        let out = gpu
            .create_buffer("exact-fit readback", 120, U::COPY_DST | U::MAP_READ)
            .unwrap();
        gpu.write_buffer(&dst, 0, &[11; 40]).unwrap();
        gpu.write_buffer(&dst, 40, &[29; 80]).unwrap();
        assert!(gpu.staging_stats().resident_bytes <= 150);
        let mut e = gpu.encoder().unwrap();
        e.copy_buffer(&dst, 0, &out, 0, 120).unwrap();
        gpu.submit(vec![e.finish().unwrap()]).unwrap();
        assert_eq!(
            gpu.read_buffer(&out, 0..120, DEADLINE).unwrap(),
            [vec![11; 40], vec![29; 80]].concat()
        );
    }
    let mut cfg = Config::default();
    cfg.max_staging_buffer_bytes = 8192;
    cfg.retained_staging_bytes = 8192;
    let mut gpu = open_config(cfg);
    let dst = gpu
        .create_buffer("separate sizes", 4096, U::COPY_DST)
        .unwrap();
    gpu.write_buffer(&dst, 0, &[1; 4096]).unwrap();
    let serial = gpu.submit(vec![]).unwrap();
    gpu.wait(serial, DEADLINE).unwrap();
    gpu.poll().unwrap();
    assert_eq!(gpu.staging_stats().retained_bytes, 4096);
    gpu.write_buffer(&dst, 0, &[2; 4]).unwrap();
    gpu.write_buffer(&dst, 0, &[3; 4096]).unwrap();
    let stats = gpu.staging_stats();
    assert_eq!(
        (stats.created, stats.reused, stats.resident_bytes),
        (2, 1, 4100)
    );
    let serial = gpu.submit(vec![]).unwrap();
    gpu.wait(serial, DEADLINE).unwrap();
    drop(gpu);
    finish(before);
}
