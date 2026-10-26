//! Authored BC blocks, including physical padding at the small mip tail.
use super::{DEADLINE, U, finish, open, open_config, setup};
use skyrim_native_gpu::{
    Config,
    bindings::{Entry, Resource},
    render::BoundGroup,
    texture::TextureDescriptor,
    types::{
        self as t, Extent3d, Origin3d, TexelCopyBufferLayout as L, TextureDimension as D,
        TextureFormat as F, TextureUses as T, TextureViewDimension as V,
    },
};
use std::num::NonZeroU64;

fn features() -> t::Features {
    t::Features::TEXTURE_COMPRESSION_BC
        | t::Features::TEXTURE_COMPRESSION_BC_SLICED_3D
        | t::Features::FLOAT32_FILTERABLE
        | t::Features::TEXTURE_FORMAT_16BIT_NORM
        | t::Features::DEPTH32FLOAT_STENCIL8
}
fn desc(format: F, dimension: D) -> TextureDescriptor {
    TextureDescriptor {
        size: Extent3d {
            width: 12,
            height: 8,
            depth_or_array_layers: if dimension == D::D3 { 3 } else { 2 },
        },
        mip_levels: 4,
        sample_count: 1,
        dimension,
        format,
        usage: T::RESOURCE | T::COPY_SRC | T::COPY_DST,
    }
}

