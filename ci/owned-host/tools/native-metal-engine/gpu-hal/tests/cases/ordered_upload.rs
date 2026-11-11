use super::{DEADLINE, U, finish, open, open_config, setup};
use skyrim_native_gpu::Config;

#[test]
fn ordered_uploads_have_exact_stream_position_without_changing_queue_prefix_writes() {
    let (_lock, before) = setup();
    let mut gpu = open();
    let dst = gpu
        .create_buffer_init(
            "ordered data",
            &1u32.to_le_bytes(),
            U::COPY_SRC | U::COPY_DST,
        )
        .unwrap();
    let out = gpu
        .create_buffer("ordered snapshots", 12, U::COPY_DST | U::MAP_READ)
        .unwrap();
    let mut e = gpu.encoder().unwrap();
    e.copy_buffer(&dst, 0, &out, 0, 4).unwrap();
    let mut data = 2u32.to_le_bytes();
    gpu.write_buffer_ordered(&mut e, &dst, 0, &data).unwrap();
    data.fill(0xFF);
    e.copy_buffer(&dst, 0, &out, 4, 4).unwrap();
    gpu.write_buffer(&dst, 0, &3u32.to_le_bytes()).unwrap(); // Still precedes ALL submitted commands.
    gpu.write_buffer_ordered(&mut e, &dst, 0, &4u32.to_le_bytes())
        .unwrap();
    e.copy_buffer(&dst, 0, &out, 8, 4).unwrap();
    assert_eq!(e.ordered_upload_bytes(), 8);
    let s = gpu.submit(vec![e.finish().unwrap()]).unwrap();
    assert_eq!(s, 1);
    let got = gpu.read_buffer(&out, 0..12, DEADLINE).unwrap();
    assert_eq!(
        got,
        [3u32, 2, 4]
            .into_iter()
            .flat_map(u32::to_le_bytes)
            .collect::<Vec<_>>()
    );
    drop(gpu);
    finish(before);
}

#[test]
fn ordered_staging_belongs_to_its_encoder_not_an_unrelated_earlier_submit() {
    let (_lock, before) = setup();
    let mut config = Config::default();
    config.retained_staging_bytes = 1024;
    let mut gpu = open_config(config);
    let dst = gpu
        .create_buffer("ordered A data", 4, U::COPY_SRC | U::COPY_DST)
        .unwrap();
    let out = gpu
        .create_buffer("ordered A output", 4, U::MAP_READ | U::COPY_DST)
        .unwrap();
    let mut a = gpu.encoder().unwrap();
    gpu.write_buffer_ordered(&mut a, &dst, 0, &7u32.to_le_bytes())
        .unwrap();
    a.copy_buffer(&dst, 0, &out, 0, 4).unwrap();
    let b = gpu.encoder().unwrap();
    let s = gpu.submit(vec![b.finish().unwrap()]).unwrap();
    gpu.wait(s, DEADLINE).unwrap();
    super::retire_all(&mut gpu);
    assert_eq!(gpu.staging_stats().resident_bytes, 4);
    assert_eq!(gpu.staging_stats().retained_buffers, 0);
    // A is still unsubmitted. Trimming/reusing the prefix pool cannot free A's lease.
    gpu.trim_staging_pool();
    assert_eq!(gpu.staging_stats().resident_bytes, 4);
    gpu.submit(vec![a.finish().unwrap()]).unwrap();
    assert_eq!(
        gpu.read_buffer(&out, 0..4, DEADLINE).unwrap(),
        7u32.to_le_bytes()
    );
    super::retire_all(&mut gpu);
    assert_eq!(gpu.staging_stats().resident_bytes, 4);
    assert_eq!(gpu.staging_stats().retained_buffers, 1);
    let reused = gpu.staging_stats().reused;
    let mut next = gpu.encoder().unwrap();
    gpu.write_buffer_ordered(&mut next, &dst, 0, &[8; 4])
        .unwrap();
    assert_eq!(gpu.staging_stats().reused, reused + 1);
    drop(next);
    assert_eq!(gpu.staging_stats().resident_bytes, 0);
    drop(gpu);
    finish(before);
}

