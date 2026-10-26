use super::{DEADLINE, U};
// Included by transfers.rs so every case shares the real validation canary.
use skyrim_native_gpu::{
    bindings::{Entry, Resource},
    render::{ColorAttachment, PipelineDescriptor},
    sink::PassSink,
    texture::TextureDescriptor,
    types::{
        self, Extent3d, Origin3d, TexelCopyBufferLayout as L, TextureDimension as D,
        TextureFormat as F, TextureUses as T, TextureViewDimension as V,
    },
};
use std::num::NonZeroU64;

#[test]
fn recorder_preserves_layout_switch_offsets_indices_and_failed_pass_atomicity() {
    let (_lock, before) = super::setup();
    let mut gpu = super::open();
    let source = r#"
@group(0) @binding(0) var<uniform> color0:vec4f;
@group(1) @binding(0) var<uniform> color1:vec4f;
@vertex fn vs(@builtin(vertex_index) i:u32)->@builtin(position) vec4f {
 let p=array<vec2f,3>(vec2f(-1.,-1.),vec2f(3.,-1.),vec2f(-1.,3.));return vec4f(p[i],0.,1.);}
@fragment fn a()->@location(0) vec4f {return color0;}
@fragment fn b()->@location(0) vec4f {return color1;}
"#;
    let shader = gpu.shader(source).unwrap();
    let gl = gpu
        .group_layout(&[types::BindGroupLayoutEntry {
            binding: 0,
            visibility: types::ShaderStages::FRAGMENT,
            ty: types::BindingType::Buffer {
                ty: types::BufferBindingType::Uniform,
                has_dynamic_offset: true,
                min_binding_size: NonZeroU64::new(16),
            },
            count: None,
        }])
        .unwrap();
    let empty = gpu.group_layout(&[]).unwrap();
    let eg = gpu.bind_group(&empty, &[]).unwrap();
    let la = gpu.pipeline_layout(&[gl.clone()]).unwrap();
    let lb = gpu.pipeline_layout(&[empty.clone(), gl.clone()]).unwrap();
    let u = gpu
        .create_buffer("recorder colors", 512, U::UNIFORM | U::COPY_DST)
        .unwrap();
    for (at, c) in [(0, [1f32, 0., 0., 1.]), (256, [0., 1., 0., 1.])] {
        gpu.write_buffer(
            &u,
            at,
            &c.into_iter().flat_map(f32::to_le_bytes).collect::<Vec<_>>(),
        )
        .unwrap();
    }
    let group = gpu
        .bind_group(
            &gl,
            &[Entry {
                binding: 0,
                resource: Resource::Buffer {
                    buffer: &u,
                    offset: 0,
                    size: 16,
                },
            }],
        )
        .unwrap();
    let colors = [Some(types::ColorTargetState {
        format: F::Rgba8Unorm,
        blend: None,
        write_mask: types::ColorWrites::ALL,
    })];
    let make = |layout, entry| {
        gpu.render_pipeline(&PipelineDescriptor {
            layout,
            vertex: &shader,
            vertex_entry: "vs",
            fragment: Some((&shader, entry)),
            colors: &colors,
            primitive: Default::default(),
            depth_stencil: None,
            multisample: Default::default(),
        })
        .unwrap()
    };
    let a = make(&la, "a");
    let b = make(&lb, "b");
    let size = Extent3d {
        width: 4,
        height: 4,
        depth_or_array_layers: 1,
    };
    let target = gpu
        .create_texture(
            "recorded output",
            TextureDescriptor {
                size,
                mip_levels: 1,
                sample_count: 1,
                dimension: D::D2,
                format: F::Rgba8Unorm,
                usage: T::COLOR_TARGET | T::COPY_SRC,
            },
        )
        .unwrap();
    let view = gpu.texture_view(&target, V::D2, 0..1, 0..1).unwrap();
    let out = gpu
        .create_buffer("record readback", 1024, U::MAP_READ | U::COPY_DST)
        .unwrap();
    let index = gpu
        .create_buffer("u16 indices", 8, U::INDEX | U::COPY_DST)
        .unwrap();
    gpu.write_buffer(&index, 0, &[0, 0, 1, 0, 2, 0, 0, 0])
        .unwrap();
    let attachments = [Some(ColorAttachment {
        view: &view,
        clear: Some(types::Color::BLACK),
    })];
    let mut record = gpu.render_recorder(&attachments, None, &[]).unwrap();
    record.set_group(0, &group, &[0]).unwrap();
    record.set_pipeline(&a).unwrap();
    record.set_scissor([0, 0, 2, 4]).unwrap();
    record.draw(0..3, 0..1).unwrap();
    record.set_pipeline(&b).unwrap();
    record.set_group(0, &eg, &[]).unwrap();
    record.set_group(1, &group, &[256]).unwrap();
    record
        .set_index(&index, types::IndexFormat::Uint16, 0, None)
        .unwrap();
    record.set_scissor([2, 0, 2, 4]).unwrap();
    record.draw_indexed(0..3, 0, 0..1).unwrap();
    // Empty state must not issue an illegal zero-area Vulkan scissor.
    record.set_scissor([0, 0, 0, 4]).unwrap();
    record.draw(0..3, 0..1).unwrap();
    let mut e = gpu.encoder().unwrap();
    let stats = e.render_recorded(&record).unwrap();
    assert_eq!(
        (
            stats.draws,
            stats.skipped_empty,
            stats.pipeline_binds,
            stats.group_binds
        ),
        (2, 1, 2, 3)
    );
    e.copy_texture_to_buffer(
        &target,
        0,
        Origin3d::ZERO,
        size,
        &out,
        L {
            offset: 0,
            bytes_per_row: Some(256),
            rows_per_image: Some(4),
        },
    )
    .unwrap();
    gpu.submit(vec![e.finish().unwrap()]).unwrap();
    let bytes = gpu.read_buffer(&out, 0..1024, DEADLINE).unwrap();
    for row in bytes.chunks(256) {
        assert_eq!(
            &row[..16],
            &[
                255, 0, 0, 255, 255, 0, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255
            ]
        );
    }
    // Recording errors latch, even when the caller ignores the first Result.
    // A previous valid draw and a new clear cannot leak from a rejected pass.
    record
        .reset(
            &[Some(ColorAttachment {
                view: &view,
                clear: Some(types::Color::WHITE),
            })],
            None,
            &[],
        )
        .unwrap();
    record.set_pipeline(&a).unwrap();
    record.set_group(0, &group, &[0]).unwrap();
    record.draw(0..3, 0..1).unwrap();
    assert!(record.set_group(0, &group, &[1]).is_err());
    assert!(record.set_group(0, &group, &[256]).is_err());
    let mut e = gpu.encoder().unwrap();
    assert!(e.render_recorded(&record).is_err());
    e.copy_texture_to_buffer(
        &target,
        0,
        Origin3d::ZERO,
        size,
        &out,
        L {
            offset: 0,
            bytes_per_row: Some(256),
            rows_per_image: Some(4),
        },
    )
    .unwrap();
    gpu.submit(vec![e.finish().unwrap()]).unwrap();
    assert_eq!(gpu.read_buffer(&out, 0..1024, DEADLINE).unwrap(), bytes);
    // Layout compatibility is rechecked after a pipeline change, even when no
    // new setter was needed for the earlier pass's group.
    record.reset(&attachments, None, &[]).unwrap();
    record.set_pipeline(&a).unwrap();
    record.set_group(0, &eg, &[]).unwrap();
    assert!(record.draw(0..3, 0..1).is_err());
    record.reset(&attachments, None, &[]).unwrap();
    record.set_pipeline(&a).unwrap();
    record.set_group(0, &group, &[0]).unwrap();
    record
        .set_index(&index, types::IndexFormat::Uint16, 0, None)
        .unwrap();
    assert!(record.draw_indexed(0..5, 0, 0..1).is_err());
    record.reset(&attachments, None, &[]).unwrap();
    let other = super::open();
    let foreign_layout = other.group_layout(&[]).unwrap();
    let foreign = other.bind_group(&foreign_layout, &[]).unwrap();
    assert!(record.set_group(0, &foreign, &[]).is_err());
    drop(record);
    drop(gpu);
    super::finish(before);
}

