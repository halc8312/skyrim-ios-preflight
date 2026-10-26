use super::{DEADLINE, finish, open, open_config, setup};
use super::{
    pipeline_cases::normalized_key,
    resolve_cache_cases::{info, selection},
};
use skyrim_native_gpu::{
    Config, Gpu,
    backend::Platform,
    bindings::Group,
    edram::targets::{
        ConstantDraw, ConstantFrame, DrawAdmission, DrawArena, GuestMemory, GuestPage,
        TargetOptions,
    },
    guest_shader::GuestPipeline,
    pipeline::PipelineCache,
    render::BoundGroup,
    types as t,
};
use std::sync::Arc;
use xenos_shader::{ShaderStage, TranslationOptions, bindings};
use xenos_state::{edram::RenderTargetKey as Key, xenos::MsaaSamples};

fn key() -> Key {
    Key {
        base_tiles: 0,
        pitch_tiles_at_32bpp: 1,
        msaa_samples: MsaaSamples::X1,
        is_depth: false,
        format: 0,
    }
}
fn program(
    gpu: &mut Gpu<Platform>,
    memory: &GuestMemory,
) -> (DrawArena, GuestPipeline, Group<Platform>) {
    // The same hand-authored public position-fetch and color export fixture.
    let vs = [
        0x00013001, 0x00002200, 0, 0x00081000, 0x00390A88, 3, 0xC80F803E, 0, 0xE2010100,
        0xC80F8000, 0, 0x22000000,
    ];
    let ps = [0x00001001, 0x00002200, 0, 0xC80F8000, 0, 0xE2000000];
    let options = TranslationOptions {
        vs_output_interpolators: 1,
        ps_interpolator_count: Some(1),
        ..Default::default()
    };
    let vertex = gpu
        .guest_shader(ShaderStage::Vertex, &vs, &options)
        .unwrap();
    let pixel = gpu.guest_shader(ShaderStage::Pixel, &ps, &options).unwrap();
    let arena = gpu.xenos_draw_arena(16 << 10).unwrap();
    let empty = gpu.group_layout(&[]).unwrap();
    let layout = gpu
        .pipeline_layout(&[
            arena.layout().clone(),
            memory.layout().clone(),
            empty.clone(),
        ])
        .unwrap();
    let mut cache = PipelineCache::new(4).unwrap();
    let program = arena
        .guest_pipeline(
            gpu,
            &mut cache,
            &normalized_key(),
            &layout,
            &vertex,
            Some(&pixel),
        )
        .unwrap();
    (arena, program, gpu.bind_group(&empty, &[]).unwrap())
}
fn page(address: u32, shift: f32) -> GuestPage {
    let mut bytes = [0; 4096];
    let vertices: Vec<u8> = [
        [-1f32 + shift, -1., 0.5],
        [3. + shift, -1., 0.5],
        [-1. + shift, 3., 0.5],
    ]
    .into_iter()
    .flatten()
    .flat_map(f32::to_be_bytes)
    .collect();
    bytes[..vertices.len()].copy_from_slice(&vertices);
    GuestPage {
        address,
        bytes: Arc::new(bytes),
        generation: None,
    }
}
fn draw(
    frame: &mut ConstantFrame<'_, '_>,
    program: &GuestPipeline,
    empty: &Group<Platform>,
    address: u32,
    red: bool,
    scissor: [u32; 4],
    override_group: Option<&Group<Platform>>,
) -> Result<DrawAdmission, skyrim_native_gpu::Error> {
    let groups = [BoundGroup {
        group: override_group.unwrap_or(empty),
        dynamic_offsets: &[],
    }];
    let color: Vec<u8> = if red {
        [1f32, 0., 0., 1.]
    } else {
        [0f32, 1., 0., 1.]
    }
    .into_iter()
    .flat_map(f32::to_le_bytes)
    .collect();
    let mut fetch = [0u8; 768];
    for (i, v) in [address | 3, 2 | 9 * 4 << 2].into_iter().enumerate() {
        fetch[i * 4..i * 4 + 4].copy_from_slice(&v.to_le_bytes());
    }
    let system = bindings::SystemConstants::default();
    frame.draw_guest(
        program,
        &ConstantDraw {
            pipeline: program.pipeline(),
            groups: &groups,
            constants: [&color, &[], &[0; 160], &fetch, system.as_bytes()],
            vertices: 0..3,
            instances: 0..1,
            indices: None,
            viewport: None,
            scissor: Some(scissor),
            blend: t::Color::TRANSPARENT,
            stencil_reference: 0,
        },
    )
}

