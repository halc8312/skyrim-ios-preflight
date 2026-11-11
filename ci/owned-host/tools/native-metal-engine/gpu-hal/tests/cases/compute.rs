use super::{DEADLINE, U, finish, open, setup};
use skyrim_native_gpu::{
    bindings::{Entry, Resource},
    render::BoundGroup,
    texture::TextureDescriptor,
    types::{
        self, Extent3d, Origin3d, TexelCopyBufferLayout as L, TextureDimension as D,
        TextureFormat as F, TextureUses as T, TextureViewDimension as V,
    },
};
use std::num::NonZeroU64;
#[test]
fn shared_compute_uses_images_uniform_offsets_multiple_groups_and_checks_cross_group_aliases() {
    let (_lock, before) = setup();
    let mut gpu = open();
    let shader = gpu
        .shader(
            r#"
struct Params{scale:u32,add:u32,pad0:u32,pad1:u32}
@group(0) @binding(0) var<uniform> p:Params;
@group(0) @binding(1) var image:texture_2d<f32>;
@group(0) @binding(2) var<storage,read> source:array<u32>;
@group(1) @binding(0) var<storage,read_write> dest:array<u32>;
@compute @workgroup_size(1) fn main(@builtin(global_invocation_id) id:vec3u) {
 let i=id.y*2u+id.x;if(i>=4u){return;}
 dest[i]=source[i]*p.scale+p.add+u32(textureLoad(image,vec2i(id.xy),0).r);
}
"#,
        )
        .unwrap();
    let buffer = |binding, ty, dynamic, min| types::BindGroupLayoutEntry {
        binding,
        visibility: types::ShaderStages::COMPUTE,
        ty: types::BindingType::Buffer {
            ty,
            has_dynamic_offset: dynamic,
            min_binding_size: NonZeroU64::new(min),
        },
        count: None,
    };
    let gl0 = gpu
        .group_layout(&[
            buffer(0, types::BufferBindingType::Uniform, true, 16),
            types::BindGroupLayoutEntry {
                binding: 1,
                visibility: types::ShaderStages::COMPUTE,
                ty: types::BindingType::Texture {
                    sample_type: types::TextureSampleType::Float { filterable: false },
                    view_dimension: V::D2,
                    multisampled: false,
                },
                count: None,
            },
            buffer(
                2,
                types::BufferBindingType::Storage { read_only: true },
                false,
                4,
            ),
        ])
        .unwrap();
    let gl1 = gpu
        .group_layout(&[buffer(
            0,
            types::BufferBindingType::Storage { read_only: false },
            false,
            4,
        )])
        .unwrap();
    let layout = gpu.pipeline_layout(&[gl0.clone(), gl1.clone()]).unwrap();
    let pipeline = gpu
        .shared_compute_pipeline(&layout, &shader, "main")
        .unwrap();
    let uniform = gpu
        .create_buffer("compute params", 512, U::UNIFORM | U::COPY_DST)
        .unwrap();
    for (offset, words) in [(0, [3u32, 7, 0, 0]), (256, [5, 11, 0, 0])] {
        gpu.write_buffer(
            &uniform,
            offset,
            &words
                .into_iter()
                .flat_map(u32::to_le_bytes)
                .collect::<Vec<_>>(),
        )
        .unwrap();
    }
    let source = gpu
        .create_buffer("compute source", 16, U::STORAGE_READ_ONLY | U::COPY_DST)
        .unwrap();
    gpu.write_buffer(
        &source,
        0,
        &[10u32, 20, 30, 40]
            .into_iter()
            .flat_map(u32::to_le_bytes)
            .collect::<Vec<_>>(),
    )
    .unwrap();
    let dest = gpu
        .create_buffer(
            "compute dest",
            16,
            U::STORAGE_READ_WRITE | U::STORAGE_READ_ONLY | U::COPY_SRC,
        )
        .unwrap();
    let out = gpu
        .create_buffer("compute snapshots", 32, U::MAP_READ | U::COPY_DST)
        .unwrap();
    let size = Extent3d {
        width: 2,
        height: 2,
        depth_or_array_layers: 1,
    };
    let image = gpu
        .create_texture(
            "compute sampled image",
            TextureDescriptor {
                size,
                mip_levels: 1,
                sample_count: 1,
                dimension: D::D2,
                format: F::Rgba8Unorm,
                usage: T::RESOURCE | T::COPY_DST,
            },
        )
        .unwrap();
    let view = gpu.texture_view(&image, V::D2, 0..1, 0..1).unwrap();
    let upload = gpu
        .create_buffer("compute pixels", 512, U::COPY_SRC | U::COPY_DST)
        .unwrap();
    let mut pixels = [0; 512];
    pixels[..8].copy_from_slice(&[255, 0, 0, 255, 0, 0, 0, 255]);
    pixels[256..264].copy_from_slice(&[0, 0, 0, 255, 255, 0, 0, 255]);
    gpu.write_buffer(&upload, 0, &pixels).unwrap();
    let make_group = |src| {
        gpu.bind_group(
            &gl0,
            &[
                Entry {
                    binding: 0,
                    resource: Resource::Buffer {
                        buffer: &uniform,
                        offset: 0,
                        size: 16,
                    },
                },
                Entry {
                    binding: 1,
                    resource: Resource::Texture(&view),
                },
                Entry {
                    binding: 2,
                    resource: Resource::Buffer {
                        buffer: src,
                        offset: 0,
                        size: 16,
                    },
                },
            ],
        )
        .unwrap()
    };
    let g0 = make_group(&source);
    let aliased = make_group(&dest);
    let g1 = gpu
        .bind_group(
            &gl1,
            &[Entry {
                binding: 0,
                resource: Resource::Buffer {
                    buffer: &dest,
                    offset: 0,
                    size: 16,
                },
            }],
        )
        .unwrap();
    let bound = |g, offsets| {
        [
            BoundGroup {
                group: g,
                dynamic_offsets: offsets,
            },
            BoundGroup {
                group: &g1,
                dynamic_offsets: &[],
            },
        ]
    };
    let mut e = gpu.encoder().unwrap();
    e.copy_buffer_to_texture(
        &upload,
        L {
            offset: 0,
            bytes_per_row: Some(256),
            rows_per_image: Some(2),
        },
        &image,
        0,
        Origin3d::ZERO,
        size,
    )
    .unwrap();
    assert!(
        e.dispatch_shared(&pipeline, &bound(&aliased, &[0]), [2, 2, 1])
            .is_err()
    );
    assert!(
        e.dispatch_shared(&pipeline, &bound(&g0, &[1]), [0, 2, 1])
            .is_err()
    );
    assert!(e.dispatch_shared(&pipeline, &[], [2, 2, 1]).is_err());
    assert!(
        e.dispatch_shared(&pipeline, &bound(&g0, &[0]), [65536, 1, 1])
            .is_err()
    );
    e.dispatch_shared(&pipeline, &bound(&g0, &[0]), [2, 2, 1])
        .unwrap();
    e.copy_buffer(&dest, 0, &out, 0, 16).unwrap();
    e.dispatch_shared(&pipeline, &bound(&g0, &[256]), [2, 2, 1])
        .unwrap();
    e.copy_buffer(&dest, 0, &out, 16, 16).unwrap();
    drop(pipeline);
    drop(g0);
    drop(aliased);
    drop(g1);
    drop(source);
    drop(uniform);
    drop(view);
    drop(image);
    gpu.submit(vec![e.finish().unwrap()]).unwrap();
    let bytes = gpu.read_buffer(&out, 0..32, DEADLINE).unwrap();
    let words: Vec<_> = bytes
        .chunks_exact(4)
        .map(|b| u32::from_le_bytes(b.try_into().unwrap()))
        .collect();
    assert_eq!(words, [38, 67, 97, 128, 62, 111, 161, 212]);
    super::retire_all(&mut gpu);
    assert_eq!(gpu.native_counters().compute_pipelines.read(), 0);
    drop(gpu);
    finish(before);
}