#[test]
fn recorder_detects_sampled_attachment_aliases_before_any_native_pass() {
    let (_lock, before) = super::setup();
    let mut gpu = super::open();
    let shader = gpu
        .shader(
            r#"
@group(0) @binding(0) var t:texture_2d<f32>;
@vertex fn vs(@builtin(vertex_index) i:u32)->@builtin(position) vec4f {
 let p=array<vec2f,3>(vec2f(-1.,-1.),vec2f(3.,-1.),vec2f(-1.,3.));return vec4f(p[i],0.,1.);}
@fragment fn fs()->@location(0) vec4f {return textureLoad(t,vec2i(0,0),0);}
"#,
        )
        .unwrap();
    let gl = gpu
        .group_layout(&[types::BindGroupLayoutEntry {
            binding: 0,
            visibility: types::ShaderStages::FRAGMENT,
            ty: types::BindingType::Texture {
                sample_type: types::TextureSampleType::Float { filterable: false },
                view_dimension: V::D2,
                multisampled: false,
            },
            count: None,
        }])
        .unwrap();
    let layout = gpu.pipeline_layout(&[gl.clone()]).unwrap();
    let pipeline = gpu
        .render_pipeline(&PipelineDescriptor {
            layout: &layout,
            vertex: &shader,
            vertex_entry: "vs",
            fragment: Some((&shader, "fs")),
            colors: &[Some(types::ColorTargetState {
                format: F::Rgba8Unorm,
                blend: None,
                write_mask: types::ColorWrites::ALL,
            })],
            primitive: Default::default(),
            depth_stencil: None,
            multisample: Default::default(),
        })
        .unwrap();
    let size = Extent3d {
        width: 4,
        height: 4,
        depth_or_array_layers: 1,
    };
    let target = gpu
        .create_texture(
            "aliased target",
            TextureDescriptor {
                size,
                mip_levels: 1,
                sample_count: 1,
                dimension: D::D2,
                format: F::Rgba8Unorm,
                usage: T::COLOR_TARGET | T::RESOURCE | T::COPY_SRC,
            },
        )
        .unwrap();
    let view = gpu.texture_view(&target, V::D2, 0..1, 0..1).unwrap();
    let group = gpu
        .bind_group(
            &gl,
            &[Entry {
                binding: 0,
                resource: Resource::Texture(&view),
            }],
        )
        .unwrap();
    let mut record = gpu
        .render_recorder(
            &[Some(ColorAttachment {
                view: &view,
                clear: Some(types::Color::WHITE),
            })],
            None,
            &[],
        )
        .unwrap();
    record.set_pipeline(&pipeline).unwrap();
    record.set_group(0, &group, &[]).unwrap();
    assert!(record.draw(0..3, 0..1).is_err());
    let out = gpu
        .create_buffer("zero output", 1024, U::MAP_READ | U::COPY_DST)
        .unwrap();
    let mut e = gpu.encoder().unwrap();
    assert!(e.render_recorded(&record).is_err());
    e.copy_texture_to_buffer(
        &target,
        0,
        Origin3d::ZERO,
        size,
        &out,
        L {
            offset: 0,
            bytes_per_row: Some(256),
            rows_per_image: Some(4),
        },
    )
    .unwrap();
    gpu.submit(vec![e.finish().unwrap()]).unwrap();
    assert_eq!(
        gpu.read_buffer(&out, 0..1024, DEADLINE).unwrap(),
        vec![0; 1024]
    );
    drop(gpu);
    super::finish(before);
}

