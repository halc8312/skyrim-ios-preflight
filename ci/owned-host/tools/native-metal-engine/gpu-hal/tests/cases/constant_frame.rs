use super::resolve_cache_cases::{info, selection};
use super::{DEADLINE, finish, open, open_config, setup};
use skyrim_native_gpu::{
    Config, Gpu,
    backend::Platform,
    edram::targets::{ConstantDraw, ConstantSlot, DrawAdmission, DrawArena, TargetOptions},
    render::{Pipeline, PipelineDescriptor},
    types as t,
};
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
fn arena_pipeline(gpu: &mut Gpu<Platform>, roomy: bool) -> (DrawArena, Pipeline<Platform>) {
    arena_pipeline_capacity(gpu, roomy, None)
}
fn arena_pipeline_capacity(
    gpu: &mut Gpu<Platform>,
    roomy: bool,
    windows: Option<u64>,
) -> (DrawArena, Pipeline<Platform>) {
    let probe = gpu.constant_stream(4096).unwrap();
    let alignment = probe.alignment();
    drop(probe);
    // Just one changed five-stage draw fits. All later ones MUST rewind and
    // encode consumers first. Bindings are deliberately not in stage order.
    let slots = [4, 0, 3, 1, 2].map(|binding| ConstantSlot {
        binding,
        visibility: t::ShaderStages::FRAGMENT,
        storage: false,
        bytes: 16,
    });
    let capacity = windows
        .map(|n| (n - 1) * alignment + 16)
        .unwrap_or(if roomy { 16 << 20 } else { 4 * alignment + 16 });
    let arena = gpu.draw_arena(capacity, slots).unwrap();
    let layout = gpu.pipeline_layout(&[arena.layout().clone()]).unwrap();
    let shader = gpu
        .shader(
            r#"
@group(0) @binding(0) var<uniform> c0:vec4u;
@group(0) @binding(1) var<uniform> c1:vec4u;
@group(0) @binding(2) var<uniform> c2:vec4u;
@group(0) @binding(3) var<uniform> c3:vec4u;
@group(0) @binding(4) var<uniform> c4:vec4u;
@vertex fn vs(@builtin(vertex_index) i:u32)->@builtin(position) vec4f {
let p=array<vec2f,3>(vec2f(-1.,-1.),vec2f(3.,-1.),vec2f(-1.,3.));return vec4f(p[i],0.,1.);}
@fragment fn fs()->@location(0) vec4f {
return vec4f(f32(c0.x%256u),f32((c1.x+2u*c2.x)%256u),f32((c3.x+3u*c4.x)%256u),255.)/255.;}
"#,
        )
        .unwrap();
    let pipeline = gpu
        .render_pipeline(&PipelineDescriptor {
            layout: &layout,
            vertex: &shader,
            vertex_entry: "vs",
            fragment: Some((&shader, "fs")),
            colors: &[Some(t::ColorTargetState {
                format: t::TextureFormat::Rgba8Unorm,
                blend: None,
                write_mask: t::ColorWrites::ALL,
            })],
            primitive: Default::default(),
            depth_stencil: None,
            multisample: Default::default(),
        })
        .unwrap();
    (arena, pipeline)
}
fn values(phase: u32) -> [Vec<u8>; 5] {
    std::array::from_fn(|stage| {
        [phase * 11 + stage as u32 * 3 + 1, 0, 0, 0]
            .into_iter()
            .flat_map(u32::to_le_bytes)
            .collect()
    })
}
fn expected(phase: u32) -> u32 {
    let c = [4, 0, 3, 1, 2];
    let mut by_binding = [0u32; 5];
    for (stage, binding) in c.into_iter().enumerate() {
        by_binding[binding] = phase * 11 + stage as u32 * 3 + 1;
    }
    (by_binding[0] % 256)
        | (((by_binding[1] + 2 * by_binding[2]) % 256) << 8)
        | (((by_binding[3] + 3 * by_binding[4]) % 256) << 16)
        | 0xff000000
}
fn draw<'a>(pipeline: &'a Pipeline<Platform>, data: &'a [Vec<u8>; 5], x: u32) -> ConstantDraw<'a> {
    ConstantDraw {
        pipeline,
        groups: &[],
        constants: std::array::from_fn(|i| data[i].as_slice()),
        vertices: 0..3,
        instances: 0..1,
        indices: None,
        viewport: None,
        scissor: Some([x, 0, 4, 16]),
        blend: t::Color::TRANSPARENT,
        stencil_reference: 0,
    }
}

