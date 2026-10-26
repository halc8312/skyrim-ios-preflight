use super::{DEADLINE, U, finish, open, setup};
use skyrim_native_gpu::{
    bindings::{Entry, Resource},
    render::{self as r, ColorAttachment, PipelineDescriptor},
    sink::PassSink,
    texture::TextureDescriptor,
    types::{
        self as t, Extent3d, Origin3d, TexelCopyBufferLayout as L, TextureDimension as D,
        TextureFormat as F, TextureUses as T, TextureViewDimension as V,
    },
};
#[test]
fn strip_nonindexed_extended_viewport_empty_area_and_reversed_depth_match_pinned_contract() {
    let (_lock, before) = setup();
    let mut gpu = open();
    let shader=gpu.shader(r#"
@vertex fn vs(@builtin(vertex_index) i:u32)->@builtin(position) vec4f {let p=array<vec2f,3>(vec2f(-1.,-1.),vec2f(3.,-1.),vec2f(-1.,3.));return vec4f(p[i],0.,1.);}
@fragment fn fs()->@location(0) vec4f {return vec4f(1.,0.,0.,1.);}
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
            primitive: t::PrimitiveState {
                topology: t::PrimitiveTopology::TriangleStrip,
                strip_index_format: Some(t::IndexFormat::Uint16),
                ..Default::default()
            },
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
            "parity target",
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
        .create_buffer("parity pixels", 1024, U::COPY_DST | U::MAP_READ)
        .unwrap();
    for recorded in [false, true] {
        for empty in [false, true] {
            let v = r::Viewport {
                x: -2.,
                y: -2.,
                width: if empty { 0. } else { 8. },
                height: 8.,
                min_depth: 0.,
                max_depth: 1.,
            };
            let attachments = [Some(ColorAttachment {
                view: &view,
                clear: Some(t::Color::BLACK),
            })];
            let mut e = gpu.encoder().unwrap();
            let stats = if recorded {
                let mut rec = gpu.render_recorder(&attachments, None, &[]).unwrap();
                rec.set_pipeline(&pipeline).unwrap();
                rec.set_viewport([v.x, v.y, v.width, v.height, 0., 1.])
                    .unwrap();
                rec.draw(0..3, 0..1).unwrap();
                e.render_recorded(&rec).unwrap()
            } else {
                e.render(
                    &attachments,
                    &[r::Draw {
                        pipeline: &pipeline,
                        groups: &[],
                        vertices: 0..3,
                        instances: 0..1,
                        indices: None,
                        viewport: Some(v),
                        scissor: None,
                        blend: t::Color::TRANSPARENT,
                        stencil_reference: 0,
                    }],
                )
                .unwrap()
            };
            assert_eq!(stats.draws, if empty { 0 } else { 1 });
            assert_eq!(stats.skipped_empty, if empty { 1 } else { 0 });
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
            let expected = if empty {
                [0, 0, 0, 255]
            } else {
                [255, 0, 0, 255]
            };
            for row in bytes.chunks(256) {
                for p in row[..16].chunks(4) {
                    assert_eq!(p, &expected);
                }
            }
        }
    }
    let mut rec = gpu
        .render_recorder(
            &[Some(ColorAttachment {
                view: &view,
                clear: None,
            })],
            None,
            &[],
        )
        .unwrap();
    assert!(rec.set_viewport([0., 0., 4., 4., 1., 0.]).is_err());
    for access in [
        t::StorageTextureAccess::ReadOnly,
        t::StorageTextureAccess::WriteOnly,
    ] {
        assert!(
            gpu.group_layout(&[t::BindGroupLayoutEntry {
                binding: 0,
                visibility: t::ShaderStages::FRAGMENT,
                ty: t::BindingType::StorageTexture {
                    access,
                    format: F::Rgba8Unorm,
                    view_dimension: V::D2
                },
                count: None
            }])
            .is_err()
        );
    }
    drop(gpu);
    finish(before);
}
#[test]
fn recorded_msaa_resolve_is_transitioned_retained_and_cannot_be_sampled_in_same_pass() {
    let (_lock, before) = setup();
    let mut gpu = open();
    let size = Extent3d {
        width: 4,
        height: 4,
        depth_or_array_layers: 1,
    };
    let ms = gpu
        .create_texture(
            "four sample target",
            TextureDescriptor {
                size,
                mip_levels: 1,
                sample_count: 4,
                dimension: D::D2,
                format: F::Rgba8Unorm,
                usage: T::COLOR_TARGET,
            },
        )
        .unwrap();
    let dst = gpu
        .create_texture(
            "resolve target",
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
    let mv = gpu.texture_view(&ms, V::D2, 0..1, 0..1).unwrap();
    let rv = gpu.texture_view(&dst, V::D2, 0..1, 0..1).unwrap();
    let attachments = [Some(ColorAttachment {
        view: &mv,
        clear: Some(t::Color {
            r: 0.,
            g: 1.,
            b: 0.,
            a: 1.,
        }),
    })];
    let mut rec = gpu
        .render_recorder(&attachments, None, &[Some(&rv)])
        .unwrap();
    let mut e = gpu.encoder().unwrap();
    e.render_recorded(&rec).unwrap();
    rec.release();
    let out = gpu
        .create_buffer("resolve readback", 1024, U::COPY_DST | U::MAP_READ)
        .unwrap();
    e.copy_texture_to_buffer(
        &dst,
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
    let original = gpu.read_buffer(&out, 0..1024, DEADLINE).unwrap();
    for row in original.chunks(256) {
        for p in row[..16].chunks(4) {
            assert_eq!(p, &[0, 255, 0, 255]);
        }
    }
    let gl = gpu
        .group_layout(&[t::BindGroupLayoutEntry {
            binding: 0,
            visibility: t::ShaderStages::FRAGMENT,
            ty: t::BindingType::Texture {
                sample_type: t::TextureSampleType::Float { filterable: false },
                view_dimension: V::D2,
                multisampled: false,
            },
            count: None,
        }])
        .unwrap();
    let group = gpu
        .bind_group(
            &gl,
            &[Entry {
                binding: 0,
                resource: Resource::Texture(&rv),
            }],
        )
        .unwrap();
    let layout = gpu.pipeline_layout(&[gl]).unwrap();
    let shader=gpu.shader(r#"
@group(0) @binding(0) var image:texture_2d<f32>;
@vertex fn vs(@builtin(vertex_index) i:u32)->@builtin(position) vec4f {let p=array<vec2f,3>(vec2f(-1.,-1.),vec2f(3.,-1.),vec2f(-1.,3.));return vec4f(p[i],0.,1.);}
@fragment fn fs()->@location(0) vec4f{return textureLoad(image,vec2i(0),0);}
"#).unwrap();
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
            multisample: t::MultisampleState {
                count: 4,
                ..Default::default()
            },
        })
        .unwrap();
    rec.reset(
        &[Some(ColorAttachment {
            view: &mv,
            clear: Some(t::Color::WHITE),
        })],
        None,
        &[Some(&rv)],
    )
    .unwrap();
    rec.set_pipeline(&pipeline).unwrap();
    rec.set_group(0, &group, &[]).unwrap();
    assert!(rec.draw(0..3, 0..1).is_err());
    let mut e = gpu.encoder().unwrap();
    assert!(e.render_recorded(&rec).is_err());
    e.copy_texture_to_buffer(
        &dst,
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
    assert_eq!(gpu.read_buffer(&out, 0..1024, DEADLINE).unwrap(), original);
    drop(gpu);
    finish(before);
}
