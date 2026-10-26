//! Read bounds are derived from the same immutable decoded shader which emits
//! the compiled source. A caller cannot assert a smaller prefix for a different
//! native pipeline, or mutate metadata after the proof is attached.
use super::*;
use crate::{
    bindings::PipelineLayout,
    edram::targets::{ConstantSlot, DrawArena},
    pipeline::PipelineCache,
    render::Pipeline,
    shader::Shader,
};
use xenos_shader::{ShaderStage, Translation, TranslationOptions, bindings};
// Reuse the independently tested public CPU proof, without linking the legacy
// WebGPU executor or maintaining a second copy of its addressing algorithm.
#[allow(dead_code)]
#[path = "../../../../skyrim-recomp-web/crates/xenos-render/src/vertex_range.rs"]
mod vertex_ranges;
const _: () =
    assert!(crate::edram::targets::GUEST_PAGE_BYTES == vertex_ranges::PAGE_BYTES as usize);

pub struct GuestShader {
    native: Shader<backend::Platform>,
    metadata: Translation,
    vertex_reads: Option<Rc<vertex_ranges::ShaderProof>>,
}
impl GuestShader {
    pub fn metadata(&self) -> &Translation {
        &self.metadata
    }
}
pub struct GuestPipeline {
    native: Pipeline<backend::Platform>,
    required: [usize; 5],
    vertex_reads: Option<Rc<vertex_ranges::ShaderProof>>,
    readonly_vertex_only: bool,
    pub(crate) textures: Vec<xenos_shader::TextureBinding>,
    pub(crate) texture_layout: Option<crate::bindings::GroupLayout<backend::Platform>>,
}
impl GuestPipeline {
    pub fn pipeline(&self) -> &Pipeline<backend::Platform> {
        &self.native
    }
    /// Shader-proven payload lengths, in producer stage order (VS, PS, bool,
    /// fetch, system), not binding order. The binding window remains unchanged.
    pub fn constant_bytes(&self) -> [usize; 5] {
        self.required
    }
    pub(crate) fn validate_captured_reads(
        &self,
        draw: &crate::edram::targets::ConstantDraw<'_>,
        captured: impl FnMut(u32) -> bool,
    ) -> Result<()> {
        self.validate_capture_parts(
            draw,
            draw.indices.is_some(),
            draw.indices.as_ref().map_or(0, |i| i.base_vertex),
            captured,
        )
    }
    pub(crate) fn validate_capture_parts(
        &self,
        draw: &crate::edram::targets::ConstantDraw<'_>,
        indexed: bool,
        base_vertex: i32,
        mut captured: impl FnMut(u32) -> bool,
    ) -> Result<()> {
        use vertex_ranges::{Entry, EntrySource, PAGE_BYTES};
        if !self.readonly_vertex_only {
            return Err(Error::Rejected(
                "native guest capture supports proven vertex reads only",
            ));
        }
        let proof = self
            .vertex_reads
            .as_ref()
            .ok_or(Error::Rejected("unproven guest vertex addressing"))?;
        if draw.constants[3].len() != bindings::FETCH_CONSTANTS_SIZE as usize
            || draw.constants[4].len() != std::mem::size_of::<bindings::SystemConstants>()
        {
            return Err(Error::Rejected("guest fetch/system capture size"));
        }
        let system = |offset: usize| {
            u32::from_le_bytes(draw.constants[4][offset..offset + 4].try_into().unwrap())
        };
        macro_rules! field {
            ($name:ident) => {
                system(std::mem::offset_of!(bindings::SystemConstants, $name))
            };
        }
        let domain = Entry {
            source: if indexed {
                EntrySource::DirectDma
            } else {
                EntrySource::Sequential
            },
            count: draw
                .vertices
                .end
                .checked_sub(draw.vertices.start)
                .ok_or(Error::Rejected("guest draw ranges"))?,
            offset: field!(vertex_base_index),
            min: field!(vertex_index_min),
            max: field!(vertex_index_max),
            first_vertex: draw.vertices.start,
            host_base_vertex: base_vertex,
            first_instance: draw.instances.start,
            instance_count: draw
                .instances
                .end
                .checked_sub(draw.instances.start)
                .ok_or(Error::Rejected("guest instance ranges"))?,
            index_endian: field!(vertex_index_endian),
            index_load: field!(flags) & bindings::flags::VERTEX_INDEX_LOAD != 0,
            expanded: false,
        }
        .domain()
        .ok_or(Error::Rejected("unproven guest vertex entry"))?;
        let fetch: [u32; 192] = std::array::from_fn(|i| {
            u32::from_le_bytes(draw.constants[3][i * 4..i * 4 + 4].try_into().unwrap())
        });
        let mut covered = true;
        proof
            .for_each_read_range(&fetch, domain, |range| {
                if covered && range.bytes != 0 {
                    let last = (range.address + range.bytes - 1) / PAGE_BYTES;
                    for page in range.address / PAGE_BYTES..=last {
                        if !captured(page * PAGE_BYTES) {
                            covered = false;
                            break;
                        }
                    }
                }
            })
            .map_err(|_| Error::Rejected("unproven guest physical read range"))?;
        if !covered {
            return Err(Error::Rejected(
                "guest draw reads outside its captured pages",
            ));
        }
        Ok(())
    }
}
pub(crate) fn xenos_slots() -> [ConstantSlot; 5] {
    use types::ShaderStages as S;
    let both = S::VERTEX | S::FRAGMENT;
    [
        ConstantSlot {
            binding: bindings::FLOAT_CONSTANTS_BINDING,
            visibility: S::VERTEX,
            storage: true,
            bytes: bindings::FLOAT_CONSTANTS_SIZE as u32,
        },
        ConstantSlot {
            binding: bindings::PIXEL_FLOAT_CONSTANTS_BINDING,
            visibility: S::FRAGMENT,
            storage: true,
            bytes: bindings::FLOAT_CONSTANTS_SIZE as u32,
        },
        ConstantSlot {
            binding: bindings::BOOL_LOOP_CONSTANTS_BINDING,
            visibility: both,
            storage: false,
            bytes: std::mem::size_of::<bindings::BoolLoopConstants>() as u32,
        },
        ConstantSlot {
            binding: bindings::FETCH_CONSTANTS_BINDING,
            visibility: both,
            storage: false,
            bytes: bindings::FETCH_CONSTANTS_SIZE as u32,
        },
        ConstantSlot {
            binding: bindings::SYSTEM_CONSTANTS_BINDING,
            visibility: both,
            storage: false,
            bytes: std::mem::size_of::<bindings::SystemConstants>() as u32,
        },
    ]
}
impl Gpu<backend::Platform> {
    pub fn xenos_draw_arena(&mut self, capacity: u64) -> Result<DrawArena> {
        self.draw_arena(capacity, xenos_slots())
    }
    pub fn guest_shader(
        &self,
        stage: ShaderStage,
        code: &[u32],
        options: &TranslationOptions,
    ) -> Result<GuestShader> {
        let decoded = xenos_shader::Shader::new(stage, code).map_err(backend_error)?;
        let mut metadata = decoded.translate(options);
        if metadata.uses_memexport || !metadata.warnings.is_empty() {
            return Err(Error::Rejected("unsupported guest shader semantics"));
        }
        let vertex_reads = (stage == ShaderStage::Vertex
            && options.vs_expansion == xenos_shader::VertexExpansion::None)
            .then(|| {
                vertex_ranges::ShaderProof::analyze(decoded.program())
                    .ok()
                    .map(Rc::new)
            })
            .flatten();
        let source = std::mem::take(&mut metadata.wgsl);
        let native = self.shader(&source)?;
        Ok(GuestShader {
            native,
            metadata,
            vertex_reads,
        })
    }
}
impl DrawArena {
    pub fn guest_pipeline(
        &self,
        gpu: &Gpu<backend::Platform>,
        cache: &mut PipelineCache<backend::Platform>,
        plan: &xenos_gfx::pipeline::Pipeline<types::TextureFormat>,
        layout: &PipelineLayout<backend::Platform>,
        vertex: &GuestShader,
        fragment: Option<&GuestShader>,
    ) -> Result<GuestPipeline> {
        if !self.is_xenos()
            || layout
                .0
                .groups
                .first()
                .is_none_or(|g| !g.compatible(self.layout()))
            || vertex.metadata.stage != ShaderStage::Vertex
            || fragment.is_some_and(|f| f.metadata.stage != ShaderStage::Pixel)
        {
            return Err(Error::Rejected("guest shader constant/stage contract"));
        }
        let native = if let Some(fragment) = fragment {
            cache.get_or_create(
                gpu,
                plan,
                layout,
                &vertex.native,
                vertex.metadata.entry_point,
                &fragment.native,
                fragment.metadata.entry_point,
            )?
        } else {
            cache.get_or_create_depth(
                gpu,
                plan,
                layout,
                &vertex.native,
                vertex.metadata.entry_point,
            )?
        };
        let reads = [
            vertex.metadata.info.float_constants_read(),
            fragment.map_or(0, |f| f.metadata.info.float_constants_read()),
        ];
        if reads.iter().any(|&n| n > 256) {
            return Err(Error::Rejected("invalid shader constant read bound"));
        }
        let mut required = xenos_slots().map(|s| s.bytes as usize);
        required[0] = reads[0] as usize * 16;
        required[1] = reads[1] as usize * 16;
        let readonly_vertex_only = vertex.metadata.texture_bindings.is_empty()
            && fragment.is_none_or(|f| !f.metadata.uses_shared_memory);
        Ok(GuestPipeline {
            native,
            required,
            vertex_reads: vertex.vertex_reads.clone(),
            readonly_vertex_only,
            textures: fragment.map_or_else(Vec::new, |f| f.metadata.texture_bindings.clone()),
            texture_layout: layout.0.groups.get(2).cloned(),
        })
    }
}