#[test]
fn constant_frame_owns_rewinds_draw_consumers_resolve_and_exactly_one_submit() {
    let (_lock, before) = setup();
    let mut gpu = open();
    let (mut arena, pipeline) = arena_pipeline(&mut gpu, false);
    let mut cache = gpu.edram_target_cache(TargetOptions::default()).unwrap();
    let index = gpu
        .create_buffer_init(
            "persistent indices across constant rewinds",
            &[0, 0, 1, 0, 2, 0, 0, 0],
            super::U::INDEX | super::U::COPY_DST,
        )
        .unwrap();
    let mut frame = cache
        .begin(&mut gpu)
        .unwrap()
        .with_constants(&mut arena)
        .unwrap();
    let target = frame.targets(&selection(key()), 16).unwrap();
    assert_eq!(target.owned_extent, [80, 16]);
    for phase in 0..12 {
        let constants = values(phase);
        let mut d = draw(&pipeline, &constants, phase * 4);
        if phase % 2 == 0 {
            d.indices = Some(skyrim_native_gpu::render::Indices {
                buffer: &index,
                format: t::IndexFormat::Uint16,
                offset: 0,
                size: 8,
                base_vertex: 0,
            });
        }
        assert_eq!(frame.draw(&d).unwrap(), DrawAdmission::Accepted);
    }
    let ticket = frame.resolve_pixels(&info(key())).unwrap().unwrap();
    assert!(ticket.serial().is_none());
    let (serial, _, stats) = frame.submit().unwrap();
    assert_eq!(serial, 1);
    assert_eq!(ticket.serial(), Some(serial));
    assert_eq!(stats.admitted, 12);
    assert_eq!(stats.output_rewinds, 11);
    assert_eq!(stats.constant_dispatches, 12);
    assert_eq!(stats.passes, 12);
    let got = ticket.read(&mut gpu, DEADLINE).unwrap();
    for row in got.chunks_exact(80) {
        for phase in 0..12 {
            assert!(
                row[phase * 4..phase * 4 + 4]
                    .iter()
                    .all(|&p| p == expected(phase as u32))
            );
        }
    }
    drop(gpu);
    finish(before);
}

#[test]
fn constant_frame_reuses_unchanged_blocks_without_patch_or_pass_per_draw() {
    let (_lock, before) = setup();
    let mut gpu = open();
    let (mut arena, pipeline) = arena_pipeline(&mut gpu, false);
    let mut cache = gpu.edram_target_cache(TargetOptions::default()).unwrap();
    let mut frame = cache
        .begin(&mut gpu)
        .unwrap()
        .with_constants(&mut arena)
        .unwrap();
    frame.targets(&selection(key()), 16).unwrap();
    for n in 0..10 {
        frame.draw(&draw(&pipeline, &values(1), n * 4)).unwrap();
    }
    let ticket = frame.resolve_pixels(&info(key())).unwrap().unwrap();
    let (_, _, stats) = frame.submit().unwrap();
    assert_eq!(
        (
            stats.admitted,
            stats.output_rewinds,
            stats.constant_dispatches,
            stats.passes
        ),
        (10, 0, 1, 1)
    );
    let pixels = ticket.read(&mut gpu, DEADLINE).unwrap();
    for row in pixels.chunks_exact(80) {
        assert!(row[..40].iter().all(|&p| p == expected(1)));
    }
    drop(gpu);
    finish(before);
}