#[test]
fn guest_memory_ordered_update_preserves_earlier_vertex_snapshot_and_skips_identical_pages() {
    let (_lock, before) = setup();
    let mut gpu = open();
    let mut memory = gpu.guest_memory(1).unwrap();
    let (mut arena, program, empty) = program(&mut gpu, &memory);
    let mut cache = gpu.edram_target_cache(TargetOptions::default()).unwrap();
    let mut frame = cache
        .begin(&mut gpu)
        .unwrap()
        .with_constants(&mut arena)
        .unwrap()
        .with_guest_memory(&mut memory)
        .unwrap();
    frame.targets(&selection(key()), 16).unwrap();
    frame.sync_guest_pages(&[page(65536, 0.)]).unwrap();
    draw(
        &mut frame,
        &program,
        &empty,
        65536,
        false,
        [0, 0, 40, 16],
        None,
    )
    .unwrap();
    let changed = page(65536, 1.);
    frame
        .sync_guest_pages(std::slice::from_ref(&changed))
        .unwrap();
    let stats = frame.stats();
    frame.sync_guest_pages(&[page(65536, 1.)]).unwrap(); // equal bytes in a DIFFERENT Arc
    frame.sync_guest_pages(&[changed]).unwrap();
    assert_eq!(frame.stats(), stats);
    draw(
        &mut frame,
        &program,
        &empty,
        65536,
        true,
        [40, 0, 40, 16],
        None,
    )
    .unwrap();
    let ticket = frame.resolve_pixels(&info(key())).unwrap().unwrap();
    let (serial, _, stats) = frame.submit().unwrap();
    assert_eq!(
        (serial, stats.guest_changed_pages, stats.guest_upload_bytes),
        (1, 2, 8196)
    );
    assert_eq!(
        (memory.mapped_blocks(), memory.last_submission()),
        (1, serial)
    );
    let pixels = ticket.read(&mut gpu, DEADLINE).unwrap();
    for row in pixels.chunks_exact(80) {
        assert!(row[..40].iter().all(|&p| p == 0xff00ff00));
        assert!(row[40..].iter().all(|&p| p == 0xff0000ff));
    }
    drop(gpu);
    finish(before);
}

#[test]
fn guest_memory_abort_keeps_gpu_bytes_and_published_page_identity_unchanged() {
    let (_lock, before) = setup();
    let mut gpu = open();
    let mut memory = gpu.guest_memory(1).unwrap();
    let (mut arena, program, empty) = program(&mut gpu, &memory);
    let mut cache = gpu.edram_target_cache(TargetOptions::default()).unwrap();
    let original = page(65536, 0.);
    let mut frame = cache
        .begin(&mut gpu)
        .unwrap()
        .with_constants(&mut arena)
        .unwrap()
        .with_guest_memory(&mut memory)
        .unwrap();
    frame
        .sync_guest_pages(std::slice::from_ref(&original))
        .unwrap();
    frame.submit().unwrap();
    let mut frame = cache
        .begin(&mut gpu)
        .unwrap()
        .with_constants(&mut arena)
        .unwrap()
        .with_guest_memory(&mut memory)
        .unwrap();
    frame.sync_guest_pages(&[page(65536, 1.)]).unwrap();
    assert!(
        frame
            .sync_guest_pages(&[page(65536, 1.), page(65536, 0.)])
            .is_err()
    );
    assert!(frame.submit().is_err());
    assert_eq!(memory.last_submission(), 1);
    let mut frame = cache
        .begin(&mut gpu)
        .unwrap()
        .with_constants(&mut arena)
        .unwrap()
        .with_guest_memory(&mut memory)
        .unwrap();
    frame.targets(&selection(key()), 16).unwrap();
    frame.sync_guest_pages(&[original]).unwrap();
    assert_eq!(frame.stats().guest_upload_bytes, 0);
    draw(
        &mut frame,
        &program,
        &empty,
        65536,
        false,
        [0, 0, 80, 16],
        None,
    )
    .unwrap();
    let ticket = frame.resolve_pixels(&info(key())).unwrap().unwrap();
    frame.submit().unwrap();
    assert!(
        ticket
            .read(&mut gpu, DEADLINE)
            .unwrap()
            .iter()
            .all(|&p| p == 0xff00ff00)
    );
    drop(gpu);
    finish(before);
}

