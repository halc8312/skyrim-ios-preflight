//! Source-compatible descriptors of the existing renderer, copied from
//! gpu-webgpu (MIT OR Apache-2.0). No C handles or C calls are implemented here.
use crate::*;

#[derive(Clone, Copy, Debug)]
pub struct TextureDescriptor<'a> {
    pub label: &'a str,
    pub size: Extent3d,
    pub mip_level_count: u32,
    pub sample_count: u32,
    pub dimension: TextureDimension,
    pub format: TextureFormat,
    pub usage: TextureUsage,
    pub view_formats: &'a [TextureFormat],
}

impl Default for TextureDescriptor<'_> {
    fn default() -> Self {
        Self {
            label: "",
            size: crate::extent(1, 1, 1),
            mip_level_count: 1,
            sample_count: 1,
            dimension: TextureDimension::D2,
            format: TextureFormat::Undefined,
            usage: TextureUsage::None,
            view_formats: &[],
        }
    }
}

#[derive(Clone, Copy, Debug, Default)]
pub struct TextureViewDescriptor<'a> {
    pub label: &'a str,
    pub format: TextureFormat,
    pub dimension: TextureViewDimension,
    pub base_mip_level: u32,
    pub mip_level_count: Option<u32>,
    pub base_array_layer: u32,
    pub array_layer_count: Option<u32>,
    pub aspect: TextureAspect,
    pub usage: TextureUsage,
}

#[derive(Clone, Copy, Debug)]
pub struct SamplerDescriptor<'a> {
    pub label: &'a str,
    pub address_mode_u: AddressMode,
    pub address_mode_v: AddressMode,
    pub address_mode_w: AddressMode,
    pub mag_filter: FilterMode,
    pub min_filter: FilterMode,
    pub mipmap_filter: MipmapFilterMode,
    pub lod_min_clamp: f32,
    pub lod_max_clamp: f32,
    pub compare: Option<CompareFunction>,
    pub max_anisotropy: u16,
}

impl Default for SamplerDescriptor<'_> {
    fn default() -> Self {
        Self {
            label: "",
            address_mode_u: AddressMode::ClampToEdge,
            address_mode_v: AddressMode::ClampToEdge,
            address_mode_w: AddressMode::ClampToEdge,
            mag_filter: FilterMode::Nearest,
            min_filter: FilterMode::Nearest,
            mipmap_filter: MipmapFilterMode::Nearest,
            lod_min_clamp: 0.0,
            lod_max_clamp: 32.0,
            compare: None,
            max_anisotropy: 1,
        }
    }
}

#[derive(Clone, Copy, Debug)]
pub enum BindingType {
    Buffer {
        ty: BufferBindingType,
        has_dynamic_offset: bool,
        min_binding_size: u64,
    },
    Sampler(SamplerBindingType),
    Texture {
        sample_type: TextureSampleType,
        view_dimension: TextureViewDimension,
        multisampled: bool,
    },
    StorageTexture {
        access: StorageTextureAccess,
        format: TextureFormat,
        view_dimension: TextureViewDimension,
    },
}

#[derive(Clone, Copy, Debug)]
pub struct BindGroupLayoutEntry {
    pub binding: u32,
    pub visibility: ShaderStage,
    pub ty: BindingType,
}

#[derive(Clone, Copy, Debug)]
pub struct VertexAttribute {
    pub format: VertexFormat,
    pub offset: u64,
    pub shader_location: u32,
}

#[derive(Clone, Copy, Debug)]
pub struct VertexBufferLayout<'a> {
    pub array_stride: u64,
    pub step_mode: VertexStepMode,
    pub attributes: &'a [VertexAttribute],
}

#[derive(Clone, Copy, Debug)]
pub struct ProgrammableStage<'a> {
    pub module: &'a ShaderModule,
    pub entry_point: &'a str,
    /// Pipeline-overridable constants (`override` declarations).
    pub constants: &'a [(&'a str, f64)],
}

#[derive(Clone, Copy, Debug)]
pub struct PrimitiveState {
    pub topology: PrimitiveTopology,
    /// Required for strip topologies with indexed draws, `Undefined` otherwise.
    pub strip_index_format: IndexFormat,
    pub front_face: FrontFace,
    pub cull_mode: CullMode,
    pub unclipped_depth: bool,
}