#[test]
fn constant_frame_staging_pressure_rejects_only_new_draw_and_previous_work_stays_submittable() {
    let (_lock, before) = setup();
    let mut config = Config::default();
    config.max_pending_upload_bytes = 2048;
    let mut gpu = open_config(config);
    let (mut arena, pipeline) = arena_pipeline(&mut gpu, false);
    let mut cache = gpu.edram_target_cache(TargetOptions::default()).unwrap();
    let mut frame = cache
        .begin(&mut gpu)
        .unwrap()
        .with_constants(&mut arena)
        .unwrap();
    frame.targets(&selection(key()), 16).unwrap();
    for phase in 0..4 {
        assert_eq!(
            frame
                .draw(&draw(&pipeline, &values(phase), phase * 4))
                .unwrap(),
            DrawAdmission::Accepted
        );
    }
    assert_eq!(
        frame.draw(&draw(&pipeline, &values(4), 16)).unwrap(),
        DrawAdmission::NeedsSubmit
    );
    assert_eq!(frame.stats().admitted, 4);
    let ticket = frame.resolve_pixels(&info(key())).unwrap().unwrap();
    let (_, _, stats) = frame.submit().unwrap();
    assert_eq!(stats.submission_requests, 1);
    // Defer CPU readback until both draws have been submitted; read_buffer
    // itself submits a synchronization command and owns a separate serial.
    let first = ticket;
    // Retry the SAME previously rejected draw in a new exclusive transaction.
    let mut frame = cache
        .begin(&mut gpu)
        .unwrap()
        .with_constants(&mut arena)
        .unwrap();
    frame.targets(&selection(key()), 16).unwrap();
    assert_eq!(
        frame.draw(&draw(&pipeline, &values(4), 16)).unwrap(),
        DrawAdmission::Accepted
    );
    let ticket = frame.resolve_pixels(&info(key())).unwrap().unwrap();
    let (serial, _, _) = frame.submit().unwrap();
    assert_eq!(serial, 2);
    let first_pixels = first.read(&mut gpu, DEADLINE).unwrap();
    for row in first_pixels.chunks_exact(80) {
        for phase in 0..4 {
            assert!(
                row[phase * 4..phase * 4 + 4]
                    .iter()
                    .all(|&p| p == expected(phase as u32))
            );
        }
    }
    let got = ticket.read(&mut gpu, DEADLINE).unwrap();
    for row in got.chunks_exact(80) {
        assert!(row[16..20].iter().all(|&p| p == expected(4)));
    }
    drop(gpu);
    finish(before);
}

#[test]
fn constant_frame_rejection_discards_already_encoded_draws_and_invalidates_resolve_tickets() {
    let (_lock, before) = setup();
    let mut gpu = open();
    let (mut arena, pipeline) = arena_pipeline(&mut gpu, false);
    let mut cache = gpu.edram_target_cache(TargetOptions::default()).unwrap();
    let mut frame = cache
        .begin(&mut gpu)
        .unwrap()
        .with_constants(&mut arena)
        .unwrap();
    frame.targets(&selection(key()), 16).unwrap();
    frame.draw(&draw(&pipeline, &values(1), 0)).unwrap();
    frame.submit().unwrap();
    let mut frame = cache
        .begin(&mut gpu)
        .unwrap()
        .with_constants(&mut arena)
        .unwrap();
    frame.targets(&selection(key()), 16).unwrap();
    frame.draw(&draw(&pipeline, &values(2), 0)).unwrap();
    let abandoned = frame.resolve_pixels(&info(key())).unwrap().unwrap();
    let mut bad = values(3);
    bad[4].truncate(0);
    assert!(frame.draw(&draw(&pipeline, &bad, 0)).is_err());
    assert!(frame.submit().is_err());
    assert_eq!(cache.last_submission(), 1);
    assert!(abandoned.read(&mut gpu, DEADLINE).is_err());
    let mut frame = cache
        .begin(&mut gpu)
        .unwrap()
        .with_constants(&mut arena)
        .unwrap();
    let ticket = frame.resolve_pixels(&info(key())).unwrap().unwrap();
    frame.submit().unwrap();
    let got = ticket.read(&mut gpu, DEADLINE).unwrap();
    for row in got.chunks_exact(80) {
        assert!(row[..4].iter().all(|&p| p == expected(1)));
    }
    drop(gpu);
    finish(before);
}

#[test]
fn constant_frame_parameter_seal_preserves_pending_pass_and_old_output_windows() {
    let (_lock, before) = setup();
    let mut gpu = open();
    let (mut arena, pipeline) = arena_pipeline(&mut gpu, true);
    let mut cache = gpu.edram_target_cache(TargetOptions::default()).unwrap();
    let mut frame = cache
        .begin(&mut gpu)
        .unwrap()
        .with_constants(&mut arena)
        .unwrap();
    frame.targets(&selection(key()), 16).unwrap();
    let count = xenos_gfx::constant_segments::MAX_RECORDS / 5 + 2;
    for phase in 0..count {
        let data = values(phase as u32);
        let mut d = draw(&pipeline, &data, if phase == 0 { 0 } else { 4 });
        if phase > 0 && phase + 1 < count {
            d.scissor = Some([0, 0, 0, 0]);
        }
        assert_eq!(frame.draw(&d).unwrap(), DrawAdmission::Accepted);
    }
    let ticket = frame.resolve_pixels(&info(key())).unwrap().unwrap();
    let (_, _, stats) = frame.submit().unwrap();
    assert_eq!(
        (
            stats.parameter_seals,
            stats.output_rewinds,
            stats.constant_dispatches,
            stats.passes
        ),
        (1, 0, 2, 1)
    );
    let pixels = ticket.read(&mut gpu, DEADLINE).unwrap();
    for row in pixels.chunks_exact(80) {
        assert!(row[..4].iter().all(|&p| p == expected(0)));
        assert!(row[4..8].iter().all(|&p| p == expected(count as u32 - 1)));
    }
    drop(gpu);
    finish(before);
}

