use super::{DEADLINE, U, finish, open, open_config, setup};
use skyrim_native_gpu::{Config, OrderedUpload};
use xenos_gfx::constant_segments::{AdmissionError, PatchSegment, StageConstants};

#[test]
fn reservation_growth_rejection_preserves_backing_and_exact_stream_position() {
    let (_lock, before) = setup();
    let mut config = Config::default();
    config.max_pending_upload_bytes = 16;
    let mut gpu = open_config(config);
    let dst = gpu
        .create_buffer("reserved destination", 16, U::COPY_DST | U::COPY_SRC)
        .unwrap();
    let out = gpu
        .create_buffer("reserved output", 16, U::COPY_DST | U::MAP_READ)
        .unwrap();
    let mut e = gpu.encoder().unwrap();
    let mut token = gpu.reserve_ordered_upload(&mut e, 4).unwrap();
    gpu.grow_ordered_upload(&mut e, &mut token, 8).unwrap();
    assert_eq!(token.capacity(), 8);
    gpu.write_buffer_ordered(&mut e, &dst, 8, &[3; 8]).unwrap();
    assert_eq!(e.ordered_staging_bytes(), 8);
    assert!(gpu.grow_ordered_upload(&mut e, &mut token, 12).is_err());
    assert!(gpu.write_buffer_ordered(&mut e, &dst, 0, &[4; 4]).is_err());
    assert_eq!(token.capacity(), 8);
    assert_eq!(e.ordered_reserved_bytes(), 8);
    // Both growth and foreign destinations fail without consuming the token.
    let mut foreign_gpu = open();
    let foreign = foreign_gpu
        .create_buffer("foreign", 4, U::COPY_DST)
        .unwrap();
    assert!(
        gpu.write_buffers_reserved(
            &mut e,
            &mut token,
            &[OrderedUpload {
                destination: &foreign,
                offset: 0,
                data: &[1; 4]
            }]
        )
        .is_err()
    );
    for (offset, data) in [(1, &b"1234"[..]), (0, &b"123456789012"[..])] {
        assert!(
            gpu.write_buffers_reserved(
                &mut e,
                &mut token,
                &[OrderedUpload {
                    destination: &dst,
                    offset,
                    data
                }]
            )
            .is_err()
        );
        assert_eq!(token.capacity(), 8);
    }
    let mut other = gpu.encoder().unwrap();
    assert!(
        gpu.write_buffers_reserved(
            &mut other,
            &mut token,
            &[OrderedUpload {
                destination: &dst,
                offset: 0,
                data: &[1; 4]
            }]
        )
        .is_err()
    );
    let created = gpu.staging_stats().created;
    gpu.write_buffers_reserved(
        &mut e,
        &mut token,
        &[OrderedUpload {
            destination: &dst,
            offset: 0,
            data: &[9; 8],
        }],
    )
    .unwrap();
    assert_eq!(
        gpu.staging_stats().created,
        created,
        "commit cannot allocate a staging lease"
    );
    assert_eq!(token.capacity(), 0);
    assert_eq!(e.ordered_reserved_bytes(), 0);
    assert!(gpu.write_buffers_reserved(&mut e, &mut token, &[]).is_err());
    e.copy_buffer(&dst, 0, &out, 0, 16).unwrap();
    gpu.submit(vec![e.finish().unwrap()]).unwrap();
    assert_eq!(
        gpu.read_buffer(&out, 0..16, DEADLINE).unwrap(),
        [vec![9; 8], vec![3; 8]].concat()
    );
    drop(other);
    drop(foreign_gpu);
    drop(gpu);
    finish(before);
}

