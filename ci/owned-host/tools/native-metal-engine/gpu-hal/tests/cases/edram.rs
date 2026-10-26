//! Public production kernels run with authored pixels only.
//! CPU address oracle is the independent public xenos-state implementation.
use super::{DEADLINE, U, finish, open, setup};
use skyrim_native_gpu::{
    bindings::{Entry, Resource},
    render::BoundGroup,
    texture::TextureDescriptor,
    types::{
        self as t, Extent3d, Origin3d, TexelCopyBufferLayout as L, TextureDimension as D,
        TextureFormat as F, TextureUses as T, TextureViewDimension as V,
    },
};
use std::num::NonZeroU64;
use xenos_gfx::edram_shaders as production;
fn pixel(x: u32, y: u32, wide: bool) -> Vec<u8> {
    if wide {
        let choices = [0u16, 0x3400, 0x3800, 0x3c00, 0x4000, 0xb800];
        (0..4)
            .flat_map(|c| choices[((x * 3 + y * 5 + c * 7) % 6) as usize].to_le_bytes())
            .collect()
    } else {
        (0..4)
            .map(|c| ((x * 17 + y * 31 + c * 59 + 13) & 255) as u8)
            .collect()
    }
}

#[test]
fn production_stencil_bit_variants_restore_all_bytes_and_preserve_discarded_pixels() {
    use skyrim_native_gpu::render::{ColorAttachment, DepthAttachment, Draw, PipelineDescriptor};
    let (_lock, before) = setup();
    let mut gpu = open();
    for samples in [1, 4] {
        let multi = samples > 1;
        let size = Extent3d {
            width: 32,
            height: 16,
            depth_or_array_layers: 1,
        };
        let scratch = gpu
            .create_texture(
                "authored stencil scratch",
                TextureDescriptor {
                    size,
                    mip_levels: 1,
                    sample_count: samples,
                    dimension: D::D2,
                    format: F::R32Uint,
                    usage: T::RESOURCE | T::COLOR_TARGET,
                },
            )
            .unwrap();
        let scratch_view = gpu.texture_view(&scratch, V::D2, 0..1, 0..1).unwrap();
        let ds = gpu
            .create_texture(
                "stencil restoration destination",
                TextureDescriptor {
                    size,
                    mip_levels: 1,
                    sample_count: samples,
                    dimension: D::D2,
                    format: F::Depth24PlusStencil8,
                    usage: T::RESOURCE | T::DEPTH_STENCIL_WRITE,
                },
            )
            .unwrap();
        let attachment = gpu.texture_view(&ds, V::D2, 0..1, 0..1).unwrap();
        let stencil_view = gpu
            .texture_view_aspect(&ds, V::D2, 0..1, 0..1, t::TextureAspect::StencilOnly)
            .unwrap();
        let depth_view = gpu
            .texture_view_aspect(&ds, V::D2, 0..1, 0..1, t::TextureAspect::DepthOnly)
            .unwrap();
        let vertex = "@vertex fn vs(@builtin(vertex_index) i:u32)->@builtin(position) vec4f {let p=array<vec2f,3>(vec2f(-1.,-1.),vec2f(3.,-1.),vec2f(-1.,3.));return vec4f(p[i],0.,1.);}";
        let sample_arg = if multi {
            ",@builtin(sample_index) si:u32"
        } else {
            ""
        };
        let si = if multi { "si" } else { "0u" };
        let source=gpu.shader(&format!("{vertex}\n@fragment fn fs(@builtin(position) p:vec4f{sample_arg})->@location(0) u32 {{let x=u32(p.x);let y=u32(p.y);let value=(x+y*32u+{si}*53u)&255u;return value|select(0u,256u,(x+y)%7u!=0u);}}")).unwrap();
        let empty = gpu.pipeline_layout(&[]).unwrap();
        let colors = [Some(t::ColorTargetState {
            format: F::R32Uint,
            blend: None,
            write_mask: t::ColorWrites::ALL,
        })];
        let source_pipeline = gpu
            .render_pipeline(&PipelineDescriptor {
                layout: &empty,
                vertex: &source,
                vertex_entry: "vs",
                fragment: Some((&source, "fs")),
                colors: &colors,
                primitive: Default::default(),
                depth_stencil: None,
                multisample: t::MultisampleState {
                    count: samples,
                    ..Default::default()
                },
            })
            .unwrap();
        let gl = gpu
            .group_layout(&[t::BindGroupLayoutEntry {
                binding: 0,
                visibility: t::ShaderStages::FRAGMENT,
                ty: t::BindingType::Texture {
                    sample_type: t::TextureSampleType::Uint,
                    view_dimension: V::D2,
                    multisampled: multi,
                },
                count: None,
            }])
            .unwrap();
        let pl = gpu.pipeline_layout(&[gl.clone()]).unwrap();
        let group = gpu
            .bind_group(
                &gl,
                &[Entry {
                    binding: 0,
                    resource: Resource::Texture(&scratch_view),
                }],
            )
            .unwrap();
        let ty = if multi {
            "texture_multisampled_2d<u32>"
        } else {
            "texture_2d<u32>"
        };
        // The normal depth-transfer pass zeros ONLY stencil samples it writes. This
        // authored prelude reproduces that contract while leaving the depth intact.
        let reset=gpu.shader(&format!("{vertex}\n@group(0) @binding(0) var scratch:{ty};@fragment fn fs(@builtin(position) p:vec4f{sample_arg}) {{if ((textureLoad(scratch,vec2i(p.xy),i32({si})).r&256u)==0u) {{discard;}}}}")).unwrap();
        let face = t::StencilFaceState {
            compare: t::CompareFunction::Always,
            fail_op: t::StencilOperation::Keep,
            depth_fail_op: t::StencilOperation::Keep,
            pass_op: t::StencilOperation::Replace,
        };
        let state = |mask| t::DepthStencilState {
            format: F::Depth24PlusStencil8,
            depth_write_enabled: Some(false),
            depth_compare: Some(t::CompareFunction::Always),
            stencil: t::StencilState {
                front: face,
                back: face,
                read_mask: 255,
                write_mask: mask,
            },
            bias: Default::default(),
        };
        let reset_pipeline = gpu
            .render_pipeline(&PipelineDescriptor {
                layout: &pl,
                vertex: &reset,
                vertex_entry: "vs",
                fragment: Some((&reset, "fs")),
                colors: &[],
                primitive: Default::default(),
                depth_stencil: Some(state(255)),
                multisample: t::MultisampleState {
                    count: samples,
                    ..Default::default()
                },
            })
            .unwrap();
        let code = production::stencil_bits_module(multi);
        assert!(gpu.shader(&code).is_err());
        for bad in [
            vec![("UNKNOWN", 1.)],
            vec![("STENCIL_BIT", f64::NAN)],
            vec![("STENCIL_BIT", 0.), ("STENCIL_BIT", 1.)],
            vec![("STENCIL_BIT", -1.)],
            vec![("STENCIL_BIT", 4294967296.)],
        ] {
            assert!(gpu.shader_specialized(&code, &bad).is_err());
        }
        let required = code.replace(
            "override STENCIL_BIT: u32 = 0u;",
            "override STENCIL_BIT: u32;",
        );
        assert!(gpu.shader_specialized(&required, &[]).is_err());
        let mut pipelines = Vec::new();
        let mut identities = std::collections::BTreeSet::new();
        for bit in 0..8 {
            let variant = gpu
                .shader_specialized(&code, &[("STENCIL_BIT", f64::from(bit))])
                .unwrap();
            assert!(identities.insert(variant.id()));
            pipelines.push(
                gpu.render_pipeline(&PipelineDescriptor {
                    layout: &pl,
                    vertex: &variant,
                    vertex_entry: "vs",
                    fragment: Some((&variant, "fs")),
                    colors: &[],
                    primitive: Default::default(),
                    depth_stencil: Some(state(1 << bit)),
                    multisample: t::MultisampleState {
                        count: samples,
                        ..Default::default()
                    },
                })
                .unwrap(),
            );
        }
        let bound = [BoundGroup {
            group: &group,
            dynamic_offsets: &[],
        }];
        let draw = |p| Draw {
            pipeline: p,
            groups: &bound,
            vertices: 0..3,
            instances: 0..1,
            indices: None,
            viewport: None,
            scissor: None,
            blend: t::Color::TRANSPARENT,
            stencil_reference: 255,
        };
        // Read each MSAA sample separately. Depth must stay bit-identical and samples
        // outside the scratch-valid mask must retain the old stencil value 0xa5.
        let read_ty = if multi {
            "texture_multisampled_2d<u32>"
        } else {
            "texture_2d<u32>"
        };
        let depth_ty = if multi {
            "texture_depth_multisampled_2d"
        } else {
            "texture_depth_2d"
        };
        let compute=gpu.shader(&format!("@group(0) @binding(0) var s:{read_ty};@group(0) @binding(1) var d:{depth_ty};@group(0) @binding(2) var<storage,read_write> out:array<u32>;@compute @workgroup_size(1) fn main(@builtin(global_invocation_id) p:vec3u) {{let i=((p.y*32u+p.x)*{samples}u+p.z)*2u;out[i]=textureLoad(s,vec2i(p.xy),i32(p.z)).r;out[i+1u]=bitcast<u32>(textureLoad(d,vec2i(p.xy),i32(p.z)));}}")).unwrap();
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
        let cl = gpu
            .group_layout(&[
                tex(0, t::TextureSampleType::Uint),
                tex(1, t::TextureSampleType::Depth),
                t::BindGroupLayoutEntry {
                    binding: 2,
                    visibility: t::ShaderStages::COMPUTE,
                    ty: t::BindingType::Buffer {
                        ty: t::BufferBindingType::Storage { read_only: false },
                        has_dynamic_offset: false,
                        min_binding_size: NonZeroU64::new(4),
                    },
                    count: None,
                },
            ])
            .unwrap();
        let cp = gpu.pipeline_layout(&[cl.clone()]).unwrap();
        let kernel = gpu.shared_compute_pipeline(&cp, &compute, "main").unwrap();
        let bytes = 32 * 16 * samples * 8;
        let output = gpu
            .create_buffer(
                "restored samples",
                u64::from(bytes),
                U::STORAGE_READ_WRITE | U::COPY_SRC,
            )
            .unwrap();
        let readback = gpu
            .create_buffer(
                "restored readback",
                u64::from(bytes) * 2,
                U::MAP_READ | U::COPY_DST,
            )
            .unwrap();
        let cg = gpu
            .bind_group(
                &cl,
                &[
                    Entry {
                        binding: 0,
                        resource: Resource::Texture(&stencil_view),
                    },
                    Entry {
                        binding: 1,
                        resource: Resource::Texture(&depth_view),
                    },
                    Entry {
                        binding: 2,
                        resource: Resource::Buffer {
                            buffer: &output,
                            offset: 0,
                            size: u64::from(bytes),
                        },
                    },
                ],
            )
            .unwrap();
        let mut e = gpu.encoder().unwrap();
        e.render(
            &[Some(ColorAttachment {
                view: &scratch_view,
                clear: Some(t::Color::TRANSPARENT),
            })],
            &[Draw {
                groups: &[],
                ..draw(&source_pipeline)
            }],
        )
        .unwrap();
        e.render_full(
            &[],
            Some(DepthAttachment {
                view: &attachment,
                clear_depth: Some(0.375),
                clear_stencil: Some(0xa5),
            }),
            &[],
            &[],
        )
        .unwrap();
        e.dispatch_shared(
            &kernel,
            &[BoundGroup {
                group: &cg,
                dynamic_offsets: &[],
            }],
            [32, 16, samples],
        )
        .unwrap();
        e.copy_buffer(&output, 0, &readback, 0, u64::from(bytes))
            .unwrap();
        e.render_full(
            &[],
            Some(DepthAttachment {
                view: &attachment,
                clear_depth: None,
                clear_stencil: None,
            }),
            &[],
            &[Draw {
                stencil_reference: 0,
                ..draw(&reset_pipeline)
            }],
        )
        .unwrap();
        let draws: Vec<_> = pipelines.iter().map(draw).collect();
        e.render_full(
            &[],
            Some(DepthAttachment {
                view: &attachment,
                clear_depth: None,
                clear_stencil: None,
            }),
            &[],
            &draws,
        )
        .unwrap();
        e.dispatch_shared(
            &kernel,
            &[BoundGroup {
                group: &cg,
                dynamic_offsets: &[],
            }],
            [32, 16, samples],
        )
        .unwrap();
        e.copy_buffer(&output, 0, &readback, u64::from(bytes), u64::from(bytes))
            .unwrap();
        gpu.submit(vec![e.finish().unwrap()]).unwrap();
        let snapshots = gpu
            .read_buffer(&readback, 0..u64::from(bytes) * 2, DEADLINE)
            .unwrap();
        let (baseline, actual) = snapshots.split_at(bytes as usize);
        // Depth24Plus may be normalized24 or float32. Assert the initialized value
        // within one24-bit step, then require BITWISE preservation of that baseline.
        for p in baseline.chunks_exact(8) {
            assert_eq!(&p[..4], &0xa5u32.to_le_bytes());
            assert!(
                (f32::from_le_bytes(p[4..8].try_into().unwrap()) - 0.375).abs() <= 1. / 16777215.
            );
        }
        let mut expected = Vec::new();
        for y in 0..16 {
            for x in 0..32 {
                for s in 0..samples {
                    let stencil = if (x + y) % 7 == 0 {
                        0xa5
                    } else {
                        (x + y * 32 + s * 53) & 255
                    };
                    let i = ((y * 32 + x) * samples + s) as usize * 8;
                    expected.extend(words(&[stencil]));
                    expected.extend_from_slice(&baseline[i + 4..i + 8]);
                }
            }
        }
        assert!(
            actual == expected,
            "stencil restoration samples={samples}; first={:?}",
            actual.iter().zip(&expected).position(|(a, b)| a != b)
        );
        #[cfg(all(target_vendor = "apple", feature = "metal-stencil-export"))]
        {
            let fast = gpu.metal_stencil_restore(samples).unwrap();
            let mut e = gpu.encoder().unwrap();
            assert!(
                fast.encode(&gpu, &mut e, &depth_view, &attachment, None)
                    .is_err()
            );
            assert!(
                fast.encode(
                    &gpu,
                    &mut e,
                    &scratch_view,
                    &attachment,
                    Some([31, 0, 2, 1])
                )
                .is_err()
            );
            e.render_full(
                &[],
                Some(DepthAttachment {
                    view: &attachment,
                    clear_depth: Some(0.375),
                    clear_stencil: Some(0xa5),
                }),
                &[],
                &[],
            )
            .unwrap();
            // No stencil-zero prelude: a full reference replaces all eight bits in one
            // draw. Clipped and scratch-invalid samples must retain the old value.
            let stats = fast
                .encode(
                    &gpu,
                    &mut e,
                    &scratch_view,
                    &attachment,
                    Some([2, 3, 27, 10]),
                )
                .unwrap();
            assert_eq!(stats.draws, 1);
            drop(fast);
            e.dispatch_shared(
                &kernel,
                &[BoundGroup {
                    group: &cg,
                    dynamic_offsets: &[],
                }],
                [32, 16, samples],
            )
            .unwrap();
            e.copy_buffer(&output, 0, &readback, 0, u64::from(bytes))
                .unwrap();
            gpu.submit(vec![e.finish().unwrap()]).unwrap();
            let actual = gpu
                .read_buffer(&readback, 0..u64::from(bytes), DEADLINE)
                .unwrap();
            let mut expected = Vec::new();
            for y in 0..16 {
                for x in 0..32 {
                    for s in 0..samples {
                        let stencil =
                            if (x + y) % 7 == 0 || !(2..29).contains(&x) || !(3..13).contains(&y) {
                                0xa5
                            } else {
                                (x + y * 32 + s * 53) & 255
                            };
                        let i = ((y * 32 + x) * samples + s) as usize * 8;
                        expected.extend(words(&[stencil]));
                        expected.extend_from_slice(&baseline[i + 4..i + 8]);
                    }
                }
            }
            assert!(
                actual == expected,
                "Metal stencil export samples={samples}; first={:?}",
                actual.iter().zip(&expected).position(|(a, b)| a != b)
            );
        }
    }
    drop(gpu);
    finish(before);
}
fn words(values: &[u32]) -> Vec<u8> {
    values.iter().flat_map(|v| v.to_le_bytes()).collect()
}
#[test]
fn production_edram_dump_wrapping_scaling_and_raw_direct_resolve_are_exact() {
    let (_lock, before) = setup();
    let mut gpu = open();
    for scale in [1u32, 2] {
        for wide in [false, true] {
            let base = 2047;
            let unit = if wide { 8 } else { 4 };
            let fmt = if wide { 7 } else { 0 };
            let guest_w = if wide { 43 } else { 83 };
            let guest_h = 19;
            let [off_x, off_y] = [3, 2];
            let width = guest_w * scale + off_x;
            let height = guest_h * scale + off_y;
            let row = (width * unit).div_ceil(256) * 256;
            let mut upload = vec![0; (row * height) as usize];
            for y in 0..height {
                for x in 0..width {
                    let i = (y * row + x * unit) as usize;
                    upload[i..i + unit as usize].copy_from_slice(&pixel(x, y, wide));
                }
            }
            let size = Extent3d {
                width,
                height,
                depth_or_array_layers: 1,
            };
            let image = gpu
                .create_texture(
                    "authored EDRAM pixels",
                    TextureDescriptor {
                        size,
                        mip_levels: 1,
                        sample_count: 1,
                        dimension: D::D2,
                        format: if wide { F::Rgba16Float } else { F::Rgba8Unorm },
                        usage: T::RESOURCE | T::COPY_DST,
                    },
                )
                .unwrap();
            let view = gpu.texture_view(&image, V::D2, 0..1, 0..1).unwrap();
            let source = gpu
                .create_buffer(
                    "pixel upload",
                    upload.len() as u64,
                    U::COPY_SRC | U::COPY_DST,
                )
                .unwrap();
            gpu.write_buffer(&source, 0, &upload).unwrap();
            let total = u64::from(xenos_state::edram::EDRAM_SIZE_BYTES) * u64::from(scale * scale);
            let tile_words = 1280 * scale * scale;
            let edram = gpu
                .create_buffer(
                    "canonical EDRAM",
                    total,
                    U::STORAGE_READ_WRITE | U::STORAGE_READ_ONLY | U::COPY_SRC,
                )
                .unwrap();
            let edram_read = gpu
                .create_buffer("EDRAM readback", total, U::MAP_READ | U::COPY_DST)
                .unwrap();
            let uniform = gpu
                .create_buffer("EDRAM params", 512, U::UNIFORM | U::COPY_DST)
                .unwrap();
            gpu.write_buffer(
                &uniform,
                0,
                &words(&[
                    base * tile_words,
                    4 * tile_words,
                    base,
                    2,
                    0,
                    if wide { 2 } else { 0 },
                    fmt,
                    width,
                    height,
                    off_x,
                    off_y,
                    0,
                ]),
            )
            .unwrap();
            let buffer = |binding, ty, min| t::BindGroupLayoutEntry {
                binding,
                visibility: t::ShaderStages::COMPUTE,
                ty: t::BindingType::Buffer {
                    ty,
                    has_dynamic_offset: false,
                    min_binding_size: NonZeroU64::new(min),
                },
                count: None,
            };
            let image_entry = t::BindGroupLayoutEntry {
                binding: 1,
                visibility: t::ShaderStages::COMPUTE,
                ty: t::BindingType::Texture {
                    sample_type: t::TextureSampleType::Float { filterable: false },
                    view_dimension: V::D2,
                    multisampled: false,
                },
                count: None,
            };
            let dump_layout = gpu
                .group_layout(&[
                    buffer(0, t::BufferBindingType::Uniform, 48),
                    image_entry,
                    buffer(3, t::BufferBindingType::Storage { read_only: false }, 4),
                ])
                .unwrap();
            let dump_pl = gpu.pipeline_layout(&[dump_layout.clone()]).unwrap();
            let dump_shader = gpu
                .shader(&production::dump_module(
                    production::SrcKind::Color {
                        multisampled: false,
                    },
                    scale,
                ))
                .unwrap();
            let dump = gpu
                .shared_compute_pipeline(&dump_pl, &dump_shader, "main")
                .unwrap();
            let dump_group = gpu
                .bind_group(
                    &dump_layout,
                    &[
                        Entry {
                            binding: 0,
                            resource: Resource::Buffer {
                                buffer: &uniform,
                                offset: 0,
                                size: 48,
                            },
                        },
                        Entry {
                            binding: 1,
                            resource: Resource::Texture(&view),
                        },
                        Entry {
                            binding: 3,
                            resource: Resource::Buffer {
                                buffer: &edram,
                                offset: 0,
                                size: total,
                            },
                        },
                    ],
                )
                .unwrap();
            let (rw, rh, rx, ry) = (19, 7, 2, 3);
            let nwords = unit / 4;
            let output_bytes = u64::from(rw * rh * unit);
            let output = gpu
                .create_buffer(
                    "buffer resolve",
                    output_bytes,
                    U::STORAGE_READ_WRITE | U::COPY_SRC,
                )
                .unwrap();
            let direct_out = gpu
                .create_buffer(
                    "direct resolve",
                    output_bytes,
                    U::STORAGE_READ_WRITE | U::COPY_SRC,
                )
                .unwrap();
            let readback = gpu
                .create_buffer(
                    "resolve snapshots",
                    output_bytes * 2,
                    U::MAP_READ | U::COPY_DST,
                )
                .unwrap();
            let params = [
                rw,
                rh,
                rx,
                ry,
                base,
                2,
                0,
                if wide { 2 } else { 0 },
                fmt,
                if wide { 1 } else { 0 },
                0,
                0,
                0,
                1f32.to_bits(),
                nwords,
                unit * 8,
                0,
                0,
                0,
                off_x,
                off_y,
                0,
                0,
                0,
            ];
            gpu.write_buffer(&uniform, 256, &words(&params)).unwrap();
            let resolve_layout = gpu
                .group_layout(&[
                    buffer(0, t::BufferBindingType::Uniform, 96),
                    buffer(1, t::BufferBindingType::Storage { read_only: true }, 4),
                    buffer(2, t::BufferBindingType::Storage { read_only: false }, 4),
                ])
                .unwrap();
            let resolve_pl = gpu.pipeline_layout(&[resolve_layout.clone()]).unwrap();
            let resolve_shader = gpu.shader(&production::resolve_module(scale)).unwrap();
            let resolve = gpu
                .shared_compute_pipeline(&resolve_pl, &resolve_shader, "main")
                .unwrap();
            let resolve_group = gpu
                .bind_group(
                    &resolve_layout,
                    &[
                        Entry {
                            binding: 0,
                            resource: Resource::Buffer {
                                buffer: &uniform,
                                offset: 256,
                                size: 96,
                            },
                        },
                        Entry {
                            binding: 1,
                            resource: Resource::Buffer {
                                buffer: &edram,
                                offset: 0,
                                size: total,
                            },
                        },
                        Entry {
                            binding: 2,
                            resource: Resource::Buffer {
                                buffer: &output,
                                offset: 0,
                                size: output_bytes,
                            },
                        },
                    ],
                )
                .unwrap();
            let direct_layout = gpu
                .group_layout(&[
                    buffer(0, t::BufferBindingType::Uniform, 96),
                    image_entry,
                    buffer(3, t::BufferBindingType::Storage { read_only: false }, 4),
                ])
                .unwrap();
            let direct_pl = gpu.pipeline_layout(&[direct_layout.clone()]).unwrap();
            let direct_shader = gpu
                .shader(&production::resolve_direct_module(
                    production::SrcKind::Color {
                        multisampled: false,
                    },
                    scale,
                ))
                .unwrap();
            let direct = gpu
                .shared_compute_pipeline(&direct_pl, &direct_shader, "main")
                .unwrap();
            let direct_group = gpu
                .bind_group(
                    &direct_layout,
                    &[
                        Entry {
                            binding: 0,
                            resource: Resource::Buffer {
                                buffer: &uniform,
                                offset: 256,
                                size: 96,
                            },
                        },
                        Entry {
                            binding: 1,
                            resource: Resource::Texture(&view),
                        },
                        Entry {
                            binding: 3,
                            resource: Resource::Buffer {
                                buffer: &direct_out,
                                offset: 0,
                                size: output_bytes,
                            },
                        },
                    ],
                )
                .unwrap();
            let mut e = gpu.encoder().unwrap();
            e.copy_buffer_to_texture(
                &source,
                L {
                    offset: 0,
                    bytes_per_row: Some(row),
                    rows_per_image: Some(height),
                },
                &image,
                0,
                Origin3d::ZERO,
                size,
            )
            .unwrap();
            e.dispatch_shared(
                &dump,
                &[BoundGroup {
                    group: &dump_group,
                    dynamic_offsets: &[],
                }],
                [(4 * tile_words).div_ceil(256), 1, 1],
            )
            .unwrap();
            e.dispatch_shared(
                &resolve,
                &[BoundGroup {
                    group: &resolve_group,
                    dynamic_offsets: &[],
                }],
                [rw.div_ceil(8), rh.div_ceil(8), 1],
            )
            .unwrap();
            e.dispatch_shared(
                &direct,
                &[BoundGroup {
                    group: &direct_group,
                    dynamic_offsets: &[],
                }],
                [rw.div_ceil(8), rh.div_ceil(8), 1],
            )
            .unwrap();
            e.copy_buffer(&edram, 0, &edram_read, 0, total).unwrap();
            e.copy_buffer(&output, 0, &readback, 0, output_bytes)
                .unwrap();
            e.copy_buffer(&direct_out, 0, &readback, output_bytes, output_bytes)
                .unwrap();
            gpu.submit(vec![e.finish().unwrap()]).unwrap();
            let actual = gpu.read_buffer(&edram_read, 0..total, DEADLINE).unwrap();
            let mut expected = vec![0; total as usize];
            for y in 0..guest_h * scale {
                for x in 0..guest_w * scale {
                    let i = xenos_state::edram::edram_offset_bytes(
                        [x, y],
                        base,
                        true,
                        2,
                        xenos_state::xenos::MsaaSamples::X1,
                        false,
                        wide,
                        0,
                        [scale, scale],
                    ) as usize;
                    expected[i..i + unit as usize].copy_from_slice(&pixel(
                        x + off_x,
                        y + off_y,
                        wide,
                    ));
                }
            }
            assert_eq!(actual.len(), expected.len());
            assert!(
                actual == expected,
                "EDRAM byte mismatch scale={scale} wide={wide}; first={:?}",
                actual.iter().zip(&expected).position(|(a, b)| a != b)
            );
            let resolved = gpu
                .read_buffer(&readback, 0..output_bytes * 2, DEADLINE)
                .unwrap();
            let mut expected_pixels = Vec::new();
            for y in 0..rh {
                for x in 0..rw {
                    expected_pixels.extend(pixel(
                        (x + rx) * scale + scale / 2 + off_x,
                        (y + ry) * scale + scale / 2 + off_y,
                        wide,
                    ));
                }
            }
            assert_eq!(&resolved[..output_bytes as usize], &expected_pixels);
            assert_eq!(&resolved[output_bytes as usize..], &expected_pixels);
        }
    }
    drop(gpu);
    finish(before);
}

