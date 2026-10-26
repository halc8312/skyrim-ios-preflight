//! Exercise the integrated native transfer path, not a hand-wired imitation.
//! The independent CPU address function supplies the expected source word for
//! every destination sample. Pixels, shaders and layouts here are authored.
use super::{DEADLINE, U, finish, open, setup};
use skyrim_native_gpu::{
    Encoder, Gpu,
    backend::Platform,
    bindings::{Entry, Resource},
    edram::{Images, StencilMode},
    render::{BoundGroup, ColorAttachment, DepthAttachment, Draw, PipelineDescriptor},
    texture::{TextureDescriptor, TextureView},
    types::{
        self as t, Extent3d, TextureDimension as D, TextureFormat as F, TextureUses as T,
        TextureViewDimension as V,
    },
};
use std::{collections::BTreeMap, num::NonZeroU64};
use xenos_gfx::{edram_shaders::SrcKind, edram_transfer::Transfer};
use xenos_state::{
    edram::{RenderTargetKey as Key, edram_offset_bytes},
    xenos::MsaaSamples as M,
};

const VERTEX: &str = "@vertex fn vs(@builtin(vertex_index) i:u32)->@builtin(position) vec4f {let p=array<vec2f,3>(vec2f(-1.,-1.),vec2f(3.,-1.),vec2f(-1.,3.));return vec4f(p[i],0.,1.);}";
fn count(m: M) -> u32 {
    if m == M::X1 { 1 } else { 4 }
}
fn key(base: u32, pitch: u32, m: M, format: u32, depth: bool) -> Key {
    Key {
        base_tiles: base,
        pitch_tiles_at_32bpp: pitch,
        msaa_samples: m,
        is_depth: depth,
        format,
    }
}
fn offset(k: Key, x: u32, y: u32, s: u32, scale: u32) -> u32 {
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
fn word(x: u32, y: u32, s: u32, half: u32) -> u32 {
    let a = [0x3400u32, 0x3800, 0x3c00, 0x4000];
    a[((x + 3 * y + s + half) % 4) as usize]
        | (a[((3 * x + y + 2 * s + half + 1) % 4) as usize] << 16)
}
fn view(
    gpu: &mut Gpu<Platform>,
    format: F,
    w: u32,
    h: u32,
    samples: u32,
    label: &str,
) -> TextureView<Platform> {
    let image = gpu
        .create_texture(
            label,
            TextureDescriptor {
                size: Extent3d {
                    width: w,
                    height: h,
                    depth_or_array_layers: 1,
                },
                mip_levels: 1,
                sample_count: samples,
                dimension: D::D2,
                format,
                usage: T::RESOURCE
                    | if format.is_depth_stencil_format() {
                        T::DEPTH_STENCIL_WRITE
                    } else {
                        T::COLOR_TARGET
                    },
            },
        )
        .unwrap();
    gpu.texture_view(&image, V::D2, 0..1, 0..1).unwrap()
}
fn seed(gpu: &Gpu<Platform>, e: &mut Encoder<Platform>, image: &TextureView<Platform>, wide: bool) {
    let desc = image.texture().descriptor();
    let samples = desc.sample_count;
    let depth = desc.format.is_depth_stencil_format();
    let multi = samples > 1;
    let sample_arg = if multi {
        ",@builtin(sample_index) s:u32"
    } else {
        ""
    };
    let sample = if multi { "s" } else { "0u" };
    let color = r#"fn w(x:u32,y:u32,s:u32,n:u32)->u32{let a=array<u32,4>(0x3400u,0x3800u,0x3c00u,0x4000u);return a[(x+3u*y+s+n)%4u]|(a[(3u*x+y+2u*s+n+1u)%4u]<<16u);}"#;
    let mut code = format!("{VERTEX}\n{color}\n");
    if depth {
        for s in 0..samples {
            code += &format!(
                "struct R{s}{{@builtin(frag_depth) d:f32,@builtin(sample_mask) m:u32}} @fragment fn fs{s}(@builtin(position) p:vec4f)->R{s}{{return R{s}(f32((u32(p.x)+u32(p.y)+{s}u)%2u),{}u);}}\n",
                1 << s
            );
        }
    } else {
        let value = if wide {
            format!(
                "vec4f(unpack2x16float(w(u32(p.x),u32(p.y),{sample},0u)),unpack2x16float(w(u32(p.x),u32(p.y),{sample},1u)))"
            )
        } else {
            format!("unpack4x8unorm(w(u32(p.x),u32(p.y),{sample},0u))")
        };
        code += &format!(
            "@fragment fn fs(@builtin(position) p:vec4f{sample_arg})->@location(0) vec4f{{return {value};}}"
        );
    }
    let shader = gpu.shader(&code).unwrap();
    let pl = gpu.pipeline_layout(&[]).unwrap();
    let face = t::StencilFaceState {
        compare: t::CompareFunction::Always,
        fail_op: t::StencilOperation::Keep,
        depth_fail_op: t::StencilOperation::Keep,
        pass_op: t::StencilOperation::Replace,
    };
    let state = depth.then(|| t::DepthStencilState {
        format: desc.format,
        depth_write_enabled: Some(true),
        depth_compare: Some(t::CompareFunction::Always),
        stencil: t::StencilState {
            front: face,
            back: face,
            read_mask: 255,
            write_mask: 255,
        },
        bias: Default::default(),
    });
    let colors = if depth {
        vec![]
    } else {
        vec![Some(t::ColorTargetState {
            format: desc.format,
            blend: None,
            write_mask: t::ColorWrites::ALL,
        })]
    };
    let n = if depth { samples } else { 1 };
    let mut pipelines = Vec::new();
    for i in 0..n {
        pipelines.push(
            gpu.render_pipeline(&PipelineDescriptor {
                layout: &pl,
                vertex: &shader,
                vertex_entry: "vs",
                fragment: Some((&shader, &if depth { format!("fs{i}") } else { "fs".into() })),
                colors: &colors,
                primitive: Default::default(),
                depth_stencil: state.clone(),
                multisample: t::MultisampleState {
                    count: samples,
                    ..Default::default()
                },
            })
            .unwrap(),
        );
    }
    let refs = [3, 37, 113, 221];
    let draws: Vec<_> = pipelines
        .iter()
        .enumerate()
        .map(|(i, p)| Draw {
            pipeline: p,
            groups: &[],
            vertices: 0..3,
            instances: 0..1,
            indices: None,
            viewport: None,
            scissor: None,
            blend: t::Color::TRANSPARENT,
            stencil_reference: refs[i],
        })
        .collect();
    if depth {
        e.render_full(
            &[],
            Some(DepthAttachment {
                view: image,
                clear_depth: Some(0.),
                clear_stencil: Some(0),
            }),
            &[],
            &draws,
        )
        .unwrap();
    } else {
        e.render(
            &[Some(ColorAttachment {
                view: image,
                clear: Some(t::Color::TRANSPARENT),
            })],
            &draws,
        )
        .unwrap();
    }
}
pub(super) fn read_samples(
    gpu: &mut Gpu<Platform>,
    e: &mut Encoder<Platform>,
    v: &TextureView<Platform>,
    wide: bool,
) -> (skyrim_native_gpu::Buffer<Platform>, u64) {
    let d = v.texture().descriptor();
    let depth = d.format.is_depth_stencil_format();
    let samples = d.sample_count;
    let multi = samples > 1;
    let w = d.size.width;
    let h = d.size.height;
    let texture_type = if depth {
        if multi {
            "texture_depth_multisampled_2d"
        } else {
            "texture_depth_2d"
        }
    } else if multi {
        "texture_multisampled_2d<f32>"
    } else {
        "texture_2d<f32>"
    };
    let stencil_type = if multi {
        "texture_multisampled_2d<u32>"
    } else {
        "texture_2d<u32>"
    };
    let value = if depth {
        "vec2u(bitcast<u32>(textureLoad(image,vec2i(p.xy),i32(p.z))),textureLoad(stencil,vec2i(p.xy),i32(p.z)).r)"
    } else if wide {
        "vec2u(pack2x16float(v.xy),pack2x16float(v.zw))"
    } else {
        "vec2u(pack4x8unorm(v),0u)"
    };
    let code = format!(
        "@group(0) @binding(0) var image:{texture_type};{} @group(0) @binding(2) var<storage,read_write> output:array<vec2u>; @compute @workgroup_size(1) fn main(@builtin(global_invocation_id) p:vec3u){{{} output[(p.y*{w}u+p.x)*{samples}u+p.z]={value};}}",
        if depth {
            format!("@group(0) @binding(1) var stencil:{stencil_type};")
        } else {
            String::new()
        },
        if depth {
            ""
        } else {
            "let v=textureLoad(image,vec2i(p.xy),i32(p.z));"
        }
    );
    let tex = |binding, sample_type| t::BindGroupLayoutEntry {
        binding,
        visibility: t::ShaderStages::COMPUTE,
        ty: t::BindingType::Texture {
            sample_type,
            view_dimension: V::D2,
            multisampled: multi,
        },
        count: None,
    };
    let mut entries = vec![
        tex(
            0,
            if depth {
                t::TextureSampleType::Depth
            } else {
                t::TextureSampleType::Float { filterable: false }
            },
        ),
        t::BindGroupLayoutEntry {
            binding: 2,
            visibility: t::ShaderStages::COMPUTE,
            ty: t::BindingType::Buffer {
                ty: t::BufferBindingType::Storage { read_only: false },
                has_dynamic_offset: false,
                min_binding_size: NonZeroU64::new(8),
            },
            count: None,
        },
    ];
    if depth {
        entries.push(tex(1, t::TextureSampleType::Uint));
    }
    let layout = gpu.group_layout(&entries).unwrap();
    let pl = gpu.pipeline_layout(&[layout.clone()]).unwrap();
    let shader = gpu.shader(&code).unwrap();
    let kernel = gpu.shared_compute_pipeline(&pl, &shader, "main").unwrap();
    let bytes = u64::from(w * h * samples * 8);
    let out = gpu
        .create_buffer(
            "transfer sample values",
            bytes,
            U::STORAGE_READ_WRITE | U::COPY_SRC,
        )
        .unwrap();
    let read = gpu
        .create_buffer("transfer readback", bytes, U::COPY_DST | U::MAP_READ)
        .unwrap();
    let dv = if depth {
        Some(
            gpu.texture_view_aspect(v.texture(), V::D2, 0..1, 0..1, t::TextureAspect::DepthOnly)
                .unwrap(),
        )
    } else {
        None
    };
    let sv = if depth {
        Some(
            gpu.texture_view_aspect(
                v.texture(),
                V::D2,
                0..1,
                0..1,
                t::TextureAspect::StencilOnly,
            )
            .unwrap(),
        )
    } else {
        None
    };
    let mut entries = vec![
        Entry {
            binding: 0,
            resource: Resource::Texture(dv.as_ref().unwrap_or(v)),
        },
        Entry {
            binding: 2,
            resource: Resource::Buffer {
                buffer: &out,
                offset: 0,
                size: bytes,
            },
        },
    ];
    if let Some(s) = &sv {
        entries.push(Entry {
            binding: 1,
            resource: Resource::Texture(s),
        });
    }
    let group = gpu.bind_group(&layout, &entries).unwrap();
    e.dispatch_shared(
        &kernel,
        &[BoundGroup {
            group: &group,
            dynamic_offsets: &[],
        }],
        [w, h, samples],
    )
    .unwrap();
    e.copy_buffer(&out, 0, &read, 0, bytes).unwrap();
    (read, bytes)
}

#[test]
fn native_edram_transfer_preserves_bits_across_pitches_formats_depth_wrap_and_scaled_msaa() {
    let (_lock, before) = setup();
    let mut gpu = open();
    let mut cache = gpu.edram_transfer_cache(2).unwrap();
    // Source format/depth, destination format/depth, source and destination MSAA.
    let cases = [
        (0, false, 0, false, M::X1, M::X1),
        (0, false, 7, false, M::X2, M::X4),
        (7, false, 0, false, M::X4, M::X2),
        (0, false, 0, true, M::X4, M::X1),
        (0, true, 0, false, M::X2, M::X4),
        (0, true, 0, true, M::X1, M::X2),
    ];
    for scale in [1, 2, 3] {
        for (sf, sd, df, dd, sm, dm) in cases {
            let src = key(2046, 2, sm, sf, sd);
            let dst = key(2047, 3, dm, df, dd);
            let sw = 73 * scale;
            let sh = 19 * scale;
            let dw = 61 * scale;
            let dh = 27 * scale;
            let format = |f, d| {
                if d {
                    F::Depth24PlusStencil8
                } else if f == 7 {
                    F::Rgba16Float
                } else {
                    F::Rgba8Unorm
                }
            };
            let source = view(
                &mut gpu,
                format(sf, sd),
                sw,
                sh,
                count(sm),
                "transfer source",
            );
            let target = view(
                &mut gpu,
                format(df, dd),
                dw,
                dh,
                count(dm),
                "transfer target",
            );
            let depth_view = if sd {
                Some(
                    gpu.texture_view_aspect(
                        source.texture(),
                        V::D2,
                        0..1,
                        0..1,
                        t::TextureAspect::DepthOnly,
                    )
                    .unwrap(),
                )
            } else {
                None
            };
            let stencil_view = if sd {
                Some(
                    gpu.texture_view_aspect(
                        source.texture(),
                        V::D2,
                        0..1,
                        0..1,
                        t::TextureAspect::StencilOnly,
                    )
                    .unwrap(),
                )
            } else {
                None
            };
            let scratch = if dd {
                Some(view(
                    &mut gpu,
                    F::R32Uint,
                    dw,
                    dh,
                    count(dm),
                    "transfer stencil scratch",
                ))
            } else {
                None
            };
            let kind = if sd {
                SrcKind::Depth {
                    multisampled: count(sm) > 1,
                }
            } else {
                SrcKind::Color {
                    multisampled: count(sm) > 1,
                }
            };
            let transfer = cache
                .get(
                    &gpu,
                    kind,
                    format(df, dd),
                    count(dm),
                    scale,
                    StencilMode::PortableBits,
                )
                .unwrap();
            let _hit = cache
                .get(
                    &gpu,
                    kind,
                    format(df, dd),
                    count(dm),
                    scale,
                    StencilMode::PortableBits,
                )
                .unwrap();
            let cut = [2, 3, 9, 10];
            let plan = Transfer::new(src, dst, [sw, sh], [2047, 2], Some(cut), scale).unwrap();
            let mut e = gpu.encoder().unwrap();
            seed(&gpu, &mut e, &source, sf == 7);
            if dd {
                e.render_full(
                    &[],
                    Some(DepthAttachment {
                        view: &target,
                        clear_depth: Some(0.),
                        clear_stencil: Some(0xa5),
                    }),
                    &[],
                    &[],
                )
                .unwrap();
            } else {
                e.render(
                    &[Some(ColorAttachment {
                        view: &target,
                        clear: Some(t::Color::TRANSPARENT),
                    })],
                    &[],
                )
                .unwrap();
            }
            let images = || Images {
                source: depth_view.as_ref().unwrap_or(&source),
                source_stencil: stencil_view.as_ref(),
                destination: &target,
                stencil_scratch: scratch.as_ref(),
            };
            let empty = Transfer::new(src, dst, [sw, sh], [2047, 0], Some(cut), scale).unwrap();
            let params = gpu.edram_transfer_params(&[empty, plan]).unwrap();
            assert_eq!(params.len(), 2);
            assert!(
                transfer
                    .encode_prepared(&gpu, &mut e, &params, 2, images())
                    .is_err()
            );
            assert_eq!(
                transfer
                    .encode_prepared(&gpu, &mut e, &params, 0, images())
                    .unwrap(),
                Default::default()
            );
            let stats = transfer
                .encode_prepared(&gpu, &mut e, &params, 1, images())
                .unwrap();
            assert_eq!(stats.main_draws, 1);
            assert_eq!(stats.stencil_draws, if dd { 8 } else { 0 });
            drop(params);
            drop(transfer);
            let (read, bytes) = read_samples(&mut gpu, &mut e, &target, df == 7);
            gpu.submit(vec![e.finish().unwrap()]).unwrap();
            let actual = gpu.read_buffer(&read, 0..bytes, DEADLINE).unwrap();
            let mut words = BTreeMap::new();
            for y in 0..sh {
                for x in 0..sw {
                    for s in 0..sm.count().unwrap() {
                        let host = if sm == M::X2 { s * 3 } else { s };
                        let address = offset(src, x, y, s, scale);
                        let lo = if sd {
                            [3, 37, 113, 221][host as usize]
                                | if (x + y + host) % 2 == 1 {
                                    0xffffff00
                                } else {
                                    0
                                }
                        } else {
                            word(x, y, host, 0)
                        };
                        assert!(words.insert(address, lo).is_none());
                        if sf == 7 {
                            assert!(words.insert(address + 4, word(x, y, host, 1)).is_none());
                        }
                    }
                }
            }
            for y in 0..dh {
                for x in 0..dw {
                    for host in 0..count(dm) {
                        let sample = if dm == M::X2 { host >> 1 } else { host };
                        let address = offset(dst, x, y, sample, scale);
                        let tile = address / (1280 * scale * scale * 4);
                        let cutout = x >= cut[0] * scale
                            && y >= cut[1] * scale
                            && x < cut[2] * scale
                            && y < cut[3] * scale;
                        let active = tile.wrapping_sub(2047) & 2047 < 2 && !cutout;
                        let i = ((y * dw + x) * count(dm) + host) as usize * 8;
                        let got = [
                            u32::from_le_bytes(actual[i..i + 4].try_into().unwrap()),
                            u32::from_le_bytes(actual[i + 4..i + 8].try_into().unwrap()),
                        ];
                        let lo = if active {
                            *words.get(&address).unwrap_or(&0)
                        } else {
                            0
                        };
                        if dd {
                            let expected = (lo >> 8) as f32 / 16777215.;
                            let got_depth = f32::from_bits(got[0]);
                            assert!(
                                (got_depth - expected).abs() <= 1.0 / 16777215.,
                                "depth scale{scale} {src:?}->{dst:?} ({x},{y},{host}) actual{got_depth} expected{expected}"
                            );
                            assert_eq!(
                                got[1],
                                if active { lo & 255 } else { 0xa5 },
                                "stencil scale{scale} {src:?}->{dst:?} ({x},{y},{host})"
                            );
                        } else {
                            let hi = if active && df == 7 {
                                *words.get(&(address + 4)).unwrap_or(&0)
                            } else {
                                0
                            };
                            assert_eq!(
                                got,
                                [lo, hi],
                                "color scale{scale} {src:?}->{dst:?} ({x},{y},{host})"
                            );
                        }
                    }
                }
            }
        }
    }
    assert_eq!(cache.stats().created, 18);
    assert_eq!(cache.stats().hits, 18);
    assert_eq!(cache.stats().evicted, 16);
    cache.clear();
    assert_eq!(cache.stats().live, 0);
    drop(cache);
    drop(gpu);
    finish(before);
}
