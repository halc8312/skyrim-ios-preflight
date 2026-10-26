use super::edram_transfer_cases::read_samples;
use super::{DEADLINE, finish, open, setup};
use skyrim_native_gpu::{
    render::{ColorAttachment, DepthAttachment, Draw, PipelineDescriptor},
    texture::TextureDescriptor,
    types::{self as t, TextureFormat as F, TextureUses as T, TextureViewDimension as V},
};

#[test]
fn untargeted_colors_are_pruned_while_coverage_depth_and_discard_survive() {
    let (_lock, before) = setup();
    let mut gpu = open();
    let shader=gpu.shader(r#"
@vertex fn vs(@builtin(vertex_index)i:u32)->@builtin(position)vec4f{let p=array<vec2f,3>(vec2f(-1.,-1.),vec2f(3.,-1.),vec2f(-1.,3.));return vec4f(p[i],0.25,1.);}
struct O{@location(0) a:vec4f,@location(2)b:vec4f,@builtin(sample_mask)m:u32}
@fragment fn fs()->O{return O(vec4f(1.,0.,0.,1.),vec4f(0.,1.,0.,1.),3u);}
@fragment fn effects(@builtin(position)p:vec4f)->@location(0)vec4f{if(p.x<1.){discard;}return vec4f(1.);}
"#).unwrap();
    assert_eq!(
        gpu.shader_with_color_targets(&shader, "fs", 5)
            .unwrap()
            .id(),
        shader.id()
    );
    let size = t::Extent3d {
        width: 2,
        height: 1,
        depth_or_array_layers: 1,
    };
    let color = gpu
        .create_texture(
            "sparse color target",
            TextureDescriptor {
                size,
                mip_levels: 1,
                sample_count: 4,
                dimension: t::TextureDimension::D2,
                format: F::Rgba8Unorm,
                usage: T::COLOR_TARGET | T::RESOURCE,
            },
        )
        .unwrap();
    let cv = gpu.texture_view(&color, V::D2, 0..1, 0..1).unwrap();
    let layout = gpu.pipeline_layout(&[]).unwrap();
    let pipeline = gpu
        .render_pipeline(&PipelineDescriptor {
            layout: &layout,
            vertex: &shader,
            vertex_entry: "vs",
            fragment: Some((&shader, "fs")),
            colors: &[
                None,
                None,
                Some(t::ColorTargetState {
                    format: F::Rgba8Unorm,
                    blend: None,
                    write_mask: t::ColorWrites::ALL,
                }),
            ],
            primitive: Default::default(),
            depth_stencil: None,
            multisample: t::MultisampleState {
                count: 4,
                ..Default::default()
            },
        })
        .unwrap();
    let mut e = gpu.encoder().unwrap();
    e.render_full(
        &[
            None,
            None,
            Some(ColorAttachment {
                view: &cv,
                clear: Some(t::Color::BLACK),
            }),
        ],
        None,
        &[],
        &[Draw {
            pipeline: &pipeline,
            groups: &[],
            vertices: 0..3,
            instances: 0..1,
            indices: None,
            viewport: None,
            scissor: None,
            blend: t::Color::TRANSPARENT,
            stencil_reference: 0,
        }],
    )
    .unwrap();
    let (read, size_bytes) = read_samples(&mut gpu, &mut e, &cv, false);
    gpu.submit(vec![e.finish().unwrap()]).unwrap();
    for (n, p) in gpu
        .read_buffer(&read, 0..size_bytes, DEADLINE)
        .unwrap()
        .chunks_exact(8)
        .enumerate()
    {
        assert_eq!(
            &p[..4],
            if n % 4 < 2 {
                &[0, 255, 0, 255]
            } else {
                &[0, 0, 0, 255]
            }
        );
    }
    let depth = gpu
        .create_texture(
            "effects depth",
            TextureDescriptor {
                size,
                mip_levels: 1,
                sample_count: 1,
                dimension: t::TextureDimension::D2,
                format: F::Depth24PlusStencil8,
                usage: T::DEPTH_STENCIL_WRITE | T::RESOURCE,
            },
        )
        .unwrap();
    let dv = gpu.texture_view(&depth, V::D2, 0..1, 0..1).unwrap();
    let pipeline = gpu
        .render_pipeline(&PipelineDescriptor {
            layout: &layout,
            vertex: &shader,
            vertex_entry: "vs",
            fragment: Some((&shader, "effects")),
            colors: &[],
            primitive: Default::default(),
            depth_stencil: Some(t::DepthStencilState {
                format: F::Depth24PlusStencil8,
                depth_write_enabled: Some(true),
                depth_compare: Some(t::CompareFunction::Always),
                stencil: Default::default(),
                bias: Default::default(),
            }),
            multisample: Default::default(),
        })
        .unwrap();
    let mut e = gpu.encoder().unwrap();
    e.render_full(
        &[],
        Some(DepthAttachment {
            view: &dv,
            clear_depth: Some(1.),
            clear_stencil: Some(0),
        }),
        &[],
        &[Draw {
            pipeline: &pipeline,
            groups: &[],
            vertices: 0..3,
            instances: 0..1,
            indices: None,
            viewport: None,
            scissor: None,
            blend: t::Color::TRANSPARENT,
            stencil_reference: 0,
        }],
    )
    .unwrap();
    let (read, size_bytes) = read_samples(&mut gpu, &mut e, &dv, false);
    drop(shader);
    gpu.submit(vec![e.finish().unwrap()]).unwrap();
    let values = gpu.read_buffer(&read, 0..size_bytes, DEADLINE).unwrap();
    for (x, p) in values.chunks_exact(8).enumerate() {
        let z = f32::from_le_bytes(p[..4].try_into().unwrap());
        assert!((z - if x == 0 { 1. } else { 0.25 }).abs() <= 1. / 16777215.);
        assert_eq!(&p[4..], &[0, 0, 0, 0]);
    }
    drop(gpu);
    finish(before);
}
