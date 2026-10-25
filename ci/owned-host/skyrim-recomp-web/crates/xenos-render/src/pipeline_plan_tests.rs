// Frozen legacy reference from native-metal-shader-preparation-v1.
// Only signature/data wrappers differ: no GPU device or handles are required.
use crate::pipeline_plan as pp;
use gpu_webgpu::*;
use xenos_state::{
    blend::{HostBlendFactor, HostBlendOperation, OutputMergerState},
    depth_stencil::DepthStencilState as GuestDepthStencil,
    primitive::HostTopology,
    viewport::{CullMode as GuestCull, FrontFace as GuestFrontFace, RasterizerState},
    xenos,
};
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
struct ColorKey {
    format: TextureFormat,
    blend: Option<[(BlendOperation, BlendFactor, BlendFactor); 2]>,
    write_mask: u32,
}

#[derive(Clone, Debug, PartialEq, Eq, Hash)]
struct DepthKey {
    format: TextureFormat,
    write: bool,
    compare: CompareFunction,
    stencil: [(CompareFunction, StencilOperation, StencilOperation, StencilOperation); 2],
    read_mask: u32,
    write_mask: u32,
    bias: i32,
    slope_scale_bits: u32,
}

#[derive(Clone, Debug, PartialEq, Eq, Hash)]
struct PipelineKey {
    vertex: u64,
    pixel: Option<u64>,
    /// Which texture bind group layout (see `texture_layouts`).
    texture_layout: u32,
    topology: PrimitiveTopology,
    strip_index_format: IndexFormat,
    cull: CullMode,
    front_face: FrontFace,
    colors: [Option<ColorKey>; 4],
    depth: Option<DepthKey>,
    /// Color attachment slots of the pass (bound slots up to the last one).
    color_slots: usize,
    sample_count: u32,
    sample_mask: u32,
    alpha_to_coverage: bool,
}

fn topology(t: HostTopology) -> PrimitiveTopology {
    match t {
        HostTopology::PointList => PrimitiveTopology::PointList,
        HostTopology::LineList => PrimitiveTopology::LineList,
        HostTopology::LineStrip => PrimitiveTopology::LineStrip,
        HostTopology::TriangleList => PrimitiveTopology::TriangleList,
        HostTopology::TriangleStrip => PrimitiveTopology::TriangleStrip,
    }
}

fn compare(f: xenos::CompareFunction) -> CompareFunction {
    use xenos::CompareFunction as C;
    match f {
        C::Never => CompareFunction::Never,
        C::Less => CompareFunction::Less,
        C::Equal => CompareFunction::Equal,
        C::LessEqual => CompareFunction::LessEqual,
        C::Greater => CompareFunction::Greater,
        C::NotEqual => CompareFunction::NotEqual,
        C::GreaterEqual => CompareFunction::GreaterEqual,
        C::Always => CompareFunction::Always,
    }
}

fn stencil_op(op: xenos::StencilOp) -> StencilOperation {
    use xenos::StencilOp as S;
    match op {
        S::Keep => StencilOperation::Keep,
        S::Zero => StencilOperation::Zero,
        S::Replace => StencilOperation::Replace,
        S::IncrementClamp => StencilOperation::IncrementClamp,
        S::DecrementClamp => StencilOperation::DecrementClamp,
        S::Invert => StencilOperation::Invert,
        S::IncrementWrap => StencilOperation::IncrementWrap,
        S::DecrementWrap => StencilOperation::DecrementWrap,
    }
}

fn blend_factor(f: HostBlendFactor) -> BlendFactor {
    match f {
        HostBlendFactor::Zero => BlendFactor::Zero,
        HostBlendFactor::One => BlendFactor::One,
        HostBlendFactor::Src => BlendFactor::Src,
        HostBlendFactor::OneMinusSrc => BlendFactor::OneMinusSrc,
        HostBlendFactor::SrcAlpha => BlendFactor::SrcAlpha,
        HostBlendFactor::OneMinusSrcAlpha => BlendFactor::OneMinusSrcAlpha,
        HostBlendFactor::Dst => BlendFactor::Dst,
        HostBlendFactor::OneMinusDst => BlendFactor::OneMinusDst,
        HostBlendFactor::DstAlpha => BlendFactor::DstAlpha,
        HostBlendFactor::OneMinusDstAlpha => BlendFactor::OneMinusDstAlpha,
        HostBlendFactor::SrcAlphaSaturated => BlendFactor::SrcAlphaSaturated,
        HostBlendFactor::Constant => BlendFactor::Constant,
        HostBlendFactor::OneMinusConstant => BlendFactor::OneMinusConstant,
    }
}

