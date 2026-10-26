use super::{DEADLINE, finish, open, open_config, setup};
use skyrim_native_gpu::{
    Config,
    edram::targets::TargetOptions,
    render::{ColorAttachment, Draw, PipelineDescriptor},
    types::{self as t, TextureFormat as F},
};
use xenos_state::{
    edram::{
        RenderTargetBinding as Binding, RenderTargetKey as Key, RenderTargetSetup as Setup,
        RenderTargetSlot as Slot,
    },
    xenos::{ColorRenderTargetFormat as C, EdramMode, MsaaSamples as M},
};
fn key(depth: bool, base: u32) -> Key {
    Key {
        base_tiles: base,
        pitch_tiles_at_32bpp: 1,
        msaa_samples: M::X1,
        is_depth: depth,
        format: 0,
    }
}
fn binding(key: Key, slot: Slot) -> Binding {
    Binding {
        slot,
        key,
        guest_base_tiles: key.base_tiles,
        guest_color_format: (!key.is_depth).then_some(C::K8888),
        exp_bias: 0,
    }
}
fn selection(key: Key) -> Setup {
    let mut s = Setup {
        edram_mode: EdramMode::ColorDepth,
        surface_pitch: 80,
        pitch_tiles_at_32bpp: 1,
        msaa_samples: M::X1,
        depth: None,
        color: [None; 4],
    };
    if key.is_depth {
        s.depth = Some(binding(key, Slot::Depth));
    } else {
        s.color[0] = Some(binding(key, Slot::Color(0)));
    }
    s
}

