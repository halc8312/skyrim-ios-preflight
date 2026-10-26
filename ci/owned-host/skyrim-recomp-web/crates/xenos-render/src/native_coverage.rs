//! Opt-in, numeric-only coverage of ACTUAL translated draw programs. Archive
//! shaders may still have declaration-dependent placeholders, so they cannot
//! certify the commands the game eventually submits. No extra guest RAM reads,
//! shader bytes, path, hash or register values leave this module in its log.
use crate::prepared_shader::PreparedShader;
use crate::vertex_range::{Entry, EntrySource, ShaderProof};
use std::collections::{BTreeMap, BTreeSet};
use xenos_shader::bindings::{SystemConstants, flags};

#[derive(Default)]
pub(crate) struct Coverage {
    vertices: BTreeMap<u64, Result<ShaderProof, &'static str>>,
    pixels: BTreeSet<u64>,
    unique: BTreeMap<&'static str, u64>,
    draws: BTreeMap<&'static str, u64>,
}
impl Coverage {
    pub fn from_environment() -> Option<Self> {
        (std::env::var("SKYRIM_NATIVE_COVERAGE").as_deref() == Ok("1")).then(Self::default)
    }
    #[allow(clippy::too_many_arguments)]
    pub fn observe(
        &mut self,
        vertex_id: u64,
        vertex: &PreparedShader,
        pixel: Option<(u64, &PreparedShader)>,
        system: &SystemConstants,
        fetch: &[u32; 192],
        count: u32,
        indexed: bool,
        expanded: bool,
    ) {
        let proof = self.vertices.entry(vertex_id).or_insert_with(|| {
            let result = ShaderProof::analyze(vertex.shader().program());
            *self.unique.entry(result.as_ref().map_or_else(|e| *e, |_| "vertex_proven")).or_default() += 1;
            if !vertex.metadata().warnings.is_empty() {
                *self.unique.entry("vertex_semantic_warning").or_default() += 1;
            }
            result
        });
        if let Some((id, shader)) = pixel {
            if self.pixels.insert(id) && !shader.metadata().warnings.is_empty() {
                *self.unique.entry("pixel_semantic_warning").or_default() += 1;
            }
        }
        let count_kind = |map: &mut BTreeMap<&'static str, u64>, key| *map.entry(key).or_default() += 1;
        count_kind(&mut self.draws, "total");
        if expanded { count_kind(&mut self.draws, "expanded"); }
        if system.flags & flags::VERTEX_INDEX_LOAD != 0 { count_kind(&mut self.draws, "shader_index_load"); }
        let semantics = vertex.metadata().warnings.is_empty()
            && !vertex.metadata().uses_memexport
            && pixel.is_none_or(|(_, p)| p.metadata().warnings.is_empty() && !p.metadata().uses_memexport);
        if !semantics { count_kind(&mut self.draws, "unsupported_semantics"); }
        let readonly = vertex.metadata().texture_bindings.is_empty()
            && pixel.is_none_or(|(_, p)| !p.metadata().uses_shared_memory);
        if !readonly { count_kind(&mut self.draws, "outside_vertex_readonly_contract"); }
        let domain = Entry {
            source: if indexed { EntrySource::DirectDma } else { EntrySource::Sequential },
            count, offset: system.vertex_base_index as u32,
            min: system.vertex_index_min, max: system.vertex_index_max,
            first_vertex: 0, host_base_vertex: 0, first_instance: 0, instance_count: 1,
            index_endian: system.vertex_index_endian,
            index_load: system.flags & flags::VERTEX_INDEX_LOAD != 0, expanded,
        }.domain();
        match (proof, domain) {
            (Err(reason), _) => count_kind(&mut self.draws, reason),
            (_, None) => count_kind(&mut self.draws, "entry_domain"),
            (Ok(proof), Some(domain)) => match proof.for_each_read_range(fetch, domain, |_| {}) {
                Err(reason) => count_kind(&mut self.draws, reason),
                Ok(()) => {
                    count_kind(&mut self.draws, "read_recipe_proven");
                    if semantics && readonly {
                        count_kind(&mut self.draws, "closed_shader_entry_recipe");
                    }
                }
            },
        }
    }
    pub fn report(&self, frame: u64) {
        log::info!("native-coverage frame={frame} unique_vs={} unique_ps={} unique={:?} draws={:?}; recipe_only_not_full_backend",
            self.vertices.len(), self.pixels.len(), self.unique, self.draws);
    }
}
impl Drop for Coverage {
    fn drop(&mut self) { self.report(u64::MAX); }
}

#[cfg(test)]
mod tests {
    use super::*;
    const VS: [u32; 12] = [0x00013001, 0x00002200, 0, 0x00081000, 0x00390A88, 3,
        0xC80F803E, 0, 0xE2010100, 0xC80F8000, 0, 0x22000000];
    #[test]
    fn authored_draw_count_and_unique_shaders_remain_distinct() {
        let (vertex, _) = PreparedShader::new(xenos_shader::ShaderStage::Vertex, &VS, &Default::default()).unwrap();
        let mut coverage = Coverage::default();
        let mut system = SystemConstants::default();
        system.vertex_index_max = 2;
        for _ in 0..3 {
            coverage.observe(1, &vertex, None, &system, &[0;192], 3, false, false);
        }
        coverage.observe(1, &vertex, None, &system, &[0;192], 3, false, true);
        assert_eq!(coverage.vertices.len(), 1);
        assert_eq!(coverage.draws["total"], 4);
        assert_eq!(coverage.draws["entry_domain"], 1);
        assert_eq!(coverage.draws["closed_shader_entry_recipe"], 3);
    }
}