fn blend_operation(op: HostBlendOperation) -> BlendOperation {
    match op {
        HostBlendOperation::Add => BlendOperation::Add,
        HostBlendOperation::Subtract => BlendOperation::Subtract,
        HostBlendOperation::ReverseSubtract => BlendOperation::ReverseSubtract,
        HostBlendOperation::Min => BlendOperation::Min,
        HostBlendOperation::Max => BlendOperation::Max,
    }
}

fn has_stencil(format: TextureFormat) -> bool {
    matches!(format, TextureFormat::Depth24PlusStencil8 | TextureFormat::Depth32FloatStencil8 | TextureFormat::Stencil8)
}

struct DrawTargets {
    colors: [Option<((), TextureFormat)>; 4],
    depth: Option<((), TextureFormat)>,
    blendable: [bool; 4],
    sample_count: u32,
    sample_mask: u32,
}
fn color_slots(t: &DrawTargets) -> usize {
    t.colors.iter().rposition(Option::is_some).map_or(0, |i| i + 1)
}
struct Shader {
    id: u64,
}
struct Host {
    topology: HostTopology,
}
fn legacy(
    draw: pp::Draw,
    a: pp::Attachments<TextureFormat>,
    rasterizer: &RasterizerState,
    output: &OutputMergerState,
    depth_stencil: &GuestDepthStencil,
    scale: u32,
) -> PipelineKey {
    let targets = DrawTargets {
        colors: a.colors.map(|c| c.map(|v| ((), v.format))),
        depth: a.depth.map(|d| ((), d.format)),
        blendable: a.colors.map(|c| c.is_some_and(|v| v.blendable)),
        sample_count: a.sample_count,
        sample_mask: a.sample_mask,
    };
    let vs = Shader { id: draw.vertex_shader };
    let ps_owner = draw.pixel_shader.map(|id| Shader { id });
    let ps = ps_owner.as_ref();
    let texture_layout = draw.texture_layout;
    let host = Host { topology: draw.topology };
    let strip_index_format = match draw.strip_indices {
        pp::IndexWidth::None => IndexFormat::Undefined,
        pp::IndexWidth::U16 => IndexFormat::Uint16,
        pp::IndexWidth::U32 => IndexFormat::Uint32,
    };
    // Every bound slot gets a target (write mask 0 where the draw doesn't write it), so the
    // pipeline matches the pass.
    let colors = std::array::from_fn(|i| {
        let (_, format) = targets.colors[i].as_ref()?;
        let state = output.targets[i];
        Some(ColorKey {
            format: *format,
            blend: state.and_then(|s| s.blend).filter(|_| targets.blendable[i]).map(|(c, a)| {
                [c, a].map(|x| (blend_operation(x.operation), blend_factor(x.src_factor), blend_factor(x.dst_factor)))
            }),
            write_mask: state.map_or(0, |s| s.write_mask),
        })
    });
    let depth = targets.depth.as_ref().map(|(_, format)| {
        let stencil = has_stencil(*format) && depth_stencil.stencil_enabled;
        let face = |s: xenos_state::depth_stencil::StencilFaceState| {
            if stencil {
                (compare(s.compare), stencil_op(s.fail_op), stencil_op(s.depth_fail_op), stencil_op(s.pass_op))
            } else {
                (CompareFunction::Always, StencilOperation::Keep, StencilOperation::Keep, StencilOperation::Keep)
            }
        };
        // Depth changes less per host pixel at higher resolution scales (Xenia scales it too).
        let (bias, slope) = if rasterizer.polygonal {
            (rasterizer.depth_bias.constant, rasterizer.depth_bias.slope_scale * scale as f32)
        } else {
            (0, 0.0)
        };
        DepthKey {
            format: *format,
            write: depth_stencil.depth_write_enabled,
            compare: compare(depth_stencil.depth_compare),
            stencil: [face(depth_stencil.stencil_front), face(depth_stencil.stencil_back)],
            read_mask: if stencil { u32::from(depth_stencil.stencil_read_mask) } else { 0xFF },
            write_mask: if stencil { u32::from(depth_stencil.stencil_write_mask) } else { 0 },
            bias,
            slope_scale_bits: slope.to_bits(),
        }
    });
    PipelineKey {
        vertex: vs.id,
        pixel: ps.map(|p| p.id),
        texture_layout,
        topology: topology(host.topology),
        strip_index_format,
        cull: match rasterizer.cull_mode {
            GuestCull::None => CullMode::None,
            GuestCull::Front => CullMode::Front,
            GuestCull::Back => CullMode::Back,
        },
        front_face: match rasterizer.front_face {
            GuestFrontFace::Ccw => FrontFace::CCW,
            GuestFrontFace::Cw => FrontFace::CW,
        },
        colors,
        depth,
        color_slots: color_slots(&targets),
        sample_count: targets.sample_count,
        sample_mask: targets.sample_mask,
        // Exact guest coverage is output before color exponent bias; host
        // A2C would incorrectly mask those samples a second time.
        alpha_to_coverage: false,
    }
}

