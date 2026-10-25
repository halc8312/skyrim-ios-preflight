//! Ordered, immutable GPU-produced color/depth versions, independent of APIs.
//!
//! Each target has exactly one producer. A load copies an earlier stored version
//! into a fresh target before the pass; it never mutates that earlier version.
//! Copies initialize the whole destination from a bounded source rectangle.
//! This bounded conformance slice does not model EDRAM aliasing, resolve/MSAA,
//! guest fences, cross-frame ownership, or production allocation/reuse.
use crate::{
    frame_plan::{BufferVersion, Error, FramePlan, MAX_PACKET, Pass, Version},
    sampled_frame as sf,
};
use std::collections::{BTreeMap, BTreeSet};

pub const MAX_TARGETS: usize = 32;
pub const MAX_OPERATIONS: usize = 64;
pub const MAX_GPU_BYTES: u64 = 64 << 20;

#[path = "target_lifetime.rs"]
mod lifetime;
pub use lifetime::{AllocationAudit, AllocationPlan, Slot};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum Kind {
    Color = 1,
    Depth = 2,
}
#[derive(Clone, Copy, Debug)]
pub struct TargetVersion {
    pub version: Version,
    pub kind: Kind,
    pub width: u32,
    pub height: u32,
}
#[derive(Clone, Copy, Debug)]
pub struct ImageSlice {
    pub version: Version,
    pub mip_level: u32,
}
#[derive(Clone, Copy, Debug)]
pub struct Render {
    pub pass: u32,
    pub color: Version,
    pub depth: Version,
    pub color_load: Option<Version>,
    pub depth_load: Option<Version>,
    pub store_color: bool,
    pub store_depth: bool,
}
#[derive(Clone, Copy, Debug)]
pub struct Copy {
    pub source: ImageSlice,
    pub origin: [u32; 2],
    /// Destination dimensions define the copied extent, starting at (0,0).
    pub destination: Version,
}
#[derive(Clone, Copy, Debug)]
pub enum Operation {
    Render(Render),
    Copy(Copy),
}

/// Owned geometry whose attachment initialization is proved by TargetFrame's
/// ordered operations. It cannot be rendered as a standalone FramePlan unless
/// its first pass clears both attachments.
#[derive(Debug)]
pub struct Geometry {
    pub width: u32,
    pub height: u32,
    pub resources: Vec<BufferVersion>,
    pub passes: Vec<Pass>,
}

