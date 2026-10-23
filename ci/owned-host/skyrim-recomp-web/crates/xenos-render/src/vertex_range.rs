//! Conservative synchronization ranges for a restricted vertex shader subset.
//! A proof is cached per decoded shader; current fetch constants are evaluated
//! per draw. Rejection means use the original full-buffer synchronization.
#[cfg(test)]
use std::collections::BTreeMap;
use xenos_shader::parsed::{
    AddressingMode, CfCondition, InstructionResult, ParsedCf, StorageSource, StorageTarget, SwizzleSource,
};
use xenos_shader::program::Instruction;
use xenos_shader::ucode::vertex_format;
use xenos_shader::{Program, Shader, ShaderInfo, ShaderStage};

pub const PAGE_BYTES: u32 = 4096;
pub const PHYSICAL_BYTES: u32 = 1 << 29;
// Every integer and its optional +0.5 is exactly representable below 2^23.
const MAX_INDEX: u32 = (1 << 23) - 1;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct IndexDomain {
    low: u32,
    high: u32,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum EntrySource {
    Sequential,
    DirectDma,
    Converted,
}

/// Facts about the actual host draw and its WGSL entry. Converted indices and
/// expanded draws retain full synchronization; no CPU index scan is performed.
#[derive(Clone, Copy, Debug)]
pub struct Entry {
    pub source: EntrySource,
    pub count: u32,
    pub offset: u32,
    pub min: u32,
    pub max: u32,
    pub first_vertex: u32,
    pub host_base_vertex: i32,
    pub first_instance: u32,
    pub instance_count: u32,
    pub index_endian: u32,
    pub index_load: bool,
    pub expanded: bool,
}

impl Entry {
    pub fn domain(self) -> Option<IndexDomain> {
        if self.expanded
            || self.index_load
            || self.first_vertex != 0
            || self.host_base_vertex != 0
            || self.first_instance != 0
            || self.instance_count != 1
            || self.count == 0
            || self.index_endian > 3
        {
            return None;
        }
        match self.source {
            EntrySource::Sequential if self.index_endian == 0 => {
                IndexDomain::nonindexed(self.count, self.offset, self.min, self.max, false).ok()
            }
            EntrySource::DirectDma => IndexDomain::direct_clamped(self.min, self.max, false).ok(),
            _ => None,
        }
    }
}

impl IndexDomain {
    /// Mirrors the WGSL entry's monotone nonwrapping subset. Caller must prove
    /// nonindexed, no expansion, and a raw host vertex interval starting at 0.
    pub fn nonindexed(count: u32, base: u32, min: u32, max: u32, expanded: bool) -> Result<Self, &'static str> {
        if expanded || count == 0 || min > max {
            return Err("draw_shape");
        }
        let high = (count - 1).checked_add(base).ok_or("index_wrap")?;
        if high > i32::MAX as u32 {
            return Err("signed_index");
        }
        Self::bounded(base.clamp(min, max), high.clamp(min, max))
    }

    /// A direct DMA index may have ANY value, including a later GPU-produced
    /// value. The actual unsigned WGSL clamp alone establishes these bounds.
    /// No CPU index snapshot, endian assumption, restart or min/max scan is used.
    pub fn direct_clamped(min: u32, max: u32, expanded: bool) -> Result<Self, &'static str> {
        if expanded {
            return Err("expansion");
        }
        Self::bounded(min, max)
    }

    fn bounded(low: u32, high: u32) -> Result<Self, &'static str> {
        if low > high || high > MAX_INDEX {
            return Err("index_precision_or_order");
        }
        Ok(Self { low, high })
    }

    #[cfg(test)]
    pub fn limits(self) -> (u32, u32) {
        (self.low, self.high)
    }
}

#[derive(Clone, Copy, Debug)]
struct Fetch {
    constant: u32,
    stride: u32,
    offset: u32,
    needed: u32,
}

#[derive(Debug)]
pub struct ShaderProof {
    fetches: Vec<Fetch>,
}

fn invalidates_entry_x(result: &InstructionResult) -> bool {
    result.target == StorageTarget::Register
        && result.used_write_mask() & 1 != 0
        && (result.addressing != AddressingMode::Absolute || result.index == 0)
}