#[test]
fn bc_feature_gate_zeroing_mip_tails_layers_and_volume_copies_are_exact() {
    let (_lock, before) = setup();
    let mut disabled = open();
    assert!(
        disabled
            .create_texture("unnegotiated BC", desc(F::Bc1RgbaUnorm, D::D2))
            .is_err()
    );
    drop(disabled);
    let mut gpu = open_config(Config {
        optional_texture_features: features(),
        ..Default::default()
    });
    assert!(
        gpu.features().contains(t::Features::TEXTURE_COMPRESSION_BC),
        "this conformance run requires an advertised BC adapter"
    );
    eprintln!("negotiated texture features {:?}", gpu.features());
    gpu.test_texture_initialization(true, false);
    for format in [F::Bc1RgbaUnorm, F::Bc3RgbaUnorm, F::Bc5RgSnorm] {
        for dimension in [D::D2, D::D3] {
            if dimension == D::D3
                && !gpu
                    .features()
                    .contains(t::Features::TEXTURE_COMPRESSION_BC_SLICED_3D)
            {
                assert!(
                    gpu.create_texture("unsupported BC volume", desc(format, dimension))
                        .is_err()
                );
                continue;
            }
            let texture = gpu
                .create_texture("authored BC", desc(format, dimension))
                .unwrap();
            let clone = gpu
                .create_texture("BC copy target", desc(format, dimension))
                .unwrap();
            let mut e = gpu.encoder().unwrap();
            let mut outputs = Vec::new();
            for mip in 0..4 {
                let size = texture.mip_size(mip).unwrap().physical_size(format);
                let block = format.block_copy_size(None).unwrap();
                let row = size.width / 4 * block;
                let rows = size.height / 4 + 1;
                let bytes = 16 + 256 * rows * size.depth_or_array_layers;
                let mut data = vec![0xcd; bytes as usize];
                let mut first = vec![0x5a; bytes as usize];
                let mut second = first.clone();
                for z in 0..size.depth_or_array_layers {
                    for y in 0..size.height / 4 {
                        let at = (16 + (z * rows + y) * 256) as usize;
                        for x in 0..row as usize {
                            data[at + x] = (x as u8)
                                .wrapping_mul(17)
                                .wrapping_add(mip as u8 * 29)
                                .wrapping_add(z as u8 * 43)
                                .wrapping_add(y as u8 * 71);
                            first[at + x] = 0;
                            second[at + x] = data[at + x];
                        }
                    }
                }
                let upload = gpu
                    .create_buffer("BC upload", u64::from(bytes), U::COPY_SRC | U::COPY_DST)
                    .unwrap();
                gpu.write_buffer(&upload, 0, &data).unwrap();
                let out = gpu
                    .create_buffer(
                        "BC snapshots",
                        u64::from(bytes) * 2,
                        U::MAP_READ | U::COPY_DST,
                    )
                    .unwrap();
                gpu.write_buffer(&out, 0, &vec![0x5a; bytes as usize * 2])
                    .unwrap();
                let layout = L {
                    offset: 16,
                    bytes_per_row: Some(256),
                    rows_per_image: Some(rows),
                };
                e.copy_texture_to_buffer(&texture, mip, Origin3d::ZERO, size, &out, layout)
                    .unwrap();
                assert!(
                    e.copy_buffer_to_texture(
                        &upload,
                        layout,
                        &texture,
                        mip,
                        Origin3d { x: 1, y: 0, z: 0 },
                        size
                    )
                    .is_err()
                );
                assert!(
                    e.copy_buffer_to_texture(
                        &upload,
                        L {
                            offset: 4,
                            ..layout
                        },
                        &texture,
                        mip,
                        Origin3d::ZERO,
                        size
                    )
                    .is_err()
                );
                assert!(
                    e.copy_buffer_to_texture(
                        &upload,
                        layout,
                        &texture,
                        mip,
                        Origin3d::ZERO,
                        Extent3d {
                            width: size.width - 1,
                            ..size
                        }
                    )
                    .is_err()
                );
                e.copy_buffer_to_texture(&upload, layout, &texture, mip, Origin3d::ZERO, size)
                    .unwrap();
                e.copy_texture(
                    &texture,
                    mip,
                    Origin3d::ZERO,
                    &clone,
                    mip,
                    Origin3d::ZERO,
                    size,
                )
                .unwrap();
                e.copy_texture_to_buffer(
                    &clone,
                    mip,
                    Origin3d::ZERO,
                    size,
                    &out,
                    L {
                        offset: u64::from(bytes) + 16,
                        ..layout
                    },
                )
                .unwrap();
                first.extend(second);
                outputs.push((out, first));
            }
            gpu.submit(vec![e.finish().unwrap()]).unwrap();
            for (out, expected) in outputs {
                let actual = gpu
                    .read_buffer(&out, 0..expected.len() as u64, DEADLINE)
                    .unwrap();
                assert!(
                    actual == expected,
                    "BC physical copy {format:?} {dimension:?}: first={:?}",
                    actual.iter().zip(&expected).position(|(a, b)| a != b)
                );
            }
        }
    }
    drop(gpu);
    finish(before);
}

