//! Authored multi-owner resolves, compared against independent CPU EDRAM byte
//! addresses and literal color words, never a matching GPU inverse transform.
use super::{DEADLINE, finish, open, setup};
use skyrim_native_gpu::{
    edram::targets::TargetOptions,
    render::{ColorAttachment, Draw, PipelineDescriptor},
    types::{self as t, TextureFormat as F},
};
use std::collections::HashMap;
use xenos_state::{
    bitfields::RbCopyControl,
    edram::{
        RenderTargetBinding as Binding, RenderTargetKey as Key, RenderTargetSetup as Setup,
        RenderTargetSlot as Slot, edram_offset_bytes,
    },
    resolve::{ResolveEdramInfo, ResolveInfo, ResolveRect},
    xenos::{
        ColorRenderTargetFormat as C, CopyCommand, CopySampleSelect as Select, EdramMode,
        MsaaSamples as M, SurfaceNumberFormat as N, TextureFormat as G,
    },
};

pub(super) fn selection(k: Key) -> Setup {
    Setup {
        edram_mode: EdramMode::ColorDepth,
        surface_pitch: k.width_pixels(),
        pitch_tiles_at_32bpp: k.pitch_tiles_at_32bpp,
        msaa_samples: k.msaa_samples,
        color: [
            Some(Binding {
                slot: Slot::Color(0),
                key: k,
                guest_base_tiles: k.base_tiles,
                guest_color_format: Some(C::from_raw(k.format)),
                exp_bias: 0,
            }),
            None,
            None,
            None,
        ],
        depth: None,
    }
}
pub(super) fn info(k: Key) -> ResolveInfo {
    let mut r = ResolveInfo {
        copy_control: RbCopyControl {
            copy_command: CopyCommand::Raw,
            ..Default::default()
        },
        rect: ResolveRect {
            x: 0,
            y: 0,
            width: k.width_pixels(),
            height: 16 >> k.msaa_samples.samples_y_log2(),
        },
        color_edram: Some(ResolveEdramInfo {
            pitch_tiles: k.pitch_tiles(),
            msaa_samples: k.msaa_samples,
            is_depth: false,
            base_tiles: k.base_tiles,
            format: k.format,
            format_is_64bpp: k.is_64bpp(),
            fill_half_pixel_offset: false,
            decode_pwl_gamma: false,
        }),
        color_original_base: k.base_tiles,
        copy_sample_select: Select::from_raw(k.msaa_samples.count().unwrap() - 1),
        copy_dest_extent_start: 0x1000,
        copy_dest_extent_length: 128 << 10,
        copy_dest_base: 0x1000,
        dest_pitch_aligned: 160,
        dest_height_aligned: 32,
        dest_format: if k.is_64bpp() {
            G::K16161616Float
        } else {
            G::K8888
        },
        ..Default::default()
    };
    r.copy_dest_info.copy_dest_format = r.dest_format;
    r.copy_dest_info.copy_dest_number = if k.is_64bpp() {
        N::Float
    } else {
        N::UnsignedRepeatingFraction
    };
    r
}
fn pixel(x: u32, y: u32, s: u32, salt: u32, wide: bool) -> [u32; 2] {
    if wide {
        let a = [0x3400, 0x3800, 0x3c00, 0x4000];
        let at = (x + 3 * y + s + salt) as usize;
        [
            a[at % 4] | (a[(at + 1) % 4] << 16),
            a[(at + 2) % 4] | (a[(at + 3) % 4] << 16),
        ]
    } else {
        [
            (x + salt) % 256
                | (((y * 3 + salt) % 256) << 8)
                | (((s * 53 + salt) % 256) << 16)
                | (255 << 24),
            0,
        ]
    }
}
fn address(k: Key, x: u32, y: u32, s: u32, scale: u32) -> u32 {
    edram_offset_bytes(
        [x, y],
        k.base_tiles,
        true,
        k.pitch_tiles(),
        k.msaa_samples,
        k.is_depth,
        k.is_64bpp(),
        s,
        [scale, scale],
    )
}

