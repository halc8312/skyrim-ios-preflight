//! Actual owner-thread queue -> translated draws -> resolve -> CPU publication.
//! Every instruction, texture, vertex and page is authored; no game files.
use super::{DEADLINE, finish, open, setup};
use super::{
    guest_store_cases as store,
    pipeline_cases::normalized_key,
    resolve_cache_cases::{info, selection},
};
use skyrim_native_gpu::{
    Config, Error,
    edram::targets::{GuestPage, TargetOptions},
    session::{
        Batch, CpuCommit, DrawPacket, IndexData, NativeSession, NativeWorker, NoCpuCommit,
        OfferError, Packet, Program, WaitError,
    },
    types as t,
};
use std::{
    collections::BTreeMap,
    sync::{Arc, Condvar, Mutex},
    time::Duration,
};
use xenos_shader::{TranslationOptions, bindings};

fn draw(color: [f32; 4], indexed: bool) -> DrawPacket {
    let options = TranslationOptions {
        vs_output_interpolators: 1,
        ps_interpolator_count: Some(1),
        ..Default::default()
    };
    let mut bytes = [0; 4096];
    let vertices: Vec<_> = [[-1.0f32, -1., 0.5], [3., -1., 0.5], [-1., 3., 0.5]]
        .into_iter()
        .flatten()
        .flat_map(f32::to_be_bytes)
        .collect();
    bytes[..vertices.len()].copy_from_slice(&vertices);
    let mut fetch = vec![0u8; 768];
    for (i, w) in [0x200003u32, 2 | 9 * 4 << 2].into_iter().enumerate() {
        fetch[i * 4..i * 4 + 4].copy_from_slice(&w.to_le_bytes());
    }
    let mut system = bindings::SystemConstants::default();
    if indexed {
        system.vertex_index_max = 2;
    }
    DrawPacket {
        vertex: Program {
            code: Arc::from([
                0x00013001, 0x00002200, 0, 0x00081000, 0x00390A88, 3, 0xC80F803E, 0, 0xE2010100,
                0xC80F8000, 0, 0x22000000,
            ]),
            options: options.clone(),
        },
        pixel: Some(Program {
            code: Arc::from([0x00001001, 0x00002200, 0, 0xC80F8000, 0, 0xE2000000]),
            options,
        }),
        pipeline: normalized_key(),
        targets: selection(store::key()),
        min_height: 16,
        constants: [
            Arc::from(
                color
                    .into_iter()
                    .flat_map(f32::to_le_bytes)
                    .collect::<Vec<_>>(),
            ),
            Arc::from([]),
            Arc::from([0; 160]),
            Arc::from(fetch),
            Arc::from(system.as_bytes()),
        ],
        pages: Arc::from([GuestPage {
            address: 0x200000,
            generation: None,
            bytes: Arc::new(bytes),
        }]),
        indices: indexed.then(|| IndexData {
            bytes: Arc::from(
                [0u16, 1, 2]
                    .into_iter()
                    .flat_map(u16::to_le_bytes)
                    .collect::<Vec<_>>(),
            ),
            format: t::IndexFormat::Uint16,
            base_vertex: 0,
        }),
        vertices: 0..3,
        instances: 0..1,
        viewport: None,
        scissor: None,
        blend: t::Color::TRANSPARENT,
        stencil_reference: 0,
    }
}
fn green_batch(indexed: bool) -> Batch {
    Batch::new(vec![
        Packet::Draw(Box::new(draw([0., 1., 0., 1.], indexed))),
        Packet::ReadPixels(info(store::key())),
    ])
    .unwrap()
}
#[test]
fn native_session_many_owned_draws_rollover_without_per_draw_submissions() {
    let (_lock, before) = setup();
    for indexed in [false, true] {
        let mut session = NativeSession::new(open(), TargetOptions::default(), 16 << 10).unwrap();
        let mut packets = Vec::new();
        for n in 0..96 {
            packets.push(Packet::Draw(Box::new(draw(
                [n as f32 / 95., 0., 0., 1.],
                indexed,
            ))));
        }
        packets.push(Packet::Draw(Box::new(draw([0., 1., 0., 1.], indexed))));
        packets.push(Packet::ReadPixels(info(store::key())));
        let result = session
            .execute(Batch::new(packets).unwrap(), &NoCpuCommit, DEADLINE)
            .unwrap();
        assert_eq!(result.draws, 97);
        assert_eq!(result.submissions, 1);
        assert!(result.output_rewinds > 0);
        assert_eq!(result.pixels.len(), 1);
        assert!(
            result.pixels[0]
                .words
                .as_ref()
                .unwrap()
                .iter()
                .all(|&w| w == 0xff00ff00)
        );
    }
    finish(before);
}
#[test]
fn native_session_retains_exact_packets_over_actual_staging_pressure() {
    let (_lock, before) = setup();
    let config = Config {
        max_pending_upload_bytes: 20 << 10,
        max_staging_buffer_bytes: 80 << 10,
        ..Config::default()
    };
    let mut session = NativeSession::new(
        super::open_config(config),
        TargetOptions::default(),
        16 << 10,
    )
    .unwrap();
    let mut packets = Vec::new();
    for n in 0..18 {
        let mut p = draw([0., 1., 0., 1.], true);
        let page = Arc::make_mut(&mut p.pages).first_mut().unwrap();
        // Every capture positions the triangle at a DIFFERENT visible strip.
        // Reusing the last snapshot would leave earlier strips black.
        let left = -1.0 + n as f32 * 8.0 / 80.0;
        let vertices: Vec<_> = [
            [left, -1.0f32, 0.5],
            [left + 16.0 / 80.0, -1.0, 0.5],
            [left, 3.0, 0.5],
        ]
        .into_iter()
        .flatten()
        .flat_map(f32::to_be_bytes)
        .collect();
        Arc::make_mut(&mut page.bytes)[..vertices.len()].copy_from_slice(&vertices);
        p.scissor = Some([n * 4, 0, 4, 16]);
        packets.push(Packet::Draw(Box::new(p)));
    }
    packets.push(Packet::ReadPixels(info(store::key())));
    let result = session
        .execute(Batch::new(packets).unwrap(), &NoCpuCommit, DEADLINE)
        .unwrap();
    assert_eq!(result.draws, 18);
    assert!(result.submissions > 1);
    assert!(result.pressure_retries > 0);
    for (i, &word) in result.pixels[0].words.as_ref().unwrap().iter().enumerate() {
        assert_eq!(word, if i % 80 < 72 { 0xff00ff00 } else { 0 }, "pixel {i}");
    }
    drop(session);
    finish(before);
}
struct Commit {
    pages: Mutex<BTreeMap<u32, GuestPage>>,
    gate: Mutex<(bool, bool)>,
    changed: Condvar,
}
impl CpuCommit for Commit {
    fn compare_exchange(&self, a: &GuestPage, b: &GuestPage) -> bool {
        let mut gate = self.gate.lock().unwrap();
        gate.0 = true;
        self.changed.notify_all();
        while !gate.1 {
            gate = self.changed.wait(gate).unwrap();
        }
        drop(gate);
        let mut pages = self.pages.lock().unwrap();
        if pages
            .get(&a.address)
            .is_none_or(|p| p.generation != a.generation || p.bytes != a.bytes)
        {
            return false;
        }
        pages.insert(b.address, b.clone());
        true
    }
}
fn config() -> Config {
    Config {
        validation: true,
        ..Config::default()
    }
}
#[test]
fn native_worker_completion_waits_for_cpu_visibility_and_queue_pressure_keeps_input() {
    let (_lock, before) = setup();
    let pages = store::pages();
    let commit = Arc::new(Commit {
        pages: Mutex::new(pages.iter().map(|p| (p.address, p.clone())).collect()),
        gate: Mutex::new((false, false)),
        changed: Condvar::new(),
    });
    let worker = NativeWorker::open(
        config(),
        TargetOptions::default(),
        16 << 10,
        commit.clone(),
        DEADLINE,
    )
    .unwrap();
    let batch = Batch::new(vec![
        Packet::Draw(Box::new(draw([0., 1., 0., 1.], false))),
        Packet::Store {
            info: info(store::key()),
            pages: Arc::from(pages),
        },
    ])
    .unwrap();
    let ticket = worker.offer(batch).ok().unwrap();
    let gate = commit
        .changed
        .wait_timeout_while(commit.gate.lock().unwrap(), DEADLINE, |g| !g.0)
        .unwrap();
    assert!(gate.0.0);
    drop(gate);
    assert!(matches!(
        ticket.wait(Duration::from_millis(1)),
        Err(WaitError::Pending)
    ));
    assert_eq!(worker.completed(), 0);
    let queued: Vec<_> = (0..4)
        .map(|_| worker.offer(green_batch(false)).ok().unwrap())
        .collect();
    let retained = match worker.offer(green_batch(true)) {
        Err(OfferError::Busy(batch)) => batch,
        _ => panic!("queue must retain rejected input"),
    };
    assert_eq!(retained.len(), 2);
    {
        commit.gate.lock().unwrap().1 = true;
        commit.changed.notify_all();
    }
    let done = ticket.wait(DEADLINE).unwrap();
    assert!(done.cpu_pages_written > 0);
    for t in queued {
        assert!(
            t.wait(DEADLINE).unwrap().pixels[0]
                .words
                .as_ref()
                .unwrap()
                .iter()
                .all(|&p| p == 0xff00ff00)
        );
    }
    let final_ticket = worker.offer(retained).ok().unwrap();
    assert_eq!(final_ticket.sequence(), 6);
    assert!(
        final_ticket.wait(DEADLINE).unwrap().pixels[0]
            .words
            .as_ref()
            .unwrap()
            .iter()
            .all(|&p| p == 0xff00ff00)
    );
    assert_eq!(worker.completed(), 6);
    assert!(
        commit
            .pages
            .lock()
            .unwrap()
            .values()
            .any(|p| p.generation == Some(11))
    );
    drop(worker);
    finish(before);
}
#[test]
fn native_worker_rejected_cpu_commit_does_not_complete_later_batches() {
    let (_lock, before) = setup();
    let worker = NativeWorker::open(
        config(),
        TargetOptions::default(),
        16 << 10,
        Arc::new(NoCpuCommit),
        DEADLINE,
    )
    .unwrap();
    let first = worker.offer(green_batch(false)).ok().unwrap();
    first.wait(DEADLINE).unwrap();
    let failing = Batch::new(vec![Packet::Store {
        info: info(store::key()),
        pages: Arc::from(store::pages()),
    }])
    .unwrap();
    let fail = worker.offer(failing).ok().unwrap();
    let later = worker.offer(green_batch(false)).ok().unwrap();
    assert!(matches!(
        fail.wait(DEADLINE),
        Err(WaitError::Failed(Error::Rejected(
            "native session CPU visibility conflict"
        )))
    ));
    assert!(matches!(later.wait(DEADLINE), Err(WaitError::Failed(_))));
    assert_eq!(worker.completed(), 1);
    drop(worker);
    finish(before);
}
#[test]
fn native_session_missing_capture_fails_before_submission_and_cannot_replay() {
    let (_lock, before) = setup();
    let mut session = NativeSession::new(open(), TargetOptions::default(), 16 << 10).unwrap();
    let mut p = draw([0., 1., 0., 1.], false);
    p.pages = Arc::from([]);
    assert!(matches!(
        session.execute(
            Batch::new(vec![Packet::Draw(Box::new(p))]).unwrap(),
            &NoCpuCommit,
            DEADLINE
        ),
        Err(Error::Rejected(
            "guest draw reads outside its captured pages"
        ))
    ));
    assert!(
        session
            .execute(green_batch(false), &NoCpuCommit, DEADLINE)
            .is_err()
    );
    drop(session);
    finish(before);
}