#[test]
fn native_bc_sampling_and_anisotropic_sampler_preserve_authored_endpoint_colors() {
    let (_lock, before) = setup();
    let mut gpu = open_config(Config {
        optional_texture_features: features(),
        ..Default::default()
    });
    let size = Extent3d {
        width: 8,
        height: 4,
        depth_or_array_layers: 1,
    };
    let image = gpu
        .create_texture(
            "BC endpoints",
            TextureDescriptor {
                size,
                mip_levels: 1,
                sample_count: 1,
                dimension: D::D2,
                format: F::Bc1RgbaUnorm,
                usage: T::RESOURCE | T::COPY_DST,
            },
        )
        .unwrap();
    let view = gpu.texture_view(&image, V::D2, 0..1, 0..1).unwrap();
    // RGB565 endpoints red/green; first4x4 block uses red, second uses green.
    let bytes = [
        0x00, 0xf8, 0xe0, 0x07, 0, 0, 0, 0, 0x00, 0xf8, 0xe0, 0x07, 0x55, 0x55, 0x55, 0x55,
    ];
    let upload = gpu
        .create_buffer("BC endpoint blocks", 16, U::COPY_SRC | U::COPY_DST)
        .unwrap();
    gpu.write_buffer(&upload, 0, &bytes).unwrap();
    let mut sampler = t::SamplerDescriptor::default();
    sampler.mag_filter = t::FilterMode::Linear;
    sampler.min_filter = t::FilterMode::Linear;
    sampler.mipmap_filter = t::MipmapFilterMode::Linear;
    sampler.anisotropy_clamp = 16;
    let anisotropic = gpu.sampler(&sampler).unwrap();
    let bad = t::SamplerDescriptor {
        mag_filter: t::FilterMode::Nearest,
        ..sampler
    };
    assert!(gpu.sampler(&bad).is_err());
    let shader=gpu.shader("@group(0) @binding(0) var tex:texture_2d<f32>;@group(0) @binding(1) var sm:sampler;@group(0) @binding(2) var<storage,read_write> out:array<vec4u>;@compute @workgroup_size(1) fn main(@builtin(global_invocation_id) p:vec3u){let c=textureSampleLevel(tex,sm,(vec2f(p.xy)+vec2f(0.5))/vec2f(8.,4.),0.);out[p.y*8u+p.x]=vec4u(round(c*255.));}").unwrap();
    let gl = gpu
        .group_layout(&[
            t::BindGroupLayoutEntry {
                binding: 0,
                visibility: t::ShaderStages::COMPUTE,
                ty: t::BindingType::Texture {
                    sample_type: t::TextureSampleType::Float { filterable: true },
                    view_dimension: V::D2,
                    multisampled: false,
                },
                count: None,
            },
            t::BindGroupLayoutEntry {
                binding: 1,
                visibility: t::ShaderStages::COMPUTE,
                ty: t::BindingType::Sampler(t::SamplerBindingType::Filtering),
                count: None,
            },
            t::BindGroupLayoutEntry {
                binding: 2,
                visibility: t::ShaderStages::COMPUTE,
                ty: t::BindingType::Buffer {
                    ty: t::BufferBindingType::Storage { read_only: false },
                    has_dynamic_offset: false,
                    min_binding_size: NonZeroU64::new(16),
                },
                count: None,
            },
        ])
        .unwrap();
    let pl = gpu.pipeline_layout(&[gl.clone()]).unwrap();
    let pipeline = gpu.shared_compute_pipeline(&pl, &shader, "main").unwrap();
    let output = gpu
        .create_buffer(
            "BC sampled output",
            512,
            U::STORAGE_READ_WRITE | U::COPY_SRC,
        )
        .unwrap();
    let read = gpu
        .create_buffer("BC sampled readback", 512, U::MAP_READ | U::COPY_DST)
        .unwrap();
    let group = gpu
        .bind_group(
            &gl,
            &[
                Entry {
                    binding: 0,
                    resource: Resource::Texture(&view),
                },
                Entry {
                    binding: 1,
                    resource: Resource::Sampler(&anisotropic),
                },
                Entry {
                    binding: 2,
                    resource: Resource::Buffer {
                        buffer: &output,
                        offset: 0,
                        size: 512,
                    },
                },
            ],
        )
        .unwrap();
    let mut e = gpu.encoder().unwrap();
    e.copy_buffer_to_texture(
        &upload,
        L {
            offset: 0,
            bytes_per_row: Some(256),
            rows_per_image: Some(1),
        },
        &image,
        0,
        Origin3d::ZERO,
        size,
    )
    .unwrap();
    e.dispatch_shared(
        &pipeline,
        &[BoundGroup {
            group: &group,
            dynamic_offsets: &[],
        }],
        [8, 4, 1],
    )
    .unwrap();
    e.copy_buffer(&output, 0, &read, 0, 512).unwrap();
    gpu.submit(vec![e.finish().unwrap()]).unwrap();
    let actual = gpu.read_buffer(&read, 0..512, DEADLINE).unwrap();
    let mut expected = Vec::new();
    for _y in 0..4 {
        for x in 0..8 {
            for c in if x < 4 {
                [255u32, 0, 0, 255]
            } else {
                [0, 255, 0, 255]
            } {
                expected.extend(c.to_le_bytes());
            }
        }
    }
    assert_eq!(actual, expected);
    drop(gpu);
    finish(before);
}