#[test]
fn reservation_abandonment_releases_once_and_encoder_finish_rejects_uncommitted_work() {
    let (_lock, before) = setup();
    let mut gpu = open();
    let mut e = gpu.encoder().unwrap();
    let token = gpu.reserve_ordered_upload(&mut e, 16).unwrap();
    assert_eq!(gpu.staging_stats().resident_bytes, 16);
    drop(token);
    assert_eq!(e.ordered_reserved_bytes(), 0);
    assert_eq!(gpu.staging_stats().resident_bytes, 0);
    let mut token = gpu.reserve_ordered_upload(&mut e, 16).unwrap();
    assert!(e.finish().is_err());
    assert_eq!(token.capacity(), 0);
    assert_eq!(gpu.staging_stats().resident_bytes, 0);
    let mut next = gpu.encoder().unwrap();
    assert!(gpu.grow_ordered_upload(&mut next, &mut token, 16).is_err());
    let pending = gpu.reserve_ordered_upload(&mut next, 32).unwrap();
    drop(next);
    assert_eq!(pending.capacity(), 0);
    assert_eq!(gpu.staging_stats().resident_bytes, 0);
    drop(pending);
    drop(token);
    // Known abandoned unsubmitted work is not an unknown submission.
    let clean = gpu.encoder().unwrap();
    let s = gpu.submit(vec![clean.finish().unwrap()]).unwrap();
    gpu.wait(s, DEADLINE).unwrap();
    drop(gpu);
    finish(before);
}

#[test]
fn reservation_cannot_be_stolen_by_other_encoders_or_queue_prefix_uploads() {
    let (_lock, before) = setup();
    let mut config = Config::default();
    config.max_staging_buffer_bytes = 16;
    let mut gpu = open_config(config);
    let dst = gpu
        .create_buffer("global reserved destination", 16, U::COPY_DST)
        .unwrap();
    let mut a = gpu.encoder().unwrap();
    let mut b = gpu.encoder().unwrap();
    let mut token = gpu.reserve_ordered_upload(&mut a, 16).unwrap();
    assert!(gpu.reserve_ordered_upload(&mut b, 4).is_err());
    assert!(gpu.write_buffer_ordered(&mut b, &dst, 0, &[2; 4]).is_err());
    assert!(gpu.write_buffer(&dst, 0, &[3; 4]).is_err());
    assert_eq!(token.capacity(), 16);
    gpu.write_buffers_reserved(
        &mut a,
        &mut token,
        &[OrderedUpload {
            destination: &dst,
            offset: 0,
            data: &[7; 16],
        }],
    )
    .unwrap();
    let s = gpu.submit(vec![a.finish().unwrap()]).unwrap();
    gpu.wait(s, DEADLINE).unwrap();
    gpu.poll().unwrap();
    assert_eq!(gpu.staging_stats().resident_bytes, 0);
    gpu.write_buffer_ordered(&mut b, &dst, 0, &[8; 4]).unwrap();
    drop(b);
    drop(gpu);
    finish(before);
}

#[test]
fn reserved_snapshots_follow_real_retirement_and_quarantine_on_unknown_submit() {
    let (_lock, before) = setup();
    let mut config = Config::default();
    config.retained_staging_bytes = 128;
    config.drop_timeout = std::time::Duration::from_millis(1);
    let mut gpu = open_config(config);
    let dst = gpu
        .create_buffer("reserved flight", 64, U::COPY_DST)
        .unwrap();
    let mut e = gpu.encoder().unwrap();
    let mut token = gpu.reserve_ordered_upload(&mut e, 64).unwrap();
    gpu.write_buffers_reserved(
        &mut e,
        &mut token,
        &[OrderedUpload {
            destination: &dst,
            offset: 0,
            data: &[7; 64],
        }],
    )
    .unwrap();
    gpu.delay_fence_observation(Some(0));
    let s = gpu.submit(vec![e.finish().unwrap()]).unwrap();
    gpu.poll().unwrap();
    assert_eq!(gpu.staging_stats().retained_buffers, 0);
    assert_eq!(gpu.staging_stats().resident_bytes, 64);
    gpu.delay_fence_observation(None);
    gpu.wait(s, DEADLINE).unwrap();
    gpu.poll().unwrap();
    assert_eq!(gpu.staging_stats().retained_buffers, 1);
    let mut e = gpu.encoder().unwrap();
    let mut token = gpu.reserve_ordered_upload(&mut e, 64).unwrap();
    assert_eq!(gpu.staging_stats().retained_buffers, 0);
    gpu.write_buffers_reserved(
        &mut e,
        &mut token,
        &[OrderedUpload {
            destination: &dst,
            offset: 0,
            data: &[8; 64],
        }],
    )
    .unwrap();
    gpu.fail_next_submit();
    assert!(gpu.submit(vec![e.finish().unwrap()]).is_err());
    gpu.trim_staging_pool();
    assert_eq!(gpu.staging_stats().resident_bytes, 64);
    assert_eq!(gpu.staging_stats().retained_buffers, 0);
    drop(gpu);
    finish(before);
}