fn legacy_projection(
    k: &PipelineKey,
) -> (Vec<ColorTargetState>, Option<DepthStencilState>, PrimitiveState, MultisampleState) {
    let c = k.colors[..k.color_slots]
        .iter()
        .map(|c| match c {
            Some(c) => ColorTargetState {
                format: c.format,
                blend: c.blend.map(|[c, a]| BlendState {
                    color: BlendComponent { operation: c.0, src_factor: c.1, dst_factor: c.2 },
                    alpha: BlendComponent { operation: a.0, src_factor: a.1, dst_factor: a.2 },
                }),
                write_mask: ColorWriteMask(u64::from(c.write_mask)),
            },
            None => {
                ColorTargetState { format: TextureFormat::Undefined, blend: None, write_mask: ColorWriteMask::None }
            }
        })
        .collect();
    let f = |s: (CompareFunction, StencilOperation, StencilOperation, StencilOperation)| StencilFaceState {
        compare: s.0,
        fail_op: s.1,
        depth_fail_op: s.2,
        pass_op: s.3,
    };
    let d = k.depth.as_ref().map(|d| DepthStencilState {
        format: d.format,
        depth_write_enabled: d.write,
        depth_compare: d.compare,
        stencil_front: f(d.stencil[0]),
        stencil_back: f(d.stencil[1]),
        stencil_read_mask: d.read_mask,
        stencil_write_mask: d.write_mask,
        depth_bias: d.bias,
        depth_bias_slope_scale: f32::from_bits(d.slope_scale_bits),
        depth_bias_clamp: 0.0,
    });
    (
        c,
        d,
        PrimitiveState {
            topology: k.topology,
            strip_index_format: k.strip_index_format,
            front_face: k.front_face,
            cull_mode: k.cull,
            unclipped_depth: false,
        },
        MultisampleState { count: k.sample_count, mask: k.sample_mask, alpha_to_coverage_enabled: k.alpha_to_coverage },
    )
}
fn fixture(
    k: usize,
) -> (pp::Draw, pp::Attachments<TextureFormat>, RasterizerState, OutputMergerState, GuestDepthStencil) {
    let regs = vec![0u32; 0x5003];
    let mut r = RasterizerState::from_registers(&regs, xenos::PrimitiveType::TriangleList, false);
    r.cull_mode = [GuestCull::None, GuestCull::Front, GuestCull::Back][k % 3];
    r.front_face = if k & 2 == 0 { GuestFrontFace::Ccw } else { GuestFrontFace::Cw };
    r.polygonal = k & 4 == 0;
    r.depth_bias.constant = k as i32 - 2048;
    r.depth_bias.slope_scale = [0.0, -0.0, 0.125, -0.25, f32::from_bits(0x7fc00123)][k % 5];
    let mut o = OutputMergerState::from_registers(&regs, 0xffff);
    let base = o.targets[0].unwrap();
    let ops = [
        HostBlendOperation::Add,
        HostBlendOperation::Subtract,
        HostBlendOperation::ReverseSubtract,
        HostBlendOperation::Min,
        HostBlendOperation::Max,
    ];
    let factors = [
        HostBlendFactor::Zero,
        HostBlendFactor::One,
        HostBlendFactor::Src,
        HostBlendFactor::OneMinusSrc,
        HostBlendFactor::SrcAlpha,
        HostBlendFactor::OneMinusSrcAlpha,
        HostBlendFactor::Dst,
        HostBlendFactor::OneMinusDst,
        HostBlendFactor::DstAlpha,
        HostBlendFactor::OneMinusDstAlpha,
        HostBlendFactor::SrcAlphaSaturated,
        HostBlendFactor::Constant,
        HostBlendFactor::OneMinusConstant,
    ];
    o.targets = std::array::from_fn(|i| {
        if (k >> i) & 1 == 0 {
            None
        } else {
            Some(xenos_state::blend::ColorTargetState {
                blend: if (k >> (i + 4)) & 1 == 0 {
                    None
                } else {
                    Some((
                        xenos_state::blend::BlendComponent {
                            operation: ops[(k + i) % 5],
                            src_factor: factors[(k + i) % 13],
                            dst_factor: factors[(k * 3 + i) % 13],
                        },
                        xenos_state::blend::BlendComponent {
                            operation: ops[(k + 2 * i) % 5],
                            src_factor: factors[(k + 7 + i) % 13],
                            dst_factor: factors[(k * 7 + i) % 13],
                        },
                    ))
                },
                write_mask: ((k >> i) & 15) as u32,
                ..base
            })
        }
    });
    let mut d = GuestDepthStencil::from_registers(&regs, xenos_state::bitfields::RbDepthcontrol::decode(0), true);
    let cmp = [
        xenos::CompareFunction::Never,
        xenos::CompareFunction::Less,
        xenos::CompareFunction::Equal,
        xenos::CompareFunction::LessEqual,
        xenos::CompareFunction::Greater,
        xenos::CompareFunction::NotEqual,
        xenos::CompareFunction::GreaterEqual,
        xenos::CompareFunction::Always,
    ];
    let stencil = [
        xenos::StencilOp::Keep,
        xenos::StencilOp::Zero,
        xenos::StencilOp::Replace,
        xenos::StencilOp::IncrementClamp,
        xenos::StencilOp::DecrementClamp,
        xenos::StencilOp::Invert,
        xenos::StencilOp::IncrementWrap,
        xenos::StencilOp::DecrementWrap,
    ];
    d.depth_compare = cmp[k % 8];
    d.depth_write_enabled = k & 8 != 0;
    d.stencil_enabled = k & 16 != 0;
    d.stencil_front = xenos_state::depth_stencil::StencilFaceState {
        compare: cmp[(k + 1) % 8],
        fail_op: stencil[k % 8],
        depth_fail_op: stencil[(k + 1) % 8],
        pass_op: stencil[(k + 2) % 8],
    };
    d.stencil_back = xenos_state::depth_stencil::StencilFaceState {
        compare: cmp[(k + 3) % 8],
        fail_op: stencil[(k + 3) % 8],
        depth_fail_op: stencil[(k + 4) % 8],
        pass_op: stencil[(k + 5) % 8],
    };
    d.stencil_read_mask = k as u8;
    d.stencil_write_mask = (k >> 3) as u8;
    let a = pp::Attachments {
        colors: std::array::from_fn(|i| {
            if k & (1 << (i + 5)) == 0 {
                None
            } else {
                Some(pp::ColorFormat {
                    format: [
                        TextureFormat::RGBA8Unorm,
                        TextureFormat::RGBA16Float,
                        TextureFormat::RGBA32Float,
                        TextureFormat::RGBA32Uint,
                    ][i],
                    blendable: k & (1 << (i + 2)) != 0,
                })
            }
        }),
        depth: if k & 32 == 0 {
            None
        } else {
            Some(pp::DepthFormat {
                format: if k & 64 == 0 { TextureFormat::Depth32Float } else { TextureFormat::Depth24PlusStencil8 },
                stencil: k & 64 != 0,
            })
        },
        sample_count: [1, 2, 4, 8][k % 4],
        sample_mask: [0, u32::MAX, 1, 5][(k >> 2) % 4],
    };
    let draw = pp::Draw {
        vertex_shader: 1,
        pixel_shader: if k & 128 == 0 { None } else { Some(2) },
        texture_layout: (k % 7) as u32,
        topology: [
            HostTopology::PointList,
            HostTopology::LineList,
            HostTopology::LineStrip,
            HostTopology::TriangleList,
            HostTopology::TriangleStrip,
        ][k % 5],
        strip_indices: [pp::IndexWidth::None, pp::IndexWidth::U16, pp::IndexWidth::U32][k % 3],
    };
    (draw, a, r, o, d)
}
#[test]
fn shared_plan_matches_frozen_webgpu_descriptor_for_4096_states() {
    for k in 0..4096 {
        let (draw, a, r, o, d) = fixture(k);
        let scale = [1, 2, 3][k % 3];
        let old = legacy(draw, a, &r, &o, &d, scale);
        let expected = legacy_projection(&old);
        let p = pp::prepare(draw, a, &r, &o, &d, scale);
        let actual = (
            crate::pipeline_webgpu::colors(&p),
            crate::pipeline_webgpu::depth(&p),
            crate::pipeline_webgpu::primitive(&p),
            crate::pipeline_webgpu::multisample(&p),
        );
        assert_eq!(format!("{actual:?}"), format!("{expected:?}"), "case {k}");
        assert_eq!((p.vertex, p.pixel, p.texture_layout), (old.vertex, old.pixel, old.texture_layout));
        if let (Some(a), Some(e)) = (actual.1, expected.1) {
            assert_eq!(a.depth_bias_slope_scale.to_bits(), e.depth_bias_slope_scale.to_bits());
        }
    }
}

