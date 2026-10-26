//! Native EDRAM ownership-transfer slice using the SAME public shader/address
//! contract as the legacy cache. `targets` supplies the transactional ownership
//! cache used by the prepared native session; the live guest adapter is separate.
//! Every parameter buffer is immutable through retirement; no mutable raw slot.
use crate::*;
use crate::{
    backend::Platform,
    bindings::{Entry, GroupLayout, Resource},
    render::{BoundGroup, ColorAttachment, DepthAttachment, Draw, Pipeline, PipelineDescriptor},
    texture::TextureView,
};
use std::collections::HashMap;
use types::{
    ShaderStages as S, TextureFormat as F, TextureSampleType as Sample, TextureViewDimension as V,
};
use xenos_gfx::{
    edram_shaders as kernels,
    edram_transfer::{ColorStorage, Transfer, color_storage},
};
use xenos_state::{edram::RenderTargetKey, xenos::MsaaSamples};
pub mod targets;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub enum StencilMode {
    #[default]
    PortableBits,
    #[cfg(all(target_vendor = "apple", feature = "metal-stencil-export"))]
    MetalExport,
}
#[derive(Clone)]
enum Stencil {
    Bits {
        layout: GroupLayout<Platform>,
        pipelines: Vec<Pipeline<Platform>>,
    },
    #[cfg(all(target_vendor = "apple", feature = "metal-stencil-export"))]
    Metal(crate::render::StencilRestore),
}
#[derive(Clone)]
pub struct TransferPipeline {
    source: kernels::SrcKind,
    destination: F,
    samples: u32,
    scale: u32,
    layout: GroupLayout<Platform>,
    main: Pipeline<Platform>,
    stencil: Option<Stencil>,
}
pub struct Images<'a> {
    pub source: &'a TextureView<Platform>,
    pub source_stencil: Option<&'a TextureView<Platform>>,
    pub destination: &'a TextureView<Platform>,
    pub stencil_scratch: Option<&'a TextureView<Platform>>,
}
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct TransferStats {
    pub main_draws: u64,
    pub stencil_draws: u64,
}