#[test]
fn recorder_release_and_failed_reset_drop_old_ownership_but_not_encoded_work() {
    let (_lock, before) = super::setup();
    let mut gpu = super::open();
    let gl = gpu
        .group_layout(&[types::BindGroupLayoutEntry {
            binding: 0,
            visibility: types::ShaderStages::FRAGMENT,
            ty: types::BindingType::Buffer {
                ty: types::BufferBindingType::Uniform,
                has_dynamic_offset: false,
                min_binding_size: NonZeroU64::new(16),
            },
            count: None,
        }])
        .unwrap();
    let size = Extent3d {
        width: 4,
        height: 4,
        depth_or_array_layers: 1,
    };
    let target = gpu
        .create_texture(
            "released target",
            TextureDescriptor {
                size,
                mip_levels: 1,
                sample_count: 1,
                dimension: D::D2,
                format: F::Rgba8Unorm,
                usage: T::COLOR_TARGET | T::COPY_SRC,
            },
        )
        .unwrap();
    let view = gpu.texture_view(&target, V::D2, 0..1, 0..1).unwrap();
    let attachment = || {
        Some(ColorAttachment {
            view: &view,
            clear: Some(types::Color {
                r: 0.,
                g: 1.,
                b: 0.,
                a: 1.,
            }),
        })
    };
    let mut record = gpu.render_recorder(&[attachment()], None, &[]).unwrap();
    let serial = gpu.submit(vec![]).unwrap();
    gpu.wait(serial, DEADLINE).unwrap();
    gpu.poll().unwrap();
    let baseline_buffers = gpu.native_counters().buffers.read();
    for invalid_reset in [false, true] {
        let u = gpu
            .create_buffer("released uniform", 16, U::UNIFORM)
            .unwrap();
        let group = gpu
            .bind_group(
                &gl,
                &[Entry {
                    binding: 0,
                    resource: Resource::Buffer {
                        buffer: &u,
                        offset: 0,
                        size: 16,
                    },
                }],
            )
            .unwrap();
        let ids = [u.id(), group.id(), gl.id(), view.id()];
        for (i, id) in ids.iter().enumerate() {
            assert_ne!(*id, 0);
            assert!(!ids[..i].contains(id));
        }
        record.reset(&[attachment()], None, &[]).unwrap();
        record.set_group(0, &group, &[]).unwrap();
        let serial = gpu.submit(vec![]).unwrap();
        gpu.wait(serial, DEADLINE).unwrap();
        gpu.poll().unwrap();
        drop(group);
        drop(u);
        assert_eq!(gpu.native_counters().buffers.read(), baseline_buffers + 1);
        if invalid_reset {
            assert!(
                record
                    .reset(&[attachment(), attachment()], None, &[])
                    .is_err()
            );
        } else {
            record.release();
        }
        assert_eq!(gpu.native_counters().buffers.read(), baseline_buffers);
        assert!(gpu.encoder().unwrap().render_recorded(&record).is_err());
    }
    record.reset(&[attachment()], None, &[]).unwrap();
    let mut e = gpu.encoder().unwrap();
    e.render_recorded(&record).unwrap();
    let out = gpu
        .create_buffer("released replay", 1024, U::MAP_READ | U::COPY_DST)
        .unwrap();
    e.copy_texture_to_buffer(
        &target,
        0,
        Origin3d::ZERO,
        size,
        &out,
        L {
            offset: 0,
            bytes_per_row: Some(256),
            rows_per_image: Some(4),
        },
    )
    .unwrap();
    gpu.delay_fence_observation(Some(0));
    gpu.submit(vec![e.finish().unwrap()]).unwrap();
    record.release();
    drop(view);
    drop(target);
    gpu.poll().unwrap();
    assert!(gpu.native_counters().texture_views.read() > 0);
    gpu.delay_fence_observation(None);
    let bytes = gpu.read_buffer(&out, 0..1024, DEADLINE).unwrap();
    for row in bytes.chunks(256) {
        for pixel in row[..16].chunks(4) {
            assert_eq!(pixel, &[0, 255, 0, 255]);
        }
    }
    gpu.poll().unwrap();
    assert_eq!(gpu.native_counters().texture_views.read(), 0);
    drop(gpu);
    super::finish(before);
}
