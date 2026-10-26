//! Validated whole-pass plans. The host can lend its existing draw packets;
//! this layer emits directly to HAL instead of building another command replay.
//! First slice: vertex pulling, color attachments, indexed draws, sampled
//! textures, dynamic uniforms, blend/scissor/viewport, depth/stencil and MSAA.
use super::*;
use super::{
    bindings::{Group, PipelineLayout},
    shader::{Shader, inputs, outputs},
    texture::TextureView,
};
use types::{TextureFormat as Format, TextureSampleType as Sample, TextureUses as T};

struct PipelineInner<B: Backend> {
    owner: Rc<Owner<B>>,
    raw: Option<B::RenderPipeline>,
    id: u64,
    layout: PipelineLayout<B>,
    required_sizes: Vec<(u32,u32,u64)>,
    _programs: PipelinePrograms<B>,
    colors: Vec<Option<Format>>,
    depth: Option<Format>,
    samples: u32,
    primitive: types::PrimitiveState,
    plan_epoch: Cell<u64>,
}
enum PipelinePrograms<B: Backend> {
    Validated {
        _vertex: Shader<B>,
        _fragment: Option<Shader<B>>,
    },
    #[cfg(all(target_vendor = "apple", feature = "metal-stencil-export"))]
    MetalStencil {
        _shader: metal_stencil::RetainedShader,
    },
}
#[cfg(all(target_vendor = "apple", feature = "metal-stencil-export"))]
mod metal_stencil;
#[cfg(all(target_vendor = "apple", feature = "metal-stencil-export"))]
pub use metal_stencil::StencilRestore;
impl<B: Backend> Drop for PipelineInner<B> {
    fn drop(&mut self) {
        unsafe {
            self.owner
                .device
                .destroy_render_pipeline(self.raw.take().unwrap());
        }
    }
}
pub struct Pipeline<B: Backend>(Rc<PipelineInner<B>>);
impl<B: Backend> Clone for Pipeline<B> {
    fn clone(&self) -> Self {
        Self(self.0.clone())
    }
}
impl<B: Backend> Pipeline<B> {
    pub(crate) fn layout(&self) -> &PipelineLayout<B> { &self.0.layout }
    pub(crate) fn id(&self) -> u64 { self.0.id }
}
pub struct PipelineDescriptor<'a, B: Backend> {
    pub layout: &'a PipelineLayout<B>,
    pub vertex: &'a Shader<B>,
    pub vertex_entry: &'a str,
    pub fragment: Option<(&'a Shader<B>, &'a str)>,
    pub colors: &'a [Option<types::ColorTargetState>],
    pub primitive: types::PrimitiveState,
    pub depth_stencil: Option<types::DepthStencilState>,
    pub multisample: types::MultisampleState,
}
pub struct ColorAttachment<'a, B: Backend> {
    pub view: &'a TextureView<B>,
    /// None loads the initialized existing contents.
    pub clear: Option<types::Color>,
}
pub struct DepthAttachment<'a, B: Backend> {
    pub view: &'a TextureView<B>,
    pub clear_depth: Option<f32>,
    pub clear_stencil: Option<u32>,
}
pub struct BoundGroup<'a, B: Backend> {
    pub group: &'a Group<B>,
    pub dynamic_offsets: &'a [u32],
}
pub struct Indices<'a, B: Backend> {
    pub buffer: &'a Buffer<B>,
    pub format: types::IndexFormat,
    pub offset: u64,
    pub size: u64,
    pub base_vertex: i32,
}
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct Viewport {
    pub x: f32,
    pub y: f32,
    pub width: f32,
    pub height: f32,
    pub min_depth: f32,
    pub max_depth: f32,
}
pub struct Draw<'a, B: Backend> {
    pub pipeline: &'a Pipeline<B>,
    pub groups: &'a [BoundGroup<'a, B>],
    pub vertices: Range<u32>,
    pub instances: Range<u32>,
    pub indices: Option<Indices<'a, B>>,
    pub viewport: Option<Viewport>,
    pub scissor: Option<[u32; 4]>,
    pub blend: types::Color,
    pub stencil_reference: u32,
}
#[derive(Default, Debug, Clone, Copy)]
pub struct PassStats {
    pub draws: u64,
    pub skipped_empty: u64,
    pub unique_groups: u64,
    pub pipeline_binds: u64,
    pub group_binds: u64,
}