#[derive(Debug)]
pub struct TargetFrame {
    inputs: sf::SampledFrame,
    targets: Vec<TargetVersion>,
    operations: Vec<Operation>,
    bindings: Vec<Vec<sf::Binding>>,
    output: Version,
}
impl TargetFrame {
    pub fn from_geometry(
        geometry: Geometry,
        textures: Vec<sf::TextureVersion>,
        samplers: Vec<sf::SamplerVersion>,
        targets: Vec<TargetVersion>,
        operations: Vec<Operation>,
        bindings: Vec<Vec<sf::Binding>>,
        output: Version,
    ) -> Result<Self, Error> {
        let plan = FramePlan::target_geometry(geometry.width, geometry.height, geometry.resources, geometry.passes)?;
        Self::new(plan, textures, samplers, targets, operations, bindings, output)
    }
    pub fn new(
        geometry: FramePlan,
        textures: Vec<sf::TextureVersion>,
        samplers: Vec<sf::SamplerVersion>,
        targets: Vec<TargetVersion>,
        operations: Vec<Operation>,
        bindings: Vec<Vec<sf::Binding>>,
        output: Version,
    ) -> Result<Self, Error> {
        let frame = Self {
            inputs: sf::SampledFrame::inputs(geometry, textures, samplers)?,
            targets,
            operations,
            bindings,
            output,
        };
        frame.validate()?;
        Ok(frame)
    }
    /// Attachment loads in this geometry need the enclosing operation graph.
    /// Standalone renderers retain their first-clear guard.
    pub fn geometry(&self) -> &FramePlan {
        self.inputs.geometry()
    }
    pub fn textures(&self) -> &[sf::TextureVersion] {
        self.inputs.textures()
    }
    pub fn samplers(&self) -> &[sf::SamplerVersion] {
        self.inputs.samplers()
    }
    pub fn targets(&self) -> &[TargetVersion] {
        &self.targets
    }
    pub fn operations(&self) -> &[Operation] {
        &self.operations
    }
    pub fn bindings(&self) -> &[Vec<sf::Binding>] {
        &self.bindings
    }
    pub fn output(&self) -> &TargetVersion {
        self.target(self.output).expect("sealed output")
    }
    pub fn target(&self, version: Version) -> Result<&TargetVersion, Error> {
        self.targets.iter().find(|t| t.version == version).ok_or(Error::MissingVersion)
    }
    fn image(&self, image: ImageSlice) -> Result<(Kind, u32, u32), Error> {
        if let Some(t) = self.textures().iter().find(|t| t.version == image.version) {
            if image.mip_level as usize >= t.mips.len() {
                return Err(Error::Range);
            }
            Ok((Kind::Color, (t.width >> image.mip_level).max(1), (t.height >> image.mip_level).max(1)))
        } else {
            let t = self.target(image.version)?;
            if image.mip_level != 0 {
                return Err(Error::Range);
            }
            Ok((t.kind, t.width, t.height))
        }
    }
    fn validate(&self) -> Result<(), Error> {
        if self.targets.is_empty()
            || self.targets.len() > MAX_TARGETS
            || self.operations.is_empty()
            || self.operations.len() > MAX_OPERATIONS
            || self.bindings.len() != self.geometry().passes().iter().map(|p| p.draws.len()).sum::<usize>()
        {
            return Err(Error::Limit);
        }
        let mut images: BTreeSet<_> = self.textures().iter().map(|t| t.version).collect();
        let cpu: BTreeSet<_> = images.clone();
        let samplers: BTreeSet<_> = self.samplers().iter().map(|s| s.version).collect();
        let mut gpu_bytes = 0u64;
        for t in &self.targets {
            if t.version.generation == 0 || !images.insert(t.version) {
                return Err(Error::Identity);
            }
            if !(1..=1024).contains(&t.width) || !(1..=1024).contains(&t.height) {
                return Err(Error::Limit);
            }
            gpu_bytes = gpu_bytes.checked_add(u64::from(t.width) * u64::from(t.height) * 4).ok_or(Error::Limit)?;
        }
        if gpu_bytes > MAX_GPU_BYTES {
            return Err(Error::Limit);
        }
        let mut binding_count = 0usize;
        for bindings in &self.bindings {
            if bindings.len() > 32 {
                return Err(Error::Limit);
            }
            let mut slots = BTreeSet::new();
            for b in bindings {
                if b.binding > 127 || !slots.insert(b.binding) {
                    return Err(Error::Binding);
                }
                match b.resource {
                    sf::Resource::Texture(v) => {
                        if !images.contains(&v) {
                            return Err(Error::MissingVersion);
                        }
                        if !cpu.contains(&v) && self.target(v)?.kind != Kind::Color {
                            return Err(Error::Binding);
                        }
                    }
                    sf::Resource::Sampler(v) if !samplers.contains(&v) => return Err(Error::MissingVersion),
                    _ => {}
                }
            }
            binding_count = binding_count.checked_add(bindings.len()).ok_or(Error::Limit)?;
        }
        let total =
            80 + self.inputs.packet().len() + self.targets.len() * 32 + self.operations.len() * 64 + binding_count * 16;
        if binding_count > sf::MAX_SAMPLED_BINDINGS || total > MAX_PACKET {
            return Err(Error::Limit);
        }
        let mut produced = BTreeSet::new();
        let mut available = cpu;
        let mut pass_index = 0usize;
        let mut draw_index = 0usize;
        for op in &self.operations {
            match *op {
                Operation::Render(r) => {
                    if r.pass as usize != pass_index {
                        return Err(Error::State);
                    }
                    let pass = self.geometry().passes().get(pass_index).ok_or(Error::Range)?;
                    let color = self.target(r.color)?;
                    let depth = self.target(r.depth)?;
                    for (target, kind, load, clear) in [
                        (color, Kind::Color, r.color_load, pass.clear_color.is_some()),
                        (depth, Kind::Depth, r.depth_load, pass.clear_depth.is_some()),
                    ] {
                        if target.kind != kind
                            || target.width != self.geometry().width()
                            || target.height != self.geometry().height()
                        {
                            return Err(Error::State);
                        }
                        if produced.contains(&target.version) {
                            return Err(Error::Identity);
                        }
                        if clear == load.is_some() {
                            return Err(Error::Uninitialized);
                        }
                        if let Some(v) = load {
                            let source = self.target(v)?;
                            if source.kind != kind || source.width != target.width || source.height != target.height {
                                return Err(Error::State);
                            }
                            if !available.contains(&v) || v == target.version {
                                return Err(Error::Uninitialized);
                            }
                        }
                    }
                    for _ in &pass.draws {
                        for b in &self.bindings[draw_index] {
                            if let sf::Resource::Texture(v) = b.resource {
                                if !available.contains(&v) || v == r.color || v == r.depth {
                                    return Err(Error::Uninitialized);
                                }
                            }
                        }
                        draw_index += 1;
                    }
                    produced.insert(r.color);
                    produced.insert(r.depth);
                    if r.store_color {
                        available.insert(r.color);
                    }
                    if r.store_depth {
                        available.insert(r.depth);
                    }
                    pass_index += 1;
                }
                Operation::Copy(c) => {
                    let target = self.target(c.destination)?;
                    if produced.contains(&c.destination) {
                        return Err(Error::Identity);
                    }
                    let (kind, w, h) = self.image(c.source)?;
                    if kind != target.kind {
                        return Err(Error::State);
                    }
                    // No partial depth copies: both backends share this conservative contract.
                    if kind == Kind::Depth && (c.origin != [0, 0] || w != target.width || h != target.height) {
                        return Err(Error::Range);
                    }
                    if u64::from(c.origin[0]) + u64::from(target.width) > u64::from(w)
                        || u64::from(c.origin[1]) + u64::from(target.height) > u64::from(h)
                    {
                        return Err(Error::Range);
                    }
                    if !available.contains(&c.source.version) || c.source.version == c.destination {
                        return Err(Error::Uninitialized);
                    }
                    produced.insert(c.destination);
                    available.insert(c.destination);
                }
            }
        }
        if pass_index != self.geometry().passes().len() || produced.len() != self.targets.len() {
            return Err(Error::Uninitialized);
        }
        if self.target(self.output)?.kind != Kind::Color || !available.contains(&self.output) {
            return Err(Error::Uninitialized);
        }
        Ok(())
    }
    /// OHRT0001: 80-byte header, unchanged OHST resource bundle (no bindings),
    /// target32 / operation64 / binding16 tables. Target images have no CPU bytes.
    pub fn packet(&self) -> Vec<u8> {
        let base = self.inputs.packet();
        let to = 80 + base.len();
        let oo = to + self.targets.len() * 32;
        let bo = oo + self.operations.len() * 64;
        let nb = self.bindings.iter().map(Vec::len).sum::<usize>();
        let mut out = vec![0u8; bo + nb * 16];
        out[80..to].copy_from_slice(&base);
        let put = |b: &mut [u8], a: usize, v: u32| b[a..a + 4].copy_from_slice(&v.to_le_bytes());
        let indexes: BTreeMap<_, _> = self.targets.iter().enumerate().map(|(i, t)| (t.version, i as u32)).collect();
        for (i, v) in [
            0x5452484f,
            0x31303030,
            1,
            out.len() as u32,
            80,
            base.len() as u32,
            self.targets.len() as u32,
            self.operations.len() as u32,
            nb as u32,
            indexes[&self.output],
            to as u32,
            oo as u32,
            bo as u32,
        ]
        .into_iter()
        .enumerate()
        {
            put(&mut out, i * 4, v);
        }
        for (i, t) in self.targets.iter().enumerate() {
            let at = to + i * 32;
            for (j, v) in [t.version.id, t.kind as u32].into_iter().enumerate() {
                put(&mut out, at + j * 4, v);
            }
            out[at + 8..at + 16].copy_from_slice(&t.version.generation.to_le_bytes());
            put(&mut out, at + 16, t.width);
            put(&mut out, at + 20, t.height);
        }
        let optional = |v: Option<Version>| v.map_or(0, |v| indexes[&v] + 1);
        for (i, op) in self.operations.iter().enumerate() {
            let words = match *op {
                Operation::Render(r) => [
                    1,
                    r.pass,
                    indexes[&r.color],
                    indexes[&r.depth],
                    optional(r.color_load),
                    optional(r.depth_load),
                    u32::from(r.store_color) | (u32::from(r.store_depth) << 1),
                ],
                Operation::Copy(c) => {
                    let (kind, index) = self
                        .textures()
                        .iter()
                        .position(|t| t.version == c.source.version)
                        .map_or_else(|| (2, indexes[&c.source.version]), |i| (1, i as u32));
                    [2, kind, index, c.source.mip_level, indexes[&c.destination], c.origin[0], c.origin[1]]
                }
            };
            for (j, v) in words.into_iter().enumerate() {
                put(&mut out, oo + i * 64 + j * 4, v);
            }
        }
        let mut bi = 0;
        for (draw, bindings) in self.bindings.iter().enumerate() {
            for b in bindings {
                let (kind, index) = match b.resource {
                    sf::Resource::Texture(v) => self
                        .textures()
                        .iter()
                        .position(|t| t.version == v)
                        .map_or_else(|| (3, indexes[&v]), |i| (1, i as u32)),
                    sf::Resource::Sampler(v) => {
                        (2, self.samplers().iter().position(|s| s.version == v).expect("sealed sampler") as u32)
                    }
                };
                for (j, v) in [draw as u32, b.binding, kind, index].into_iter().enumerate() {
                    put(&mut out, bo + bi * 16 + j * 4, v);
                }
                bi += 1;
            }
        }
        out
    }
}

#[cfg(test)]
#[path = "target_frame_tests.rs"]
mod tests;