#[path = "../../../../../skyrim-recomp-web/crates/xenos-shader/tests/common/mod.rs"]
mod assembler;
#[test]
fn native_session_store_sample_and_front_use_gpu_generation_then_reject_stale_next_batch() {
    use assembler::{Alu, TFetch, assemble, exece, r};
    use xenos_texture::{SwizzleSource as S, TextureFetchConstant as F, TextureFormat};
    let (_lock, before) = setup();
    let mut session = NativeSession::new(open(), TargetOptions::default(), 16 << 10).unwrap();
    let pages = store::pages();
    let commit = Commit {
        pages: Mutex::new(pages.iter().map(|p| (p.address, p.clone())).collect()),
        gate: Mutex::new((false, true)),
        changed: Condvar::new(),
    };
    let mut fetch = F::decode([2, 0, 0, 0, 0, 0]);
    fetch.format = TextureFormat::k_8_8_8_8;
    fetch.tiled = true;
    fetch.base_page = 1;
    fetch.width = 80;
    fetch.height = 16;
    fetch.pitch_div_32 = 5;
    fetch.swizzle = [S::X, S::Y, S::Z, S::W];
    let mut sampled = draw([0.25, 0.25, 0., 1.], false);
    sampled.pixel.as_mut().unwrap().code = Arc::from(assemble(&[exece(vec![
        TFetch {
            fetch_constant: 1,
            dest: 1,
            src: 0,
            ..Default::default()
        }
        .into(),
        Alu::vector(2, 0, 15, &[r(1), r(1)]).export().into(),
    ])]));
    sampled.pixel.as_mut().unwrap().options.texture_binding_base = 96;
    for (i, w) in fetch.encode().into_iter().enumerate() {
        Arc::make_mut(&mut sampled.constants[3])[24 + i * 4..28 + i * 4]
            .copy_from_slice(&w.to_le_bytes());
    }
    let mut all = sampled.pages.to_vec();
    all.extend_from_slice(&pages);
    sampled.pages = Arc::from(all);
    let packets = vec![
        Packet::Draw(Box::new(draw([1., 0., 0., 1.], false))),
        Packet::Store {
            info: info(store::key()),
            pages: Arc::from(pages.clone()),
        },
        Packet::Draw(Box::new(sampled)),
        Packet::ReadPixels(info(store::key())),
        Packet::FrontBuffer {
            fetch: fetch.encode(),
            pages: Arc::from(pages.clone()),
            display_size: [80, 16],
            options: Default::default(),
        },
    ];
    let result = session
        .execute(Batch::new(packets).unwrap(), &commit, DEADLINE)
        .unwrap();
    assert_eq!(result.submissions, 1);
    assert_eq!(result.front_buffer, Some([80, 16]));
    assert_eq!(result.pixels[0].packet_index, 3);
    assert!(
        result.pixels[0]
            .words
            .as_ref()
            .unwrap()
            .iter()
            .all(|&p| p == 0xff0000ff)
    );
    let display = session.test_front_pixels([80, 16], DEADLINE).unwrap();
    assert!(display.chunks_exact(4).all(|p| p == [0, 0, 255, 255]));
    let stale = Batch::new(vec![Packet::FrontBuffer {
        fetch: fetch.encode(),
        pages: Arc::from(pages),
        display_size: [80, 16],
        options: Default::default(),
    }])
    .unwrap();
    assert!(matches!(
        session.execute(stale, &commit, DEADLINE),
        Err(Error::Rejected("invalid guest capture generation"))
    ));
    drop(session);
    finish(before);
}
#[test]
fn native_session_empty_readback_has_its_own_explicit_result() {
    let (_lock, before) = setup();
    let mut session = NativeSession::new(open(), TargetOptions::default(), 16 << 10).unwrap();
    let mut empty = info(store::key());
    empty.copy_dest_extent_length = 0;
    let result = session
        .execute(
            Batch::new(vec![
                Packet::ReadPixels(empty),
                Packet::ReadPixels(info(store::key())),
            ])
            .unwrap(),
            &NoCpuCommit,
            DEADLINE,
        )
        .unwrap();
    assert_eq!(result.pixels.len(), 2);
    assert_eq!(result.pixels[0].packet_index, 0);
    assert!(result.pixels[0].words.is_none());
    assert_eq!(result.pixels[1].packet_index, 1);
    assert!(result.pixels[1].words.is_some());
    drop(session);
    finish(before);
}