// End an active native pass during normal return and unwinding. A panic after
// partial encoding poisons this owner instead of permitting a partial submit.
pub(crate) struct RenderScope<'a, B: Backend> {
    pub raw: &'a mut B::CommandEncoder,
    pub owner: &'a Owner<B>,
}
impl<B: Backend> Drop for RenderScope<'_, B> {
    fn drop(&mut self) {
        if std::thread::panicking() {
            let _ = self
                .owner
                .fail_next("native render encoding unwound".into());
        }
        unsafe {
            self.raw.end_render_pass();
        }
    }
}
// Match the pinned wgpu-core29 coordinate and depth contract. Viewports may
// extend outside the attachment; scissor clips rasterization. Empty viewports
// validate but emit no draw, avoiding illegal zero-area backend viewports.
fn validate_viewport<B: Backend>(owner: &Owner<B>, v: Viewport) -> Result<()> {
    let limit = owner.limits.max_texture_dimension_2d as f32;
    let range = limit * 2.;
    if ![v.x, v.y, v.width, v.height, v.min_depth, v.max_depth]
        .into_iter()
        .all(f32::is_finite)
        || v.width < 0.
        || v.height < 0.
        || v.width > limit
        || v.height > limit
        || v.x < -range
        || v.y < -range
        || v.x + v.width > range - 1.
        || v.y + v.height > range - 1.
        || !(0. ..=1.).contains(&v.min_depth)
        || !(0. ..=1.).contains(&v.max_depth)
        || v.min_depth > v.max_depth
    {
        return Err(Error::Rejected("viewport bounds"));
    }
    Ok(())
}
fn clear_color(format: Format, c: types::Color) -> Result<()> {
    let values = [c.r, c.g, c.b, c.a];
    let valid = match format.sample_type(None, Some(types::Features::empty())) {
        Some(Sample::Uint) => values
            .into_iter()
            .all(|v| v.is_finite() && v.fract() == 0. && (0. ..=u32::MAX as f64).contains(&v)),
        Some(Sample::Sint) => values.into_iter().all(|v| {
            v.is_finite() && v.fract() == 0. && (i32::MIN as f64..=i32::MAX as f64).contains(&v)
        }),
        Some(Sample::Float { .. }) => values
            .into_iter()
            .all(|v| v.is_finite() && (v as f32).is_finite()),
        _ => false,
    };
    if !valid {
        return Err(Error::Rejected("clear color representation"));
    }
    Ok(())
}
fn needs_source_alpha(blend: types::BlendState) -> bool {
    use types::BlendFactor as F;
    [
        blend.color.src_factor,
        blend.color.dst_factor,
        blend.alpha.src_factor,
        blend.alpha.dst_factor,
    ]
    .into_iter()
    .any(|f| matches!(f, F::SrcAlpha | F::OneMinusSrcAlpha | F::SrcAlphaSaturated))
}