#[test]
fn production_msaa_and_depth_stencil_dump_preserves_each_guest_sample_at_each_scale() {
    use skyrim_native_gpu::render::{ColorAttachment, DepthAttachment, Draw, PipelineDescriptor};
    use xenos_state::xenos::MsaaSamples;
    let (_lock, before) = setup();
    let mut gpu = open();
    let authored = r#"
@vertex fn vs(@builtin(vertex_index) i:u32)->@builtin(position) vec4f {let p=array<vec2f,3>(vec2f(-1.,-1.),vec2f(3.,-1.),vec2f(-1.,3.));return vec4f(p[i],0.,1.);}
struct C{@location(0) color:vec4f,@builtin(sample_mask) mask:u32}
@fragment fn c0()->C{return C(vec4f(1.,0.,0.,1.),1u);}
@fragment fn c1()->C{return C(vec4f(0.,1.,0.,1.),2u);}
@fragment fn c2()->C{return C(vec4f(0.,0.,1.,1.),4u);}
@fragment fn c3()->C{return C(vec4f(1.,1.,1.,1.),8u);}
struct D{@builtin(frag_depth) depth:f32,@builtin(sample_mask) mask:u32}
@fragment fn d0()->D{return D(0.,1u);}
@fragment fn d1()->D{return D(1.,2u);}
@fragment fn d2()->D{return D(0.,4u);}
@fragment fn d3()->D{return D(1.,8u);}
"#;
    let shader = gpu.shader(authored).unwrap();
    let empty = gpu.pipeline_layout(&[]).unwrap();
    for scale in [1u32, 2] {
        for msaa in [MsaaSamples::X1, MsaaSamples::X2, MsaaSamples::X4] {
            for depth in [false, true] {
                let samples = if msaa == MsaaSamples::X1 { 1 } else { 4 };
                let w = 8 * scale;
                let h = 8 * scale;
                let size = Extent3d {
                    width: w,
                    height: h,
                    depth_or_array_layers: 1,
                };
                let image = gpu
                    .create_texture(
                        "sample-distinct target",
                        TextureDescriptor {
                            size,
                            mip_levels: 1,
                            sample_count: samples,
                            dimension: D::D2,
                            format: if depth {
                                F::Depth24PlusStencil8
                            } else {
                                F::Rgba8Unorm
                            },
                            usage: T::RESOURCE
                                | if depth {
                                    T::DEPTH_STENCIL_WRITE
                                } else {
                                    T::COLOR_TARGET
                                },
                        },
                    )
                    .unwrap();
                let attachment = gpu.texture_view(&image, V::D2, 0..1, 0..1).unwrap();
                let tex = gpu
                    .texture_view_aspect(
                        &image,
                        V::D2,
                        0..1,
                        0..1,
                        if depth {
                            t::TextureAspect::DepthOnly
                        } else {
                            t::TextureAspect::All
                        },
                    )
                    .unwrap();
                let stencil = if depth {
                    Some(
                        gpu.texture_view_aspect(
                            &image,
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
                let colors = if depth {
                    vec![]
                } else {
                    vec![Some(t::ColorTargetState {
                        format: F::Rgba8Unorm,
                        blend: None,
                        write_mask: t::ColorWrites::ALL,
                    })]
                };
                let face = t::StencilFaceState {
                    compare: t::CompareFunction::Always,
                    fail_op: t::StencilOperation::Keep,
                    depth_fail_op: t::StencilOperation::Keep,
                    pass_op: t::StencilOperation::Replace,
                };
                let ds = depth.then(|| t::DepthStencilState {
                    format: F::Depth24PlusStencil8,
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
                let mut pipelines = Vec::new();
                for i in 0..samples {
                    pipelines.push(
                        gpu.render_pipeline(&PipelineDescriptor {
                            layout: &empty,
                            vertex: &shader,
                            vertex_entry: "vs",
                            fragment: Some((
                                &shader,
                                &format!("{}{i}", if depth { "d" } else { "c" }),
                            )),
                            colors: &colors,
                            primitive: Default::default(),
                            depth_stencil: ds.clone(),
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
                let count = 1280 * scale * scale;
                let bytes = u64::from(count * 4);
                let edram = gpu
                    .create_buffer("sample EDRAM", bytes, U::STORAGE_READ_WRITE | U::COPY_SRC)
                    .unwrap();
                let out = gpu
                    .create_buffer("sample EDRAM readback", bytes, U::MAP_READ | U::COPY_DST)
                    .unwrap();
                let params = gpu
                    .create_buffer("sample dump params", 48, U::UNIFORM | U::COPY_DST)
                    .unwrap();
                gpu.write_buffer(
                    &params,
                    0,
                    &words(&[
                        0,
                        count,
                        0,
                        1,
                        msaa.raw(),
                        u32::from(depth),
                        if depth { 16 } else { 0 },
                        w,
                        h,
                        0,
                        0,
                        0,
                    ]),
                )
                .unwrap();
                let buffer = |binding, ty, min| t::BindGroupLayoutEntry {
                    binding,
                    visibility: t::ShaderStages::COMPUTE,
                    ty: t::BindingType::Buffer {
                        ty,
                        has_dynamic_offset: false,
                        min_binding_size: NonZeroU64::new(min),
                    },
                    count: None,
                };
                let texture = |binding, sample_type| t::BindGroupLayoutEntry {
                    binding,
                    visibility: t::ShaderStages::COMPUTE,
                    ty: t::BindingType::Texture {
                        sample_type,
                        view_dimension: V::D2,
                        multisampled: samples > 1,
                    },
                    count: None,
                };
                let mut entries = vec![
                    buffer(0, t::BufferBindingType::Uniform, 48),
                    texture(
                        1,
                        if depth {
                            t::TextureSampleType::Depth
                        } else {
                            t::TextureSampleType::Float { filterable: false }
                        },
                    ),
                    buffer(3, t::BufferBindingType::Storage { read_only: false }, 4),
                ];
                if depth {
                    entries.push(texture(2, t::TextureSampleType::Uint));
                }
                let gl = gpu.group_layout(&entries).unwrap();
                let pl = gpu.pipeline_layout(&[gl.clone()]).unwrap();
                let src = if depth {
                    production::SrcKind::Depth {
                        multisampled: samples > 1,
                    }
                } else {
                    production::SrcKind::Color {
                        multisampled: samples > 1,
                    }
                };
                let kernel = gpu.shader(&production::dump_module(src, scale)).unwrap();
                let compute = gpu.shared_compute_pipeline(&pl, &kernel, "main").unwrap();
                let mut bindings = vec![
                    Entry {
                        binding: 0,
                        resource: Resource::Buffer {
                            buffer: &params,
                            offset: 0,
                            size: 48,
                        },
                    },
                    Entry {
                        binding: 1,
                        resource: Resource::Texture(&tex),
                    },
                    Entry {
                        binding: 3,
                        resource: Resource::Buffer {
                            buffer: &edram,
                            offset: 0,
                            size: bytes,
                        },
                    },
                ];
                if let Some(v) = &stencil {
                    bindings.push(Entry {
                        binding: 2,
                        resource: Resource::Texture(v),
                    });
                }
                let group = gpu.bind_group(&gl, &bindings).unwrap();
                let mut e = gpu.encoder().unwrap();
                if depth {
                    e.render_full(
                        &[],
                        Some(DepthAttachment {
                            view: &attachment,
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
                            view: &attachment,
                            clear: Some(t::Color::TRANSPARENT),
                        })],
                        &draws,
                    )
                    .unwrap();
                }
                e.dispatch_shared(
                    &compute,
                    &[BoundGroup {
                        group: &group,
                        dynamic_offsets: &[],
                    }],
                    [count.div_ceil(256), 1, 1],
                )
                .unwrap();
                e.copy_buffer(&edram, 0, &out, 0, bytes).unwrap();
                gpu.submit(vec![e.finish().unwrap()]).unwrap();
                let actual = gpu.read_buffer(&out, 0..bytes, DEADLINE).unwrap();
                let mut expected = vec![0; bytes as usize];
                let cs = [0xff0000ffu32, 0xff00ff00, 0xffff0000, 0xffffffff];
                for y in 0..h {
                    for x in 0..w {
                        for s in 0..msaa.count().unwrap() {
                            let host = if msaa == MsaaSamples::X2 { s * 3 } else { s } as usize;
                            let value = if depth {
                                refs[host] | if host % 2 == 1 { 0xffffff00 } else { 0 }
                            } else {
                                cs[host]
                            };
                            let i = xenos_state::edram::edram_offset_bytes(
                                [x, y],
                                0,
                                true,
                                1,
                                msaa,
                                depth,
                                false,
                                s,
                                [scale, scale],
                            ) as usize;
                            expected[i..i + 4].copy_from_slice(&value.to_le_bytes());
                        }
                    }
                }
                assert!(
                    actual == expected,
                    "sample EDRAM mismatch scale={scale} msaa={msaa:?} depth={depth}, first={:?}",
                    actual.iter().zip(&expected).position(|(a, b)| a != b)
                );
            }
        }
    }
    drop(gpu);
    finish(before);
}
