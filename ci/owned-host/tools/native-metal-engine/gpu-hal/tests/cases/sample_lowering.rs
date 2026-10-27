use super::edram_transfer_cases::read_samples;
use super::{DEADLINE, finish, open, setup};
use skyrim_native_gpu::{
    Gpu,
    backend::{Backend, Platform},
    render::{ColorAttachment, DepthAttachment, Draw, PipelineDescriptor},
    texture::{TextureDescriptor, TextureView},
    types::{self as t, TextureFormat as F, TextureUses as T, TextureViewDimension as V},
};

const CODE: &str = r#"
@vertex fn vs(@builtin(vertex_index) i:u32)->@builtin(position) vec4f {
 let p=array<vec2f,3>(vec2f(-1.,-1.),vec2f(3.,-1.),vec2f(-1.,3.));return vec4f(p[i],0.25,1.);}
@fragment fn direct(@builtin(position) p:vec4f)->@location(0) vec4f {
 for(var i=0u;i<2u;i++){if(i==1u && p.x<1.){return vec4f(1.,0.,0.,1.);}}
 return vec4f(0.,1.,0.,1.);}
struct C{@location(0) c:vec4f,@builtin(frag_depth) d:f32}
@fragment fn structured(@builtin(position) p:vec4f)->C {
 switch(u32(p.x)){case 0u:{return C(vec4f(1.,0.,0.,1.),0.25);}default:{return C(vec4f(0.,1.,0.,1.),0.25);}}}
struct M{@location(0) c:vec4f,@builtin(sample_mask) mask:u32,@builtin(frag_depth) d:f32}
@fragment fn existing(@builtin(position) p:vec4f)->M {
 if(p.x<1.){return M(vec4f(1.,0.,0.,1.),5u,0.25);}return M(vec4f(0.,1.,0.,1.),3u,0.25);}
@fragment fn mask_only()->@builtin(sample_mask) u32 {return 9u;}
@fragment fn no_output(){}
@fragment fn discarded(@builtin(position) p:vec4f)->@location(0) vec4f {
 if(p.x<1.){discard;}return vec4f(0.,1.,0.,1.);}
fn per_sample(p:vec4f,s:u32)->vec4f {
 let x=u32(p.x);return vec4f(f32(40u*(s+1u)+10u*x),f32(50u*(3u-s)),f32(60u*(s^x)),255.)/255.;}
@fragment fn sample_direct(@builtin(position) p:vec4f,@builtin(sample_index) s:u32)->@location(0) vec4f {
 return per_sample(p,s);}
struct I{@builtin(position) p:vec4f,@builtin(sample_index) s:u32}
@fragment fn sample_structured(i:I)->C {return C(per_sample(i.p,i.s),f32(i.s+1u)/8.);}
@fragment fn sample_existing(i:I)->M {
 return M(per_sample(i.p,i.s),select(5u,10u,i.p.x>=1.),f32(i.s+1u)/8.);}
@fragment fn sample_discarded(i:I)->C {
 if(i.s==1u || (i.p.x<1. && i.s==2u)){discard;}
 return C(per_sample(i.p,i.s),f32(i.s+1u)/8.);}
