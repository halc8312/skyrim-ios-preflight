//! Native projection of the SAME normalized Xenos pipeline used by the legacy
//! Executor. A cache hit includes actual immutable shader/layout identity, not
//! just a caller-supplied guest shader number. No foreign queue or raw handle.
use super::*;
use crate::{
    bindings::PipelineLayout,
    render::{Pipeline, PipelineDescriptor},
    shader::Shader,
};
use std::collections::HashMap;
use xenos_gfx::pipeline as plan;
use xenos_state::{
    blend::{HostBlendFactor, HostBlendOperation},
    primitive::HostTopology,
    viewport::{CullMode, FrontFace},
    xenos,
};

pub struct Projection {
    pub colors: Vec<Option<types::ColorTargetState>>,
    pub depth: Option<types::DepthStencilState>,
    pub primitive: types::PrimitiveState,
    pub multisample: types::MultisampleState,
}
pub fn project(key: &plan::Pipeline<types::TextureFormat>) -> Result<Projection> {
    if key.color_slots > 4 || key.colors[key.color_slots..].iter().any(Option::is_some) {
        return Err(Error::Rejected("normalized pipeline color slots"));
    }
    let colors = key.colors[..key.color_slots]
        .iter()
        .map(|c| {
            c.as_ref()
                .map(|c| {
                    Ok(types::ColorTargetState {
                        format: c.format,
                        blend: c.blend.map(|[color, alpha]| {
                            let component =
                                |c: xenos_state::blend::BlendComponent| types::BlendComponent {
                                    src_factor: blend_factor(c.src_factor),
                                    dst_factor: blend_factor(c.dst_factor),
                                    operation: blend_op(c.operation),
                                };
                            types::BlendState {
                                color: component(color),
                                alpha: component(alpha),
                            }
                        }),
                        write_mask: types::ColorWrites::from_bits(c.write_mask)
                            .ok_or(Error::Rejected("pipeline color write mask"))?,
                    })
                })
                .transpose()
        })
        .collect::<Result<Vec<_>>>()?;
    let face = |s: xenos_state::depth_stencil::StencilFaceState| types::StencilFaceState {
        compare: compare(s.compare),
        fail_op: stencil(s.fail_op),
        depth_fail_op: stencil(s.depth_fail_op),
        pass_op: stencil(s.pass_op),
    };
    let depth = key.depth.as_ref().map(|d| types::DepthStencilState {
        format: d.format,
        depth_write_enabled: Some(d.write),
        depth_compare: Some(compare(d.compare)),
        stencil: types::StencilState {
            front: face(d.stencil[0]),
            back: face(d.stencil[1]),
            read_mask: d.read_mask,
            write_mask: d.write_mask,
        },
        bias: types::DepthBiasState {
            constant: d.bias,
            slope_scale: f32::from_bits(d.slope_scale_bits),
            clamp: 0.,
        },
    });
    let primitive = types::PrimitiveState {
        topology: match key.topology {
            HostTopology::PointList => types::PrimitiveTopology::PointList,
            HostTopology::LineList => types::PrimitiveTopology::LineList,
            HostTopology::LineStrip => types::PrimitiveTopology::LineStrip,
            HostTopology::TriangleList => types::PrimitiveTopology::TriangleList,
            HostTopology::TriangleStrip => types::PrimitiveTopology::TriangleStrip,
        },
        strip_index_format: match key.strip_index_format {
            plan::IndexWidth::None => None,
            plan::IndexWidth::U16 => Some(types::IndexFormat::Uint16),
            plan::IndexWidth::U32 => Some(types::IndexFormat::Uint32),
        },
        front_face: match key.front_face {
            FrontFace::Ccw => types::FrontFace::Ccw,
            FrontFace::Cw => types::FrontFace::Cw,
        },
        cull_mode: match key.cull {
            CullMode::None => None,
            CullMode::Front => Some(types::Face::Front),
            CullMode::Back => Some(types::Face::Back),
        },
        ..Default::default()
    };
    // The existing CPU contract uses u32; HAL's all-samples default is u64.
    let mask = if key.sample_mask == u32::MAX {
        !0
    } else {
        u64::from(key.sample_mask)
    };
    Ok(Projection {
        colors,
        depth,
        primitive,
        multisample: types::MultisampleState {
            count: key.sample_count,
            mask,
            alpha_to_coverage_enabled: key.alpha_to_coverage,
        },
    })
}