#[test]
fn guest_memory_first_draw_pressure_can_submit_preparation_without_recapturing_live_bytes() {
    let (_lock, before) = setup();
    let mut config = Config::default();
    config.max_pending_upload_bytes = 8192;
    let mut gpu = open_config(config);
    let mut memory = gpu.guest_memory(1).unwrap();
    let (mut arena, program, empty) = program(&mut gpu, &memory);
    let mut cache = gpu.edram_target_cache(TargetOptions::default()).unwrap();
    let captured = page(65536, 0.);
    let mut frame = cache
        .begin(&mut gpu)
        .unwrap()
        .with_constants(&mut arena)
        .unwrap()
        .with_guest_memory(&mut memory)
        .unwrap();
    frame.targets(&selection(key()), 16).unwrap();
    frame
        .sync_guest_pages(std::slice::from_ref(&captured))
        .unwrap();
    assert_eq!(
        draw(
            &mut frame,
            &program,
            &empty,
            65536,
            false,
            [0, 0, 80, 16],
            None
        )
        .unwrap(),
        DrawAdmission::NeedsSubmit
    );
    let (serial, _, _) = frame.submit().unwrap();
    gpu.wait(serial, DEADLINE).unwrap();
    // A newer live capture is available, but this pending draw keeps its old
    // owned snapshot. The API never substitutes a fresh read during retry.
    let _newer = page(65536, 1.);
    let mut frame = cache
        .begin(&mut gpu)
        .unwrap()
        .with_constants(&mut arena)
        .unwrap()
        .with_guest_memory(&mut memory)
        .unwrap();
    frame.targets(&selection(key()), 16).unwrap();
    frame.sync_guest_pages(&[captured]).unwrap();
    assert_eq!(frame.stats().guest_changed_pages, 0);
    draw(
        &mut frame,
        &program,
        &empty,
        65536,
        false,
        [0, 0, 80, 16],
        None,
    )
    .unwrap();
    let ticket = frame.resolve_pixels(&info(key())).unwrap().unwrap();
    frame.submit().unwrap();
    assert!(
        ticket
            .read(&mut gpu, DEADLINE)
            .unwrap()
            .iter()
            .all(|&p| p == 0xff00ff00)
    );
    drop(gpu);
    finish(before);
}

