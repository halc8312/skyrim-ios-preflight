//! Scalar WebGPU projection of the shared CPU pipeline plan. No device calls.
use crate::pipeline_plan::{IndexWidth, Pipeline};
use gpu_webgpu::*;
use xenos_state::{
    blend::{HostBlendFactor, HostBlendOperation},
    primitive::HostTopology,
    viewport::{CullMode as GuestCull, FrontFace as GuestFrontFace},
    xenos,
};
type Key = Pipeline<TextureFormat>;

/// Derive capabilities from the bound host format, including host fallbacks.
/// Guest depth-format metadata must not decide whether a host target has stencil.
pub(crate) fn depth_format(format: TextureFormat) -> crate::pipeline_plan::DepthFormat<TextureFormat> {
    crate::pipeline_plan::DepthFormat {
        format,
        stencil: matches!(
            format,
            TextureFormat::Depth24PlusStencil8 | TextureFormat::Depth32FloatStencil8 | TextureFormat::Stencil8
        ),
    }
}

pub(crate) fn colors(key: &Key) -> Vec<ColorTargetState> {
    key.colors[..key.color_slots]
        .iter()
        .map(|c| match c {
            Some(c) => ColorTargetState {
                format: c.format,
                blend: c.blend.map(|[c, a]| BlendState {
                    color: BlendComponent {
                        operation: blend_operation(c.operation),
                        src_factor: blend_factor(c.src_factor),
                        dst_factor: blend_factor(c.dst_factor),
                    },
                    alpha: BlendComponent {
                        operation: blend_operation(a.operation),
                        src_factor: blend_factor(a.src_factor),
                        dst_factor: blend_factor(a.dst_factor),
                    },
                }),
                write_mask: ColorWriteMask(u64::from(c.write_mask)),
            },
            None => {
                ColorTargetState { format: TextureFormat::Undefined, blend: None, write_mask: ColorWriteMask::None }
            }
        })
        .collect()
}
pub(crate) fn depth(key: &Key) -> Option<DepthStencilState> {
    let face = |f: xenos_state::depth_stencil::StencilFaceState| StencilFaceState {
        compare: compare(f.compare),
        fail_op: stencil_op(f.fail_op),
        depth_fail_op: stencil_op(f.depth_fail_op),
        pass_op: stencil_op(f.pass_op),
    };
    key.depth.as_ref().map(|d| DepthStencilState {
        format: d.format,
        depth_write_enabled: d.write,
        depth_compare: compare(d.compare),
        stencil_front: face(d.stencil[0]),
        stencil_back: face(d.stencil[1]),
        stencil_read_mask: d.read_mask,
        stencil_write_mask: d.write_mask,
        depth_bias: d.bias,
        depth_bias_slope_scale: f32::from_bits(d.slope_scale_bits),
        depth_bias_clamp: 0.0,
    })
}
pub(crate) fn primitive(key: &Key) -> PrimitiveState {
    PrimitiveState {
        topology: topology(key.topology),
        strip_index_format: match key.strip_index_format {
            IndexWidth::None => IndexFormat::Undefined,
            IndexWidth::U16 => IndexFormat::Uint16,
            IndexWidth::U32 => IndexFormat::Uint32,
        },
        front_face: match key.front_face {
            GuestFrontFace::Ccw => FrontFace::CCW,
            GuestFrontFace::Cw => FrontFace::CW,
        },
        cull_mode: match key.cull {
            GuestCull::None => CullMode::None,
            GuestCull::Front => CullMode::Front,
            GuestCull::Back => CullMode::Back,
        },
        unclipped_depth: false,
    }
}
pub(crate) fn multisample(key: &Key) -> MultisampleState {
    MultisampleState {
        count: key.sample_count,
        mask: key.sample_mask,
        alpha_to_coverage_enabled: key.alpha_to_coverage,
    }
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