#[test]
fn negotiated_float_filtering_normalized16_and_d32_stencil_keep_format_semantics() {
    use skyrim_native_gpu::render::DepthAttachment;
    let (_lock, before) = setup();
    let mut gpu = open_config(Config {
        optional_texture_features: features(),
        ..Default::default()
    });
    let required = t::Features::FLOAT32_FILTERABLE
        | t::Features::TEXTURE_FORMAT_16BIT_NORM
        | t::Features::DEPTH32FLOAT_STENCIL8;
    assert!(
        gpu.features().contains(required),
        "this format conformance run requires the advertised optional formats"
    );
    let size = Extent3d {
        width: 2,
        height: 1,
        depth_or_array_layers: 1,
    };
    let make = |format, usage| TextureDescriptor {
        size,
        mip_levels: 1,
        sample_count: 1,
        dimension: D::D2,
        format,
        usage,
    };
    let float = gpu
        .create_texture(
            "float32 filtering",
            make(F::R32Float, T::RESOURCE | T::COPY_DST),
        )
        .unwrap();
    let fv = gpu.texture_view(&float, V::D2, 0..1, 0..1).unwrap();
    let norm = gpu
        .create_texture(
            "normalized16",
            make(F::Rgba16Unorm, T::RESOURCE | T::COPY_DST),
        )
        .unwrap();
    let nv = gpu.texture_view(&norm, V::D2, 0..1, 0..1).unwrap();
    let depth = gpu
        .create_texture(
            "D32S8",
            make(
                F::Depth32FloatStencil8,
                T::RESOURCE | T::DEPTH_STENCIL_WRITE,
            ),
        )
        .unwrap();
    let attachment = gpu.texture_view(&depth, V::D2, 0..1, 0..1).unwrap();
    let dv = gpu
        .texture_view_aspect(&depth, V::D2, 0..1, 0..1, t::TextureAspect::DepthOnly)
        .unwrap();
    let sv = gpu
        .texture_view_aspect(&depth, V::D2, 0..1, 0..1, t::TextureAspect::StencilOnly)
        .unwrap();
    let upload = gpu
        .create_buffer("format source", 24, U::COPY_SRC | U::COPY_DST)
        .unwrap();
    let mut bytes = Vec::new();
    for f in [0f32, 1.] {
        bytes.extend(f.to_le_bytes());
    }
    for n in [65535u16, 0, 65535, 65535, 0, 65535, 0, 65535] {
        bytes.extend(n.to_le_bytes());
    }
    gpu.write_buffer(&upload, 0, &bytes).unwrap();
    let sm = gpu
        .sampler(&t::SamplerDescriptor {
            mag_filter: t::FilterMode::Linear,
            min_filter: t::FilterMode::Linear,
            ..Default::default()
        })
        .unwrap();
    let tex = |binding, sample_type| t::BindGroupLayoutEntry {
        binding,
        visibility: t::ShaderStages::COMPUTE,
        ty: t::BindingType::Texture {
            sample_type,
            view_dimension: V::D2,
            multisampled: false,
        },
        count: None,
    };
    let gl = gpu
        .group_layout(&[
            tex(0, t::TextureSampleType::Float { filterable: true }),
            tex(1, t::TextureSampleType::Float { filterable: false }),
            tex(2, t::TextureSampleType::Depth),
            tex(3, t::TextureSampleType::Uint),
            t::BindGroupLayoutEntry {
                binding: 4,
                visibility: t::ShaderStages::COMPUTE,
                ty: t::BindingType::Sampler(t::SamplerBindingType::Filtering),
                count: None,
            },
            t::BindGroupLayoutEntry {
                binding: 5,
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
    let layout = gpu.pipeline_layout(&[gl.clone()]).unwrap();
    let shader=gpu.shader("@group(0) @binding(0) var f:texture_2d<f32>;@group(0) @binding(1) var n:texture_2d<f32>;@group(0) @binding(2) var d:texture_depth_2d;@group(0) @binding(3) var s:texture_2d<u32>;@group(0) @binding(4) var sm:sampler;@group(0) @binding(5) var<storage,read_write> out:array<u32>;@compute @workgroup_size(1) fn main(){out[0]=bitcast<u32>(textureSampleLevel(f,sm,vec2f(0.5),0.).r);out[1]=bitcast<u32>(textureLoad(d,vec2i(0),0));out[2]=textureLoad(s,vec2i(0),0).r;let a=bitcast<vec4u>(textureLoad(n,vec2i(0),0));let b=bitcast<vec4u>(textureLoad(n,vec2i(1,0),0));for(var i=0u;i<4u;i++){out[3u+i]=a[i];out[7u+i]=b[i];}}").unwrap();
    let pipeline = gpu
        .shared_compute_pipeline(&layout, &shader, "main")
        .unwrap();
    let output = gpu
        .create_buffer("format output", 44, U::STORAGE_READ_WRITE | U::COPY_SRC)
        .unwrap();
    let read = gpu
        .create_buffer("format readback", 44, U::MAP_READ | U::COPY_DST)
        .unwrap();
    let group = gpu
        .bind_group(
            &gl,
            &[
                Entry {
                    binding: 0,
                    resource: Resource::Texture(&fv),
                },
                Entry {
                    binding: 1,
                    resource: Resource::Texture(&nv),
                },
                Entry {
                    binding: 2,
                    resource: Resource::Texture(&dv),
                },
                Entry {
                    binding: 3,
                    resource: Resource::Texture(&sv),
                },
                Entry {
                    binding: 4,
                    resource: Resource::Sampler(&sm),
                },
                Entry {
                    binding: 5,
                    resource: Resource::Buffer {
                        buffer: &output,
                        offset: 0,
                        size: 44,
                    },
                },
            ],
        )
        .unwrap();
    let mut e = gpu.encoder().unwrap();
    let row = L {
        offset: 0,
        bytes_per_row: Some(256),
        rows_per_image: Some(1),
    };
    e.copy_buffer_to_texture(&upload, row, &float, 0, Origin3d::ZERO, size)
        .unwrap();
    e.copy_buffer_to_texture(
        &upload,
        L { offset: 8, ..row },
        &norm,
        0,
        Origin3d::ZERO,
        size,
    )
    .unwrap();
    e.render_full(
        &[],
        Some(DepthAttachment {
            view: &attachment,
            clear_depth: Some(0.375),
            clear_stencil: Some(85),
        }),
        &[],
        &[],
    )
    .unwrap();
    e.dispatch_shared(
        &pipeline,
        &[BoundGroup {
            group: &group,
            dynamic_offsets: &[],
        }],
        [1, 1, 1],
    )
    .unwrap();
    e.copy_buffer(&output, 0, &read, 0, 44).unwrap();
    gpu.submit(vec![e.finish().unwrap()]).unwrap();
    let expected = [
        0.5f32.to_bits(),
        0.375f32.to_bits(),
        85,
        1f32.to_bits(),
        0,
        1f32.to_bits(),
        1f32.to_bits(),
        0,
        1f32.to_bits(),
        0,
        1f32.to_bits(),
    ];
    let actual = gpu.read_buffer(&read, 0..44, DEADLINE).unwrap();
    assert_eq!(
        actual,
        expected
            .into_iter()
            .flat_map(u32::to_le_bytes)
            .collect::<Vec<_>>()
    );
    drop(gpu);
    finish(before);
}