#[test]
fn ordered_upload_budget_abandonment_retirement_and_unknown_submission_preserve_lifetimes() {
    let (_lock, before) = setup();
    let mut config = Config::default();
    config.max_pending_upload_bytes = 8;
    config.drop_timeout = std::time::Duration::from_millis(1);
    let mut gpu = open_config(config);
    let dst = gpu
        .create_buffer("bounded ordered data", 16, U::COPY_DST)
        .unwrap();
    let mut e = gpu.encoder().unwrap();
    gpu.write_buffer_ordered(&mut e, &dst, 0, &[1; 4]).unwrap();
    assert!(gpu.write_buffer_ordered(&mut e, &dst, 0, &[2; 8]).is_err());
    assert_eq!(e.ordered_upload_bytes(), 4);
    assert!(gpu.write_buffer_ordered(&mut e, &dst, 1, &[2; 4]).is_err());
    let mut other = open();
    let mut foreign = other.encoder().unwrap();
    assert!(
        gpu.write_buffer_ordered(&mut foreign, &dst, 0, &[1; 4])
            .is_err()
    );
    let other_buffer = other
        .create_buffer("foreign ordered destination", 4, U::COPY_DST)
        .unwrap();
    assert!(
        gpu.write_buffer_ordered(&mut e, &other_buffer, 0, &[1; 4])
            .is_err()
    );
    drop(foreign);
    assert_eq!(gpu.staging_stats().resident_bytes, 4);
    drop(e);
    assert_eq!(gpu.staging_stats().resident_bytes, 0);
    let mut e = gpu.encoder().unwrap();
    gpu.write_buffer_ordered(&mut e, &dst, 0, &[2; 8]).unwrap();
    let abandoned = e.finish().unwrap();
    assert_eq!(gpu.staging_stats().resident_bytes, 8);
    drop(abandoned);
    assert_eq!(gpu.staging_stats().resident_bytes, 0);
    let mut e = gpu.encoder().unwrap();
    gpu.write_buffer_ordered(&mut e, &dst, 0, &[3; 8]).unwrap();
    gpu.delay_fence_observation(Some(0));
    let s = gpu.submit(vec![e.finish().unwrap()]).unwrap();
    gpu.poll().unwrap();
    assert_eq!(gpu.staging_stats().resident_bytes, 8);
    gpu.delay_fence_observation(None);
    gpu.wait(s, DEADLINE).unwrap();
    super::retire_all(&mut gpu);
    assert_eq!(gpu.staging_stats().resident_bytes, 0);
    let mut e = gpu.encoder().unwrap();
    gpu.write_buffer_ordered(&mut e, &dst, 0, &[4; 8]).unwrap();
    gpu.fail_next_submit();
    assert!(gpu.submit(vec![e.finish().unwrap()]).is_err());
    assert_eq!(gpu.staging_stats().resident_bytes, 8);
    gpu.trim_staging_pool();
    assert_eq!(gpu.staging_stats().resident_bytes, 8);
    assert!(gpu.encoder().is_err());
    drop(other);
    drop(gpu);
    finish(before);
}

#[test]
fn ordered_upload_batches_validate_later_inputs_atomically_and_apply_overlaps_in_order() {
    use skyrim_native_gpu::OrderedUpload;
    let (_lock, before) = setup();
    let mut gpu = open();
    let b = gpu
        .create_buffer("batch destination", 16, U::COPY_DST | U::COPY_SRC)
        .unwrap();
    let r = gpu
        .create_buffer("batch readback", 16, U::COPY_DST | U::MAP_READ)
        .unwrap();
    let mut e = gpu.encoder().unwrap();
    let baseline = gpu.live_buffer_count();
    assert!(
        gpu.write_buffers_ordered(
            &mut e,
            &[
                OrderedUpload {
                    destination: &b,
                    offset: 0,
                    data: &[1; 16]
                },
                OrderedUpload {
                    destination: &b,
                    offset: 13,
                    data: &[2; 4]
                }
            ]
        )
        .is_err()
    );
    assert_eq!(e.ordered_upload_bytes(), 0);
    assert_eq!(gpu.live_buffer_count(), baseline);
    gpu.write_buffers_ordered(
        &mut e,
        &[
            OrderedUpload {
                destination: &b,
                offset: 0,
                data: &[1; 16],
            },
            OrderedUpload {
                destination: &b,
                offset: 4,
                data: &[2; 8],
            },
            OrderedUpload {
                destination: &b,
                offset: 8,
                data: &[3; 4],
            },
        ],
    )
    .unwrap();
    assert_eq!(e.ordered_upload_bytes(), 28);
    assert_eq!(
        gpu.live_buffer_count(),
        baseline + 1,
        "one retained lease for all three snapshots"
    );
    e.copy_buffer(&b, 0, &r, 0, 16).unwrap();
    gpu.submit(vec![e.finish().unwrap()]).unwrap();
    assert_eq!(
        gpu.read_buffer(&r, 0..16, DEADLINE).unwrap(),
        [1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 1, 1, 1, 1]
    );
    drop(gpu);
    finish(before);
}