fn validate_colors<B: Backend>(
    owner: &Owner<B>,
    colors: &[Option<types::ColorTargetState>],
) -> Result<()> {
    if colors.len() > owner.limits.max_color_attachments as usize {
        return Err(Error::Rejected("color attachment count"));
    }
    let mut bytes = 0u32;
    for color in colors.iter().flatten() {
        if color.format.is_depth_stencil_format()
            || !owner.features.contains(color.format.required_features())
            || !types::ColorWrites::ALL.contains(color.write_mask)
        {
            return Err(Error::Rejected("color format/write mask"));
        }
        let capabilities = unsafe { owner._adapter.texture_format_capabilities(color.format) };
        if !capabilities.contains(hal::TextureFormatCapabilities::COLOR_ATTACHMENT) {
            return Err(Error::Rejected("color format capability"));
        }
        if let Some(blend) = color.blend {
            if !capabilities.contains(hal::TextureFormatCapabilities::COLOR_ATTACHMENT_BLEND) {
                return Err(Error::Rejected("color blend capability"));
            }
            if blend.color.src_factor.ref_second_blend_source()
                || blend.color.dst_factor.ref_second_blend_source()
                || blend.alpha.src_factor.ref_second_blend_source()
                || blend.alpha.dst_factor.ref_second_blend_source()
            {
                return Err(Error::Rejected("dual-source blending not implemented"));
            }
        }
        let align = color
            .format
            .target_component_alignment()
            .ok_or(Error::Rejected("color target alignment"))?;
        bytes = bytes
            .next_multiple_of(align)
            .checked_add(
                color
                    .format
                    .target_pixel_byte_cost()
                    .ok_or(Error::Rejected("color target cost"))?,
            )
            .ok_or(Error::Rejected("color target bytes overflow"))?;
    }
    if bytes > owner.limits.max_color_attachment_bytes_per_sample {
        return Err(Error::Rejected("color target bytes per sample"));
    }
    Ok(())
}
impl<B: Backend> Gpu<B> {
    pub fn render_pipeline(&self, d: &PipelineDescriptor<'_, B>) -> Result<Pipeline<B>> {
        self.owner.healthy()?;
        if !Rc::ptr_eq(&self.owner, &d.layout.0.owner)
            || !Rc::ptr_eq(&self.owner, &d.vertex.0.owner)
            || d.fragment
                .is_some_and(|(fragment, _)| !Rc::ptr_eq(&self.owner, &fragment.0.owner))
        {
            return Err(Error::Rejected("render pipeline device"));
        }
        validate_colors(&self.owner, d.colors)?;
        if !matches!(d.multisample.count, 1 | 4)
            || d.multisample.alpha_to_coverage_enabled && d.multisample.count == 1
        {
            return Err(Error::Rejected("pipeline multisample state"));
        }
        let active_mask = (1u64 << d.multisample.count) - 1;
        if !B::STATIC_SAMPLE_MASK
            && d.multisample.mask & active_mask != active_mask
            && !cfg!(feature = "metal-fixed-sample-mask")
        {
            return Err(Error::Rejected(
                "pipeline fixed sample mask requires lowering",
            ));
        }
        for color in d.colors.iter().flatten() {
            if d.multisample.count == 4
                && !unsafe {
                    self.owner
                        ._adapter
                        .texture_format_capabilities(color.format)
                }
                .contains(hal::TextureFormatCapabilities::MULTISAMPLE_X4)
            {
                return Err(Error::Rejected("color pipeline sample count"));
            }
        }
        if let Some(depth) = &d.depth_stencil {
            if !matches!(
                depth.format,
                Format::Depth32Float
                    | Format::Depth24Plus
                    | Format::Depth24PlusStencil8
                    | Format::Depth32FloatStencil8
            ) || !self
                .owner
                .features
                .contains(depth.format.required_features())
                || depth.depth_write_enabled.is_none()
                || depth.depth_compare.is_none()
                || !depth.bias.slope_scale.is_finite()
                || !depth.bias.clamp.is_finite()
                || depth.bias.clamp != 0.
                    && !self
                        .owner
                        .downlevel
                        .contains(types::DownlevelFlags::DEPTH_BIAS_CLAMP)
                || !depth.format.has_stencil_aspect()
                    && depth.stencil != types::StencilState::default()
            {
                return Err(Error::Rejected("depth/stencil pipeline state"));
            }
            let caps = unsafe {
                self.owner
                    ._adapter
                    .texture_format_capabilities(depth.format)
            };
            if !caps.contains(hal::TextureFormatCapabilities::DEPTH_STENCIL_ATTACHMENT)
                || d.multisample.count == 4
                    && !caps.contains(hal::TextureFormatCapabilities::MULTISAMPLE_X4)
            {
                return Err(Error::Rejected("depth pipeline capability/sample count"));
            }
        }
        if d.primitive.unclipped_depth
            || d.primitive.conservative
            || d.primitive.polygon_mode != types::PolygonMode::Fill
            || d.primitive.strip_index_format.is_some()
                && !matches!(
                    d.primitive.topology,
                    types::PrimitiveTopology::LineStrip | types::PrimitiveTopology::TriangleStrip
                )
        {
            return Err(Error::Rejected("primitive state unsupported"));
        }
        let vs = d.vertex.entry(d.vertex_entry, naga::ShaderStage::Vertex)?;
        d.vertex.check_layout(vs, d.layout)?;
        let mut required_sizes=d.vertex.buffer_requirements(vs)?;
        let vi = inputs(&d.vertex.0.module, &d.vertex.0.module.entry_points[vs])?;
        if !vi.locations.is_empty()
            || vi
                .builtins
                .iter()
                .any(|b| !matches!(b, naga::BuiltIn::VertexIndex | naga::BuiltIn::InstanceIndex))
        {
            return Err(Error::Rejected(
                "vertex input subset; validated pulling only",
            ));
        }
        let vo = outputs(&d.vertex.0.module, &d.vertex.0.module.entry_points[vs])?;
        if !vo
            .builtins
            .iter()
            .any(|b| matches!(b, naga::BuiltIn::Position { .. }))
            || vo
                .builtins
                .iter()
                .any(|b| !matches!(b, naga::BuiltIn::Position { .. }))
            || d.primitive.topology == types::PrimitiveTopology::PointList
        {
            return Err(Error::Rejected(
                "vertex builtins/point topology not implemented",
            ));
        }
        if vo.locations.len() > self.owner.limits.max_inter_stage_shader_variables as usize
            || vo
                .locations
                .keys()
                .any(|&n| n >= self.owner.limits.max_inter_stage_shader_variables)
        {
            return Err(Error::Rejected("vertex interface limit"));
        }
        if let Some((fragment, fragment_entry)) = d.fragment {
            let fs = fragment.entry(fragment_entry, naga::ShaderStage::Fragment)?;
            fragment.check_layout(fs, d.layout)?;
            required_sizes.extend(fragment.buffer_requirements(fs)?);
            let fi = inputs(&fragment.0.module, &fragment.0.module.entry_points[fs])?;
            if fi.builtins.iter().any(|b| {
                !matches!(
                    b,
                    naga::BuiltIn::Position { .. }
                        | naga::BuiltIn::FrontFacing
                        | naga::BuiltIn::SampleIndex
                        | naga::BuiltIn::SampleMask
                )
            }) {
                return Err(Error::Rejected("fragment input builtins not implemented"));
            }
            if fi
                .locations
                .keys()
                .any(|&n| n >= self.owner.limits.max_inter_stage_shader_variables)
                || fi
                    .locations
                    .iter()
                    .any(|(location, ty)| vo.locations.get(location) != Some(ty))
            {
                return Err(Error::Rejected("vertex/fragment interface mismatch"));
            }
            let fo = outputs(&fragment.0.module, &fragment.0.module.entry_points[fs])?;
            if fo
                .locations
                .keys()
                .any(|&n| n >= self.owner.limits.max_color_attachments)
            {
                return Err(Error::Rejected("fragment output location limit"));
            }
            if fo.builtins.iter().any(|b| match b {
                naga::BuiltIn::FragDepth => d.depth_stencil.is_none(),
                naga::BuiltIn::SampleMask => d.multisample.alpha_to_coverage_enabled,
                _ => true,
            }) {
                return Err(Error::Rejected("fragment built-in output contract"));
            }
            if d.multisample.alpha_to_coverage_enabled
                && (d.colors.first().is_none_or(Option::is_none)
                    || fo.locations.get(&0).is_none_or(|v| {
                        v.kind != naga::ScalarKind::Float || v.width != 4 || v.lanes != 4
                    }))
            {
                return Err(Error::Rejected(
                    "alpha-to-coverage requires location-zero vec4 float",
                ));
            }
            for (index, color) in d.colors.iter().enumerate() {
                let Some(color) = color else {
                    continue;
                };
                if color.write_mask.is_empty() {
                    continue;
                }
                let output = fo
                    .locations
                    .get(&(index as u32))
                    .ok_or(Error::Rejected("color output missing"))?;
                let kind = match color
                    .format
                    .sample_type(None, Some(types::Features::empty()))
                {
                    Some(Sample::Float { .. }) => naga::ScalarKind::Float,
                    Some(Sample::Uint) => naga::ScalarKind::Uint,
                    Some(Sample::Sint) => naga::ScalarKind::Sint,
                    _ => return Err(Error::Rejected("color output sample type")),
                };
                if output.kind != kind
                    || output.width != 4
                    || output.lanes < color.format.components()
                    || color.blend.is_some_and(needs_source_alpha) && output.lanes != 4
                {
                    return Err(Error::Rejected("color output scalar/vector type"));
                }
            }
        } else if d.colors.iter().any(Option::is_some)
            || d.depth_stencil.is_none()
            || d.multisample.alpha_to_coverage_enabled
        {
            return Err(Error::Rejected(
                "fragmentless pipeline requires only depth/stencil",
            ));
        }
        // HAL29 Metal ignores fixed masks. An opt-in, deliberately restricted
        // shader lowering supplies the same covered color/depth/stencil samples
        // for pure, center-interpolated shaders. The original descriptor was
        // validated above, so this cannot legitimize an invalid missing FS.
        let active_colors = d.colors.iter().enumerate().fold(0u32, |bits, (n, c)| {
            bits | if c.is_some() { 1 << n } else { 0 }
        });
        let color_variant = d
            .fragment
            .map(|(source, entry)| self.shader_with_color_targets(source, entry, active_colors))
            .transpose()?;
        let selected_fragment = d
            .fragment
            .map(|(_, entry)| (color_variant.as_ref().unwrap(), entry));
        let mut multisample = d.multisample;
        let lowered_fragment = if !B::STATIC_SAMPLE_MASK && multisample.mask != !0 {
            let active = (1u32 << multisample.count) - 1;
            let mask = multisample.mask as u32 & active;
            multisample.mask = !0;
            if mask == active {
                None
            } else {
                if multisample.alpha_to_coverage_enabled {
                    return Err(Error::Rejected(
                        "hardware alpha-to-coverage with lowered fixed mask",
                    ));
                }
                Some(if let Some((source, entry)) = selected_fragment {
                    (
                        self.shader_with_sample_mask(source, entry, mask)?,
                        entry.to_owned(),
                    )
                } else {
                    (self.shader(&format!("@fragment fn native_mask_only()->@builtin(sample_mask) u32{{return {mask}u;}}"))?,
                        "native_mask_only".to_owned())
                })
            }
        } else {
            None
        };
        let fragment = lowered_fragment
            .as_ref()
            .map(|(shader, entry)| (shader, entry.as_str()))
            .or(selected_fragment);
        let id = id()?;
        let constants = Default::default();
        let raw = unsafe {
            self.owner
                .device
                .create_render_pipeline(&hal::RenderPipelineDescriptor {
                    label: Some("native render pipeline"),
                    layout: d.layout.raw(),
                    vertex_processor: hal::VertexProcessor::Standard {
                        vertex_buffers: &[],
                        vertex_stage: hal::ProgrammableStage {
                            module: d.vertex.raw(),
                            entry_point: d.vertex_entry,
                            constants: &constants,
                            zero_initialize_workgroup_memory: true,
                        },
                    },
                    primitive: d.primitive,
                    depth_stencil: d.depth_stencil.clone(),
                    multisample,
                    fragment_stage: fragment.map(|(fragment, entry)| hal::ProgrammableStage {
                        module: fragment.raw(),
                        entry_point: entry,
                        constants: &constants,
                        zero_initialize_workgroup_memory: true,
                    }),
                    color_targets: d.colors,
                    multiview_mask: None,
                    cache: None,
                })
        }
        .map_err(|e| self.owner.pipeline_error(e))?;
        Ok(Pipeline(Rc::new(PipelineInner {
            owner: self.owner.clone(),
            raw: Some(raw),
            id,
            layout: d.layout.clone(),
            required_sizes,
            _programs: PipelinePrograms::Validated {
                _vertex: d.vertex.clone(),
                _fragment: fragment.map(|(f, _)| f.clone()),
            },
            colors: d
                .colors
                .iter()
                .map(|c| c.as_ref().map(|c| c.format))
                .collect(),
            depth: d.depth_stencil.as_ref().map(|d| d.format),
            samples: d.multisample.count,
            primitive: d.primitive,
            plan_epoch: Cell::new(0),
        })))
    }
}