impl Default for PrimitiveState {
    fn default() -> Self {
        Self {
            topology: PrimitiveTopology::TriangleList,
            strip_index_format: IndexFormat::Undefined,
            front_face: FrontFace::CCW,
            cull_mode: CullMode::None,
            unclipped_depth: false,
        }
    }
}

#[derive(Clone, Copy, Debug)]
pub struct StencilFaceState {
    pub compare: CompareFunction,
    pub fail_op: StencilOperation,
    pub depth_fail_op: StencilOperation,
    pub pass_op: StencilOperation,
}

impl Default for StencilFaceState {
    fn default() -> Self {
        Self {
            compare: CompareFunction::Always,
            fail_op: StencilOperation::Keep,
            depth_fail_op: StencilOperation::Keep,
            pass_op: StencilOperation::Keep,
        }
    }
}

#[derive(Clone, Copy, Debug)]
pub struct DepthStencilState {
    pub format: TextureFormat,
    pub depth_write_enabled: bool,
    pub depth_compare: CompareFunction,
    pub stencil_front: StencilFaceState,
    pub stencil_back: StencilFaceState,
    pub stencil_read_mask: u32,
    pub stencil_write_mask: u32,
    pub depth_bias: i32,
    pub depth_bias_slope_scale: f32,
    pub depth_bias_clamp: f32,
}

#[derive(Clone, Copy, Debug)]
pub struct MultisampleState {
    pub count: u32,
    pub mask: u32,
    pub alpha_to_coverage_enabled: bool,
}

impl Default for MultisampleState {
    fn default() -> Self {
        Self {
            count: 1,
            mask: !0,
            alpha_to_coverage_enabled: false,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct BlendComponent {
    pub operation: BlendOperation,
    pub src_factor: BlendFactor,
    pub dst_factor: BlendFactor,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct BlendState {
    pub color: BlendComponent,
    pub alpha: BlendComponent,
}

#[derive(Clone, Copy, Debug)]
pub struct ColorTargetState {
    pub format: TextureFormat,
    pub blend: Option<BlendState>,
    pub write_mask: ColorWriteMask,
}

#[derive(Clone, Copy, Debug)]
pub struct FragmentState<'a> {
    pub stage: ProgrammableStage<'a>,
    pub targets: &'a [ColorTargetState],
}

#[derive(Clone, Copy, Debug)]
pub struct RenderPipelineDescriptor<'a> {
    pub label: &'a str,
    /// `None` derives the layout from the shaders (`get_bind_group_layout`).
    pub layout: Option<&'a PipelineLayout>,
    pub vertex: ProgrammableStage<'a>,
    pub vertex_buffers: &'a [VertexBufferLayout<'a>],
    pub primitive: PrimitiveState,
    pub depth_stencil: Option<DepthStencilState>,
    pub multisample: MultisampleState,
    pub fragment: Option<FragmentState<'a>>,
}

#[derive(Clone, Copy, Debug)]
pub struct ComputePipelineDescriptor<'a> {
    pub label: &'a str,
    pub layout: Option<&'a PipelineLayout>,
    pub stage: ProgrammableStage<'a>,
}

impl DepthStencilState {
    /// Depth test/write with `compare`, stencil disabled.
    pub fn depth(format: TextureFormat, write: bool, compare: CompareFunction) -> Self {
        Self {
            format,
            depth_write_enabled: write,
            depth_compare: compare,
            stencil_front: StencilFaceState::default(),
            stencil_back: StencilFaceState::default(),
            stencil_read_mask: !0,
            stencil_write_mask: !0,
            depth_bias: 0,
            depth_bias_slope_scale: 0.0,
            depth_bias_clamp: 0.0,
        }
    }
}

impl BlendState {
    pub const ALPHA_BLENDING: Self = Self {
        color: BlendComponent {
            operation: BlendOperation::Add,
            src_factor: BlendFactor::SrcAlpha,
            dst_factor: BlendFactor::OneMinusSrcAlpha,
        },
        alpha: BlendComponent::OVER,
    };
}

impl ColorTargetState {
    pub fn new(format: TextureFormat) -> Self {
        Self {
            format,
            blend: None,
            write_mask: ColorWriteMask::All,
        }
    }
}