"#;
fn target(gpu: &mut Gpu<Platform>, format: F) -> TextureView<Platform> {
    let image = gpu
        .create_texture(
            "coverage oracle",
            TextureDescriptor {
                size: t::Extent3d {
                    width: 2,
                    height: 1,
                    depth_or_array_layers: 1,
                },
                mip_levels: 1,
                sample_count: 4,
                dimension: t::TextureDimension::D2,
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

#[test]
fn restricted_lowering_matches_all_fixed_masks_and_independent_color_depth_stencil_samples() {
    let (_lock, before) = setup();
    let mut gpu = open();
    let source = gpu.shader(CODE).unwrap();
    let source_id = source.id();
    let layout = gpu.pipeline_layout(&[]).unwrap();
    let color = target(&mut gpu, F::Rgba8Unorm);
    let depth = target(&mut gpu, F::Depth24PlusStencil8);
    let colors = [Some(t::ColorTargetState {
        format: F::Rgba8Unorm,
        blend: None,
        write_mask: t::ColorWrites::ALL,
    })];
    let stencil = t::StencilFaceState {
        compare: t::CompareFunction::Always,
        fail_op: t::StencilOperation::Keep,
        depth_fail_op: t::StencilOperation::Keep,
        pass_op: t::StencilOperation::Replace,
    };
    for entry in [
        "direct",
        "structured",
        "existing",
        "mask_only",
        "no_output",
        "discarded",
        "sample_direct",
        "sample_structured",
        "sample_existing",
        "sample_discarded",
    ] {
        let has_color = !matches!(entry, "mask_only" | "no_output");
        for mask in 0..16u32 {
            let lowered = gpu.shader_with_sample_mask(&source, entry, mask).unwrap();
            assert_ne!(source_id, lowered.id());
            let mut baseline = None;
            for fixed in [true, false] {
                if fixed && !<Platform as Backend>::STATIC_SAMPLE_MASK {
                    continue;
                }
                let p = gpu
                    .render_pipeline(&PipelineDescriptor {
                        layout: &layout,
                        vertex: &source,
                        vertex_entry: "vs",
                        fragment: Some((if fixed { &source } else { &lowered }, entry)),
                        colors: if has_color { &colors } else { &[] },
                        primitive: Default::default(),
                        depth_stencil: Some(t::DepthStencilState {
                            format: F::Depth24PlusStencil8,
                            depth_write_enabled: Some(true),
                            depth_compare: Some(t::CompareFunction::Always),
                            stencil: t::StencilState {
                                front: stencil,
                                back: stencil,
                                read_mask: 255,
                                write_mask: 255,
                            },
                            bias: Default::default(),
                        }),
                        multisample: t::MultisampleState {
                            count: 4,
                            mask: if fixed { u64::from(mask) } else { !0 },
                            alpha_to_coverage_enabled: false,
                        },
                    })
                    .unwrap();
                let mut encoder = gpu.encoder().unwrap();
                let attachment = [Some(ColorAttachment {
                    view: &color,
                    clear: Some(t::Color::BLACK),
                })];
                encoder
                    .render_full(
                        if has_color { &attachment } else { &[] },
                        Some(DepthAttachment {
                            view: &depth,
                            clear_depth: Some(1.),
                            clear_stencil: Some(11),
                        }),
                        &[],
                        &[Draw {
                            pipeline: &p,
                            groups: &[],
                            vertices: 0..3,
                            instances: 0..1,
                            indices: None,
                            viewport: None,
                            scissor: None,
                            blend: t::Color::TRANSPARENT,
                            stencil_reference: 63,
                        }],
                    )
                    .unwrap();
                let c = if has_color {
                    Some(read_samples(&mut gpu, &mut encoder, &color, false))
                } else {
                    None
                };
                let d = read_samples(&mut gpu, &mut encoder, &depth, false);
                gpu.submit(vec![encoder.finish().unwrap()]).unwrap();
                let d = gpu.read_buffer(&d.0, 0..d.1, DEADLINE).unwrap();
                let c = c.map(|r| gpu.read_buffer(&r.0, 0..r.1, DEADLINE).unwrap());
                for x in 0..2usize {
                    for s in 0..4usize {
                        let program_mask = match entry {
                            "existing" => {
                                if x == 0 {
                                    5
                                } else {
                                    3
                                }
                            }
                            "mask_only" => 9,
                            "discarded" if x == 0 => 0,
                            "sample_existing" => {
                                if x == 0 {
                                    5
                                } else {
                                    10
                                }
                            }
                            "sample_discarded" if s == 1 || (x == 0 && s == 2) => 0,
                            _ => 15,
                        };
                        let on = mask & program_mask & (1 << s) != 0;
                        let at = (x * 4 + s) * 8;
                        let z =
                            f32::from_bits(u32::from_le_bytes(d[at..at + 4].try_into().unwrap()));
                        let drawn_depth =
                            if entry.starts_with("sample_") && entry != "sample_direct" {
                                (s + 1) as f32 / 8.
                            } else {
                                0.25
                            };
                        assert!(
                            (z - if on { drawn_depth } else { 1. }).abs() <= 1. / 16777215.,
                            "{entry} mask{mask} fixed{fixed} depth x{x} s{s}: {z}"
                        );
                        assert_eq!(
                            u32::from_le_bytes(d[at + 4..at + 8].try_into().unwrap()),
                            if on { 63 } else { 11 },
                            "{entry} mask{mask} stencil"
                        );
                        if let Some(c) = &c {
                            let expected = if !on {
                                [0, 0, 0, 255]
                            } else if entry.starts_with("sample_") {
                                [
                                    (40 * (s + 1) + 10 * x) as u8,
                                    (50 * (3 - s)) as u8,
                                    (60 * (s ^ x)) as u8,
                                    255,
                                ]
                            } else if x == 0 {
                                [255, 0, 0, 255]
                            } else {
                                [0, 255, 0, 255]
                            };
                            assert_eq!(
                                &c[at..at + 4],
                                &expected,
                                "{entry} mask{mask} fixed{fixed} x{x} s{s}"
                            );
                        }
                    }
                }
                if fixed {
                    baseline = Some((c, d));
                } else if let Some(base) = &baseline {
                    assert_eq!(&(c, d), base, "fixed/lowered {entry} mask{mask}");
                }
            }
        }
    }
    assert_eq!(source.id(), source_id);
    drop(gpu);
    finish(before);
}

#[test]
fn lowering_rejects_invocation_sensitive_programs_and_foreign_owners() {
    let (_lock, before) = setup();
    let gpu = open();
    for code in [
        "@fragment fn fs(@builtin(sample_mask) v:u32)->@location(0) vec4f{return vec4f(f32(v));}",
        "struct I{@builtin(sample_mask) v:u32} @fragment fn fs(v:I)->@location(0) vec4f{return vec4f(f32(v.v));}",
        "@fragment fn fs(@builtin(sample_index) v:u32)->@location(0) vec4f{return vec4f(dpdx(f32(v)));}",
        "struct I{@builtin(sample_index) s:u32,@builtin(sample_mask) m:u32} @fragment fn fs(v:I)->@location(0) vec4f{return vec4f(f32(v.s+v.m));}",
        "@group(0) @binding(0) var<storage,read_write> out:array<u32>; @fragment fn fs(@builtin(sample_index) s:u32)->@location(0) vec4f{out[s]=s;return vec4f(1.);}",
        "@fragment fn fs(@location(0) @interpolate(perspective,centroid) v:f32)->@location(0) vec4f{return vec4f(v);}",
        "@group(0) @binding(0) var<storage,read_write> v:array<u32>; fn write(){v[0]=1u;} @fragment fn fs()->@location(0) vec4f{write();return vec4f(1.);}",
        "@group(0) @binding(0) var<storage,read_write> v:atomic<u32>; @fragment fn fs()->@location(0) vec4f{let old=atomicAdd(&v,1u);return vec4f(f32(old));}",
    ] {
        let shader = gpu.shader(code).unwrap();
        assert!(
            gpu.shader_with_sample_mask(&shader, "fs", 9).is_err(),
            "{code}"
        );
    }
    let other = open();
    let foreign = other.shader("@fragment fn fs(){}").unwrap();
    assert!(gpu.shader_with_sample_mask(&foreign, "fs", 9).is_err());
    // Private invocation-local state is not an externally observable side effect.
    let private = gpu
        .shader("var<private> v:u32; @fragment fn fs()->@builtin(sample_mask) u32{v=7u;return v;}")
        .unwrap();
    assert!(gpu.shader_with_sample_mask(&private, "fs", 9).is_ok());
    let source = gpu.shader(CODE).unwrap();
    let lowered = gpu.shader_with_sample_mask(&source, "direct", 9).unwrap();
    let layout = gpu.pipeline_layout(&[]).unwrap();
    assert!(
        gpu.render_pipeline(&PipelineDescriptor {
            layout: &layout,
            vertex: &source,
            vertex_entry: "vs",
            fragment: Some((&lowered, "direct")),
            colors: &[Some(t::ColorTargetState {
                format: F::Rgba8Unorm,
                blend: None,
                write_mask: t::ColorWrites::ALL
            })],
            primitive: Default::default(),
            depth_stencil: None,
            multisample: t::MultisampleState {
                count: 4,
                mask: !0,
                alpha_to_coverage_enabled: true
            },
        })
        .is_err()
    );
    drop(other);
    drop(gpu);
    finish(before);
}

#[test]
fn full_active_coverage_does_not_require_metal_lowering_for_unused_upper_mask_bits() {
    let (_lock, before) = setup();
    let mut gpu = open();
    let source = gpu.shader(CODE).unwrap();
    let layout = gpu.pipeline_layout(&[]).unwrap();
    let color = target(&mut gpu, F::Rgba8Unorm);
    for mask in [15u64, u64::from(u32::MAX), u64::MAX, 15 | (1u64 << 48)] {
        let pipeline = gpu
            .render_pipeline(&PipelineDescriptor {
                layout: &layout,
                vertex: &source,
                vertex_entry: "vs",
                fragment: Some((&source, "direct")),
                colors: &[Some(t::ColorTargetState {
                    format: F::Rgba8Unorm,
                    blend: None,
                    write_mask: t::ColorWrites::ALL,
                })],
                primitive: Default::default(),
                depth_stencil: None,
                multisample: t::MultisampleState {
                    count: 4,
                    mask,
                    alpha_to_coverage_enabled: false,
                },
            })
            .unwrap();
        let mut e = gpu.encoder().unwrap();
        e.render(
            &[Some(ColorAttachment {
                view: &color,
                clear: Some(t::Color::BLACK),
            })],
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
        let result = read_samples(&mut gpu, &mut e, &color, false);
        gpu.submit(vec![e.finish().unwrap()]).unwrap();
        let pixels = gpu.read_buffer(&result.0, 0..result.1, DEADLINE).unwrap();
        for x in 0..2 {
            for sample in 0..4 {
                let at = (x * 4 + sample) * 8;
                assert_eq!(
                    &pixels[at..at + 4],
                    if x == 0 {
                        &[255, 0, 0, 255]
                    } else {
                        &[0, 255, 0, 255]
                    }
                );
            }
        }
    }
    drop(gpu);
    finish(before);
}