impl ShaderProof {
    pub fn analyze(program: &Program) -> Result<Self, &'static str> {
        if program.stage != ShaderStage::Vertex {
            return Err("not_vertex");
        }
        let mut entry_x = true;
        let mut full = None;
        let mut fetches = Vec::new();
        for block in &program.cf {
            let exec = match block.cf {
                ParsedCf::Nop | ParsedCf::Alloc { .. } | ParsedCf::MarkVsFetchDone => continue,
                ParsedCf::Exec(exec) if exec.condition == CfCondition::Always => exec,
                _ => return Err("control_flow"),
            };
            for instruction in &block.instructions {
                match &instruction.instruction {
                    Instruction::Alu(alu) => {
                        if [alu.vector_result.target, alu.scalar_result.target]
                            .iter()
                            .any(|t| matches!(t, StorageTarget::ExportAddress | StorageTarget::ExportData))
                        {
                            return Err("memexport");
                        }
                        entry_x &= !invalidates_entry_x(&alu.vector_result) && !invalidates_entry_x(&alu.scalar_result);
                    }
                    Instruction::TextureFetch(_) => return Err("texture_fetch"),
                    Instruction::VertexFetch(vf) => {
                        if vf.is_predicated {
                            return Err("predicated_fetch");
                        }
                        let a = vf.attributes;
                        if vf.fetch_constant >= 96 || a.offset < 0 {
                            return Err("fetch_bounds");
                        }
                        if !vf.is_mini_fetch {
                            let index = vf.index_operand;
                            if a.stride != 0
                                && !(entry_x
                                    && index.source == StorageSource::Register
                                    && index.index == 0
                                    && index.addressing == AddressingMode::Absolute
                                    && index.component(0) == SwizzleSource::X
                                    && !index.is_negated
                                    && !index.is_absolute_value)
                            {
                                return Err("unproven_index");
                            }
                            full = Some((vf.fetch_constant, a.stride));
                        }
                        // A mini fetch reuses the last EXECUTED full address,
                        // even if an intervening instruction overwrites r0.x.
                        if full != Some((vf.fetch_constant, a.stride)) {
                            return Err("mini_state");
                        }
                        let needed = vertex_format::needed_words(a.data_format, vf.result.used_result_components());
                        if needed != 0 {
                            fetches.push(Fetch {
                                constant: vf.fetch_constant,
                                stride: a.stride,
                                offset: a.offset as u32,
                                needed,
                            });
                        }
                        entry_x &= !invalidates_entry_x(&vf.result);
                    }
                }
            }
            if exec.is_end {
                break;
            }
        }
        Ok(Self { fetches })
    }

    /// One conservative envelope per fetch constant. Sparse holes may remain;
    /// all potentially executed word loads are included. Current constants are
    /// snapshotted by the renderer's existing register ownership, not cached here.
    #[cfg(test)]
    pub fn ranges(&self, constants: &[u32; 192], domain: IndexDomain) -> Result<Vec<Range>, &'static str> {
        let mut merged = BTreeMap::<u32, (u64, u64)>::new();
        for f in &self.fetches {
            let first = constants[(2 * f.constant) as usize];
            let second = constants[(2 * f.constant + 1) as usize];
            let base = u64::from(first >> 2);
            // Shader masks to 27 physical dword bits; require unambiguous identity.
            if base >= u64::from(PHYSICAL_BYTES / 4) {
                return Err("physical_alias");
            }
            let end = base + u64::from((second >> 2) & 0xFF_FFFF);
            if end > u64::from(PHYSICAL_BYTES / 4) {
                return Err("physical_extent");
            }
            let low = base
                + u64::from(domain.low) * u64::from(f.stride)
                + u64::from(f.offset)
                + u64::from(f.needed.trailing_zeros());
            let high = base
                + u64::from(domain.high) * u64::from(f.stride)
                + u64::from(f.offset)
                + u64::from(32 - f.needed.leading_zeros());
            // Reject all i32 arithmetic-wrap cases, even if clamping might make
            // a particular invocation return zero. Never model modulo addresses.
            if high > i32::MAX as u64 {
                return Err("address_wrap");
            }
            if low >= end {
                continue;
            } // Actual vfetch returns zero, reads nothing.
            let high = high.min(end);
            let span = merged.entry(f.constant).or_insert((low, high));
            span.0 = span.0.min(low);
            span.1 = span.1.max(high);
        }
        Ok(merged
            .into_iter()
            .map(|(constant, (low, high))| Range {
                constant,
                address: (low * 4) as u32,
                bytes: ((high - low) * 4) as u32,
            })
            .collect())
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Range {
    pub constant: u32,
    pub address: u32,
    pub bytes: u32,
}
impl Range {
    pub fn pages(self) -> u32 {
        if self.bytes == 0 { 0 } else { (self.address + self.bytes - 1) / PAGE_BYTES - self.address / PAGE_BYTES + 1 }
    }
    #[cfg(test)]
    pub fn contains_word(self, word: u32) -> bool {
        let byte = u64::from(word) * 4;
        byte >= u64::from(self.address) && byte + 4 <= u64::from(self.address) + u64::from(self.bytes)
    }
}

const MINIMUM_DECLARED_PAGES: u32 = 16;
const MINIMUM_SAVED_PAGES: u32 = 8;

#[derive(Debug)]
struct Term {
    stride: u32,
    low_offset: u32,
    high_offset: u32,
}
#[derive(Debug)]
struct Binding {
    constant: u32,
    terms: Vec<Term>,
}

/// Construct once from the immutable decoded shader, alongside translation.
/// Only this constructor allocates. The evaluator has no shader/CFG input.
#[derive(Debug)]
pub struct CachedPlan {
    bindings: Vec<Binding>,
}

