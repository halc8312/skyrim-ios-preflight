//! Real public Xenos translator -> immutable read proof -> exclusive native
//! constants/draw/EDRAM -> resolve. All instructions and vertices are authored.
use super::{DEADLINE, finish, open, setup};
use super::{
    pipeline_cases::normalized_key,
    resolve_cache_cases::{info, selection},
};
use skyrim_native_gpu::{
    edram::targets::{ConstantDraw, DrawAdmission, GuestPage, TargetOptions},
    pipeline::PipelineCache,
    render::BoundGroup,
    types as t,
};
use std::sync::Arc;
use xenos_shader::{ShaderStage, TranslationOptions, bindings};
use xenos_state::{edram::RenderTargetKey as Key, xenos::MsaaSamples};

#[test]
fn translated_guest_pipeline_proves_short_constants_through_full_native_frame_and_resolve() {
    let (_lock, before) = setup();
    let mut gpu = open();
    const VS: [u32; 12] = [
        0x00013001, 0x00002200, 0, 0x00081000, 0x00390A88, 3, 0xC80F803E, 0, 0xE2010100,
        0xC80F8000, 0, 0x22000000,
    ];
    const PS: [u32; 6] = [0x00001001, 0x00002200, 0, 0xC80F8000, 0, 0xE2000000];
    let options = TranslationOptions {
        vs_output_interpolators: 1,
        ps_interpolator_count: Some(1),
        ..Default::default()
    };
    assert!(
        gpu.guest_shader(ShaderStage::Vertex, &[], &options)
            .is_err()
    );
    let vertex = gpu
        .guest_shader(ShaderStage::Vertex, &VS, &options)
        .unwrap();
    let pixel = gpu.guest_shader(ShaderStage::Pixel, &PS, &options).unwrap();
    assert!(vertex.metadata().wgsl.is_empty());
    let mut arena = gpu.xenos_draw_arena(16 << 10).unwrap();
    let mut memory = gpu.guest_memory(1).unwrap();
    let shared_layout = memory.layout().clone();
    let empty_layout = gpu.group_layout(&[]).unwrap();
    let layout = gpu
        .pipeline_layout(&[
            arena.layout().clone(),
            shared_layout.clone(),
            empty_layout.clone(),
        ])
        .unwrap();
    let mut cache = PipelineCache::new(8).unwrap();
    let key = normalized_key();
    let program = arena
        .guest_pipeline(&gpu, &mut cache, &key, &layout, &vertex, Some(&pixel))
        .unwrap();
    assert_eq!(program.constant_bytes(), [16, 0, 160, 768, 112]);
    // Mutating a COPY of metadata cannot grant a different live read proof.
    let mut copy = vertex.metadata().clone();
    copy.info.float_dynamic_addressing = true;
    assert_eq!(copy.info.float_constants_read(), 256);
    assert_eq!(program.constant_bytes()[0], 16);
    let mut changed_key = key.clone();
    changed_key.colors[0].as_mut().unwrap().write_mask = 7;
    let wrong = arena
        .guest_pipeline(
            &gpu,
            &mut cache,
            &changed_key,
            &layout,
            &vertex,
            Some(&pixel),
        )
        .unwrap();
    assert!(
        arena
            .guest_pipeline(&gpu, &mut cache, &key, &layout, &pixel, Some(&vertex))
            .is_err()
    );
    let mut captured = [0u8; 4096];
    let vertices: Vec<u8> = [[-1.0f32, -1., 0.5], [3., -1., 0.5], [-1., 3., 0.5]]
        .into_iter()
        .flatten()
        .flat_map(f32::to_be_bytes)
        .collect();
    captured[..vertices.len()].copy_from_slice(&vertices);
    let page = GuestPage {
        generation: None,
        address: 65536,
        bytes: Arc::new(captured),
    };
    let empty = gpu.bind_group(&empty_layout, &[]).unwrap();
    let groups = [BoundGroup {
        group: &empty,
        dynamic_offsets: &[],
    }];
    let green: Vec<u8> = [0f32, 1., 0., 1.]
        .into_iter()
        .flat_map(f32::to_le_bytes)
        .collect();
    let bools = vec![0u8; 160];
    let mut fetch = vec![0u8; 768];
    for (i, v) in [0x10000u32 | 3, 2 | 9 * 4 << 2].into_iter().enumerate() {
        fetch[i * 4..i * 4 + 4].copy_from_slice(&v.to_le_bytes());
    }
    let system = bindings::SystemConstants::default();
    let draw = ConstantDraw {
        pipeline: program.pipeline(),
        groups: &groups,
        constants: [&green, &[], &bools, &fetch, system.as_bytes()],
        vertices: 0..3,
        instances: 0..1,
        indices: None,
        viewport: None,
        scissor: None,
        blend: t::Color::TRANSPARENT,
        stencil_reference: 0,
    };
    let key = Key {
        base_tiles: 0,
        pitch_tiles_at_32bpp: 1,
        msaa_samples: MsaaSamples::X1,
        is_depth: false,
        format: 0,
    };
    let mut targets = gpu.edram_target_cache(TargetOptions::default()).unwrap();
    // A short prefix without a matching immutable pipeline is rejected.
    for use_wrong_proof in [false, true] {
        let mut frame = targets
            .begin(&mut gpu)
            .unwrap()
            .with_constants(&mut arena)
            .unwrap()
            .with_guest_memory(&mut memory)
            .unwrap();
        frame.targets(&selection(key), 16).unwrap();
        assert!(
            if use_wrong_proof {
                frame.draw_guest(&wrong, &draw)
            } else {
                frame.draw(&draw)
            }
            .is_err()
        );
        assert!(frame.submit().is_err());
        assert_eq!(targets.last_submission(), 0);
    }
    let mut frame = targets
        .begin(&mut gpu)
        .unwrap()
        .with_constants(&mut arena)
        .unwrap()
        .with_guest_memory(&mut memory)
        .unwrap();
    frame.targets(&selection(key), 16).unwrap();
    frame.sync_guest_pages(&[page]).unwrap();
    assert_eq!(
        frame.draw_guest(&program, &draw).unwrap(),
        DrawAdmission::Accepted
    );
    let ticket = frame.resolve_pixels(&info(key)).unwrap().unwrap();
    let (serial, _, stats) = frame.submit().unwrap();
    assert_eq!(serial, 1);
    assert_eq!(stats.snapshot_bytes, 64 + 5 * 52 + 1056);
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
