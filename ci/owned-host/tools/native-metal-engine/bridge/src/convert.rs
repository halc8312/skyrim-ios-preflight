use crate::*;

pub fn err(e: native::Error) -> Error {
    Error::Invalid(format!("native GPU: {e:?}"))
}
// Explicit matches avoid depending on the ABI's numeric enum representation.
macro_rules! same {
    ($fn:ident, $from:ident, $to:ident, $($a:ident),+ $(,)?) => {
        pub fn $fn(v: $from) -> Result<types::$to> { match v { $($from::$a => Ok(types::$to::$a),)+ _ => Err(Error::Invalid(format!("unsupported {} value {}",stringify!($from),v.0))) } }
    }
}
same!(
    compare,
    CompareFunction,
    CompareFunction,
    Never,
    Less,
    Equal,
    LessEqual,
    Greater,
    NotEqual,
    GreaterEqual,
    Always
);
same!(
    stencil_op,
    StencilOperation,
    StencilOperation,
    Keep,
    Zero,
    Replace,
    Invert,
    IncrementClamp,
    DecrementClamp,
    IncrementWrap,
    DecrementWrap
);
same!(
    blend_op,
    BlendOperation,
    BlendOperation,
    Add,
    Subtract,
    ReverseSubtract,
    Min,
    Max
);
same!(
    blend_factor,
    BlendFactor,
    BlendFactor,
    Zero,
    One,
    Src,
    OneMinusSrc,
    SrcAlpha,
    OneMinusSrcAlpha,
    Dst,
    OneMinusDst,
    DstAlpha,
    OneMinusDstAlpha,
    SrcAlphaSaturated,
    Constant,
    OneMinusConstant
);
same!(
    topology,
    PrimitiveTopology,
    PrimitiveTopology,
    PointList,
    LineList,
    LineStrip,
    TriangleList,
    TriangleStrip
);
same!(index, IndexFormat, IndexFormat, Uint16, Uint32);
same!(dimension, TextureDimension, TextureDimension, D1, D2, D3);
same!(
    view_dimension,
    TextureViewDimension,
    TextureViewDimension,
    D1,
    D2,
    D2Array,
    Cube,
    CubeArray,
    D3
);
same!(
    aspect,
    TextureAspect,
    TextureAspect,
    All,
    StencilOnly,
    DepthOnly
);
same!(
    address,
    AddressMode,
    AddressMode,
    ClampToEdge,
    Repeat,
    MirrorRepeat
);
same!(filter, FilterMode, FilterMode, Nearest, Linear);
same!(
    mip_filter,
    MipmapFilterMode,
    MipmapFilterMode,
    Nearest,
    Linear
);
pub fn size(v: Extent3d) -> types::Extent3d {
    types::Extent3d {
        width: v.width,
        height: v.height,
        depth_or_array_layers: v.depthOrArrayLayers,
    }
}
pub fn origin(v: Origin3d) -> types::Origin3d {
    types::Origin3d {
        x: v.x,
        y: v.y,
        z: v.z,
    }
}
pub fn color(v: Color) -> types::Color {
    types::Color {
        r: v.r,
        g: v.g,
        b: v.b,
        a: v.a,
    }
}
pub fn primitive(v: PrimitiveState) -> Result<types::PrimitiveState> {
    Ok(types::PrimitiveState {
        topology: topology(v.topology)?,
        strip_index_format: if v.strip_index_format == IndexFormat::Undefined {
            None
        } else {
            Some(index(v.strip_index_format)?)
        },
        front_face: match v.front_face {
            FrontFace::CCW => types::FrontFace::Ccw,
            FrontFace::CW => types::FrontFace::Cw,
            _ => return Err(Error::Invalid("front face".into())),
        },
        cull_mode: match v.cull_mode {
            CullMode::None => None,
            CullMode::Front => Some(types::Face::Front),
            CullMode::Back => Some(types::Face::Back),
            _ => return Err(Error::Invalid("cull mode".into())),
        },
        unclipped_depth: v.unclipped_depth,
        ..Default::default()
    })
}
fn stencil(v: StencilFaceState) -> Result<types::StencilFaceState> {
    Ok(types::StencilFaceState {
        compare: compare(v.compare)?,
        fail_op: stencil_op(v.fail_op)?,
        depth_fail_op: stencil_op(v.depth_fail_op)?,
        pass_op: stencil_op(v.pass_op)?,
    })
}
pub fn depth(v: DepthStencilState) -> Result<types::DepthStencilState> {
    Ok(types::DepthStencilState {
        format: format(v.format)?,
        depth_write_enabled: Some(v.depth_write_enabled),
        depth_compare: Some(compare(v.depth_compare)?),
        stencil: types::StencilState {
            front: stencil(v.stencil_front)?,
            back: stencil(v.stencil_back)?,
            read_mask: v.stencil_read_mask,
            write_mask: v.stencil_write_mask,
        },
        bias: types::DepthBiasState {
            constant: v.depth_bias,
            slope_scale: v.depth_bias_slope_scale,
            clamp: v.depth_bias_clamp,
        },
    })
}
fn blend(v: BlendComponent) -> Result<types::BlendComponent> {
    Ok(types::BlendComponent {
        operation: blend_op(v.operation)?,
        src_factor: blend_factor(v.src_factor)?,
        dst_factor: blend_factor(v.dst_factor)?,
    })
}
pub fn target(v: ColorTargetState) -> Result<Option<types::ColorTargetState>> {
    if v.format == TextureFormat::Undefined {
        return Ok(None);
    }
    if v.write_mask.0 > u32::MAX as u64 {
        return Err(Error::Invalid("color write mask bits".into()));
    }
    Ok(Some(types::ColorTargetState {
        format: format(v.format)?,
        blend: v
            .blend
            .map(|v| {
                Ok::<_, Error>(types::BlendState {
                    color: blend(v.color)?,
                    alpha: blend(v.alpha)?,
                })
            })
            .transpose()?,
        write_mask: types::ColorWrites::from_bits(v.write_mask.0 as u32)
            .ok_or_else(|| Error::Invalid("color write mask".into()))?,
    }))
}
pub fn binding(v: BindGroupLayoutEntry) -> Result<types::BindGroupLayoutEntry> {
    if v.visibility.0 > u32::MAX as u64 {
        return Err(Error::Invalid("shader visibility bits".into()));
    }
    let ty = match v.ty {
        BindingType::Buffer {
            ty,
            has_dynamic_offset,
            min_binding_size,
        } => types::BindingType::Buffer {
            ty: match ty {
                BufferBindingType::Uniform => types::BufferBindingType::Uniform,
                BufferBindingType::Storage => {
                    types::BufferBindingType::Storage { read_only: false }
                }
                BufferBindingType::ReadOnlyStorage => {
                    types::BufferBindingType::Storage { read_only: true }
                }
                _ => return Err(Error::Invalid("buffer binding type".into())),
            },
            has_dynamic_offset,
            min_binding_size: std::num::NonZeroU64::new(min_binding_size),
        },
        BindingType::Sampler(v) => types::BindingType::Sampler(match v {
            SamplerBindingType::Filtering => types::SamplerBindingType::Filtering,
            SamplerBindingType::NonFiltering => types::SamplerBindingType::NonFiltering,
            SamplerBindingType::Comparison => types::SamplerBindingType::Comparison,
            _ => return Err(Error::Invalid("sampler type".into())),
        }),
        BindingType::Texture {
            sample_type,
            view_dimension: v,
            multisampled,
        } => types::BindingType::Texture {
            sample_type: match sample_type {
                TextureSampleType::Float => types::TextureSampleType::Float { filterable: true },
                TextureSampleType::UnfilterableFloat => {
                    types::TextureSampleType::Float { filterable: false }
                }
                TextureSampleType::Depth => types::TextureSampleType::Depth,
                TextureSampleType::Sint => types::TextureSampleType::Sint,
                TextureSampleType::Uint => types::TextureSampleType::Uint,
                _ => return Err(Error::Invalid("texture sample type".into())),
            },
            view_dimension: view_dimension(v)?,
            multisampled,
        },
        BindingType::StorageTexture { .. } => return Err(Error::Unsupported("storage textures")),
    };
    Ok(types::BindGroupLayoutEntry {
        binding: v.binding,
        visibility: types::ShaderStages::from_bits(v.visibility.0 as u32)
            .ok_or_else(|| Error::Invalid("shader visibility".into()))?,
        ty,
        count: None,
    })
}
pub fn texture_usage(
    usage: TextureUsage,
    format: types::TextureFormat,
) -> Result<types::TextureUses> {
    let known = TextureUsage::CopySrc
        | TextureUsage::CopyDst
        | TextureUsage::TextureBinding
        | TextureUsage::RenderAttachment;
    if usage.0 & !known.0 != 0 {
        return Err(Error::Unsupported("texture usage"));
    }
    let mut out = types::TextureUses::empty();
    if usage.contains(TextureUsage::CopySrc) {
        out |= types::TextureUses::COPY_SRC;
    }
    if usage.contains(TextureUsage::CopyDst) {
        out |= types::TextureUses::COPY_DST;
    }
    if usage.contains(TextureUsage::TextureBinding) {
        out |= types::TextureUses::RESOURCE;
    }
    if usage.contains(TextureUsage::RenderAttachment) {
        out |= if format.is_depth_stencil_format() {
            types::TextureUses::DEPTH_STENCIL_READ | types::TextureUses::DEPTH_STENCIL_WRITE
        } else {
            types::TextureUses::COLOR_TARGET
        };
    }
    Ok(out)
}