/// Binding observations at the synchronization stage, not GPU execution counts.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Work {
    pub bindings: u32,
    pub eligible: u32,
    pub narrowed: u32,
    pub avoided_pages: u64,
    pub evaluated_terms: u32,
}

impl CachedPlan {
    /// ShaderCache::build entry: reuse its already decoded/analyzed
    /// Shader. This does not translate or decode it a second time.
    pub fn from_shader(shader: &Shader) -> Result<Self, &'static str> {
        Self::from_parts(shader.program(), shader.info())
    }

    /// Convenience for the isolated decoded-program oracle.
    #[cfg(test)]
    pub fn analyze(program: &Program) -> Result<Self, &'static str> {
        Self::from_parts(program, &ShaderInfo::analyze(program))
    }

    fn from_parts(program: &Program, info: &ShaderInfo) -> Result<Self, &'static str> {
        let proof = ShaderProof::analyze(program)?;
        // Reuse the same shader's existing metadata.
        // Retain its original order and even zero-read/unreachable bindings, so
        // every refusal still emits the original baseline sequence unchanged.
        let mut bindings: Vec<_> =
            info.vertex_bindings.iter().map(|b| Binding { constant: b.fetch_constant, terms: Vec::new() }).collect();
        if bindings.iter().any(|b| b.constant >= 96) {
            return Err("fetch_bounds");
        }
        for fetch in proof.fetches {
            let binding = bindings.iter_mut().find(|b| b.constant == fetch.constant).ok_or("metadata_mismatch")?;
            let low_offset = fetch.offset + fetch.needed.trailing_zeros();
            let high_offset = fetch.offset + 32 - fetch.needed.leading_zeros();
            if let Some(term) = binding.terms.iter_mut().find(|t| t.stride == fetch.stride) {
                term.low_offset = term.low_offset.min(low_offset);
                term.high_offset = term.high_offset.max(high_offset);
            } else {
                binding.terms.push(Term { stride: fetch.stride, low_offset, high_offset });
            }
        }
        Ok(Self { bindings })
    }

    /// `domain=None` means the caller could not prove the host entry mapping.
    /// Emit exact baseline ranges in that case. Caller appends to its existing
    /// range storage; this method creates no collection and never reads memory.
    /// A range is narrowed only if at least eight and half its pages disappear.
    pub fn for_each_range(
        &self,
        constants: &[u32; 192],
        domain: Option<IndexDomain>,
        mut emit: impl FnMut(Range),
    ) -> Work {
        let mut work = Work::default();
        for binding in &self.bindings {
            work.bindings += 1;
            let first = constants[2 * binding.constant as usize];
            let second = constants[2 * binding.constant as usize + 1];
            let original =
                Range { constant: binding.constant, address: first & !3, bytes: ((second >> 2) & 0xFF_FFFF) * 4 };
            // Avoid even envelope/page arithmetic for unproven/aliased extents.
            if domain.is_none()
                || original.address >= PHYSICAL_BYTES
                || u64::from(original.address) + u64::from(original.bytes) > u64::from(PHYSICAL_BYTES)
            {
                emit(original);
                continue;
            }
            let pages = original.pages();
            if pages < MINIMUM_DECLARED_PAGES {
                emit(original);
                continue;
            }
            work.eligible += 1;
            let d = domain.expect("checked above");
            let base = u64::from(original.address / 4);
            let end = base + u64::from(original.bytes / 4);
            let mut span: Option<(u64, u64)> = None;
            let mut fallback = false;
            for term in &binding.terms {
                work.evaluated_terms += 1;
                let low = base + u64::from(d.low) * u64::from(term.stride) + u64::from(term.low_offset);
                let high = base + u64::from(d.high) * u64::from(term.stride) + u64::from(term.high_offset);
                if high > i32::MAX as u64 {
                    fallback = true;
                    break;
                }
                if low >= end {
                    continue;
                }
                let high = high.min(end);
                span = Some(match span {
                    Some((a, b)) => (a.min(low), b.max(high)),
                    None => (low, high),
                });
                let (low, high) = span.expect("just assigned");
                let candidate =
                    Range { constant: binding.constant, address: (low * 4) as u32, bytes: ((high - low) * 4) as u32 };
                let used = candidate.pages();
                // Adding another term can only widen this envelope. Once it
                // cannot save enough, skip every remaining term immediately.
                if used * 2 > pages || pages - used < MINIMUM_SAVED_PAGES {
                    fallback = true;
                    break;
                }
            }
            if fallback {
                emit(original);
                continue;
            }
            work.narrowed += 1;
            if let Some((low, high)) = span {
                let candidate =
                    Range { constant: binding.constant, address: (low * 4) as u32, bytes: ((high - low) * 4) as u32 };
                work.avoided_pages += u64::from(pages - candidate.pages());
                emit(candidate);
            } else {
                work.avoided_pages += u64::from(pages);
            }
        }
        work
    }
}

#[cfg(test)]
#[path = "vertex_range_tests.rs"]
mod tests;
