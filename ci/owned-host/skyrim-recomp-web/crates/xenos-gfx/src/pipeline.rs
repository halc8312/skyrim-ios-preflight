//! CPU-owned meaning of the legacy executor's normalized pipeline state.
//!
//! Formats are caller-owned scalar identities, not GPU objects. Backend
//! adapters provide blend/stencil capabilities and translate enums at their
//! boundary. This preserves existing rendering approximations; it neither
//! validates backend limits nor implements EDRAM ownership or MSAA resolves.

use xenos_state::{
    blend::{BlendComponent, OutputMergerState},
    depth_stencil::{DepthStencilState, StencilFaceState},
    primitive::HostTopology,
    viewport::{CullMode, FrontFace, RasterizerState},
    xenos::{CompareFunction, StencilOp},
};

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum IndexWidth {
    None,
    U16,
    U32,
}

#[derive(Clone, Copy, Debug)]
pub struct ColorFormat<F> {
    pub format: F,
    pub blendable: bool,
}
#[derive(Clone, Copy, Debug)]
pub struct DepthFormat<F> {
    pub format: F,
    /// Capability of the bound backend format, after any format fallback.
    /// Backend adapters, not guest state, supply this value.
    pub stencil: bool,
}
#[derive(Clone, Copy, Debug)]
pub struct Attachments<F> {
    pub colors: [Option<ColorFormat<F>>; 4],
    pub depth: Option<DepthFormat<F>>,
    pub sample_count: u32,
    pub sample_mask: u32,
}

#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct Color<F> {
    pub format: F,
    pub blend: Option<[BlendComponent; 2]>,
    pub write_mask: u32,
}
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct Depth<F> {
    pub format: F,
    pub write: bool,
    pub compare: CompareFunction,
    pub stencil: [StencilFaceState; 2],
    pub read_mask: u32,
    pub write_mask: u32,
    pub bias: i32,
    pub slope_scale_bits: u32,
}

#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct Pipeline<F> {
    pub vertex: u64,
    pub pixel: Option<u64>,
    pub texture_layout: u32,
    pub topology: HostTopology,
    pub strip_index_format: IndexWidth,
    pub cull: CullMode,
    pub front_face: FrontFace,
    pub colors: [Option<Color<F>>; 4],
    pub depth: Option<Depth<F>>,
    pub color_slots: usize,
    pub sample_count: u32,
    pub sample_mask: u32,
    pub alpha_to_coverage: bool,
}

#[derive(Clone, Copy, Debug)]
pub struct Draw {
    pub vertex_shader: u64,
    pub pixel_shader: Option<u64>,
    pub texture_layout: u32,
    pub topology: HostTopology,
    pub strip_indices: IndexWidth,
}

pub fn prepare<F: Copy>(
    draw: Draw,
    attachments: Attachments<F>,
    rasterizer: &RasterizerState,
    output: &OutputMergerState,
    depth_stencil: &DepthStencilState,
    resolution_scale: u32,
) -> Pipeline<F> {
    let colors = std::array::from_fn(|i| {
        attachments.colors[i].map(|a| {
            let state = output.targets[i];
            Color {
                format: a.format,
                blend: state.and_then(|s| s.blend).filter(|_| a.blendable).map(|(color, alpha)| [color, alpha]),
                write_mask: state.map_or(0, |s| s.write_mask),
            }
        })
    });
    let depth = attachments.depth.map(|a| {
        let stencil = a.stencil && depth_stencil.stencil_enabled;
        let disabled = StencilFaceState {
            compare: CompareFunction::Always,
            fail_op: StencilOp::Keep,
            depth_fail_op: StencilOp::Keep,
            pass_op: StencilOp::Keep,
        };
        let (bias, slope) = if rasterizer.polygonal {
            (rasterizer.depth_bias.constant, rasterizer.depth_bias.slope_scale * resolution_scale as f32)
        } else {
            (0, 0.0)
        };
        Depth {
            format: a.format,
            write: depth_stencil.depth_write_enabled,
            compare: depth_stencil.depth_compare,
            stencil: if stencil { [depth_stencil.stencil_front, depth_stencil.stencil_back] } else { [disabled; 2] },
            read_mask: if stencil { u32::from(depth_stencil.stencil_read_mask) } else { 0xff },
            write_mask: if stencil { u32::from(depth_stencil.stencil_write_mask) } else { 0 },
            bias,
            slope_scale_bits: slope.to_bits(),
        }
    });
    Pipeline {
        vertex: draw.vertex_shader,
        pixel: draw.pixel_shader,
        texture_layout: draw.texture_layout,
        topology: draw.topology,
        strip_index_format: draw.strip_indices,
        cull: rasterizer.cull_mode,
        front_face: rasterizer.front_face,
        colors,
        depth,
        color_slots: attachments.colors.iter().rposition(Option::is_some).map_or(0, |i| i + 1),
        sample_count: attachments.sample_count,
        sample_mask: attachments.sample_mask,
        // Guest coverage is emitted before exponent bias by the translated shader.
        alpha_to_coverage: false,
    }
}