#[test]
fn constant_frame_caught_unwind_poison_prevents_partial_submit() {
    let (_lock, before) = setup();
    let mut gpu = open();
    let (mut arena, pipeline) = arena_pipeline(&mut gpu, false);
    let mut cache = gpu.edram_target_cache(TargetOptions::default()).unwrap();
    let mut frame = cache
        .begin(&mut gpu)
        .unwrap()
        .with_constants(&mut arena)
        .unwrap();
    frame.targets(&selection(key()), 16).unwrap();
    frame.draw(&draw(&pipeline, &values(1), 0)).unwrap();
    frame.test_unwind_next_draw();
    let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        frame.draw(&draw(&pipeline, &values(2), 4))
    }));
    assert!(result.is_err());
    assert!(frame.submit().is_err());
    assert_eq!(cache.last_submission(), 0);
    assert!(gpu.encoder().is_err());
    assert_eq!(gpu.staging_stats().resident_bytes, 0);
    drop(gpu);
    finish(before);
}

#[test]
fn constant_frame_invalid_draw_preflight_cannot_consume_staging_or_rewind_valid_work() {
    let (_lock, before) = setup();
    let mut gpu = open();
    let (mut arena, pipeline) = arena_pipeline(&mut gpu, false);
    let mut cache = gpu.edram_target_cache(TargetOptions::default()).unwrap();
    for invalid_range in [false, true] {
        let mut frame = cache
            .begin(&mut gpu)
            .unwrap()
            .with_constants(&mut arena)
            .unwrap();
        frame.targets(&selection(key()), 16).unwrap();
        frame.draw(&draw(&pipeline, &values(1), 0)).unwrap();
        frame.draw(&draw(&pipeline, &values(2), 4)).unwrap();
        let stats = frame.stats();
        let staging = frame.gpu().staging_stats().created;
        let data = values(3);
        let mut bad = draw(&pipeline, &data, 8);
        if invalid_range {
            bad.vertices = 9..3;
        } else {
            bad.viewport = Some(skyrim_native_gpu::render::Viewport {
                x: f32::NAN,
                y: 0.,
                width: 80.,
                height: 16.,
                min_depth: 0.,
                max_depth: 1.,
            });
        }
        assert!(frame.draw(&bad).is_err());
        assert_eq!(frame.stats(), stats);
        assert_eq!(frame.gpu().staging_stats().created, staging);
        assert!(frame.submit().is_err());
        assert_eq!(cache.last_submission(), 0);
    }
    drop(gpu);
    finish(before);
}

#[test]
fn constant_frame_rewind_after_resolve_or_target_change_preserves_both_generations() {
    let (_lock, before) = setup();
    let mut gpu = open();
    let (mut arena, pipeline) = arena_pipeline(&mut gpu, false);
    for change_target in [false, true] {
        let mut cache = gpu.edram_target_cache(TargetOptions::default()).unwrap();
        let mut frame = cache
            .begin(&mut gpu)
            .unwrap()
            .with_constants(&mut arena)
            .unwrap();
        frame.targets(&selection(key()), 16).unwrap();
        frame.draw(&draw(&pipeline, &values(1), 0)).unwrap();
        let earlier = frame.resolve_pixels(&info(key())).unwrap().unwrap();
        let next = if change_target {
            Key {
                base_tiles: 2,
                ..key()
            }
        } else {
            key()
        };
        if change_target {
            frame.targets(&selection(next), 16).unwrap();
        }
        assert_eq!(
            frame.draw(&draw(&pipeline, &values(2), 4)).unwrap(),
            DrawAdmission::Accepted
        );
        let later = frame.resolve_pixels(&info(next)).unwrap().unwrap();
        let (_, _, stats) = frame.submit().unwrap();
        assert_eq!((stats.admitted, stats.output_rewinds), (2, 1));
        for row in earlier.read(&mut gpu, DEADLINE).unwrap().chunks_exact(80) {
            assert!(row[..4].iter().all(|&p| p == expected(1)));
        }
        for row in later.read(&mut gpu, DEADLINE).unwrap().chunks_exact(80) {
            assert!(row[4..8].iter().all(|&p| p == expected(2)));
        }
    }
    drop(gpu);
    finish(before);
}