pub(crate) struct AttachmentInfo<'a, B: Backend> {
    pub width: u32,
    pub height: u32,
    pub samples: u32,
    pub formats: Vec<Option<Format>>,
    pub depth_format: Option<Format>,
    pub images: BTreeMap<(u64, u32, u32), (&'a texture::Texture<B>, T)>,
}
pub(crate) fn validate_attachments<'a, B: Backend>(
    owner: &Rc<Owner<B>>,
    colors: &[Option<ColorAttachment<'a, B>>],
    depth: Option<&DepthAttachment<'a, B>>,
    resolves: &[Option<&'a TextureView<B>>],
) -> Result<AttachmentInfo<'a, B>> {
    owner.healthy()?;
    if colors.len() > owner.limits.max_color_attachments as usize
        || !resolves.is_empty() && resolves.len() != colors.len()
    {
        return Err(Error::Rejected("render attachment count"));
    }
    let mut dimensions = None;
    let mut formats = Vec::new();
    let mut samples = None;
    let mut images: BTreeMap<(u64, u32, u32), (&texture::Texture<B>, T)> = BTreeMap::new();
    for color in colors {
        let Some(color) = color else {
            formats.push(None);
            continue;
        };
        let view = color.view;
        let range = view.range();
        let texture = view.texture();
        if !view.belongs_to(owner)
            || view.dimension() != types::TextureViewDimension::D2
            || range.mip_level_count != Some(1)
            || range.array_layer_count != Some(1)
            || !texture.descriptor().usage.contains(T::COLOR_TARGET)
        {
            return Err(Error::Rejected("color attachment view/device/usage"));
        }
        let size = texture.mip_size(range.base_mip_level)?;
        let extent = (size.width, size.height);
        if dimensions.is_some_and(|d| d != extent) {
            return Err(Error::Rejected("attachment dimensions mismatch"));
        }
        dimensions = Some(extent);
        let count = texture.descriptor().sample_count;
        if samples.is_some_and(|n| n != count) {
            return Err(Error::Rejected("attachment sample count mismatch"));
        }
        samples = Some(count);
        let key = (texture.id(), range.base_mip_level, range.base_array_layer);
        if images.insert(key, (texture, T::COLOR_TARGET)).is_some() {
            return Err(Error::Rejected("aliased color attachments"));
        }
        if let Some(clear) = color.clear {
            clear_color(texture.descriptor().format, clear)?;
        }
        formats.push(Some(texture.descriptor().format));
    }
    for (i, resolve) in resolves.iter().enumerate() {
        let Some(view) = resolve else {
            continue;
        };
        let Some(color) = &colors[i] else {
            return Err(Error::Rejected("resolve without color"));
        };
        let r = view.range();
        let t = view.texture();
        if !view.belongs_to(owner)
            || view.dimension() != types::TextureViewDimension::D2
            || r.mip_level_count != Some(1)
            || r.array_layer_count != Some(1)
            || t.descriptor().sample_count != 1
            || samples != Some(4)
            || t.descriptor().format != color.view.texture().descriptor().format
            || !t.descriptor().usage.contains(T::COLOR_TARGET)
        {
            return Err(Error::Rejected("resolve view/format/samples/usage"));
        }
        let size = t.mip_size(r.base_mip_level)?;
        if dimensions != Some((size.width, size.height))
            || !unsafe {
                owner
                    ._adapter
                    .texture_format_capabilities(t.descriptor().format)
            }
            .contains(hal::TextureFormatCapabilities::MULTISAMPLE_RESOLVE)
        {
            return Err(Error::Rejected("resolve dimensions/capability"));
        }
        if images
            .insert(
                (t.id(), r.base_mip_level, r.base_array_layer),
                (t, T::COLOR_TARGET),
            )
            .is_some()
        {
            return Err(Error::Rejected("aliased resolve target"));
        }
    }
    let depth_format = depth.as_ref().map(|d| d.view.texture().descriptor().format);
    if let Some(depth) = &depth {
        let view = depth.view;
        let range = view.range();
        let texture = view.texture();
        let desc = texture.descriptor();
        if !view.belongs_to(owner)
            || view.dimension() != types::TextureViewDimension::D2
            || range.mip_level_count != Some(1)
            || range.array_layer_count != Some(1)
            || range.aspect != types::TextureAspect::All
            || !desc.usage.contains(T::DEPTH_STENCIL_WRITE)
            || !desc.format.has_depth_aspect()
        {
            return Err(Error::Rejected("depth attachment view/usage"));
        }
        if depth
            .clear_depth
            .is_some_and(|v| !v.is_finite() || !(0. ..=1.).contains(&v))
            || depth
                .clear_stencil
                .is_some_and(|v| !desc.format.has_stencil_aspect() || v > 255)
        {
            return Err(Error::Rejected("depth/stencil clear bounds"));
        }
        let size = texture.mip_size(range.base_mip_level)?;
        let extent = (size.width, size.height);
        if dimensions.is_some_and(|d| d != extent)
            || samples.is_some_and(|n| n != desc.sample_count)
        {
            return Err(Error::Rejected("depth dimensions/sample mismatch"));
        }
        dimensions = Some(extent);
        samples = Some(desc.sample_count);
        if images
            .insert(
                (texture.id(), range.base_mip_level, range.base_array_layer),
                (texture, T::DEPTH_STENCIL_WRITE),
            )
            .is_some()
        {
            return Err(Error::Rejected("aliased depth target"));
        }
    }
    let (width, height) =
        dimensions.ok_or(Error::Rejected("render plan requires an attachment"))?;
    Ok(AttachmentInfo {
        width,
        height,
        samples: samples.unwrap(),
        formats,
        depth_format,
        images,
    })
}