pub fn format(v: TextureFormat) -> Result<types::TextureFormat> {
    match v {
        TextureFormat::R8Unorm => Ok(types::TextureFormat::R8Unorm),
        TextureFormat::R8Snorm => Ok(types::TextureFormat::R8Snorm),
        TextureFormat::R8Uint => Ok(types::TextureFormat::R8Uint),
        TextureFormat::R8Sint => Ok(types::TextureFormat::R8Sint),
        TextureFormat::R16Uint => Ok(types::TextureFormat::R16Uint),
        TextureFormat::R16Sint => Ok(types::TextureFormat::R16Sint),
        TextureFormat::R16Float => Ok(types::TextureFormat::R16Float),
        TextureFormat::R16Unorm => Ok(types::TextureFormat::R16Unorm),
        TextureFormat::R16Snorm => Ok(types::TextureFormat::R16Snorm),
        TextureFormat::RG8Unorm => Ok(types::TextureFormat::Rg8Unorm),
        TextureFormat::RG8Snorm => Ok(types::TextureFormat::Rg8Snorm),
        TextureFormat::RG8Uint => Ok(types::TextureFormat::Rg8Uint),
        TextureFormat::RG8Sint => Ok(types::TextureFormat::Rg8Sint),
        TextureFormat::R32Float => Ok(types::TextureFormat::R32Float),
        TextureFormat::R32Uint => Ok(types::TextureFormat::R32Uint),
        TextureFormat::R32Sint => Ok(types::TextureFormat::R32Sint),
        TextureFormat::RG16Uint => Ok(types::TextureFormat::Rg16Uint),
        TextureFormat::RG16Sint => Ok(types::TextureFormat::Rg16Sint),
        TextureFormat::RG16Float => Ok(types::TextureFormat::Rg16Float),
        TextureFormat::RG16Unorm => Ok(types::TextureFormat::Rg16Unorm),
        TextureFormat::RG16Snorm => Ok(types::TextureFormat::Rg16Snorm),
        TextureFormat::RGBA8Unorm => Ok(types::TextureFormat::Rgba8Unorm),
        TextureFormat::RGBA8UnormSrgb => Ok(types::TextureFormat::Rgba8UnormSrgb),
        TextureFormat::RGBA8Snorm => Ok(types::TextureFormat::Rgba8Snorm),
        TextureFormat::RGBA8Uint => Ok(types::TextureFormat::Rgba8Uint),
        TextureFormat::RGBA8Sint => Ok(types::TextureFormat::Rgba8Sint),
        TextureFormat::BGRA8Unorm => Ok(types::TextureFormat::Bgra8Unorm),
        TextureFormat::BGRA8UnormSrgb => Ok(types::TextureFormat::Bgra8UnormSrgb),
        TextureFormat::RGB10A2Uint => Ok(types::TextureFormat::Rgb10a2Uint),
        TextureFormat::RGB10A2Unorm => Ok(types::TextureFormat::Rgb10a2Unorm),
        TextureFormat::RG11B10Ufloat => Ok(types::TextureFormat::Rg11b10Ufloat),
        TextureFormat::RGB9E5Ufloat => Ok(types::TextureFormat::Rgb9e5Ufloat),
        TextureFormat::RG32Float => Ok(types::TextureFormat::Rg32Float),
        TextureFormat::RG32Uint => Ok(types::TextureFormat::Rg32Uint),
        TextureFormat::RG32Sint => Ok(types::TextureFormat::Rg32Sint),
        TextureFormat::RGBA16Uint => Ok(types::TextureFormat::Rgba16Uint),
        TextureFormat::RGBA16Sint => Ok(types::TextureFormat::Rgba16Sint),
        TextureFormat::RGBA16Float => Ok(types::TextureFormat::Rgba16Float),
        TextureFormat::RGBA16Unorm => Ok(types::TextureFormat::Rgba16Unorm),
        TextureFormat::RGBA16Snorm => Ok(types::TextureFormat::Rgba16Snorm),
        TextureFormat::RGBA32Float => Ok(types::TextureFormat::Rgba32Float),
        TextureFormat::RGBA32Uint => Ok(types::TextureFormat::Rgba32Uint),
        TextureFormat::RGBA32Sint => Ok(types::TextureFormat::Rgba32Sint),
        TextureFormat::Stencil8 => Ok(types::TextureFormat::Stencil8),
        TextureFormat::Depth16Unorm => Ok(types::TextureFormat::Depth16Unorm),
        TextureFormat::Depth24Plus => Ok(types::TextureFormat::Depth24Plus),
        TextureFormat::Depth24PlusStencil8 => Ok(types::TextureFormat::Depth24PlusStencil8),
        TextureFormat::Depth32Float => Ok(types::TextureFormat::Depth32Float),
        TextureFormat::Depth32FloatStencil8 => Ok(types::TextureFormat::Depth32FloatStencil8),
        TextureFormat::BC1RGBAUnorm => Ok(types::TextureFormat::Bc1RgbaUnorm),
        TextureFormat::BC1RGBAUnormSrgb => Ok(types::TextureFormat::Bc1RgbaUnormSrgb),
        TextureFormat::BC2RGBAUnorm => Ok(types::TextureFormat::Bc2RgbaUnorm),
        TextureFormat::BC2RGBAUnormSrgb => Ok(types::TextureFormat::Bc2RgbaUnormSrgb),
        TextureFormat::BC3RGBAUnorm => Ok(types::TextureFormat::Bc3RgbaUnorm),
        TextureFormat::BC3RGBAUnormSrgb => Ok(types::TextureFormat::Bc3RgbaUnormSrgb),
        TextureFormat::BC4RUnorm => Ok(types::TextureFormat::Bc4RUnorm),
        TextureFormat::BC4RSnorm => Ok(types::TextureFormat::Bc4RSnorm),
        TextureFormat::BC5RGUnorm => Ok(types::TextureFormat::Bc5RgUnorm),
        TextureFormat::BC5RGSnorm => Ok(types::TextureFormat::Bc5RgSnorm),
        TextureFormat::BC6HRGBUfloat => Ok(types::TextureFormat::Bc6hRgbUfloat),
        TextureFormat::BC6HRGBFloat => Ok(types::TextureFormat::Bc6hRgbFloat),
        TextureFormat::BC7RGBAUnorm => Ok(types::TextureFormat::Bc7RgbaUnorm),
        TextureFormat::BC7RGBAUnormSrgb => Ok(types::TextureFormat::Bc7RgbaUnormSrgb),
        _ => Err(Error::Invalid(format!("texture format {}", v.0))),
    }
}

