//! Exhaustive preservation through actual host depth textures. This proves all
//! 24-bit payloads survive the selected storage format, not physical iPhone FPS.
use super::{U, finish, open_config, setup};
use skyrim_native_gpu::{
    Config,
    bindings::{Entry, Resource},
    render::{BoundGroup, DepthAttachment, Draw, PipelineDescriptor},
    texture::TextureDescriptor,
    types as t,
};
use std::{num::NonZeroU64, time::Duration};
#[test]
#[ignore = "exhaustive 2^24 depth values through each actual host format"]
fn exhaustive_depth_codec_preserves_all_24bit_patterns_and_detects_lossy_float_fallback() {
    let (_lock, before) = setup();
    let mut cfg = Config::default();
    cfg.optional_texture_features = t::Features::DEPTH32FLOAT_STENCIL8;
    let mut gpu = open_config(cfg);
    assert!(
        gpu.features().contains(t::Features::DEPTH32FLOAT_STENCIL8),
        "D32S8 oracle requires actual feature"
    );
    let uniform = t::BindGroupLayoutEntry {
        binding: 0,
        visibility: t::ShaderStages::FRAGMENT | t::ShaderStages::COMPUTE,
        ty: t::BindingType::Buffer {
            ty: t::BufferBindingType::Uniform,
            has_dynamic_offset: false,
            min_binding_size: NonZeroU64::new(16),
        },
        count: None,
    };
    let gl = gpu.group_layout(&[uniform]).unwrap();
    let layout = gpu.pipeline_layout(&[gl.clone()]).unwrap();
    let helpers = xenos_gfx::edram_shaders::encoding_module(1);
    let shader=gpu.shader(&format!("{helpers}\n{}",r#"
@group(0) @binding(0) var<uniform> q:vec4u;
@vertex fn oracle_vs(@builtin(vertex_index)i:u32)->@builtin(position)vec4f{let p=array<vec2f,3>(vec2f(-1.,-1.),vec2f(3.,-1.),vec2f(-1.,3.));return vec4f(p[i],0.,1.);}
@fragment fn oracle_fs(@builtin(position)p:vec4f)->@builtin(frag_depth)f32{let n=q.x+u32(p.y)*1024u+u32(p.x);return unpack_depth(n<<8u,q.y);}
"#)).unwrap();
    let compute_layout = gpu
        .group_layout(&[
            uniform,
            t::BindGroupLayoutEntry {
                binding: 1,
                visibility: t::ShaderStages::COMPUTE,
                ty: t::BindingType::Texture {
                    sample_type: t::TextureSampleType::Depth,
                    view_dimension: t::TextureViewDimension::D2,
                    multisampled: false,
                },
                count: None,
            },
            t::BindGroupLayoutEntry {
                binding: 2,
                visibility: t::ShaderStages::COMPUTE,
                ty: t::BindingType::Buffer {
                    ty: t::BufferBindingType::Storage { read_only: false },
                    has_dynamic_offset: false,
                    min_binding_size: NonZeroU64::new(24),
                },
                count: None,
            },
        ])
        .unwrap();
    let cl = gpu.pipeline_layout(&[compute_layout.clone()]).unwrap();
    let cs=gpu.shader(&format!("{helpers}\n{}",r#"
@group(0) @binding(0) var<uniform> q:vec4u;
@group(0) @binding(1) var input_depth:texture_depth_2d;
@group(0) @binding(2) var<storage,read_write> errors:array<atomic<u32>>;
@compute @workgroup_size(8,8) fn oracle_cs(@builtin(global_invocation_id)p:vec3u){let n=q.x+p.y*1024u+p.x;let d=textureLoad(input_depth,vec2i(p.xy),0);let actual=pack_depth(d,0u,q.y)>>8u;if(actual!=n){atomicAdd(&errors[0],1u);atomicMin(&errors[1],n);}if(n==6291457u){atomicStore(&errors[2],bitcast<u32>(d));atomicStore(&errors[3],actual);}let rt=pack_depth(unpack_depth(n<<8u,q.y),0u,q.y)>>8u;if(rt!=n){atomicAdd(&errors[4],1u);atomicMin(&errors[5],n);}}
"#)).unwrap();
    let cp = gpu.shared_compute_pipeline(&cl, &cs, "oracle_cs").unwrap();
    let params = gpu
        .create_buffer("depth codec params", 16, U::UNIFORM | U::COPY_DST)
        .unwrap();
    let errors = gpu
        .create_buffer(
            "depth mismatch counts",
            24,
            U::STORAGE_READ_WRITE | U::COPY_SRC | U::COPY_DST,
        )
        .unwrap();
    let readback = gpu
        .create_buffer(
            "depth mismatch readback",
            4 * 16 * 24,
            U::MAP_READ | U::COPY_DST,
        )
        .unwrap();
    let pg = gpu
        .bind_group(
            &gl,
            &[Entry {
                binding: 0,
                resource: Resource::Buffer {
                    buffer: &params,
                    offset: 0,
                    size: 16,
                },
            }],
        )
        .unwrap();
    let modes = [
        (t::TextureFormat::Depth24PlusStencil8, 16u32, true),
        (t::TextureFormat::Depth32FloatStencil8, 16, false),
        (t::TextureFormat::Depth32FloatStencil8, 17, false),
        (t::TextureFormat::Depth24PlusStencil8, 17, true),
    ];
    let mut failures = 0;
    for (mode, (format, guest, lossy_control)) in modes.into_iter().enumerate() {
        let image = gpu
            .create_texture(
                "exhaustive host depth",
                TextureDescriptor {
                    size: t::Extent3d {
                        width: 1024,
                        height: 1024,
                        depth_or_array_layers: 1,
                    },
                    mip_levels: 1,
                    sample_count: 1,
                    dimension: t::TextureDimension::D2,
                    format,
                    usage: t::TextureUses::DEPTH_STENCIL_WRITE | t::TextureUses::RESOURCE,
                },
            )
            .unwrap();
        let view = gpu
            .texture_view(&image, t::TextureViewDimension::D2, 0..1, 0..1)
            .unwrap();
        let sample = gpu
            .texture_view_aspect(
                &image,
                t::TextureViewDimension::D2,
                0..1,
                0..1,
                t::TextureAspect::DepthOnly,
            )
            .unwrap();
        let pipeline = gpu
            .render_pipeline(&PipelineDescriptor {
                layout: &layout,
                vertex: &shader,
                vertex_entry: "oracle_vs",
                fragment: Some((&shader, "oracle_fs")),
                colors: &[],
                primitive: Default::default(),
                depth_stencil: Some(t::DepthStencilState {
                    format,
                    depth_write_enabled: Some(true),
                    depth_compare: Some(t::CompareFunction::Always),
                    stencil: Default::default(),
                    bias: Default::default(),
                }),
                multisample: Default::default(),
            })
            .unwrap();
        let cg = gpu
            .bind_group(
                &compute_layout,
                &[
                    Entry {
                        binding: 0,
                        resource: Resource::Buffer {
                            buffer: &params,
                            offset: 0,
                            size: 16,
                        },
                    },
                    Entry {
                        binding: 1,
                        resource: Resource::Texture(&sample),
                    },
                    Entry {
                        binding: 2,
                        resource: Resource::Buffer {
                            buffer: &errors,
                            offset: 0,
                            size: 24,
                        },
                    },
                ],
            )
            .unwrap();
        let mut e = gpu.encoder().unwrap();
        for part in 0..16u32 {
            let p: Vec<u8> = [part << 20, guest, 0, 0]
                .into_iter()
                .flat_map(u32::to_le_bytes)
                .collect();
            let zero: Vec<u8> = [0u32, u32::MAX, 0, 0, 0, u32::MAX]
                .into_iter()
                .flat_map(u32::to_le_bytes)
                .collect();
            gpu.write_buffers_ordered(
                &mut e,
                &[
                    skyrim_native_gpu::OrderedUpload {
                        destination: &params,
                        offset: 0,
                        data: &p,
                    },
                    skyrim_native_gpu::OrderedUpload {
                        destination: &errors,
                        offset: 0,
                        data: &zero,
                    },
                ],
            )
            .unwrap();
            e.render_full(
                &[],
                Some(DepthAttachment {
                    view: &view,
                    clear_depth: None,
                    clear_stencil: None,
                }),
                &[],
                &[Draw {
                    pipeline: &pipeline,
                    groups: &[BoundGroup {
                        group: &pg,
                        dynamic_offsets: &[],
                    }],
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
            e.dispatch_shared(
                &cp,
                &[BoundGroup {
                    group: &cg,
                    dynamic_offsets: &[],
                }],
                [128, 128, 1],
            )
            .unwrap();
            e.copy_buffer(
                &errors,
                0,
                &readback,
                ((mode * 16 + part as usize) * 24) as u64,
                24,
            )
            .unwrap();
        }
        gpu.submit(vec![e.finish().unwrap()]).unwrap();
        let bytes = gpu
            .read_buffer(
                &readback,
                (mode * 16 * 24) as u64..((mode + 1) * 16 * 24) as u64,
                Duration::from_secs(120),
            )
            .unwrap();
        let mut mismatches = 0u64;
        let mut first = u32::MAX;
        let mut rtbad = 0u64;
        let mut rtfirst = u32::MAX;
        let mut sample = 0;
        let mut packed = 0;
        for part in bytes.chunks_exact(24) {
            mismatches += u64::from(u32::from_le_bytes(part[..4].try_into().unwrap()));
            first = first.min(u32::from_le_bytes(part[4..8].try_into().unwrap()));
            sample |= u32::from_le_bytes(part[8..12].try_into().unwrap());
            packed |= u32::from_le_bytes(part[12..16].try_into().unwrap());
            rtbad += u64::from(u32::from_le_bytes(part[16..20].try_into().unwrap()));
            rtfirst = rtfirst.min(u32::from_le_bytes(part[20..24].try_into().unwrap()));
        }
        println!(
            "depth_oracle host={format:?} guest={guest} values=16777216 mismatches={mismatches} first={first} lossy_control={lossy_control} codec_only_bad={rtbad} codec_first={rtfirst} sample_bits={sample:08x} packed={packed}"
        );
        if lossy_control {
            // Metal's Depth24Plus may itself use D32S8, in which case this
            // optional negative control is lossless; Linux D24 proves the loss.
            #[cfg(target_os = "linux")]
            assert!(
                mismatches > 0,
                "expected actual D24 storage to expose float24 loss"
            );
        } else {
            failures += usize::from(mismatches != 0);
        }
    }
    drop(gpu);
    finish(before);
    assert_eq!(failures, 0, "see exact per-format mismatch counts above");
}