impl<B: Backend + 'static> Encoder<B> {
    pub fn render(
        &mut self,
        colors: &[Option<ColorAttachment<'_, B>>],
        draws: &[Draw<'_, B>],
    ) -> Result<PassStats> {
        self.render_full(colors, None, &[], draws)
    }
    pub fn render_full(
        &mut self,
        colors: &[Option<ColorAttachment<'_, B>>],
        depth: Option<DepthAttachment<'_, B>>,
        resolves: &[Option<&TextureView<B>>],
        draws: &[Draw<'_, B>],
    ) -> Result<PassStats> {
        self.owner.healthy()?;
        if colors.len() > self.owner.limits.max_color_attachments as usize
            || draws.len() > 65536
            || !resolves.is_empty() && resolves.len() != colors.len()
        {
            return Err(Error::Rejected("render plan size"));
        }
        let info = validate_attachments(&self.owner, colors, depth.as_ref(), resolves)?;
        let (width, height) = (info.width, info.height);
        let samples = Some(info.samples);
        let formats = info.formats;
        let depth_format = info.depth_format;
        let mut images = info.images;
        let epoch = id()?;
        let mut buffers: BTreeMap<u64, (&Buffer<B>, U)> = BTreeMap::new();
        let mut unique_groups = Vec::new();
        let mut unique_pipelines = Vec::new();
        let default_viewport = Viewport {
            x: 0.,
            y: 0.,
            width: width as f32,
            height: height as f32,
            min_depth: 0.,
            max_depth: 1.,
        };
        for draw in draws {
            let p = &draw.pipeline.0;
            if !Rc::ptr_eq(&self.owner, &p.owner)
                || draw.vertices.start > draw.vertices.end
                || draw.instances.start > draw.instances.end
            {
                return Err(Error::Rejected("draw device/ranges"));
            }
            if (draw.instances.start != 0
                || draw.indices.as_ref().is_some_and(|i| i.base_vertex != 0))
                && !self
                    .owner
                    .downlevel
                    .contains(types::DownlevelFlags::BASE_VERTEX)
            {
                return Err(Error::Rejected("base vertex/instance capability"));
            }
            if p.plan_epoch.replace(epoch) != epoch {
                if p.colors != formats || p.depth != depth_format || Some(p.samples) != samples {
                    return Err(Error::Rejected("pipeline/pass formats or samples"));
                }
                unique_pipelines.push(draw.pipeline);
            }
            if draw.groups.len() != p.layout.0.groups.len() {
                return Err(Error::Rejected("draw bind group count"));
            }
            for (slot,(bound, layout)) in draw.groups.iter().zip(&p.layout.0.groups).enumerate() {
                bound.group.check_sizes(slot as u32, &p.required_sizes)?;
                if !layout.compatible(&bound.group.0.layout) {
                    return Err(Error::Rejected("draw group layout mismatch"));
                }
                bound.group.validate_offsets(bound.dynamic_offsets)?;
                if bound.group.0.plan_epoch.replace(epoch) == epoch {
                    continue;
                }
                unique_groups.push(bound.group);
                for (buffer, usage) in &bound.group.0.buffers {
                    if !U::INCLUSIVE.contains(*usage) {
                        return Err(Error::Rejected("writable graphics buffer binding"));
                    }
                    buffers
                        .entry(buffer.id())
                        .and_modify(|(_, u)| *u |= *usage)
                        .or_insert((buffer, *usage));
                }
                for view in &bound.group.0.textures {
                    let r = view.range();
                    for mip in r.base_mip_level..r.base_mip_level + r.mip_level_count.unwrap() {
                        for layer in
                            r.base_array_layer..r.base_array_layer + r.array_layer_count.unwrap()
                        {
                            let key = (view.texture().id(), mip, layer);
                            if images.get(&key).is_some_and(|(_, u)| *u != T::RESOURCE) {
                                return Err(Error::Rejected(
                                    "sampled attachment aliases render target",
                                ));
                            }
                            images.insert(key, (view.texture(), T::RESOURCE));
                        }
                    }
                }
            }
            if let Some(index) = &draw.indices {
                self.check(index.buffer, U::INDEX)?;
                let unit = match index.format {
                    types::IndexFormat::Uint16 => 2,
                    types::IndexFormat::Uint32 => 4,
                };
                if index.size == 0
                    || index.offset % unit != 0
                    || index.size % unit != 0
                    || index
                        .offset
                        .checked_add(index.size)
                        .is_none_or(|end| end > index.buffer.size())
                    || u64::from(draw.vertices.end) * unit > index.size
                    || p.primitive
                        .strip_index_format
                        .is_some_and(|f| f != index.format)
                {
                    return Err(Error::Rejected("index bounds/format/alignment"));
                }
                buffers
                    .entry(index.buffer.id())
                    .and_modify(|(_, u)| *u |= U::INDEX)
                    .or_insert((index.buffer, U::INDEX));
            }
            let v = draw.viewport.unwrap_or(default_viewport);
            validate_viewport(&self.owner, v)?;
            let [x, y, w, h] = draw.scissor.unwrap_or([0, 0, width, height]);
            if x.checked_add(w).is_none_or(|end| end > width)
                || y.checked_add(h).is_none_or(|end| end > height)
                || ![draw.blend.r, draw.blend.g, draw.blend.b, draw.blend.a]
                    .into_iter()
                    .all(|n| n.is_finite() && (n as f32).is_finite())
            {
                return Err(Error::Rejected("scissor/blend bounds"));
            }
        }
        // No HAL command is emitted until the entire immutable plan is valid.
        for (_, (buffer, usage)) in buffers {
            self.transition(buffer, usage);
        }
        for ((_, mip, layer), (texture, usage)) in images {
            self.transition_texture(texture, mip, layer, usage);
        }
        for pipeline in unique_pipelines {
            self.holds
                .entry(pipeline.0.id)
                .or_insert_with(|| pipeline.0.clone());
        }
        for group in &unique_groups {
            self.holds
                .entry(group.id())
                .or_insert_with(|| group.0.clone());
        }
        for c in colors.iter().flatten() {
            self.holds
                .entry(c.view.id())
                .or_insert_with(|| c.view.retained());
        }
        for v in resolves.iter().flatten() {
            self.holds.entry(v.id()).or_insert_with(|| v.retained());
        }
        if let Some(d) = &depth {
            self.holds
                .entry(d.view.id())
                .or_insert_with(|| d.view.retained());
        }
        let raw_colors: Vec<_> = colors
            .iter()
            .enumerate()
            .map(|(i, c)| {
                c.as_ref().map(|c| hal::ColorAttachment {
                    target: hal::Attachment {
                        view: c.view.raw(),
                        usage: T::COLOR_TARGET,
                    },
                    depth_slice: None,
                    resolve_target: resolves.get(i).and_then(|v| *v).map(|v| hal::Attachment {
                        view: v.raw(),
                        usage: T::COLOR_TARGET,
                    }),
                    ops: hal::AttachmentOps::STORE
                        | if c.clear.is_none() {
                            hal::AttachmentOps::LOAD
                        } else {
                            hal::AttachmentOps::LOAD_CLEAR
                        },
                    clear_value: c.clear.unwrap_or(types::Color::TRANSPARENT),
                })
            })
            .collect();
        let mut stats = PassStats {
            unique_groups: unique_groups.len() as u64,
            ..Default::default()
        };
        let mut last_pipeline = 0;
        let mut last_layout = 0;
        let mut last_groups: Vec<Option<(u64, &[u32])>> = Vec::new();
        let mut last_viewport = None;
        let mut last_scissor = None;
        let mut last_blend = None;
        let mut last_index = None;
        let mut last_stencil = None;
        unsafe {
            let raw = self.raw.as_mut().unwrap();
            raw.begin_render_pass(&hal::RenderPassDescriptor {
                label: Some("native checked render plan"),
                extent: types::Extent3d {
                    width,
                    height,
                    depth_or_array_layers: 1,
                },
                sample_count: samples.unwrap(),
                color_attachments: &raw_colors,
                depth_stencil_attachment: depth.as_ref().map(|d| hal::DepthStencilAttachment {
                    target: hal::Attachment {
                        view: d.view.raw(),
                        usage: T::DEPTH_STENCIL_WRITE,
                    },
                    depth_ops: hal::AttachmentOps::STORE
                        | if d.clear_depth.is_some() {
                            hal::AttachmentOps::LOAD_CLEAR
                        } else {
                            hal::AttachmentOps::LOAD
                        },
                    stencil_ops: if d.view.texture().descriptor().format.has_stencil_aspect() {
                        hal::AttachmentOps::STORE
                            | if d.clear_stencil.is_some() {
                                hal::AttachmentOps::LOAD_CLEAR
                            } else {
                                hal::AttachmentOps::LOAD
                            }
                    } else {
                        hal::AttachmentOps::LOAD_DONT_CARE | hal::AttachmentOps::STORE_DISCARD
                    },
                    clear_value: (d.clear_depth.unwrap_or(0.), d.clear_stencil.unwrap_or(0)),
                }),
                multiview_mask: None,
                timestamp_writes: None,
                occlusion_query_set: None,
            })
            .map_err(|e| self.owner.device_error(e))?;
            let scope = RenderScope {
                raw,
                owner: &self.owner,
            };
            let raw = &mut *scope.raw;
            for draw in draws {
                let scissor = draw.scissor.unwrap_or([0, 0, width, height]);
                let viewport = draw.viewport.unwrap_or(default_viewport);
                if viewport.width == 0.
                    || viewport.height == 0.
                    || scissor[2] == 0
                    || scissor[3] == 0
                    || draw.vertices.is_empty()
                    || draw.instances.is_empty()
                {
                    stats.skipped_empty += 1;
                    continue;
                }
                let p = &draw.pipeline.0;
                if last_pipeline != p.id {
                    raw.set_render_pipeline(p.raw.as_ref().unwrap());
                    last_pipeline = p.id;
                    stats.pipeline_binds += 1;
                }
                if last_layout != p.layout.id() {
                    last_groups.clear();
                    last_groups.resize_with(p.layout.0.groups.len(), || None);
                    last_layout = p.layout.id();
                }
                for (i, g) in draw.groups.iter().enumerate() {
                    let key = (g.group.id(), g.dynamic_offsets);
                    if last_groups[i] != Some(key) {
                        raw.set_bind_group(
                            p.layout.raw(),
                            i as u32,
                            g.group.raw(),
                            g.dynamic_offsets,
                        );
                        last_groups[i] = Some(key);
                        stats.group_binds += 1;
                    }
                }
                let v = draw.viewport.unwrap_or(default_viewport);
                if last_viewport != Some(v) {
                    raw.set_viewport(
                        &hal::Rect {
                            x: v.x,
                            y: v.y,
                            w: v.width,
                            h: v.height,
                        },
                        v.min_depth..v.max_depth,
                    );
                    last_viewport = Some(v);
                }
                let s = draw.scissor.unwrap_or([0, 0, width, height]);
                if last_scissor != Some(s) {
                    raw.set_scissor_rect(&hal::Rect {
                        x: s[0],
                        y: s[1],
                        w: s[2],
                        h: s[3],
                    });
                    last_scissor = Some(s);
                }
                if last_blend != Some(draw.blend) {
                    raw.set_blend_constants(&[
                        draw.blend.r as f32,
                        draw.blend.g as f32,
                        draw.blend.b as f32,
                        draw.blend.a as f32,
                    ]);
                    last_blend = Some(draw.blend);
                }
                if last_stencil != Some(draw.stencil_reference) {
                    raw.set_stencil_reference(draw.stencil_reference);
                    last_stencil = Some(draw.stencil_reference);
                }
                let count = draw.vertices.end - draw.vertices.start;
                let instances = draw.instances.end - draw.instances.start;
                if let Some(index) = &draw.indices {
                    let key = (index.buffer.id(), index.format, index.offset, index.size);
                    if last_index != Some(key) {
                        raw.set_index_buffer(
                            hal::BufferBinding::new_unchecked(
                                index.buffer.raw(),
                                index.offset,
                                NonZeroU64::new(index.size),
                            ),
                            index.format,
                        );
                        last_index = Some(key);
                    }
                    raw.draw_indexed(
                        draw.vertices.start,
                        count,
                        index.base_vertex,
                        draw.instances.start,
                        instances,
                    );
                } else {
                    raw.draw(draw.vertices.start, count, draw.instances.start, instances);
                }
                stats.draws += 1;
            }
        }
        Ok(stats)
    }
}

mod recording;
pub use recording::{NativeTypes, PassRecorder, RecorderStorage};