#[test]
fn ordered_staging_budget_polls_retired_flights_but_never_waits_on_unsubmitted_work() {
    let (_lock, before) = setup();
    let mut c = Config::default();
    c.max_staging_buffer_bytes = 8;
    c.retained_staging_bytes = 8;
    c.backpressure_timeout = std::time::Duration::from_millis(20);
    let mut gpu = open_config(c);
    let dst = gpu
        .create_buffer("small staging budget destination", 8, U::COPY_DST)
        .unwrap();
    let mut e = gpu.encoder().unwrap();
    gpu.write_buffer_ordered(&mut e, &dst, 0, &[1; 8]).unwrap();
    let serial = gpu.submit(vec![e.finish().unwrap()]).unwrap();
    // No explicit wait/poll: a full staging budget must retire prior work itself.
    let mut next = gpu.encoder().unwrap();
    gpu.write_buffer_ordered(&mut next, &dst, 0, &[2; 8])
        .unwrap();
    assert!(gpu.verified_serial() >= serial);
    assert_eq!(gpu.staging_stats().reused, 1);
    let start = std::time::Instant::now();
    assert!(
        gpu.write_buffer_ordered(&mut next, &dst, 0, &[3; 4])
            .is_err()
    );
    assert_eq!(next.ordered_upload_bytes(), 8);
    assert!(start.elapsed() < std::time::Duration::from_secs(1));
    drop(next);
    assert_eq!(gpu.staging_stats().resident_bytes, 0);
    drop(gpu);
    finish(before);
}

#[test]
fn ordered_reserved_capacity_bounds_pool_buckets_and_skips_unreclaimable_flights() {
    let (_lock, before) = setup();
    let mut c = Config::default();
    c.max_pending_upload_bytes = 32;
    c.max_staging_buffer_bytes = 64;
    c.retained_staging_bytes = 64;
    let mut gpu = open_config(c);
    let dst = gpu
        .create_buffer("bounded bucket destination", 32, U::COPY_DST)
        .unwrap();
    gpu.write_buffer(&dst, 0, &[1; 16]).unwrap();
    let serial = gpu.submit(vec![]).unwrap();
    gpu.wait(serial, DEADLINE).unwrap();
    super::retire_all(&mut gpu);
    assert_eq!(gpu.staging_stats().retained_bytes, 16);
    let mut e = gpu.encoder().unwrap();
    gpu.write_buffer_ordered(&mut e, &dst, 0, &[2; 8]).unwrap();
    assert_eq!(
        e.ordered_staging_bytes(),
        8,
        "must not borrow the idle16B bucket for8B"
    );
    gpu.write_buffer_ordered(&mut e, &dst, 8, &[3; 4]).unwrap();
    assert_eq!(e.ordered_staging_bytes(), 12);
    drop(e);
    drop(gpu);
    let mut c = Config::default();
    c.max_staging_buffer_bytes = 8;
    c.backpressure_timeout = std::time::Duration::from_millis(50);
    let mut gpu = open_config(c);
    let dst = gpu
        .create_buffer("irrelevant flight", 8, U::COPY_DST)
        .unwrap();
    gpu.delay_fence_observation(Some(0));
    let serial = gpu.submit(vec![]).unwrap();
    let mut e = gpu.encoder().unwrap();
    gpu.write_buffer_ordered(&mut e, &dst, 0, &[1; 8]).unwrap();
    let waits = gpu.staging_stats().waits;
    assert!(gpu.write_buffer_ordered(&mut e, &dst, 0, &[2; 4]).is_err());
    assert_eq!(gpu.staging_stats().waits, waits);
    assert_eq!(e.ordered_staging_bytes(), 8);
    drop(e);
    gpu.delay_fence_observation(None);
    gpu.wait(serial, DEADLINE).unwrap();
    drop(gpu);
    finish(before);
}

#[test]
fn caught_submission_unwind_fail_stops_owner_and_keeps_uncertain_leases_quarantined() {
    let (_lock, before) = setup();
    let mut config = Config::default();
    config.drop_timeout = std::time::Duration::from_millis(1);
    let mut gpu = open_config(config);
    let dst = gpu
        .create_buffer("unwind destination", 8, U::COPY_DST)
        .unwrap();
    let mut encoder = gpu.encoder().unwrap();
    gpu.write_buffer_ordered(&mut encoder, &dst, 0, &[0x57; 8])
        .unwrap();
    let commands = encoder.finish().unwrap();
    gpu.unwind_next_submit();
    let result =
        std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| gpu.submit(vec![commands])));
    assert!(result.is_err());
    assert!(
        gpu.encoder().is_err(),
        "owner must fail-stop after caught submission unwind"
    );
    assert!(gpu.poll().is_err());
    let retained = gpu.staging_stats().resident_bytes;
    assert_eq!(retained, 8);
    gpu.trim_staging_pool();
    assert_eq!(gpu.staging_stats().resident_bytes, retained);
    drop(gpu);
    finish(before);
}