#[test]
fn native_target_frames_preserve_owned_growth_depth_aliases_and_abort_without_publishing() {
    let (_lock, before) = setup();
    let mut gpu = open();
    let shader=gpu.shader(r#"
@vertex fn vs(@builtin(vertex_index) i:u32)->@builtin(position) vec4f{let p=array<vec2f,3>(vec2f(-1.,-1.),vec2f(3.,-1.),vec2f(-1.,3.));return vec4f(p[i],0.,1.);}
@fragment fn fs(@builtin(position) p:vec4f)->@location(0) vec4f{if ((u32(p.x)+u32(p.y))%2u==0u){return vec4f(1.,0.,0.,0.);}return vec4f(37./255.,1.,1.,1.);}
"#).unwrap();
    let layout = gpu.pipeline_layout(&[]).unwrap();
    let pipeline = gpu
        .render_pipeline(&PipelineDescriptor {
            layout: &layout,
            vertex: &shader,
            vertex_entry: "vs",
            fragment: Some((&shader, "fs")),
            colors: &[Some(t::ColorTargetState {
                format: F::Rgba8Unorm,
                blend: None,
                write_mask: t::ColorWrites::ALL,
            })],
            primitive: Default::default(),
            depth_stencil: None,
            multisample: Default::default(),
        })
        .unwrap();
    for scale in [1, 2] {
        let mut cache = gpu
            .edram_target_cache(TargetOptions {
                scale,
                ..Default::default()
            })
            .unwrap();
        let a = key(false, 0);
        let b = key(true, 0);
        let mut frame = cache.begin(&mut gpu).unwrap();
        let targets = frame.targets(&selection(a), 16).unwrap();
        assert_eq!((targets.width, targets.height), (80 * scale, 128 * scale));
        let old_id = targets.selected[0].view.texture().id();
        frame
            .render_full(
                &[Some(ColorAttachment {
                    view: &targets.selected[0].view,
                    clear: None,
                })],
                None,
                &[],
                &[Draw {
                    pipeline: &pipeline,
                    groups: &[],
                    vertices: 0..3,
                    instances: 0..1,
                    indices: None,
                    viewport: None,
                    scissor: Some([0, 0, 80 * scale, 16 * scale]),
                    blend: t::Color::TRANSPARENT,
                    stencil_reference: 0,
                }],
            )
            .unwrap();
        drop(targets);
        let (first, stats) = frame.submit().unwrap();
        assert_eq!(stats.created_targets, 1);
        assert_eq!(cache.target_count(), 1);
        assert_eq!(cache.ownership().len(), 1);
        assert_eq!(cache.ownership()[0].end, 1);
        // All resources and speculative ownership produced here are abandoned.
        {
            let mut frame = cache.begin(&mut gpu).unwrap();
            let targets = frame.targets(&selection(b), 16).unwrap();
            assert_eq!(targets.selected[0].format, F::Depth32FloatStencil8);
            assert_eq!(frame.stats().transfers, 1);
        }
        assert_eq!(cache.last_submission(), first);
        assert_eq!(cache.target_count(), 1);
        assert_eq!(cache.ownership()[0].owner, a);
        let mut frame = cache.begin(&mut gpu).unwrap();
        let targets = frame.targets(&selection(a), 129).unwrap();
        assert_eq!((targets.width, targets.height), (80 * scale, 256 * scale));
        assert_ne!(targets.selected[0].view.texture().id(), old_id);
        drop(targets);
        let (_, stats) = frame.submit().unwrap();
        assert_eq!(stats.grown_targets, 1);
        assert_eq!(stats.transfers, 1);
        assert_eq!(cache.ownership()[0].end, 9);
        for next in [b, a] {
            let mut frame = cache.begin(&mut gpu).unwrap();
            frame.targets(&selection(next), 129).unwrap();
            let (_, stats) = frame.submit().unwrap();
            assert_eq!(stats.transfers, 1);
            assert_eq!(stats.stencil_draws, if next.is_depth { 8 } else { 0 });
        }
        assert_eq!(cache.target_count(), 2);
        cache.trim_unowned();
        assert_eq!(cache.target_count(), 1);
        assert_eq!(cache.ownership()[0].owner, a);
        let image = cache.view(a).unwrap();
        let mut e = gpu.encoder().unwrap();
        let (read, bytes) =
            super::edram_transfer_cases::read_samples(&mut gpu, &mut e, &image, false);
        gpu.submit(vec![e.finish().unwrap()]).unwrap();
        let actual = gpu.read_buffer(&read, 0..bytes, DEADLINE).unwrap();
        for y in 0..256 * scale {
            for x in 0..80 * scale {
                let i = ((y * 80 * scale + x) * 8) as usize;
                let value = u32::from_le_bytes(actual[i..i + 4].try_into().unwrap());
                let expected = if y >= 16 * scale {
                    0
                } else if (x + y) % 2 == 0 {
                    0x000000ff
                } else {
                    0xffffff25
                };
                assert_eq!(value, expected, "scale{scale} at{x},{y}");
                assert_eq!(&actual[i + 4..i + 8], &[0; 4]);
            }
        }
    }
    drop(gpu);
    finish(before);
}

#[test]
fn target_cache_drops_equal_base_aliases_caps_owned_eviction_and_rejects_whole_frame() {
    let (_lock, before) = setup();
    let mut gpu = open();
    let a = key(false, 0);
    let mut cache = gpu
        .edram_target_cache(TargetOptions {
            max_targets: 1,
            ..Default::default()
        })
        .unwrap();
    let mut frame = cache.begin(&mut gpu).unwrap();
    frame.targets(&selection(a), 16).unwrap();
    let (first, _) = frame.submit().unwrap();
    let mut frame = cache.begin(&mut gpu).unwrap();
    let old = frame.targets(&selection(a), 16).unwrap();
    // A host-wide clear is not a guest EDRAM clear and must reject the frame.
    frame
        .render_full(
            &[Some(ColorAttachment {
                view: &old.selected[0].view,
                clear: Some(t::Color::WHITE),
            })],
            None,
            &[],
            &[],
        )
        .unwrap_err();
    assert!(frame.targets(&selection(key(true, 0)), 16).is_err());
    assert!(frame.submit().is_err());
    assert_eq!(cache.last_submission(), first);
    assert_eq!(cache.target_count(), 1);
    assert_eq!(cache.ownership()[0].owner, a);
    let image = cache.view(a).unwrap();
    let mut e = gpu.encoder().unwrap();
    let (read, bytes) = super::edram_transfer_cases::read_samples(&mut gpu, &mut e, &image, false);
    gpu.submit(vec![e.finish().unwrap()]).unwrap();
    assert!(
        gpu.read_buffer(&read, 0..bytes, DEADLINE)
            .unwrap()
            .iter()
            .all(|&x| x == 0)
    );
    let mut alias_cache = gpu.edram_target_cache(TargetOptions::default()).unwrap();
    let mut s = selection(a);
    s.color[1] = Some(binding(Key { format: 7, ..a }, Slot::Color(1)));
    s.color[2] = Some(binding(
        Key {
            format: 7,
            ..key(false, 2)
        },
        Slot::Color(2),
    ));
    s.depth = Some(binding(key(true, 0), Slot::Depth));
    let mut frame = alias_cache.begin(&mut gpu).unwrap();
    let targets = frame.targets(&s, 16).unwrap();
    assert_eq!(
        targets
            .selected
            .iter()
            .map(|s| s.binding.slot)
            .collect::<Vec<_>>(),
        vec![Slot::Color(0), Slot::Color(2)]
    );
    assert_eq!(targets.sample_mask, !0);
    frame.submit().unwrap();
    assert_eq!(
        alias_cache
            .ownership()
            .iter()
            .map(|r| (r.start, r.end, r.owner.format))
            .collect::<Vec<_>>(),
        vec![(0, 1, 0), (2, 4, 7)]
    );
    let mut bad = selection(a);
    bad.msaa_samples = M::Unknown(3);
    let mut frame = alias_cache.begin(&mut gpu).unwrap();
    assert!(frame.targets(&bad, 16).is_err());
    assert!(frame.submit().is_err());
    let mut other = open();
    assert!(alias_cache.begin(&mut other).is_err());
    drop(other);
    drop(gpu);
    finish(before);
}

#[test]
fn unknown_submission_does_not_publish_target_ownership_and_poison_prevents_future_frames() {
    let (_lock, before) = setup();
    let mut gpu = open_config(Config {
        drop_timeout: std::time::Duration::from_millis(1),
        ..Default::default()
    });
    let mut cache = gpu.edram_target_cache(TargetOptions::default()).unwrap();
    gpu.fail_next_submit();
    let mut frame = cache.begin(&mut gpu).unwrap();
    frame.targets(&selection(key(false, 0)), 16).unwrap();
    assert!(frame.submit().is_err());
    assert_eq!(cache.last_submission(), 0);
    assert_eq!(cache.target_count(), 0);
    assert!(cache.ownership().is_empty());
    assert!(cache.begin(&mut gpu).is_err());
    drop(cache);
    drop(gpu);
    finish(before);
}

#[test]
fn ownership_bounds_reject_host_clears_and_preserve_adjacent_owner_after_abandoned_resolve() {
    use skyrim_native_gpu::{render::DepthAttachment, sink::PassSink};
    use xenos_state::{
        bitfields::RbCopyControl,
        resolve::{ResolveEdramInfo, ResolveInfo, ResolveRect},
        xenos::{CopyCommand, TextureFormat as G},
    };
    let (_lock, before) = setup();
    let mut gpu = open();
    #[cfg(feature = "fault-injection")]
    gpu.test_resolve_initialization(true);
    let shader=gpu.shader(r#"
@vertex fn vs(@builtin(vertex_index)i:u32)->@builtin(position)vec4f{let p=array<vec2f,3>(vec2f(-1.,-1.),vec2f(3.,-1.),vec2f(-1.,3.));return vec4f(p[i],0.,1.);}
@fragment fn red()->@location(0)vec4f{return vec4f(1.,0.,0.,1.);}
@fragment fn green()->@location(0)vec4f{return vec4f(0.,1.,0.,1.);}
@fragment fn blue()->@location(0)vec4f{return vec4f(0.,0.,1.,1.);}
"#).unwrap();
    let layout = gpu.pipeline_layout(&[]).unwrap();
    let pipelines: Vec<_> = ["red", "green", "blue"]
        .into_iter()
        .map(|entry| {
            gpu.render_pipeline(&PipelineDescriptor {
                layout: &layout,
                vertex: &shader,
                vertex_entry: "vs",
                fragment: Some((&shader, entry)),
                colors: &[Some(t::ColorTargetState {
                    format: F::Rgba8Unorm,
                    blend: None,
                    write_mask: t::ColorWrites::ALL,
                })],
                primitive: Default::default(),
                depth_stencil: None,
                multisample: Default::default(),
            })
            .unwrap()
        })
        .collect();
    let mut cache = gpu.edram_target_cache(TargetOptions::default()).unwrap();
    let a = key(false, 0);
    let b = key(false, 1);
    for (k, h, pipeline) in [(a, 32, &pipelines[0]), (b, 16, &pipelines[1])] {
        let mut f = cache.begin(&mut gpu).unwrap();
        let targets = f.targets(&selection(k), h).unwrap();
        f.render_full(
            &[Some(ColorAttachment {
                view: &targets.selected[0].view,
                clear: None,
            })],
            None,
            &[],
            &[Draw {
                pipeline,
                groups: &[],
                vertices: 0..3,
                instances: 0..1,
                indices: None,
                viewport: None,
                scissor: Some([0, 0, 80, h]),
                blend: t::Color::TRANSPARENT,
                stencil_reference: 0,
            }],
        )
        .unwrap();
        f.submit().unwrap();
    }
    let mut info = ResolveInfo {
        copy_control: RbCopyControl {
            copy_command: CopyCommand::Raw,
            ..Default::default()
        },
        rect: ResolveRect {
            x: 0,
            y: 0,
            width: 80,
            height: 32,
        },
        color_edram: Some(ResolveEdramInfo {
            pitch_tiles: 1,
            msaa_samples: M::X1,
            is_depth: false,
            base_tiles: 0,
            format: 0,
            format_is_64bpp: false,
            fill_half_pixel_offset: false,
            decode_pwl_gamma: false,
        }),
        copy_dest_extent_start: 0x1000,
        copy_dest_extent_length: 128 << 10,
        copy_dest_base: 0x1000,
        dest_pitch_aligned: 80,
        dest_height_aligned: 32,
        dest_format: G::K8888,
        ..Default::default()
    };
    info.copy_dest_info.copy_dest_format = G::K8888;
    let owned = cache.ownership().to_vec();
    // Real read transitions and shadow writes are encoded, then abandoned. The
    // next accepted resolve must derive native barriers from submitted state.
    let abandoned = {
        let mut f = cache.begin(&mut gpu).unwrap();
        f.resolve_pixels(&info).unwrap().unwrap()
    };
    assert!(abandoned.read(&mut gpu, DEADLINE).is_err());
    for mode in 0..5 {
        let mut f = cache.begin(&mut gpu).unwrap();
        let target = f.targets(&selection(a), 16).unwrap();
        assert_eq!((target.height, target.owned_height), (128, 16));
        // Queue an actual visible mutation BEFORE the later rejection. The
        // whole frame must be discarded, including this otherwise valid draw.
        f.render_full(
            &[Some(ColorAttachment {
                view: &target.selected[0].view,
                clear: None,
            })],
            None,
            &[],
            &[Draw {
                pipeline: &pipelines[2],
                groups: &[],
                vertices: 0..3,
                instances: 0..1,
                indices: None,
                viewport: None,
                scissor: Some([0, 0, 80, 16]),
                blend: t::Color::TRANSPARENT,
                stencil_reference: 0,
            }],
        )
        .unwrap();
        let colors = [Some(ColorAttachment {
            view: &target.selected[0].view,
            clear: (mode == 0 || mode == 4).then_some(t::Color::WHITE),
        })];
        if mode >= 3 {
            let mut r = f.gpu().render_recorder(&colors, None, &[]).unwrap();
            if mode == 3 {
                r.set_pipeline(&pipelines[2]).unwrap();
                r.set_scissor([0, 0, 80, 32]).unwrap();
                r.draw(0..3, 0..1).unwrap();
            }
            assert!(f.render_recorded(&r).is_err());
        } else {
            let draw = Draw {
                pipeline: &pipelines[2],
                groups: &[],
                vertices: 0..3,
                instances: 0..1,
                indices: None,
                viewport: None,
                scissor: if mode == 2 {
                    None
                } else {
                    Some([0, 0, 80, 32])
                },
                blend: t::Color::TRANSPARENT,
                stencil_reference: 0,
            };
            assert!(f.render_full(&colors, None, &[], &[draw]).is_err());
        }
        assert!(f.submit().is_err());
        assert_eq!(cache.ownership(), owned.as_slice());
    }
    // Read before the later successful blue draw could hide leaked writes.
    // Both the original red owner and adjacent green owner must survive.
    {
        let mut f = cache.begin(&mut gpu).unwrap();
        let result = f.resolve_pixels(&info).unwrap().unwrap();
        f.submit().unwrap();
        let words = result.read(&mut gpu, DEADLINE).unwrap();
        for (i, &word) in words.iter().enumerate() {
            assert_eq!(
                word,
                if i < 80 * 16 { 0xff0000ff } else { 0xff00ff00 },
                "rejected frame leaked a mutation at pixel{i}"
            );
        }
    }
    let mut f = cache.begin(&mut gpu).unwrap();
    let targets = f.targets(&selection(a), 16).unwrap();
    let colors = [Some(ColorAttachment {
        view: &targets.selected[0].view,
        clear: None,
    })];
    let mut r = f.gpu().render_recorder(&colors, None, &[]).unwrap();
    r.set_pipeline(&pipelines[2]).unwrap();
    r.set_scissor([0, 0, 80, 16]).unwrap();
    r.draw(0..3, 0..1).unwrap();
    f.render_recorded(&r).unwrap();
    for _ in 0..3 {
        f.targets(&selection(a), 16).unwrap();
    }
    assert_eq!(f.stats().transfers, 0);
    let result = f.resolve_pixels(&info).unwrap().unwrap();
    f.submit().unwrap();
    let words = result.read(&mut gpu, DEADLINE).unwrap();
    for (i, &word) in words.iter().enumerate() {
        assert_eq!(
            word,
            if i < 80 * 16 { 0xffff0000 } else { 0xff00ff00 },
            "pixel{i}"
        );
    }
    // Depth and stencil load-op clears are equally outside the Frame contract.
    for stencil in [false, true] {
        let mut f = cache.begin(&mut gpu).unwrap();
        let target = f.targets(&selection(key(true, 8)), 16).unwrap();
        let d = DepthAttachment {
            view: &target.selected[0].view,
            clear_depth: (!stencil).then_some(0.5),
            clear_stencil: stencil.then_some(123),
        };
        assert!(f.render_full(&[], Some(d), &[], &[]).is_err());
        assert!(f.submit().is_err());
    }
    assert_eq!(cache.ownership(), owned.as_slice());
    drop(gpu);
    finish(before);
}

#[test]
fn guest_depth_requires_explicit_float_host_storage_for_both_guest_encodings() {
    let (_lock, before) = setup();
    for enabled in [false, true] {
        for format in [0, 1] {
            let mut config = Config::default();
            config.optional_texture_features = if enabled {
                t::Features::DEPTH32FLOAT_STENCIL8
            } else {
                t::Features::empty()
            };
            let mut gpu = open_config(config);
            let mut cache = gpu.edram_target_cache(TargetOptions::default()).unwrap();
            let mut f = cache.begin(&mut gpu).unwrap();
            let k = Key {
                format,
                ..key(true, 0)
            };
            if enabled
                && f.gpu()
                    .features()
                    .contains(t::Features::DEPTH32FLOAT_STENCIL8)
            {
                let selected = f.targets(&selection(k), 16).unwrap();
                assert_eq!(selected.selected[0].format, F::Depth32FloatStencil8);
                f.submit().unwrap();
            } else {
                assert!(f.targets(&selection(k), 16).is_err());
                assert!(f.submit().is_err());
                assert!(cache.ownership().is_empty());
            }
            drop(gpu);
        }
    }
    finish(before);
}