#[test]
fn host_depth_capabilities_and_independent_color_masks_match_legacy() {
    let formats = [
        None,
        Some(TextureFormat::Depth16Unorm),
        Some(TextureFormat::Depth24Plus),
        Some(TextureFormat::Depth32Float),
        Some(TextureFormat::Depth24PlusStencil8),
        Some(TextureFormat::Depth32FloatStencil8),
        Some(TextureFormat::Stencil8),
    ];
    for mask in 0..16 {
        for format in formats {
            for enabled in [false, true] {
                for scale in [1, 2, 3] {
                    let (draw, mut a, r, o, mut d) = fixture(4095);
                    a.colors = std::array::from_fn(|i| {
                        if mask & (1 << i) == 0 {
                            None
                        } else {
                            Some(pp::ColorFormat { format: TextureFormat::RGBA8Unorm, blendable: true })
                        }
                    });
                    a.depth = format.map(crate::pipeline_webgpu::depth_format);
                    d.stencil_enabled = enabled;
                    if let Some(a) = a.depth {
                        assert_eq!(a.stencil, has_stencil(a.format));
                    }
                    let old = legacy(draw, a, &r, &o, &d, scale);
                    let expected = legacy_projection(&old);
                    let p = pp::prepare(draw, a, &r, &o, &d, scale);
                    let actual = (
                        crate::pipeline_webgpu::colors(&p),
                        crate::pipeline_webgpu::depth(&p),
                        crate::pipeline_webgpu::primitive(&p),
                        crate::pipeline_webgpu::multisample(&p),
                    );
                    assert_eq!(format!("{actual:?}"), format!("{expected:?}"), "mask={mask} depth={format:?}");
                    if let (Some(a), Some(e)) = (actual.1, expected.1) {
                        assert_eq!(a.depth_bias_slope_scale.to_bits(), e.depth_bias_slope_scale.to_bits());
                    }
                }
            }
        }
    }
}
#[test]
fn cache_equivalence_and_live_shader_identity_are_preserved() {
    let (draw, mut a, r, mut o, d) = fixture(127);
    a.colors = [None; 4];
    a.depth = None;
    let p = pp::prepare(draw, a, &r, &o, &d, 1);
    o.targets = [None; 4];
    let q = pp::prepare(draw, a, &r, &o, &d, 1);
    assert_eq!(p, q);
    for changed in [
        pp::Draw { vertex_shader: 3, ..draw },
        pp::Draw { pixel_shader: Some(77), ..draw },
        pp::Draw { texture_layout: 99, ..draw },
    ] {
        assert_ne!(p, pp::prepare(changed, a, &r, &o, &d, 1));
    }
    // Compare complete old/new cache equivalence, including normalized ignored state.
    let old: Vec<_> = (0..128)
        .map(|k| {
            let (dr, a, r, o, d) = fixture(k);
            legacy(dr, a, &r, &o, &d, 1)
        })
        .collect();
    let new: Vec<_> = (0..128)
        .map(|k| {
            let (dr, a, r, o, d) = fixture(k);
            pp::prepare(dr, a, &r, &o, &d, 1)
        })
        .collect();
    for i in 0..128 {
        for j in 0..128 {
            assert_eq!(old[i] == old[j], new[i] == new[j]);
        }
    }
}
#[test]
fn scalar_formats_need_no_gpu_types_and_coverage_is_not_applied_twice() {
    let (draw, _, mut r, mut o, d) = fixture(127);
    r.polygonal = true;
    r.depth_bias.slope_scale = -0.0;
    o.alpha_to_coverage = Some(xenos_state::blend::AlphaToCoverage { offsets: [0, 1, 2, 3] });
    let a = pp::Attachments {
        colors: [Some(pp::ColorFormat { format: 7u8, blendable: true }), None, None, None],
        depth: Some(pp::DepthFormat { format: 8u8, stencil: false }),
        sample_count: 4,
        sample_mask: 5,
    };
    let p = pp::prepare(draw, a, &r, &o, &d, 1);
    assert!(!p.alpha_to_coverage);
    assert_eq!(p.sample_mask, 5);
    assert_eq!(p.depth.unwrap().slope_scale_bits, (-0.0f32).to_bits());
}