#[test]
fn guest_memory_requires_owner_and_evicts_only_after_prior_draws_are_encoded() {
    let (_lock, before) = setup();
    let mut gpu = open();
    let mut memory = gpu.guest_memory(1).unwrap();
    let (mut arena, program, empty) = program(&mut gpu, &memory);
    let mut cache = gpu.edram_target_cache(TargetOptions::default()).unwrap();
    // A translated draw cannot bypass the owner by omitting its memory.
    let mut frame = cache
        .begin(&mut gpu)
        .unwrap()
        .with_constants(&mut arena)
        .unwrap();
    frame.targets(&selection(key()), 16).unwrap();
    assert!(
        draw(
            &mut frame,
            &program,
            &empty,
            65536,
            false,
            [0, 0, 80, 16],
            None
        )
        .is_err()
    );
    assert!(frame.submit().is_err());
    assert_eq!(memory.mapped_blocks(), 0);
    let captures: Vec<_> = (0..512).map(|block| page(block * 65536, 0.)).collect();
    let mut frame = cache
        .begin(&mut gpu)
        .unwrap()
        .with_constants(&mut arena)
        .unwrap()
        .with_guest_memory(&mut memory)
        .unwrap();
    frame.sync_guest_pages(&captures).unwrap();
    // Also populate a second page of the oldest block. Reusing its slot must
    // clear this page even though the replacement capture contains page0 only.
    frame.sync_guest_pages(&[page(4096, 0.)]).unwrap();
    // Equal ages choose slot0; all first pages are captured for this draw.
    frame.sync_guest_pages(&captures).unwrap();
    frame.targets(&selection(key()), 16).unwrap();
    draw(&mut frame, &program, &empty, 0, true, [0, 0, 40, 16], None).unwrap();
    frame.sync_guest_pages(&[page(512 * 65536, 1.)]).unwrap();
    assert_eq!(frame.stats().guest_evicted_blocks, 1);
    draw(
        &mut frame,
        &program,
        &empty,
        512 * 65536,
        false,
        [40, 0, 40, 16],
        None,
    )
    .unwrap();
    let ticket = frame.resolve_pixels(&info(key())).unwrap().unwrap();
    frame.submit().unwrap();
    assert_eq!(memory.mapped_blocks(), 512);
    let pixels = ticket.read(&mut gpu, DEADLINE).unwrap();
    for row in pixels.chunks_exact(80) {
        assert!(row[..40].iter().all(|&p| p == 0xff0000ff));
        assert!(row[40..].iter().all(|&p| p == 0xff00ff00));
    }
    // This lane belonged to the evicted mapping. It is physically zeroed,
    // independent of the strict facade refusing uncaptured vertex fetches.
    assert!(
        memory
            .test_read_page(&mut gpu, 512 * 65536 + 4096, DEADLINE)
            .unwrap()
            .iter()
            .all(|&b| b == 0)
    );
    for missing in [0, 512 * 65536 + 4096] {
        let mut frame = cache
            .begin(&mut gpu)
            .unwrap()
            .with_constants(&mut arena)
            .unwrap()
            .with_guest_memory(&mut memory)
            .unwrap();
        frame.targets(&selection(key()), 16).unwrap();
        frame.sync_guest_pages(&[page(512 * 65536, 1.)]).unwrap();
        assert!(
            draw(
                &mut frame,
                &program,
                &empty,
                missing,
                true,
                [0, 0, 80, 16],
                None
            )
            .is_err()
        );
        assert!(frame.submit().is_err());
    }
    drop(gpu);
    finish(before);
}

#[test]
fn guest_memory_unknown_submit_never_publishes_metadata_or_reuses_uncertain_bytes() {
    let (_lock, before) = setup();
    let mut gpu = open();
    let mut memory = gpu.guest_memory(1).unwrap();
    let (mut arena, program, empty) = program(&mut gpu, &memory);
    let mut cache = gpu.edram_target_cache(TargetOptions::default()).unwrap();
    gpu.fail_next_submit();
    let mut frame = cache
        .begin(&mut gpu)
        .unwrap()
        .with_constants(&mut arena)
        .unwrap()
        .with_guest_memory(&mut memory)
        .unwrap();
    frame.targets(&selection(key()), 16).unwrap();
    frame.sync_guest_pages(&[page(65536, 0.)]).unwrap();
    draw(
        &mut frame,
        &program,
        &empty,
        65536,
        false,
        [0, 0, 80, 16],
        None,
    )
    .unwrap();
    let ticket = frame.resolve_pixels(&info(key())).unwrap().unwrap();
    assert!(frame.submit().is_err());
    assert_eq!(
        (
            memory.mapped_blocks(),
            memory.last_submission(),
            cache.last_submission()
        ),
        (0, 0, 0)
    );
    assert!(ticket.serial().is_none());
    assert!(ticket.read(&mut gpu, DEADLINE).is_err());
    assert!(gpu.encoder().is_err());
    assert!(gpu.staging_stats().resident_bytes > 0);
    drop(gpu);
    finish(before);
}