#[test]
fn constant_admission_rejects_staging_exhaustion_before_mutation_and_commits_previous_draws() {
    let (_lock, before) = setup();
    let mut config = Config::default();
    config.max_pending_upload_bytes = 1 << 20;
    let mut gpu = open_config(config);
    let capacity = 2 << 20;
    let mut stream = gpu.constant_stream(capacity).unwrap();
    let mut segment = PatchSegment::new(capacity, stream.alignment()).unwrap();
    let mut e = gpu.encoder().unwrap();
    let mut token = None;
    let mut accepted = Vec::new();
    for value in 0..300u32 {
        let data: Vec<u8> = (0..1024u32)
            .flat_map(|lane| (lane ^ value.wrapping_mul(0x1010101)).to_le_bytes())
            .collect();
        let prior = (
            segment.used_bytes(),
            segment.record_count(),
            segment.changed_bytes(),
        );
        let result = segment.push_draw_with_admission(
            &[StageConstants {
                stage: 0,
                data: &data,
                window: 4096,
            }],
            |demand| {
                if let Some(token) = token.as_mut() {
                    gpu.grow_ordered_upload(&mut e, token, demand.upload_bytes())
                } else {
                    token = Some(gpu.reserve_ordered_upload(&mut e, demand.upload_bytes())?);
                    Ok(())
                }
            },
        );
        match result {
            Ok(blocks) => accepted.push((blocks[0].unwrap(), data)),
            Err(AdmissionError::Reservation(_)) => {
                assert_eq!(
                    (
                        segment.used_bytes(),
                        segment.record_count(),
                        segment.changed_bytes()
                    ),
                    prior
                );
                assert!(value > 100 && value < 300);
                break;
            }
            Err(e) => panic!("unexpected CPU capacity failure: {e:?}"),
        }
    }
    assert!(
        accepted.len() < 300,
        "300 full blocks exceed the actual snapshot budget"
    );
    let out = gpu
        .create_buffer(
            "reserved constants exact oracle",
            capacity,
            U::COPY_DST | U::MAP_READ,
        )
        .unwrap();
    let sealed = segment.seal().unwrap();
    let created = gpu.staging_stats().created;
    let prepared = stream
        .encode_reserved(&mut gpu, &mut e, &sealed, &mut token)
        .unwrap();
    assert!(token.is_none());
    assert_eq!(gpu.staging_stats().created, created);
    for (block, _) in &accepted {
        let (buffer, offset, size) = prepared.window(*block).unwrap();
        e.copy_buffer(buffer, offset, &out, offset, size).unwrap();
    }
    gpu.submit(vec![e.finish().unwrap()]).unwrap();
    let got = gpu.read_buffer(&out, 0..capacity, DEADLINE).unwrap();
    for (block, expected) in accepted {
        let start = block.offset() as usize;
        assert_eq!(&got[start..start + expected.len()], expected);
    }
    drop(gpu);
    finish(before);
}

#[test]
fn reservation_growth_is_amortized_without_retired_pool_and_global_failure_keeps_old_lease() {
    let (_lock, before) = setup();
    let mut config = Config::default();
    config.max_staging_buffer_bytes = 6144;
    let mut gpu = open_config(config);
    let mut e = gpu.encoder().unwrap();
    let mut token = gpu.reserve_ordered_upload(&mut e, 4).unwrap();
    for bytes in (8..=4096).step_by(4) {
        gpu.grow_ordered_upload(&mut e, &mut token, bytes).unwrap();
    }
    assert_eq!(token.capacity(), 4096);
    assert_eq!(
        gpu.staging_stats().created,
        11,
        "power-of-two growth, not 1024 per-draw allocations"
    );
    assert!(gpu.grow_ordered_upload(&mut e, &mut token, 4100).is_err());
    assert_eq!(token.capacity(), 4096);
    assert_eq!(gpu.staging_stats().resident_bytes, 4096);
    assert_eq!(e.ordered_reserved_bytes(), 4096);
    drop(e);
    drop(token);
    assert_eq!(gpu.staging_stats().resident_bytes, 0);
    drop(gpu);
    finish(before);
}