/// A closed, immutable set of parameter slots. All uploads happen before the
/// first use, so queue-level uploads cannot overwrite an earlier encoded draw.
/// One buffer/upload serves a batch; bindings retain it through GPU retirement.
pub struct PreparedTransfers {
    buffer: Buffer<Platform>,
    plans: Vec<Transfer>,
    stride: u32,
}
impl PreparedTransfers {
    pub fn len(&self) -> usize {
        self.plans.len()
    }
    pub fn is_empty(&self) -> bool {
        self.plans.is_empty()
    }
}
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
struct PipelineKey {
    source: kernels::SrcKind,
    destination: F,
    samples: u32,
    scale: u32,
    mode: StencilMode,
}
#[derive(Default, Clone, Copy, Debug, PartialEq, Eq)]
pub struct CacheStats {
    pub hits: u64,
    pub created: u64,
    pub evicted: u64,
    pub live: usize,
}
/// One device, bounded immutable pipelines. Eviction drops only the cache's
/// reference; queued work retains its own pipelines and shader modules.
pub struct TransferCache {
    owner: Rc<Owner<Platform>>,
    entries: HashMap<PipelineKey, (TransferPipeline, u64)>,
    capacity: usize,
    clock: u64,
    stats: CacheStats,
}
impl TransferCache {
    pub fn stats(&self) -> CacheStats {
        CacheStats {
            live: self.entries.len(),
            ..self.stats
        }
    }
    pub fn clear(&mut self) {
        self.entries.clear();
    }
    pub fn get(
        &mut self,
        gpu: &Gpu<Platform>,
        source: kernels::SrcKind,
        destination: F,
        samples: u32,
        scale: u32,
        mode: StencilMode,
    ) -> Result<TransferPipeline> {
        gpu.owner.healthy()?;
        if !Rc::ptr_eq(&self.owner, &gpu.owner) {
            return Err(Error::Rejected("EDRAM pipeline cache device"));
        }
        self.clock = self
            .clock
            .checked_add(1)
            .ok_or(Error::Rejected("EDRAM cache clock exhaustion"))?;
        let key = PipelineKey {
            source,
            destination,
            samples,
            scale,
            mode,
        };
        if let Some((pipeline, used)) = self.entries.get_mut(&key) {
            *used = self.clock;
            self.stats.hits += 1;
            return Ok(pipeline.clone());
        }
        let pipeline = gpu.edram_transfer_pipeline(source, destination, samples, scale, mode)?;
        if self.entries.len() == self.capacity {
            let oldest = *self
                .entries
                .iter()
                .min_by_key(|(_, (_, used))| *used)
                .unwrap()
                .0;
            self.entries.remove(&oldest);
            self.stats.evicted += 1;
        }
        self.entries.insert(key, (pipeline.clone(), self.clock));
        self.stats.created += 1;
        Ok(pipeline)
    }
}
fn host_samples(k: RenderTargetKey) -> u32 {
    if k.msaa_samples == MsaaSamples::X1 {
        1
    } else {
        4
    }
}
fn color_format(k: RenderTargetKey) -> Option<F> {
    Some(match color_storage(k.color_format())? {
        ColorStorage::Rgba8 => F::Rgba8Unorm,
        ColorStorage::Rgb10A2 => F::Rgb10a2Unorm,
        ColorStorage::Rg16Float => F::Rg16Float,
        ColorStorage::Rgba16Float => F::Rgba16Float,
        ColorStorage::R32Float => F::R32Float,
        ColorStorage::Rg32Float => F::Rg32Float,
    })
}
fn format_matches(k: RenderTargetKey, f: F) -> bool {
    if k.is_depth {
        matches!(f, F::Depth24PlusStencil8 | F::Depth32FloatStencil8)
    } else {
        color_format(k) == Some(f)
    }
}
fn stencil_state(format: F, mask: u32, depth_write: bool) -> types::DepthStencilState {
    let face = types::StencilFaceState {
        compare: types::CompareFunction::Always,
        fail_op: types::StencilOperation::Keep,
        depth_fail_op: types::StencilOperation::Keep,
        pass_op: types::StencilOperation::Replace,
    };
    types::DepthStencilState {
        format,
        depth_write_enabled: Some(depth_write),
        depth_compare: Some(types::CompareFunction::Always),
        stencil: types::StencilState {
            front: face,
            back: face,
            read_mask: 255,
            write_mask: mask,
        },
        bias: Default::default(),
    }
}
impl Gpu<Platform> {
    pub fn edram_transfer_cache(&self, capacity: usize) -> Result<TransferCache> {
        self.owner.healthy()?;
        if !(1..=256).contains(&capacity) {
            return Err(Error::Rejected("EDRAM pipeline cache capacity"));
        }
        Ok(TransferCache {
            owner: self.owner.clone(),
            entries: HashMap::with_capacity(capacity),
            capacity,
            clock: 0,
            stats: Default::default(),
        })
    }
    pub fn edram_transfer_params(&mut self, plans: &[Transfer]) -> Result<PreparedTransfers> {
        self.owner.healthy()?;
        if plans.is_empty() || plans.len() > 4096 {
            return Err(Error::Rejected("EDRAM transfer batch capacity"));
        }
        let alignment = self.owner.limits.min_uniform_buffer_offset_alignment.max(1);
        let stride = 80u32
            .div_ceil(alignment)
            .checked_mul(alignment)
            .ok_or(Error::Rejected("EDRAM uniform stride"))?;
        let size = (plans.len() as u64)
            .checked_mul(u64::from(stride))
            .ok_or(Error::Rejected("EDRAM parameter size"))?;
        // Native dynamic offsets are u32, even when buffer sizes use u64.
        if size > u64::from(u32::MAX) {
            return Err(Error::Rejected("EDRAM parameter dynamic offset"));
        }

        let mut bytes = vec![0; size as usize];
        for (i, plan) in plans.iter().enumerate() {
            for (j, word) in plan.words().iter().enumerate() {
                let p = i * stride as usize + j * 4;
                bytes[p..p + 4].copy_from_slice(&word.to_le_bytes());
            }
        }
        let buffer = self.create_buffer_init(
            "immutable EDRAM parameter batch",
            &bytes,
            U::UNIFORM | U::COPY_DST,
        )?;
        Ok(PreparedTransfers {
            buffer,
            plans: plans.to_vec(),
            stride,
        })
    }
    pub fn edram_transfer_pipeline(
        &self,
        source: kernels::SrcKind,
        destination: F,
        samples: u32,
        scale: u32,
        mode: StencilMode,
    ) -> Result<TransferPipeline> {
        if !(1..=3).contains(&scale) || !matches!(samples, 1 | 4) {
            return Err(Error::Rejected("EDRAM pipeline scale/sample count"));
        }
        let depth = destination.is_depth_stencil_format();
        if depth
            && !matches!(
                destination,
                F::Depth24PlusStencil8 | F::Depth32FloatStencil8
            )
        {
            return Err(Error::Rejected("EDRAM depth/stencil format"));
        }
        let sampled = |binding, sample_type| types::BindGroupLayoutEntry {
            binding,
            visibility: S::FRAGMENT,
            ty: types::BindingType::Texture {
                sample_type,
                view_dimension: V::D2,
                multisampled: source.multisampled(),
            },
            count: None,
        };
        let mut entries = vec![
            types::BindGroupLayoutEntry {
                binding: 0,
                visibility: S::FRAGMENT,
                ty: types::BindingType::Buffer {
                    ty: types::BufferBindingType::Uniform,
                    has_dynamic_offset: true,
                    min_binding_size: NonZeroU64::new(80),
                },
                count: None,
            },
            sampled(
                1,
                if matches!(source, kernels::SrcKind::Depth { .. }) {
                    Sample::Depth
                } else {
                    Sample::Float { filterable: false }
                },
            ),
        ];
        if matches!(source, kernels::SrcKind::Depth { .. }) {
            entries.push(sampled(2, Sample::Uint));
        }
        let layout = self.group_layout(&entries)?;
        let pl = self.pipeline_layout(&[layout.clone()])?;
        let shader = self.shader(&kernels::transfer_module(source, samples > 1, scale))?;
        let colors = [Some(types::ColorTargetState {
            format: if depth { F::R32Uint } else { destination },
            blend: None,
            write_mask: types::ColorWrites::ALL,
        })];
        let main = self.render_pipeline(&PipelineDescriptor {
            layout: &pl,
            vertex: &shader,
            vertex_entry: "vs",
            fragment: Some((
                &shader,
                if depth {
                    "fs_depth_scratch"
                } else {
                    "fs_color"
                },
            )),
            colors: &colors,
            primitive: Default::default(),
            depth_stencil: depth.then(|| stencil_state(destination, 255, true)),
            multisample: types::MultisampleState {
                count: samples,
                ..Default::default()
            },
        })?;
        let stencil = if depth {
            Some(match mode {
                StencilMode::PortableBits => {
                    let layout = self.group_layout(&[types::BindGroupLayoutEntry {
                        binding: 0,
                        visibility: S::FRAGMENT,
                        ty: types::BindingType::Texture {
                            sample_type: Sample::Uint,
                            view_dimension: V::D2,
                            multisampled: samples > 1,
                        },
                        count: None,
                    }])?;
                    let pl = self.pipeline_layout(&[layout.clone()])?;
                    let source = kernels::stencil_bits_module(samples > 1);
                    let mut pipelines = Vec::new();
                    for bit in 0..8 {
                        let shader =
                            self.shader_specialized(&source, &[("STENCIL_BIT", f64::from(bit))])?;
                        pipelines.push(self.render_pipeline(&PipelineDescriptor {
                            layout: &pl,
                            vertex: &shader,
                            vertex_entry: "vs",
                            fragment: Some((&shader, "fs")),
                            colors: &[],
                            primitive: Default::default(),
                            depth_stencil: Some(stencil_state(destination, 1 << bit, false)),
                            multisample: types::MultisampleState {
                                count: samples,
                                ..Default::default()
                            },
                        })?);
                    }
                    Stencil::Bits { layout, pipelines }
                }
                #[cfg(all(target_vendor = "apple", feature = "metal-stencil-export"))]
                StencilMode::MetalExport => {
                    if destination != F::Depth24PlusStencil8 {
                        return Err(Error::Rejected(
                            "Metal stencil export format is not validated yet",
                        ));
                    }
                    Stencil::Metal(self.metal_stencil_restore(samples)?)
                }
            })
        } else {
            None
        };
        Ok(TransferPipeline {
            source,
            destination,
            samples,
            scale,
            layout,
            main,
            stencil,
        })
    }
}
impl TransferPipeline {
    pub fn encode(
        &self,
        gpu: &mut Gpu<Platform>,
        encoder: &mut Encoder<Platform>,
        plan: &Transfer,
        images: Images<'_>,
    ) -> Result<TransferStats> {
        let params = gpu.edram_transfer_params(std::slice::from_ref(plan))?;
        self.encode_prepared(gpu, encoder, &params, 0, images)
    }
    pub fn encode_prepared(
        &self,
        gpu: &Gpu<Platform>,
        encoder: &mut Encoder<Platform>,
        params: &PreparedTransfers,
        index: usize,
        images: Images<'_>,
    ) -> Result<TransferStats> {
        gpu.owner.healthy()?;
        let plan = params
            .plans
            .get(index)
            .ok_or(Error::Rejected("EDRAM transfer parameter index"))?;
        if !Rc::ptr_eq(&gpu.owner, &params.buffer.0.owner) {
            return Err(Error::Rejected("EDRAM transfer parameter device"));
        }
        let src = images.source;
        let dst = images.destination;
        let sd = src.texture().descriptor();
        let dd = dst.texture().descriptor();
        if !Rc::ptr_eq(&gpu.owner, &encoder.owner)
            || !src.belongs_to(&gpu.owner)
            || !dst.belongs_to(&gpu.owner)
            || self.scale != plan.scale()
            || dd.format != self.destination
            || dd.sample_count != self.samples
            || host_samples(plan.source()) != sd.sample_count
            || host_samples(plan.destination()) != dd.sample_count
            || self.source.multisampled() != (sd.sample_count > 1)
            || matches!(self.source, kernels::SrcKind::Depth { .. }) != plan.source().is_depth
            || !format_matches(plan.source(), sd.format)
            || !format_matches(plan.destination(), dd.format)
            || src.texture().id() == dst.texture().id()
        {
            return Err(Error::Rejected(
                "EDRAM transfer resource/key/device contract",
            ));
        }
        for v in [src, dst] {
            let r = v.range();
            if v.dimension() != V::D2
                || r.mip_level_count != Some(1)
                || r.array_layer_count != Some(1)
            {
                return Err(Error::Rejected("EDRAM transfer single-subresource views"));
            }
        }
        let source_size = src.texture().mip_size(src.range().base_mip_level)?;
        let dest_size = dst.texture().mip_size(dst.range().base_mip_level)?;
        let destination_usage = if plan.destination().is_depth {
            types::TextureUses::DEPTH_STENCIL_WRITE
        } else {
            types::TextureUses::COLOR_TARGET
        };
        if !sd.usage.contains(types::TextureUses::RESOURCE)
            || !dd.usage.contains(destination_usage)
            || dst.range().aspect != types::TextureAspect::All
        {
            return Err(Error::Rejected("EDRAM transfer resource usage/aspect"));
        }
        if [source_size.width, source_size.height] != plan.source_extent() {
            return Err(Error::Rejected("EDRAM source extent"));
        }
        if plan.source().is_depth {
            let stencil = images
                .source_stencil
                .ok_or(Error::Rejected("EDRAM source stencil missing"))?;
            let a = src.range();
            let b = stencil.range();
            if src.range().aspect != types::TextureAspect::DepthOnly
                || stencil.texture().id() != src.texture().id()
                || b.aspect != types::TextureAspect::StencilOnly
                || a.base_mip_level != b.base_mip_level
                || a.mip_level_count != b.mip_level_count
                || a.base_array_layer != b.base_array_layer
                || a.array_layer_count != b.array_layer_count
            {
                return Err(Error::Rejected("EDRAM source depth/stencil view pairing"));
            }
        } else if images.source_stencil.is_some() {
            return Err(Error::Rejected("unexpected source stencil"));
        }
        if plan.destination().is_depth {
            let scratch = images
                .stencil_scratch
                .ok_or(Error::Rejected("EDRAM stencil scratch missing"))?;
            let d = scratch.texture().descriptor();
            let r = scratch.range();
            let size = scratch.texture().mip_size(r.base_mip_level)?;
            if !scratch.belongs_to(&gpu.owner)
                || scratch.dimension() != V::D2
                || r.mip_level_count != Some(1)
                || r.array_layer_count != Some(1)
                || d.format != F::R32Uint
                || d.sample_count != self.samples
                || !d
                    .usage
                    .contains(types::TextureUses::RESOURCE | types::TextureUses::COLOR_TARGET)
                || [size.width, size.height] != [dest_size.width, dest_size.height]
                || scratch.texture().id() == src.texture().id()
                || scratch.texture().id() == dst.texture().id()
            {
                return Err(Error::Rejected("EDRAM stencil scratch contract"));
            }
        } else if images.stencil_scratch.is_some() {
            return Err(Error::Rejected("unexpected stencil scratch"));
        }
        let Some(scissor) = plan
            .scissor(dest_size.width, dest_size.height)
            .map_err(|_| Error::Rejected("EDRAM transfer clip"))?
        else {
            return Ok(TransferStats::default());
        };
        let mut entries = vec![
            Entry {
                binding: 0,
                resource: Resource::Buffer {
                    buffer: &params.buffer,
                    offset: 0,
                    size: 80,
                },
            },
            Entry {
                binding: 1,
                resource: Resource::Texture(src),
            },
        ];
        if let Some(stencil) = images.source_stencil {
            entries.push(Entry {
                binding: 2,
                resource: Resource::Texture(stencil),
            });
        }
        let group = gpu.bind_group(&self.layout, &entries)?;
        let offsets = [index as u32 * params.stride];
        let bound = [BoundGroup {
            group: &group,
            dynamic_offsets: &offsets,
        }];
        let bit_group = match &self.stencil {
            Some(Stencil::Bits { layout, .. }) => Some(gpu.bind_group(
                layout,
                &[Entry {
                    binding: 0,
                    resource: Resource::Texture(images.stencil_scratch.unwrap()),
                }],
            )?),
            _ => None,
        };
        let color = ColorAttachment {
            view: images.stencil_scratch.unwrap_or(dst),
            clear: self.stencil.as_ref().map(|_| types::Color::TRANSPARENT),
        };
        let stats = encoder.render_full(
            &[Some(color)],
            self.stencil.as_ref().map(|_| DepthAttachment {
                view: dst,
                clear_depth: None,
                clear_stencil: None,
            }),
            &[],
            &[Draw {
                pipeline: &self.main,
                groups: &bound,
                vertices: 0..3,
                instances: 0..1,
                indices: None,
                viewport: None,
                scissor: Some(scissor),
                blend: types::Color::TRANSPARENT,
                stencil_reference: 0,
            }],
        )?;
        let result = match &self.stencil {
            Some(Stencil::Bits { pipelines, .. }) => {
                let bound = [BoundGroup {
                    group: bit_group.as_ref().unwrap(),
                    dynamic_offsets: &[],
                }];
                let draws: Vec<_> = pipelines
                    .iter()
                    .map(|p| Draw {
                        pipeline: p,
                        groups: &bound,
                        vertices: 0..3,
                        instances: 0..1,
                        indices: None,
                        viewport: None,
                        scissor: Some(scissor),
                        blend: types::Color::TRANSPARENT,
                        stencil_reference: 255,
                    })
                    .collect();
                encoder
                    .render_full(
                        &[],
                        Some(DepthAttachment {
                            view: dst,
                            clear_depth: None,
                            clear_stencil: None,
                        }),
                        &[],
                        &draws,
                    )
                    .map(|s| s.draws)
            }
            #[cfg(all(target_vendor = "apple", feature = "metal-stencil-export"))]
            Some(Stencil::Metal(fast)) => fast
                .encode(
                    gpu,
                    encoder,
                    images.stencil_scratch.unwrap(),
                    dst,
                    Some(scissor),
                )
                .map(|s| s.draws),
            None => Ok(0),
        };
        // A late internal error cannot permit a legacy retry over partially encoded
        // ownership transfers. Latch failure; the caller must end this session.
        let stencil_draws = result.map_err(|e| {
            gpu.owner
                .fail_next(format!("EDRAM transfer after partial encoding: {e:?}"))
        })?;
        Ok(TransferStats {
            main_draws: stats.draws,
            stencil_draws,
        })
    }
}