#[test]
fn guest_memory_chunk_growth_and_last_physical_page_use_the_same_snapshot_abi() {
    let (_lock, before) = setup();
    let mut gpu = open();
    let mut memory = gpu.guest_memory(2).unwrap();
    let (mut arena, program, empty) = program(&mut gpu, &memory);
    let mut cache = gpu.edram_target_cache(TargetOptions::default()).unwrap();
    let mut frame = cache
        .begin(&mut gpu)
        .unwrap()
        .with_constants(&mut arena)
        .unwrap()
        .with_guest_memory(&mut memory)
        .unwrap();
    let mut captures: Vec<_> = (0..512).map(|block| page(block * 65536, 0.)).collect();
    let last = (512 << 20) - 4096;
    captures.push(page(last, 1.));
    frame.sync_guest_pages(&captures).unwrap();
    frame.targets(&selection(key()), 16).unwrap();
    draw(&mut frame, &program, &empty, 0, true, [0, 0, 40, 16], None).unwrap();
    frame
        .sync_guest_pages(std::slice::from_ref(captures.last().unwrap()))
        .unwrap();
    draw(
        &mut frame,
        &program,
        &empty,
        last,
        false,
        [40, 0, 40, 16],
        None,
    )
    .unwrap();
    let ticket = frame.resolve_pixels(&info(key())).unwrap().unwrap();
    let (_, _, stats) = frame.submit().unwrap();
    assert_eq!(
        (
            memory.mapped_blocks(),
            stats.guest_changed_pages,
            stats.guest_evicted_blocks
        ),
        (513, 513, 0)
    );
    for row in ticket.read(&mut gpu, DEADLINE).unwrap().chunks_exact(80) {
        assert!(row[..40].iter().all(|&p| p == 0xff0000ff));
        assert!(row[40..].iter().all(|&p| p == 0xff00ff00));
    }
    drop(gpu);
    finish(before);
}

#[test]
fn guest_memory_rejected_preparation_releases_new_chunks_without_needing_a_submit() {
    let (_lock, before) = setup();
    let mut config = Config::default();
    config.max_pending_upload_bytes = 4096; // One page PLUS its mapping cannot fit.
    config.max_resident_buffer_bytes = 64 << 20;
    let mut gpu = open_config(config);
    let mut memory = gpu.guest_memory(1).unwrap();
    let (mut arena, _program, _empty) = program(&mut gpu, &memory);
    let mut cache = gpu.edram_target_cache(TargetOptions::default()).unwrap();
    let baseline = gpu.live_buffer_count();
    for _ in 0..6 {
        let mut frame = cache
            .begin(&mut gpu)
            .unwrap()
            .with_constants(&mut arena)
            .unwrap()
            .with_guest_memory(&mut memory)
            .unwrap();
        assert!(matches!(
            frame.sync_guest_pages(&[page(65536, 0.)]),
            Err(skyrim_native_gpu::Error::Rejected(
                "ordered upload encoder byte budget"
            ))
        ));
        drop(frame);
        assert_eq!(
            gpu.live_buffer_count(),
            baseline,
            "rejected allocation retained by a queue-prefix initialization"
        );
        assert_eq!((memory.mapped_blocks(), memory.last_submission()), (0, 0));
    }
    assert_eq!(gpu.verified_serial(), 0);
    drop(gpu);
    finish(before);
}

#[test]
fn guest_memory_draw_rejects_pages_not_in_the_latest_capture_even_if_cached() {
    let (_lock, before) = setup();
    let mut gpu = open();
    let mut memory = gpu.guest_memory(1).unwrap();
    let (mut arena, program, empty) = program(&mut gpu, &memory);
    let mut cache = gpu.edram_target_cache(TargetOptions::default()).unwrap();
    let mut frame = cache
        .begin(&mut gpu)
        .unwrap()
        .with_constants(&mut arena)
        .unwrap()
        .with_guest_memory(&mut memory)
        .unwrap();
    frame.sync_guest_pages(&[page(65536, 0.)]).unwrap();
    frame.submit().unwrap();
    let mut frame = cache
        .begin(&mut gpu)
        .unwrap()
        .with_constants(&mut arena)
        .unwrap()
        .with_guest_memory(&mut memory)
        .unwrap();
    frame.targets(&selection(key()), 16).unwrap();
    frame.sync_guest_pages(&[page(65536 + 4096, 1.)]).unwrap();
    assert!(
        draw(
            &mut frame,
            &program,
            &empty,
            65536,
            false,
            [0, 0, 80, 16],
            None
        )
        .is_err(),
        "cached old page must not substitute for this draw's missing capture"
    );
    assert!(frame.submit().is_err());
    assert_eq!(memory.last_submission(), 1);
    drop(gpu);
    finish(before);
}