pub fn limits(l: &types::Limits) -> Limits {
    Limits {
        nextInChain: std::ptr::null_mut(),
        maxTextureDimension1D: (l.max_texture_dimension_1d) as u32,
        maxTextureDimension2D: (l.max_texture_dimension_2d) as u32,
        maxTextureDimension3D: (l.max_texture_dimension_3d) as u32,
        maxTextureArrayLayers: (l.max_texture_array_layers) as u32,
        maxBindGroups: (l.max_bind_groups) as u32,
        maxBindGroupsPlusVertexBuffers: (l.max_bind_groups + l.max_vertex_buffers) as u32,
        maxBindingsPerBindGroup: (l.max_bindings_per_bind_group) as u32,
        maxDynamicUniformBuffersPerPipelineLayout: (l
            .max_dynamic_uniform_buffers_per_pipeline_layout)
            as u32,
        maxDynamicStorageBuffersPerPipelineLayout: (l
            .max_dynamic_storage_buffers_per_pipeline_layout)
            as u32,
        maxSampledTexturesPerShaderStage: (l.max_sampled_textures_per_shader_stage) as u32,
        maxSamplersPerShaderStage: (l.max_samplers_per_shader_stage) as u32,
        maxStorageBuffersPerShaderStage: (l.max_storage_buffers_per_shader_stage) as u32,
        maxStorageTexturesPerShaderStage: (l.max_storage_textures_per_shader_stage) as u32,
        maxUniformBuffersPerShaderStage: (l.max_uniform_buffers_per_shader_stage) as u32,
        maxUniformBufferBindingSize: (l.max_uniform_buffer_binding_size) as u64,
        maxStorageBufferBindingSize: (l.max_storage_buffer_binding_size) as u64,
        minUniformBufferOffsetAlignment: (l.min_uniform_buffer_offset_alignment) as u32,
        minStorageBufferOffsetAlignment: (l.min_storage_buffer_offset_alignment) as u32,
        maxVertexBuffers: (l.max_vertex_buffers) as u32,
        maxBufferSize: (l.max_buffer_size) as u64,
        maxVertexAttributes: (l.max_vertex_attributes) as u32,
        maxVertexBufferArrayStride: (l.max_vertex_buffer_array_stride) as u32,
        maxInterStageShaderVariables: (l.max_inter_stage_shader_variables) as u32,
        maxColorAttachments: (l.max_color_attachments) as u32,
        maxColorAttachmentBytesPerSample: (l.max_color_attachment_bytes_per_sample) as u32,
        maxComputeWorkgroupStorageSize: (l.max_compute_workgroup_storage_size) as u32,
        maxComputeInvocationsPerWorkgroup: (l.max_compute_invocations_per_workgroup) as u32,
        maxComputeWorkgroupSizeX: (l.max_compute_workgroup_size_x) as u32,
        maxComputeWorkgroupSizeY: (l.max_compute_workgroup_size_y) as u32,
        maxComputeWorkgroupSizeZ: (l.max_compute_workgroup_size_z) as u32,
        maxComputeWorkgroupsPerDimension: (l.max_compute_workgroups_per_dimension) as u32,
        maxImmediateSize: (l.max_immediate_size) as u32,
    }
}
pub fn check_limits(request: &Limits, actual: &types::Limits) -> Result<()> {
    if !request.nextInChain.is_null() {
        return Err(Error::Unsupported("chained device limits"));
    }
    let a = limits(actual);
    if request.maxTextureDimension1D != 0
        && request.maxTextureDimension1D != u32::MAX
        && request.maxTextureDimension1D > a.maxTextureDimension1D
    {
        return Err(Error::RequestDevice("limit maxTextureDimension1D".into()));
    }
    if request.maxTextureDimension2D != 0
        && request.maxTextureDimension2D != u32::MAX
        && request.maxTextureDimension2D > a.maxTextureDimension2D
    {
        return Err(Error::RequestDevice("limit maxTextureDimension2D".into()));
    }
    if request.maxTextureDimension3D != 0
        && request.maxTextureDimension3D != u32::MAX
        && request.maxTextureDimension3D > a.maxTextureDimension3D
    {
        return Err(Error::RequestDevice("limit maxTextureDimension3D".into()));
    }
    if request.maxTextureArrayLayers != 0
        && request.maxTextureArrayLayers != u32::MAX
        && request.maxTextureArrayLayers > a.maxTextureArrayLayers
    {
        return Err(Error::RequestDevice("limit maxTextureArrayLayers".into()));
    }
    if request.maxBindGroups != 0
        && request.maxBindGroups != u32::MAX
        && request.maxBindGroups > a.maxBindGroups
    {
        return Err(Error::RequestDevice("limit maxBindGroups".into()));
    }
    if request.maxBindGroupsPlusVertexBuffers != 0
        && request.maxBindGroupsPlusVertexBuffers != u32::MAX
        && request.maxBindGroupsPlusVertexBuffers > a.maxBindGroupsPlusVertexBuffers
    {
        return Err(Error::RequestDevice(
            "limit maxBindGroupsPlusVertexBuffers".into(),
        ));
    }
    if request.maxBindingsPerBindGroup != 0
        && request.maxBindingsPerBindGroup != u32::MAX
        && request.maxBindingsPerBindGroup > a.maxBindingsPerBindGroup
    {
        return Err(Error::RequestDevice("limit maxBindingsPerBindGroup".into()));
    }
    if request.maxDynamicUniformBuffersPerPipelineLayout != 0
        && request.maxDynamicUniformBuffersPerPipelineLayout != u32::MAX
        && request.maxDynamicUniformBuffersPerPipelineLayout
            > a.maxDynamicUniformBuffersPerPipelineLayout
    {
        return Err(Error::RequestDevice(
            "limit maxDynamicUniformBuffersPerPipelineLayout".into(),
        ));
    }
    if request.maxDynamicStorageBuffersPerPipelineLayout != 0
        && request.maxDynamicStorageBuffersPerPipelineLayout != u32::MAX
        && request.maxDynamicStorageBuffersPerPipelineLayout
            > a.maxDynamicStorageBuffersPerPipelineLayout
    {
        return Err(Error::RequestDevice(
            "limit maxDynamicStorageBuffersPerPipelineLayout".into(),
        ));
    }
    if request.maxSampledTexturesPerShaderStage != 0
        && request.maxSampledTexturesPerShaderStage != u32::MAX
        && request.maxSampledTexturesPerShaderStage > a.maxSampledTexturesPerShaderStage
    {
        return Err(Error::RequestDevice(
            "limit maxSampledTexturesPerShaderStage".into(),
        ));
    }
    if request.maxSamplersPerShaderStage != 0
        && request.maxSamplersPerShaderStage != u32::MAX
        && request.maxSamplersPerShaderStage > a.maxSamplersPerShaderStage
    {
        return Err(Error::RequestDevice(
            "limit maxSamplersPerShaderStage".into(),
        ));
    }
    if request.maxStorageBuffersPerShaderStage != 0
        && request.maxStorageBuffersPerShaderStage != u32::MAX
        && request.maxStorageBuffersPerShaderStage > a.maxStorageBuffersPerShaderStage
    {
        return Err(Error::RequestDevice(
            "limit maxStorageBuffersPerShaderStage".into(),
        ));
    }
    if request.maxStorageTexturesPerShaderStage != 0
        && request.maxStorageTexturesPerShaderStage != u32::MAX
        && request.maxStorageTexturesPerShaderStage > a.maxStorageTexturesPerShaderStage
    {
        return Err(Error::RequestDevice(
            "limit maxStorageTexturesPerShaderStage".into(),
        ));
    }
    if request.maxUniformBuffersPerShaderStage != 0
        && request.maxUniformBuffersPerShaderStage != u32::MAX
        && request.maxUniformBuffersPerShaderStage > a.maxUniformBuffersPerShaderStage
    {
        return Err(Error::RequestDevice(
            "limit maxUniformBuffersPerShaderStage".into(),
        ));
    }
    if request.maxUniformBufferBindingSize != 0
        && request.maxUniformBufferBindingSize != u64::MAX
        && request.maxUniformBufferBindingSize > a.maxUniformBufferBindingSize
    {
        return Err(Error::RequestDevice(
            "limit maxUniformBufferBindingSize".into(),
        ));
    }
    if request.maxStorageBufferBindingSize != 0
        && request.maxStorageBufferBindingSize != u64::MAX
        && request.maxStorageBufferBindingSize > a.maxStorageBufferBindingSize
    {
        return Err(Error::RequestDevice(
            "limit maxStorageBufferBindingSize".into(),
        ));
    }
    if request.minUniformBufferOffsetAlignment != 0
        && request.minUniformBufferOffsetAlignment != u32::MAX
        && request.minUniformBufferOffsetAlignment < a.minUniformBufferOffsetAlignment
    {
        return Err(Error::RequestDevice(
            "limit minUniformBufferOffsetAlignment".into(),
        ));
    }
    if request.minStorageBufferOffsetAlignment != 0
        && request.minStorageBufferOffsetAlignment != u32::MAX
        && request.minStorageBufferOffsetAlignment < a.minStorageBufferOffsetAlignment
    {
        return Err(Error::RequestDevice(
            "limit minStorageBufferOffsetAlignment".into(),
        ));
    }
    if request.maxVertexBuffers != 0
        && request.maxVertexBuffers != u32::MAX
        && request.maxVertexBuffers > a.maxVertexBuffers
    {
        return Err(Error::RequestDevice("limit maxVertexBuffers".into()));
    }
    if request.maxBufferSize != 0
        && request.maxBufferSize != u64::MAX
        && request.maxBufferSize > a.maxBufferSize
    {
        return Err(Error::RequestDevice("limit maxBufferSize".into()));
    }
    if request.maxVertexAttributes != 0
        && request.maxVertexAttributes != u32::MAX
        && request.maxVertexAttributes > a.maxVertexAttributes
    {
        return Err(Error::RequestDevice("limit maxVertexAttributes".into()));
    }
    if request.maxVertexBufferArrayStride != 0
        && request.maxVertexBufferArrayStride != u32::MAX
        && request.maxVertexBufferArrayStride > a.maxVertexBufferArrayStride
    {
        return Err(Error::RequestDevice(
            "limit maxVertexBufferArrayStride".into(),
        ));
    }
    if request.maxInterStageShaderVariables != 0
        && request.maxInterStageShaderVariables != u32::MAX
        && request.maxInterStageShaderVariables > a.maxInterStageShaderVariables
    {
        return Err(Error::RequestDevice(
            "limit maxInterStageShaderVariables".into(),
        ));
    }
    if request.maxColorAttachments != 0
        && request.maxColorAttachments != u32::MAX
        && request.maxColorAttachments > a.maxColorAttachments
    {
        return Err(Error::RequestDevice("limit maxColorAttachments".into()));
    }
    if request.maxColorAttachmentBytesPerSample != 0
        && request.maxColorAttachmentBytesPerSample != u32::MAX
        && request.maxColorAttachmentBytesPerSample > a.maxColorAttachmentBytesPerSample
    {
        return Err(Error::RequestDevice(
            "limit maxColorAttachmentBytesPerSample".into(),
        ));
    }
    if request.maxComputeWorkgroupStorageSize != 0
        && request.maxComputeWorkgroupStorageSize != u32::MAX
        && request.maxComputeWorkgroupStorageSize > a.maxComputeWorkgroupStorageSize
    {
        return Err(Error::RequestDevice(
            "limit maxComputeWorkgroupStorageSize".into(),
        ));
    }
    if request.maxComputeInvocationsPerWorkgroup != 0
        && request.maxComputeInvocationsPerWorkgroup != u32::MAX
        && request.maxComputeInvocationsPerWorkgroup > a.maxComputeInvocationsPerWorkgroup
    {
        return Err(Error::RequestDevice(
            "limit maxComputeInvocationsPerWorkgroup".into(),
        ));
    }
    if request.maxComputeWorkgroupSizeX != 0
        && request.maxComputeWorkgroupSizeX != u32::MAX
        && request.maxComputeWorkgroupSizeX > a.maxComputeWorkgroupSizeX
    {
        return Err(Error::RequestDevice(
            "limit maxComputeWorkgroupSizeX".into(),
        ));
    }
    if request.maxComputeWorkgroupSizeY != 0
        && request.maxComputeWorkgroupSizeY != u32::MAX
        && request.maxComputeWorkgroupSizeY > a.maxComputeWorkgroupSizeY
    {
        return Err(Error::RequestDevice(
            "limit maxComputeWorkgroupSizeY".into(),
        ));
    }
    if request.maxComputeWorkgroupSizeZ != 0
        && request.maxComputeWorkgroupSizeZ != u32::MAX
        && request.maxComputeWorkgroupSizeZ > a.maxComputeWorkgroupSizeZ
    {
        return Err(Error::RequestDevice(
            "limit maxComputeWorkgroupSizeZ".into(),
        ));
    }
    if request.maxComputeWorkgroupsPerDimension != 0
        && request.maxComputeWorkgroupsPerDimension != u32::MAX
        && request.maxComputeWorkgroupsPerDimension > a.maxComputeWorkgroupsPerDimension
    {
        return Err(Error::RequestDevice(
            "limit maxComputeWorkgroupsPerDimension".into(),
        ));
    }
    if request.maxImmediateSize != 0
        && request.maxImmediateSize != u32::MAX
        && request.maxImmediateSize > a.maxImmediateSize
    {
        return Err(Error::RequestDevice("limit maxImmediateSize".into()));
    }
    Ok(())
}
