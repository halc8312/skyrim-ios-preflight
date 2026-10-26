use super::{DEADLINE, U, finish, open, setup};
use skyrim_native_gpu::{
    bindings::{Entry, Resource},
    pipeline::PipelineCache,
    render::{BoundGroup, ColorAttachment, DepthAttachment, Draw, PipelineDescriptor},
    sink::PassSink,
    texture::TextureDescriptor,
    types::{
        self, Extent3d, Origin3d, TexelCopyBufferLayout as L, TextureDimension as D,
        TextureFormat as F, TextureUses as T, TextureViewDimension as V,
    },
};
#[test]
fn genuine_fragmentless_pipeline_writes_depth_and_matches_borrowed_and_recorded_routes() {
    let (_lock, before) = setup();
    let mut gpu = open();
    let shader = gpu
        .shader(
            r#"
@group(0) @binding(0) var d:texture_depth_2d;
@vertex fn vs(@builtin(vertex_index) i:u32)->@builtin(position) vec4f {
 let p=array<vec2f,3>(vec2f(-1.,-1.),vec2f(3.,-1.),vec2f(-1.,3.));return vec4f(p[i],0.25,1.);}
@fragment fn sample(@builtin(position) at:vec4f)->@location(0) vec4f {
 let depth=textureLoad(d,vec2i(at.xy),0);return vec4f(depth,depth,depth,1.);}
"#,
        )
        .unwrap();
    let empty = gpu.pipeline_layout(&[]).unwrap();
    let mut key = super::pipeline_cases::normalized_key();
    key.pixel = None;
    key.colors = [None, None, None, None];
    key.color_slots = 0;
    let stencil = xenos_state::depth_stencil::StencilFaceState {
        compare: xenos_state::xenos::CompareFunction::Always,
        fail_op: xenos_state::xenos::StencilOp::Keep,
        depth_fail_op: xenos_state::xenos::StencilOp::Keep,
        pass_op: xenos_state::xenos::StencilOp::Keep,
    };
    key.depth = Some(xenos_gfx::pipeline::Depth {
        format: F::Depth32Float,
        write: true,
        compare: xenos_state::xenos::CompareFunction::Less,
        stencil: [stencil; 2],
        read_mask: 0,
        write_mask: 0,
        bias: 0,
        slope_scale_bits: 0,
    });
    let mut cache = PipelineCache::new(2).unwrap();
    let p = cache
        .get_or_create_depth(&gpu, &key, &empty, &shader, "vs")
        .unwrap();
    let gl = gpu
        .group_layout(&[types::BindGroupLayoutEntry {
            binding: 0,
            visibility: types::ShaderStages::FRAGMENT,
            ty: types::BindingType::Texture {
                sample_type: types::TextureSampleType::Depth,
                view_dimension: V::D2,
                multisampled: false,
            },
            count: None,
        }])
        .unwrap();
    let layout = gpu.pipeline_layout(&[gl.clone()]).unwrap();
    let sample = gpu
        .render_pipeline(&PipelineDescriptor {
            layout: &layout,
            vertex: &shader,
            vertex_entry: "vs",
            fragment: Some((&shader, "sample")),
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
    let depth = gpu
        .create_texture(
            "depth only",
            TextureDescriptor {
                size,
                mip_levels: 1,
                sample_count: 1,
                dimension: D::D2,
                format: F::Depth32Float,
                usage: T::DEPTH_STENCIL_WRITE | T::RESOURCE,
            },
        )
        .unwrap();
    let dv = gpu.texture_view(&depth, V::D2, 0..1, 0..1).unwrap();
    let group = gpu
        .bind_group(
            &gl,
            &[Entry {
                binding: 0,
                resource: Resource::Texture(&dv),
            }],
        )
        .unwrap();
    let color = gpu
        .create_texture(
            "depth sample",
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
    let cv = gpu.texture_view(&color, V::D2, 0..1, 0..1).unwrap();
    let out = gpu
        .create_buffer("depth pixels", 1024, U::MAP_READ | U::COPY_DST)
        .unwrap();
    for recorded in [false, true] {
        let attachment = Some(DepthAttachment {
            view: &dv,
            clear_depth: Some(1.),
            clear_stencil: None,
        });
        let mut e = gpu.encoder().unwrap();
        if recorded {
            let mut r = gpu.render_recorder(&[], attachment, &[]).unwrap();
            r.set_pipeline(&p).unwrap();
            r.set_scissor([0, 0, 2, 4]).unwrap();
            r.draw(0..3, 0..1).unwrap();
            e.render_recorded(&r).unwrap();
        } else {
            e.render_full(
                &[],
                attachment,
                &[],
                &[Draw {
                    pipeline: &p,
                    groups: &[],
                    vertices: 0..3,
                    instances: 0..1,
                    indices: None,
                    viewport: None,
                    scissor: Some([0, 0, 2, 4]),
                    blend: types::Color::TRANSPARENT,
                    stencil_reference: 0,
                }],
            )
            .unwrap();
        }
        let groups = [BoundGroup {
            group: &group,
            dynamic_offsets: &[],
        }];
        e.render(
            &[Some(ColorAttachment {
                view: &cv,
                clear: Some(types::Color::BLACK),
            })],
            &[Draw {
                pipeline: &sample,
                groups: &groups,
                vertices: 0..3,
                instances: 0..1,
                indices: None,
                viewport: None,
                scissor: None,
                blend: types::Color::TRANSPARENT,
                stencil_reference: 0,
            }],
        )
        .unwrap();
        e.copy_texture_to_buffer(
            &color,
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
                    64, 64, 64, 255, 64, 64, 64, 255, 255, 255, 255, 255, 255, 255, 255, 255
                ]
            );
        }
    }
    drop(gpu);
    finish(before);
}