#[derive(Clone, Copy, Debug, Default)]
pub struct BufferDescriptor<'a> {
    pub label: &'a str,
    pub size: u64,
    pub usage: BufferUsage,
    pub mapped_at_creation: bool,
}

#[derive(Clone, Debug, Default)]
pub struct DeviceDescriptor<'a> {
    pub label: &'a str,
    pub required_features: &'a [FeatureName],
    /// Optional minimum requirements, checked against the selected adapter.
    /// The native bridge exposes that adapter's limits even when this is `None`.
    pub required_limits: Option<Limits>,
}

#[derive(Clone, Copy, Debug)]
pub struct BindGroupEntry<'a> {
    pub binding: u32,
    pub resource: BindingResource<'a>,
}

#[derive(Clone, Copy, Debug)]
pub enum BindingResource<'a> {
    /// `size: None` binds the rest of the buffer.
    Buffer {
        buffer: &'a Buffer,
        offset: u64,
        size: Option<u64>,
    },
    Sampler(&'a Sampler),
    TextureView(&'a TextureView),
}

#[derive(Clone, Copy, Debug)]
pub struct TexelCopyBufferInfo<'a> {
    pub buffer: &'a Buffer,
    pub offset: u64,
    pub bytes_per_row: u32,
    /// Needed only for multi-layer/3D copies.
    pub rows_per_image: Option<u32>,
}

#[derive(Clone, Copy, Debug)]
pub struct TexelCopyTextureInfo<'a> {
    pub texture: &'a Texture,
    pub mip_level: u32,
    pub origin: Origin3d,
    pub aspect: TextureAspect,
}

#[derive(Clone, Copy, Debug)]
pub struct RenderPassColorAttachment<'a> {
    pub view: &'a TextureView,
    pub resolve_target: Option<&'a TextureView>,
    /// Slice of a 3D texture to render to.
    pub depth_slice: Option<u32>,
    pub load_op: LoadOp,
    pub store_op: StoreOp,
    pub clear_value: Color,
}

#[derive(Clone, Copy, Debug)]
pub struct RenderPassDepthStencilAttachment<'a> {
    pub view: &'a TextureView,
    pub depth_load_op: LoadOp,
    pub depth_store_op: StoreOp,
    pub depth_clear_value: f32,
    pub depth_read_only: bool,
    pub stencil_load_op: LoadOp,
    pub stencil_store_op: StoreOp,
    pub stencil_clear_value: u32,
    pub stencil_read_only: bool,
}

impl<'a> ProgrammableStage<'a> {
    pub fn new(module: &'a ShaderModule, entry_point: &'a str) -> Self {
        Self {
            module,
            entry_point,
            constants: &[],
        }
    }
}
impl BlendComponent {
    pub const REPLACE: Self = Self {
        operation: BlendOperation::Add,
        src_factor: BlendFactor::One,
        dst_factor: BlendFactor::Zero,
    };
    pub const OVER: Self = Self {
        operation: BlendOperation::Add,
        src_factor: BlendFactor::One,
        dst_factor: BlendFactor::OneMinusSrcAlpha,
    };
}
impl<'a> TexelCopyTextureInfo<'a> {
    pub fn whole(texture: &'a Texture) -> Self {
        Self {
            texture,
            mip_level: 0,
            origin: origin(0, 0, 0),
            aspect: TextureAspect::All,
        }
    }
}
impl<'a> RenderPassColorAttachment<'a> {
    pub fn clear(view: &'a TextureView, color: Color) -> Self {
        Self {
            view,
            resolve_target: None,
            depth_slice: None,
            load_op: LoadOp::Clear,
            store_op: StoreOp::Store,
            clear_value: color,
        }
    }
    pub fn load(view: &'a TextureView) -> Self {
        Self {
            load_op: LoadOp::Load,
            ..Self::clear(view, Color::default())
        }
    }
}
impl<'a> RenderPassDepthStencilAttachment<'a> {
    pub fn depth_clear(view: &'a TextureView, depth: f32) -> Self {
        Self {
            view,
            depth_load_op: LoadOp::Clear,
            depth_store_op: StoreOp::Store,
            depth_clear_value: depth,
            depth_read_only: false,
            stencil_load_op: LoadOp::Undefined,
            stencil_store_op: StoreOp::Undefined,
            stencil_clear_value: 0,
            stencil_read_only: true,
        }
    }
}