#[test]
fn guest_memory_sync_pressure_with_chunk_growth_keeps_prior_draw_and_retries_same_capture() {
    let (_lock, before) = setup();
    let mut config = Config::default();
    config.max_staging_buffer_bytes = 4 << 20;
    let mut gpu = open_config(config);
    let mut memory = gpu.guest_memory(2).unwrap();
    let (mut arena, program, empty) = program(&mut gpu, &memory);
    let mut cache = gpu.edram_target_cache(TargetOptions::default()).unwrap();
    let captures: Vec<_> = (0..512).map(|block| page(block * 65536, 0.)).collect();
    let mut frame = cache
        .begin(&mut gpu)
        .unwrap()
        .with_constants(&mut arena)
        .unwrap()
        .with_guest_memory(&mut memory)
        .unwrap();
    frame.sync_guest_pages(&captures).unwrap();
    let (s, _, _) = frame.submit().unwrap();
    gpu.wait(s, DEADLINE).unwrap();
    // Prefix work plus a backed constant snapshot leaves too little global
    // staging for one new page, but both can make progress after submit.
    let prefix = gpu
        .create_buffer_init(
            "authored competing prefix",
            &vec![9; (4 << 20) - 4096],
            super::U::COPY_DST,
        )
        .unwrap();
    let mut frame = cache
        .begin(&mut gpu)
        .unwrap()
        .with_constants(&mut arena)
        .unwrap()
        .with_guest_memory(&mut memory)
        .unwrap();
    frame.targets(&selection(key()), 16).unwrap();
    frame.sync_guest_pages(&[page(0, 0.)]).unwrap();
    draw(&mut frame, &program, &empty, 0, true, [0, 0, 40, 16], None).unwrap();
    let old_group = frame.test_guest_group_id().unwrap();
    let buffers = frame.gpu().live_buffer_count();
    let captured = page(512 * 65536, 1.);
    assert_eq!(
        frame
            .sync_guest_pages(std::slice::from_ref(&captured))
            .unwrap(),
        DrawAdmission::NeedsSubmit
    );
    assert_eq!(frame.test_guest_group_id().unwrap(), old_group);
    assert_eq!(frame.gpu().live_buffer_count(), buffers);
    assert_eq!(frame.stats().guest_changed_pages, 0);
    let first = frame.resolve_pixels(&info(key())).unwrap().unwrap();
    let (s, _, _) = frame.submit().unwrap();
    assert_eq!(memory.mapped_blocks(), 512);
    gpu.wait(s, DEADLINE).unwrap();
    drop(prefix);
    let mut frame = cache
        .begin(&mut gpu)
        .unwrap()
        .with_constants(&mut arena)
        .unwrap()
        .with_guest_memory(&mut memory)
        .unwrap();
    frame.targets(&selection(key()), 16).unwrap();
    assert_eq!(
        frame.sync_guest_pages(&[captured]).unwrap(),
        DrawAdmission::Accepted
    );
    assert_ne!(frame.test_guest_group_id().unwrap(), old_group);
    draw(
        &mut frame,
        &program,
        &empty,
        512 * 65536,
        false,
        [40, 0, 40, 16],
        None,
    )
    .unwrap();
    let later = frame.resolve_pixels(&info(key())).unwrap().unwrap();
    frame.submit().unwrap();
    assert_eq!(memory.mapped_blocks(), 513);
    for row in first.read(&mut gpu, DEADLINE).unwrap().chunks_exact(80) {
        assert!(row[..40].iter().all(|&p| p == 0xff0000ff));
    }
    for row in later.read(&mut gpu, DEADLINE).unwrap().chunks_exact(80) {
        assert!(row[..40].iter().all(|&p| p == 0xff0000ff));
        assert!(row[40..].iter().all(|&p| p == 0xff00ff00));
    }
    drop(gpu);
    finish(before);
}

