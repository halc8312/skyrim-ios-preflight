use super::{DEADLINE, U, finish, open, setup};
use skyrim_native_gpu::{
    pipeline::PipelineCache,
    render::ColorAttachment,
    sink::PassSink,
    texture::TextureDescriptor,
    types::{
        self, Extent3d, Origin3d, TexelCopyBufferLayout as L, TextureDimension as D,
        TextureFormat as F, TextureUses as T, TextureViewDimension as V,
    },
};
use xenos_gfx::pipeline as pp;

pub(super) fn normalized_key() -> pp::Pipeline<F> {
    use xenos_state::{
        bitfields::RbDepthcontrol, blend::OutputMergerState, depth_stencil::DepthStencilState,
        viewport::RasterizerState, xenos,
    };
    let mut regs = vec![0u32; 0x5003];
    // A zero blend register means ZERO*src+ZERO*dst, not replace. Author the
    // intended ONE/ZERO blend explicitly; keep the GPU pixel oracle unchanged.
    regs[xenos_state::registers::RB_BLENDCONTROL0 as usize] =
        xenos_state::bitfields::RbBlendcontrol {
            color_srcblend: xenos::BlendFactor::One,
            color_comb_fcn: xenos::BlendOp::Add,
            color_destblend: xenos::BlendFactor::Zero,
            alpha_srcblend: xenos::BlendFactor::One,
            alpha_comb_fcn: xenos::BlendOp::Add,
            alpha_destblend: xenos::BlendFactor::Zero,
        }
        .encode();
    let raster = RasterizerState::from_registers(&regs, xenos::PrimitiveType::TriangleList, false);
    let output = OutputMergerState::from_registers(&regs, 0xf);
    let depth = DepthStencilState::from_registers(&regs, RbDepthcontrol::decode(0), true);
    pp::prepare(
        pp::Draw {
            vertex_shader: 1,
            pixel_shader: Some(2),
            texture_layout: 0,
            topology: xenos_state::primitive::HostTopology::TriangleList,
            strip_indices: pp::IndexWidth::None,
        },
        pp::Attachments {
            colors: [
                Some(pp::ColorFormat {
                    format: F::Rgba8Unorm,
                    blendable: true,
                }),
                None,
                None,
                None,
            ],
            depth: None,
            sample_count: 1,
            sample_mask: u32::MAX,
        },
        &raster,
        &output,
        &depth,
        1,
    )
}

#[test]
fn shared_normalized_pipeline_cache_keys_real_shaders_entries_and_preserves_evicted_draws() {
    let (_lock, before) = setup();
    let mut gpu = open();
    let source = r#"
@vertex fn vs(@builtin(vertex_index) i:u32)->@builtin(position) vec4f {
 let p=array<vec2f,3>(vec2f(-1.,-1.),vec2f(3.,-1.),vec2f(-1.,3.));return vec4f(p[i],0.,1.);}
@fragment fn red()->@location(0) vec4f{return vec4f(1.,0.,0.,1.);}
@fragment fn green()->@location(0) vec4f{return vec4f(0.,1.,0.,1.);}
"#;
    let shader = gpu.shader(source).unwrap();
    let changed = gpu
        .shader(&source.replace("vec4f(1.,0.,0.,1.)", "vec4f(0.,0.,1.,1.)"))
        .unwrap();
    let layout = gpu.pipeline_layout(&[]).unwrap();
    let key = normalized_key();
    let mut cache = PipelineCache::new(2).unwrap();
    let red = cache
        .get_or_create(&gpu, &key, &layout, &shader, "vs", &shader, "red")
        .unwrap();
    let green = cache
        .get_or_create(&gpu, &key, &layout, &shader, "vs", &shader, "green")
        .unwrap();
    drop(
        cache
            .get_or_create(&gpu, &key, &layout, &shader, "vs", &shader, "red")
            .unwrap(),
    );
    // Same guest key and entry name, different actual immutable shader.
    let blue = cache
        .get_or_create(&gpu, &key, &layout, &changed, "vs", &changed, "red")
        .unwrap();
    let stats = cache.stats();
    assert_eq!(
        (stats.created, stats.hits, stats.evicted, stats.live),
        (3, 1, 1, 2)
    );
    let other = open();
    assert!(
        cache
            .get_or_create(&other, &key, &layout, &shader, "vs", &shader, "red")
            .is_err()
    );
    drop(other);
    let mut invalid = key.clone();
    invalid.color_slots = 5;
    assert!(
        cache
            .get_or_create(&gpu, &invalid, &layout, &shader, "vs", &shader, "red")
            .is_err()
    );
    invalid = key.clone();
    invalid.pixel = None;
    assert!(
        cache
            .get_or_create(&gpu, &invalid, &layout, &shader, "vs", &shader, "red")
            .is_err()
    );
    assert_eq!((cache.stats().created, cache.stats().evicted), (3, 1));
    let size = Extent3d {
        width: 4,
        height: 4,
        depth_or_array_layers: 1,
    };
    let target = gpu
        .create_texture(
            "pipeline cache pixels",
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
        .create_buffer("pipeline cache readback", 1024, U::MAP_READ | U::COPY_DST)
        .unwrap();
    let mut record = gpu
        .render_recorder(
            &[Some(ColorAttachment {
                view: &view,
                clear: Some(types::Color::BLACK),
            })],
            None,
            &[],
        )
        .unwrap();
    for (p, rect) in [
        (&red, [0, 0, 1, 4]),
        (&green, [1, 0, 2, 4]),
        (&blue, [3, 0, 1, 4]),
    ] {
        record.set_pipeline(p).unwrap();
        record.set_scissor(rect).unwrap();
        record.draw(0..3, 0..1).unwrap();
    }
    let mut e = gpu.encoder().unwrap();
    e.render_recorded(&record).unwrap();
    drop(record);
    drop(red);
    drop(green);
    drop(blue);
    cache.clear();
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
                255, 0, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255
            ]
        );
    }
    assert_eq!(gpu.native_counters().render_pipelines.read(), 0);
    drop(gpu);
    finish(before);
}