#[test]
fn native_session_evicts_idle_programs_while_retaining_live_packet_programs() {
    let (_lock, before) = setup();
    let mut session = NativeSession::new(open(), TargetOptions::default(), 16 << 10).unwrap();
    session.test_cache_limits(4, 4).unwrap();
    for n in 0..12 {
        let mut p = draw([0., 1., 0., 1.], false);
        p.pipeline.vertex = n;
        let result = session
            .execute(
                Batch::new(vec![
                    Packet::Draw(Box::new(p)),
                    Packet::ReadPixels(info(store::key())),
                ])
                .unwrap(),
                &NoCpuCommit,
                DEADLINE,
            )
            .unwrap();
        assert!(
            result.pixels[0]
                .words
                .as_ref()
                .unwrap()
                .iter()
                .all(|&w| w == 0xff00ff00)
        );
        let (shaders, programs) = session.test_cache_counts();
        assert!(shaders <= 4 && programs <= 4);
    }
    drop(session);
    finish(before);
}

#[test]
fn native_worker_stop_cancels_queued_work_without_replaying_or_completing_it() {
    let (_lock, before) = setup();
    let pages = store::pages();
    let commit = Arc::new(Commit {
        pages: Mutex::new(pages.iter().map(|p| (p.address, p.clone())).collect()),
        gate: Mutex::new((false, false)),
        changed: Condvar::new(),
    });
    let worker = NativeWorker::open(
        config(),
        TargetOptions::default(),
        16 << 10,
        commit.clone(),
        DEADLINE,
    )
    .unwrap();
    let active = worker
        .offer(
            Batch::new(vec![Packet::Store {
                info: info(store::key()),
                pages: Arc::from(pages),
            }])
            .unwrap(),
        )
        .ok()
        .unwrap();
    let gate = commit
        .changed
        .wait_timeout_while(commit.gate.lock().unwrap(), DEADLINE, |g| !g.0)
        .unwrap();
    assert!(gate.0.0);
    drop(gate);
    let queued = worker.offer(green_batch(false)).ok().unwrap();
    worker.request_stop();
    assert!(matches!(
        worker.offer(green_batch(false)),
        Err(OfferError::Stopped(_))
    ));
    {
        commit.gate.lock().unwrap().1 = true;
        commit.changed.notify_all();
    }
    active.wait(DEADLINE).unwrap();
    assert!(matches!(
        queued.wait(DEADLINE),
        Err(WaitError::Failed(Error::Rejected("native worker stopping")))
    ));
    assert_eq!(worker.completed(), 1);
    drop(worker);
    finish(before);
}
#[test]
fn native_session_failure_in_earlier_submission_cannot_complete_final_batch() {
    let (_lock, before) = setup();
    let config = Config {
        max_pending_upload_bytes: 20 << 10,
        max_staging_buffer_bytes: 80 << 10,
        ..Config::default()
    };
    let gpu = super::open_config(config);
    gpu.fail_completion(1);
    let mut session = NativeSession::new(gpu, TargetOptions::default(), 16 << 10).unwrap();
    let packets = (0..18)
        .map(|n| {
            let mut p = draw([0., 1., 0., 1.], false);
            Arc::make_mut(&mut Arc::make_mut(&mut p.pages)[0].bytes)[4000] = n;
            Packet::Draw(Box::new(p))
        })
        .collect();
    assert!(matches!(
        session.execute(Batch::new(packets).unwrap(), &NoCpuCommit, DEADLINE),
        Err(Error::Failed { serial: 1, .. })
    ));
    drop(session);
    finish(before);
}