#[test]
fn guest_memory_rejects_unproven_entry_aliases_and_consumed_capture_before_admission() {
    let (_lock, before) = setup();
    let mut gpu = open();
    let mut memory = gpu.guest_memory(1).unwrap();
    let (mut arena, program, empty) = program(&mut gpu, &memory);
    let mut cache = gpu.edram_target_cache(TargetOptions::default()).unwrap();
    for case in 0..8 {
        let mut frame = cache
            .begin(&mut gpu)
            .unwrap()
            .with_constants(&mut arena)
            .unwrap()
            .with_guest_memory(&mut memory)
            .unwrap();
        frame.targets(&selection(key()), 16).unwrap();
        if case != 6 {
            frame.sync_guest_pages(&[page(65536, 0.)]).unwrap();
        }
        if case == 7 {
            draw(
                &mut frame,
                &program,
                &empty,
                65536,
                false,
                [0, 0, 80, 16],
                None,
            )
            .unwrap();
        }
        let mut system = bindings::SystemConstants::default();
        match case {
            0 => system.flags |= bindings::flags::VERTEX_INDEX_LOAD,
            1 => system.vertex_index_endian = 1,
            2 => system.vertex_base_index = -1,
            5 => {
                system.vertex_index_min = 1;
                system.vertex_index_max = 0;
            }
            _ => {}
        }
        let address = match case {
            3 => 0xa0010003u32,
            4 => 65536 + 4096 + 3,
            _ => 65536 + 3,
        };
        let mut fetch = [0u8; 768];
        fetch[..4].copy_from_slice(&address.to_le_bytes());
        fetch[4..8].copy_from_slice(&(2u32 | 36 << 2).to_le_bytes());
        let groups = [BoundGroup {
            group: &empty,
            dynamic_offsets: &[],
        }];
        let color = [0u8; 16];
        let captured = ConstantDraw {
            pipeline: program.pipeline(),
            groups: &groups,
            constants: [&color, &[], &[0; 160], &fetch, system.as_bytes()],
            vertices: 0..3,
            instances: 0..1,
            indices: None,
            viewport: None,
            scissor: None,
            blend: t::Color::TRANSPARENT,
            stencil_reference: 0,
        };
        let stats = frame.stats();
        assert!(frame.draw_guest(&program, &captured).is_err(), "case{case}");
        assert_eq!(frame.stats(), stats);
        assert!(frame.submit().is_err());
        assert_eq!(memory.last_submission(), 0);
    }
    drop(gpu);
    finish(before);
}

#[test]
fn guest_memory_raw_draw_cannot_inherit_private_group_from_accepted_guest_draw() {
    let (_lock, before) = setup();
    let mut gpu = open();
    let mut memory = gpu.guest_memory(1).unwrap();
    let (mut arena, program, empty) = program(&mut gpu, &memory);
    let mut cache = gpu.edram_target_cache(TargetOptions::default()).unwrap();
    let mut frame = cache
        .begin(&mut gpu)
        .unwrap()
        .with_constants(&mut arena)
        .unwrap()
        .with_guest_memory(&mut memory)
        .unwrap();
    frame.targets(&selection(key()), 16).unwrap();
    frame.sync_guest_pages(&[page(65536, 0.)]).unwrap();
    draw(
        &mut frame,
        &program,
        &empty,
        65536,
        false,
        [0, 0, 80, 16],
        None,
    )
    .unwrap();
    let groups = [BoundGroup {
        group: &empty,
        dynamic_offsets: &[],
    }];
    let full = [0u8; 4096];
    let system = bindings::SystemConstants::default();
    let raw = ConstantDraw {
        pipeline: program.pipeline(),
        groups: &groups,
        constants: [&full, &full, &[0; 160], &[0; 768], system.as_bytes()],
        vertices: 0..3,
        instances: 0..1,
        indices: None,
        viewport: None,
        scissor: None,
        blend: t::Color::TRANSPARENT,
        stencil_reference: 0,
    };
    assert_eq!(
        frame.draw(&raw),
        Err(skyrim_native_gpu::Error::Rejected(
            "draw pipeline constant/group contract"
        ))
    );
    assert_eq!(frame.stats().admitted, 1);
    assert!(frame.submit().is_err());
    assert_eq!(memory.last_submission(), 0);
    drop(gpu);
    finish(before);
}