#[test]
fn target_owned_resolve_handles_two_owners_wrap_msaa_scaling_and_immutable_parameter_versions() {
    let (_lock, before) = setup();
    let mut gpu = open();
    #[cfg(feature = "fault-injection")]
    gpu.test_resolve_initialization(true);
    let layout = gpu.pipeline_layout(&[]).unwrap();
    let shader=gpu.shader(r#"
@vertex fn vs(@builtin(vertex_index) i:u32)->@builtin(position) vec4f{let p=array<vec2f,3>(vec2f(-1.,-1.),vec2f(3.,-1.),vec2f(-1.,3.));return vec4f(p[i],0.,1.);}
fn color(p:vec4f,s:u32,salt:u32)->vec4f{return vec4f(f32((u32(p.x)+salt)%256u),f32((u32(p.y)*3u+salt)%256u),f32((s*53u+salt)%256u),255.)/255.;}
fn wide(p:vec4f,s:u32,salt:u32)->vec4f{let a=array<f32,4>(0.25,0.5,1.,2.);let n=u32(p.x)+3u*u32(p.y)+s+salt;return vec4f(a[n%4u],a[(n+1u)%4u],a[(n+2u)%4u],a[(n+3u)%4u]);}
@fragment fn a(@builtin(position)p:vec4f,@builtin(sample_index)s:u32)->@location(0)vec4f{return color(p,s,7u);}
@fragment fn b(@builtin(position)p:vec4f,@builtin(sample_index)s:u32)->@location(0)vec4f{return color(p,s,31u);}
@fragment fn aw(@builtin(position)p:vec4f,@builtin(sample_index)s:u32)->@location(0)vec4f{return wide(p,s,7u);}
@fragment fn bw(@builtin(position)p:vec4f,@builtin(sample_index)s:u32)->@location(0)vec4f{return wide(p,s,31u);}
"#).unwrap();
    for scale in [1, 2, 3] {
        for m in [M::X1, M::X2, M::X4] {
            for wide in [false, true] {
                let a = Key {
                    base_tiles: 2047,
                    pitch_tiles_at_32bpp: 2,
                    msaa_samples: m,
                    is_depth: false,
                    format: if wide { 7 } else { 0 },
                };
                let b = Key { base_tiles: 0, ..a };
                let h = 16 >> m.samples_y_log2();
                let samples = if m == M::X1 { 1 } else { 4 };
                let mut cache = gpu
                    .edram_target_cache(TargetOptions {
                        scale,
                        ..Default::default()
                    })
                    .unwrap();
                let mut memory = HashMap::new();
                let mut frame = cache.begin(&mut gpu).unwrap();
                for (key, entry, salt) in [
                    (a, if wide { "aw" } else { "a" }, 7),
                    (b, if wide { "bw" } else { "b" }, 31),
                ] {
                    let targets = frame.targets(&selection(key), h).unwrap();
                    let p = frame
                        .gpu()
                        .render_pipeline(&PipelineDescriptor {
                            layout: &layout,
                            vertex: &shader,
                            vertex_entry: "vs",
                            fragment: Some((&shader, entry)),
                            colors: &[Some(t::ColorTargetState {
                                format: if wide { F::Rgba16Float } else { F::Rgba8Unorm },
                                blend: None,
                                write_mask: t::ColorWrites::ALL,
                            })],
                            primitive: Default::default(),
                            depth_stencil: None,
                            multisample: t::MultisampleState {
                                count: samples,
                                ..Default::default()
                            },
                        })
                        .unwrap();
                    frame
                        .render_full(
                            &[Some(ColorAttachment {
                                view: &targets.selected[0].view,
                                clear: None,
                            })],
                            None,
                            &[],
                            &[Draw {
                                pipeline: &p,
                                groups: &[],
                                vertices: 0..3,
                                instances: 0..1,
                                indices: None,
                                viewport: None,
                                scissor: Some([0, 0, targets.width, h * scale]),
                                blend: t::Color::TRANSPARENT,
                                stencil_reference: 0,
                            }],
                        )
                        .unwrap();
                    for y in 0..h * scale {
                        for x in 0..targets.width {
                            for s in 0..m.count().unwrap() {
                                let host = if m == M::X2 && s == 1 { 3 } else { s };
                                let p = pixel(x, y, host, salt, wide);
                                let at = address(key, x, y, s, scale);
                                memory.insert(at, p[0]);
                                if wide {
                                    memory.insert(at + 4, p[1]);
                                }
                            }
                        }
                    }
                }
                let request = info(a);
                let first = frame.resolve_pixels(&request).unwrap().unwrap();
                assert_eq!(first.serial(), None);
                let mut swapped = request.clone();
                swapped.copy_dest_info.copy_dest_swap = true;
                let second = frame.resolve_pixels(&swapped).unwrap().unwrap();
                let (serial, stats) = frame.submit().unwrap();
                assert_eq!(first.serial(), Some(serial));
                assert_eq!(stats.resolves, 2);
                assert!(stats.dump_dispatches >= 4);
                let words = first.read(&mut gpu, DEADLINE).unwrap();
                let swapped = second.read(&mut gpu, DEADLINE).unwrap();
                let mut expected = Vec::new();
                let mut expected_swap = Vec::new();
                for y in 0..h {
                    for x in 0..a.width_pixels() {
                        let at = address(
                            a,
                            x * scale + scale / 2,
                            y * scale + scale / 2,
                            m.count().unwrap() - 1,
                            scale,
                        );
                        let lo = *memory.get(&at).unwrap_or(&0);
                        let hi = *memory.get(&(at + 4)).unwrap_or(&0);
                        expected.push(lo);
                        if wide {
                            expected.push(hi);
                            expected_swap.extend([
                                (hi & 65535) | (lo & 0xffff0000),
                                (lo & 65535) | (hi & 0xffff0000),
                            ]);
                        } else {
                            expected_swap
                                .push((lo & 0xff00ff00) | ((lo & 255) << 16) | ((lo >> 16) & 255));
                        }
                    }
                }
                assert_eq!(words, expected, "scale{scale} msaa{m:?} wide{wide}");
                assert_eq!(
                    swapped, expected_swap,
                    "swapped scale{scale} msaa{m:?} wide{wide}"
                );
                // Reusing the shadow across submissions must refresh from current owners.
                let mut frame = cache.begin(&mut gpu).unwrap();
                let again = frame.resolve_pixels(&request).unwrap().unwrap();
                frame.submit().unwrap();
                assert_eq!(again.read(&mut gpu, DEADLINE).unwrap(), expected);
                // The ticket survives abort but cannot observe its initialized-zero buffer
                // as if a resolve had actually run. CPU ownership stays unchanged.
                let previous = cache.last_submission();
                let aborted = {
                    let mut frame = cache.begin(&mut gpu).unwrap();
                    frame.resolve_pixels(&request).unwrap().unwrap()
                };
                assert!(aborted.read(&mut gpu, DEADLINE).is_err());
                assert_eq!(cache.last_submission(), previous);
            }
        }
    }
    drop(gpu);
    finish(before);
}

#[test]
fn resolve_contract_rejections_and_unknown_submit_do_not_publish_or_expose_pixels() {
    let (_lock, before) = setup();
    let mut gpu = open();
    #[cfg(feature = "fault-injection")]
    gpu.test_resolve_initialization(true);
    let key = Key {
        base_tiles: 2047,
        pitch_tiles_at_32bpp: 2,
        msaa_samples: M::X1,
        is_depth: false,
        format: 0,
    };
    let request = info(key);
    let mut cache = gpu.edram_target_cache(Default::default()).unwrap();
    let good = {
        let mut frame = cache.begin(&mut gpu).unwrap();
        let pixels = frame.resolve_pixels(&request).unwrap().unwrap();
        frame.submit().unwrap();
        pixels
    };
    assert!(
        good.read(&mut gpu, DEADLINE)
            .unwrap()
            .iter()
            .all(|&n| n == 0)
    );
    let mut other = open();
    assert!(good.read(&mut other, DEADLINE).is_err());
    drop(other);
    for case in 0..6 {
        let mut bad = request.clone();
        match case {
            0 => bad.color_edram.as_mut().unwrap().base_tiles = u32::MAX,
            1 => bad.color_edram.as_mut().unwrap().pitch_tiles = 0,
            2 => bad.rect.width = 8193,
            3 => { bad.copy_control.color_clear_enable = true; bad.color_original_base = u32::MAX; },
            4 => {
                bad.rect.width = 8192;
                bad.rect.height = 8192;
            }
            _ => bad.copy_sample_select = Select::S3,
        }
        let last = cache.last_submission();
        let mut frame = cache.begin(&mut gpu).unwrap();
        assert!(frame.resolve_pixels(&bad).is_err());
        assert!(frame.submit().is_err());
        assert_eq!(cache.last_submission(), last);
    }
    gpu.fail_next_submit();
    let last = cache.last_submission();
    let mut frame = cache.begin(&mut gpu).unwrap();
    let ticket = frame.resolve_pixels(&request).unwrap().unwrap();
    assert!(frame.submit().is_err());
    assert_eq!(cache.last_submission(), last);
    assert!(ticket.read(&mut gpu, DEADLINE).is_err());
    drop(gpu);
    finish(before);
}