#[derive(Clone, PartialEq, Eq, Hash)]
struct Key {
    plan: plan::Pipeline<types::TextureFormat>,
    layout: u64,
    vertex: u64,
    fragment: Option<(u64, usize)>,
    vs: usize,
}
struct Cached<B: Backend> {
    pipeline: Pipeline<B>,
    used: u64,
}
#[derive(Default, Clone, Copy, Debug)]
pub struct CacheStats {
    pub hits: u64,
    pub created: u64,
    pub evicted: u64,
    pub live: usize,
}
pub struct PipelineCache<B: Backend> {
    entries: HashMap<Key, Cached<B>>,
    capacity: usize,
    clock: u64,
    stats: CacheStats,
}
impl<B: Backend> PipelineCache<B> {
    pub fn new(capacity: usize) -> Result<Self> {
        if capacity == 0 || capacity > 4096 {
            return Err(Error::Rejected("pipeline cache capacity"));
        }
        Ok(Self {
            entries: HashMap::with_capacity(capacity),
            capacity,
            clock: 0,
            stats: CacheStats::default(),
        })
    }
    pub fn clear(&mut self) {
        self.entries.clear();
    }
    pub fn stats(&self) -> CacheStats {
        CacheStats {
            live: self.entries.len(),
            ..self.stats
        }
    }
    pub fn get_or_create_depth(
        &mut self,
        gpu: &Gpu<B>,
        plan: &plan::Pipeline<types::TextureFormat>,
        layout: &PipelineLayout<B>,
        vertex: &Shader<B>,
        vertex_entry: &str,
    ) -> Result<Pipeline<B>> {
        self.get(gpu, plan, layout, vertex, vertex_entry, None)
    }
    pub fn get_or_create(
        &mut self,
        gpu: &Gpu<B>,
        plan: &plan::Pipeline<types::TextureFormat>,
        layout: &PipelineLayout<B>,
        vertex: &Shader<B>,
        vertex_entry: &str,
        fragment: &Shader<B>,
        fragment_entry: &str,
    ) -> Result<Pipeline<B>> {
        self.get(
            gpu,
            plan,
            layout,
            vertex,
            vertex_entry,
            Some((fragment, fragment_entry)),
        )
    }
    fn get(
        &mut self,
        gpu: &Gpu<B>,
        plan: &plan::Pipeline<types::TextureFormat>,
        layout: &PipelineLayout<B>,
        vertex: &Shader<B>,
        vertex_entry: &str,
        fragment: Option<(&Shader<B>, &str)>,
    ) -> Result<Pipeline<B>> {
        gpu.owner.healthy()?;
        if !Rc::ptr_eq(&gpu.owner, &layout.0.owner)
            || !Rc::ptr_eq(&gpu.owner, &vertex.0.owner)
            || fragment.is_some_and(|(f, _)| !Rc::ptr_eq(&gpu.owner, &f.0.owner))
        {
            return Err(Error::Rejected("pipeline cache device"));
        }
        if plan.pixel.is_some() != fragment.is_some() {
            return Err(Error::Rejected("normalized fragment stage presence"));
        }
        let key = Key {
            plan: plan.clone(),
            layout: layout.id(),
            vertex: vertex.id(),
            vs: vertex.entry(vertex_entry, naga::ShaderStage::Vertex)?,
            fragment: fragment
                .map(|(f, entry)| {
                    f.entry(entry, naga::ShaderStage::Fragment)
                        .map(|index| (f.id(), index))
                })
                .transpose()?,
        };
        self.clock = self
            .clock
            .checked_add(1)
            .ok_or(Error::Rejected("pipeline cache clock exhaustion"))?;
        if let Some(entry) = self.entries.get_mut(&key) {
            entry.used = self.clock;
            self.stats.hits += 1;
            return Ok(entry.pipeline.clone());
        }
        let projected = project(plan)?;
        let pipeline = gpu.render_pipeline(&PipelineDescriptor {
            layout,
            vertex,
            vertex_entry,
            fragment,
            colors: &projected.colors,
            primitive: projected.primitive,
            depth_stencil: projected.depth,
            multisample: projected.multisample,
        })?;
        if self.entries.len() == self.capacity {
            let oldest = self
                .entries
                .iter()
                .min_by_key(|(_, e)| e.used)
                .map(|(k, _)| k.clone())
                .unwrap();
            self.entries.remove(&oldest);
            self.stats.evicted += 1;
        }
        self.entries.insert(
            key,
            Cached {
                pipeline: pipeline.clone(),
                used: self.clock,
            },
        );
        self.stats.created += 1;
        Ok(pipeline)
    }
}

fn compare(f: xenos::CompareFunction) -> types::CompareFunction {
    use types::CompareFunction as H;
    use xenos::CompareFunction as X;
    match f {
        X::Never => H::Never,
        X::Less => H::Less,
        X::Equal => H::Equal,
        X::LessEqual => H::LessEqual,
        X::Greater => H::Greater,
        X::NotEqual => H::NotEqual,
        X::GreaterEqual => H::GreaterEqual,
        X::Always => H::Always,
    }
}
fn stencil(f: xenos::StencilOp) -> types::StencilOperation {
    use types::StencilOperation as H;
    use xenos::StencilOp as X;
    match f {
        X::Keep => H::Keep,
        X::Zero => H::Zero,
        X::Replace => H::Replace,
        X::IncrementClamp => H::IncrementClamp,
        X::DecrementClamp => H::DecrementClamp,
        X::Invert => H::Invert,
        X::IncrementWrap => H::IncrementWrap,
        X::DecrementWrap => H::DecrementWrap,
    }
}
fn blend_factor(f: HostBlendFactor) -> types::BlendFactor {
    use HostBlendFactor as X;
    use types::BlendFactor as H;
    match f {
        X::Zero => H::Zero,
        X::One => H::One,
        X::Src => H::Src,
        X::OneMinusSrc => H::OneMinusSrc,
        X::SrcAlpha => H::SrcAlpha,
        X::OneMinusSrcAlpha => H::OneMinusSrcAlpha,
        X::Dst => H::Dst,
        X::OneMinusDst => H::OneMinusDst,
        X::DstAlpha => H::DstAlpha,
        X::OneMinusDstAlpha => H::OneMinusDstAlpha,
        X::SrcAlphaSaturated => H::SrcAlphaSaturated,
        X::Constant => H::Constant,
        X::OneMinusConstant => H::OneMinusConstant,
    }
}
fn blend_op(f: HostBlendOperation) -> types::BlendOperation {
    use HostBlendOperation as X;
    use types::BlendOperation as H;
    match f {
        X::Add => H::Add,
        X::Subtract => H::Subtract,
        X::ReverseSubtract => H::ReverseSubtract,
        X::Min => H::Min,
        X::Max => H::Max,
    }
}