#[test]
fn constant_frame_parameter_then_output_pressure_retries_each_remedy_only_once() {
    let (_lock, before) = setup();
    let mut gpu = open();
    let max = xenos_gfx::constant_segments::MAX_RECORDS;
    let (mut arena, pipeline) = arena_pipeline_capacity(&mut gpu, true, Some(max as u64 + 2));
    let mut cache = gpu.edram_target_cache(TargetOptions::default()).unwrap();
    let mut frame = cache
        .begin(&mut gpu)
        .unwrap()
        .with_constants(&mut arena)
        .unwrap();
    frame.targets(&selection(key()), 16).unwrap();
    // 16380 records, then four changed stages reach the exact record limit.
    // Two more output windows fit, so parameter exhaustion on stage0 hides
    // output exhaustion on stage2 until the parameter-only retry.
    for phase in 0..max / 5 {
        let data = values(phase as u32);
        let mut d = draw(&pipeline, &data, 0);
        if phase != 0 {
            d.scissor = Some([0, 0, 0, 0]);
        }
        frame.draw(&d).unwrap();
    }
    let mut penultimate = values((max / 5) as u32);
    penultimate[4] = values((max / 5 - 1) as u32)[4].clone();
    let mut d = draw(&pipeline, &penultimate, 4);
    d.scissor = Some([0, 0, 0, 0]);
    frame.draw(&d).unwrap();
    let last = (max / 5 + 1) as u32;
    assert_eq!(
        frame.draw(&draw(&pipeline, &values(last), 4)).unwrap(),
        DrawAdmission::Accepted
    );
    let ticket = frame.resolve_pixels(&info(key())).unwrap().unwrap();
    let (_, _, stats) = frame.submit().unwrap();
    assert_eq!(
        (stats.parameter_seals, stats.output_rewinds, stats.passes),
        (1, 1, 2)
    );
    for row in ticket.read(&mut gpu, DEADLINE).unwrap().chunks_exact(80) {
        assert!(row[..4].iter().all(|&p| p == expected(0)));
        assert!(row[4..8].iter().all(|&p| p == expected(last)));
    }
    drop(gpu);
    finish(before);
}

#[test]
fn constant_frame_seals_old_reservation_instead_of_submitting_on_growth_peak() {
    let (_lock, before) = setup();
    let mut config = Config::default();
    config.max_staging_buffer_bytes = 1024;
    let mut gpu = open_config(config);
    let (mut arena, pipeline) = arena_pipeline(&mut gpu, true);
    let mut cache = gpu.edram_target_cache(TargetOptions::default()).unwrap();
    let mut frame = cache
        .begin(&mut gpu)
        .unwrap()
        .with_constants(&mut arena)
        .unwrap();
    frame.targets(&selection(key()), 16).unwrap();
    frame.draw(&draw(&pipeline, &values(1), 0)).unwrap();
    // 512 old + 1024 replacement would fail, but sealing keeps the old lease
    // and needs only a new512. Both draws remain in the SAME pending pass.
    assert_eq!(
        frame.draw(&draw(&pipeline, &values(2), 4)).unwrap(),
        DrawAdmission::Accepted
    );
    assert_eq!(
        (
            frame.stats().parameter_seals,
            frame.stats().submission_requests
        ),
        (1, 0)
    );
    let (serial, _, stats) = frame.submit().unwrap();
    assert_eq!((serial, stats.constant_dispatches, stats.passes), (1, 2, 1));
    // This deliberately fills the whole staging budget. Resolve has separate
    // parameter uploads, so execute it after retiring the draw transaction.
    gpu.wait(serial, DEADLINE).unwrap();
    let mut frame = cache
        .begin(&mut gpu)
        .unwrap()
        .with_constants(&mut arena)
        .unwrap();
    let ticket = frame.resolve_pixels(&info(key())).unwrap().unwrap();
    frame.submit().unwrap();
    for row in ticket.read(&mut gpu, DEADLINE).unwrap().chunks_exact(80) {
        assert!(row[..4].iter().all(|&p| p == expected(1)));
        assert!(row[4..8].iter().all(|&p| p == expected(2)));
    }
    drop(gpu);
    finish(before);
}
