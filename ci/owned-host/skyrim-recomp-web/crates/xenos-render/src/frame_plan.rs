//! Immutable, backend-independent render-pass slice. One RGBA8 and one D32
//! attachment, owned buffer versions, triangle lists/strips and ordered passes.
//! This is not yet a complete Xenos frame: texture/EDRAM alias/resolve/copy,
//! arbitrary shader pipelines, MSAA and guest fences must refuse at a caller's
//! feature gate until their contracts are implemented.

use crate::prepared::DrawState;
use std::{
    collections::{BTreeMap, BTreeSet},
    sync::Arc,
};

pub const MAX_PACKET: usize = 4 << 20;
pub const MAX_RESOURCES: usize = 64;
pub const MAX_PASSES: usize = 16;
pub const MAX_DRAWS: usize = 128;
pub const MAX_BINDINGS: usize = 1280;

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub struct Version {
    pub id: u32,
    pub generation: u64,
}
#[derive(Clone, Debug)]
pub struct BufferVersion {
    pub version: Version,
    pub bytes: Arc<[u8]>,
}
#[derive(Clone, Copy, Debug)]
pub struct BufferSlice {
    pub version: Version,
    pub offset: u32,
    pub bytes: u32,
}
#[derive(Clone, Copy, Debug)]
pub struct Binding {
    pub group: u32,
    pub binding: u32,
    pub slice: BufferSlice,
}
#[derive(Clone, Copy, Debug)]
pub struct IndexSlice {
    pub slice: BufferSlice,
    pub stride: u32,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum Topology {
    Triangles = 1,
    TriangleStrip = 2,
}

#[derive(Clone, Debug)]
pub struct Draw {
    pub state: DrawState,
    pub topology: Topology,
    pub count: u32,
    pub indices: Option<IndexSlice>,
    pub bindings: Vec<Binding>,
    pub blend: bool,
    pub depth_less_write: bool,
}
#[derive(Clone, Debug)]
pub struct Pass {
    pub clear_color: Option<[f64; 4]>,
    pub clear_depth: Option<f64>,
    pub draws: Vec<Draw>,
}
#[derive(Debug)]
pub struct FramePlan {
    width: u32,
    height: u32,
    resources: Vec<BufferVersion>,
    passes: Vec<Pass>,
}
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Error {
    Limit,
    Identity,
    MissingVersion,
    Range,
    State,
    Binding,
    Uninitialized,
}

impl FramePlan {
    pub fn new(width: u32, height: u32, resources: Vec<BufferVersion>, passes: Vec<Pass>) -> Result<Self, Error> {
        let plan = Self { width, height, resources, passes };
        plan.validate()?;
        Ok(plan)
    }
    pub fn width(&self) -> u32 {
        self.width
    }
    pub fn height(&self) -> u32 {
        self.height
    }
    pub fn resources(&self) -> &[BufferVersion] {
        &self.resources
    }
    pub fn passes(&self) -> &[Pass] {
        &self.passes
    }
    pub fn buffer(&self, slice: BufferSlice) -> Result<&[u8], Error> {
        let r = self.resources.iter().find(|r| r.version == slice.version).ok_or(Error::MissingVersion)?;
        let end = (slice.offset as usize).checked_add(slice.bytes as usize).ok_or(Error::Range)?;
        r.bytes.get(slice.offset as usize..end).ok_or(Error::Range)
    }
    fn validate(&self) -> Result<(), Error> {
        if !(1..=1024).contains(&self.width)
            || !(1..=1024).contains(&self.height)
            || self.resources.len() > MAX_RESOURCES
            || self.passes.is_empty()
            || self.passes.len() > MAX_PASSES
        {
            return Err(Error::Limit);
        }
        let mut identities = BTreeSet::new();
        let mut total = 64usize;
        let mut draws = 0usize;
        let mut bindings = 0usize;
        for r in &self.resources {
            if r.version.generation == 0 || !identities.insert(r.version) {
                return Err(Error::Identity);
            }
            if r.bytes.is_empty() || r.bytes.len() > MAX_PACKET || r.bytes.len() % 4 != 0 {
                return Err(Error::Range);
            }
            total = total.checked_add(24 + r.bytes.len() + 7).ok_or(Error::Limit)?;
        }
        if self.passes[0].clear_color.is_none() || self.passes[0].clear_depth.is_none() {
            return Err(Error::Uninitialized);
        }
        for pass in &self.passes {
            if pass.clear_color.iter().flatten().any(|v| !v.is_finite() || !(0.0..=1.0).contains(v))
                || pass.clear_depth.is_some_and(|v| !v.is_finite() || !(0.0..=1.0).contains(&v))
            {
                return Err(Error::State);
            }
            total = total.checked_add(64).ok_or(Error::Limit)?;
            draws = draws.checked_add(pass.draws.len()).ok_or(Error::Limit)?;
            for d in &pass.draws {
                let [x, y, w, h, z0, z1] = d.state.viewport();
                let [sx, sy, sw, sh] = d.state.scissor;
                if d.state.reserved != 0
                    || d.count == 0
                    || d.count > 65536
                    || ![x, y, w, h, z0, z1].into_iter().all(f32::is_finite)
                    || x < 0.0
                    || y < 0.0
                    || w <= 0.0
                    || h <= 0.0
                    || f64::from(x) + f64::from(w) > f64::from(self.width)
                    || f64::from(y) + f64::from(h) > f64::from(self.height)
                    || !(0.0..=1.0).contains(&z0)
                    || !(0.0..=1.0).contains(&z1)
                    || z0 > z1
                    || sw == 0
                    || sh == 0
                    || u64::from(sx) + u64::from(sw) > u64::from(self.width)
                    || u64::from(sy) + u64::from(sh) > u64::from(self.height)
                    || d.state.blend().iter().any(|v| !v.is_finite() || !(0.0..=1.0).contains(v))
                {
                    return Err(Error::State);
                }
                let mut slots = BTreeSet::new();
                for binding in &d.bindings {
                    if binding.group > 1 || binding.binding > 4 || !slots.insert((binding.group, binding.binding)) {
                        return Err(Error::Binding);
                    }
                    self.buffer(binding.slice)?;
                    // Conservative shared contract for WebGPU and Metal on
                    // device/Simulator; not a claim about every Metal GPU limit.
                    if binding.slice.bytes == 0 || binding.slice.offset % 256 != 0 || binding.slice.bytes % 4 != 0 {
                        return Err(Error::Range);
                    }
                }
                if let Some(index) = d.indices {
                    if !matches!(index.stride, 2 | 4)
                        || index.slice.offset % 4 != 0
                        || u64::from(d.count) * u64::from(index.stride) > u64::from(index.slice.bytes)
                    {
                        return Err(Error::Range);
                    }
                    self.buffer(index.slice)?;
                }
                bindings = bindings.checked_add(d.bindings.len()).ok_or(Error::Limit)?;
                total = total.checked_add(128 + d.bindings.len() * 32).ok_or(Error::Limit)?;
            }
        }
        if draws > MAX_DRAWS || bindings > MAX_BINDINGS || total > MAX_PACKET {
            return Err(Error::Limit);
        }
        Ok(())
    }

    /// Canonical little-endian packet. No native pointers, padding bytes or GPU
    /// handles. Every buffer version is embedded once, not looked up in live RAM.
    pub fn packet(&self) -> Vec<u8> {
        let nr = self.resources.len();
        let np = self.passes.len();
        let nd = self.passes.iter().map(|p| p.draws.len()).sum::<usize>();
        let nb = self.passes.iter().flat_map(|p| &p.draws).map(|d| d.bindings.len()).sum::<usize>();
        let ro = 64;
        let po = ro + nr * 24;
        let doff = po + np * 64;
        let bo = doff + nd * 128;
        let data = bo + nb * 32;
        let mut out = vec![0u8; data];
        let u32_at = |b: &mut [u8], o: usize, v: u32| b[o..o + 4].copy_from_slice(&v.to_le_bytes());
        let u64_at = |b: &mut [u8], o: usize, v: u64| b[o..o + 8].copy_from_slice(&v.to_le_bytes());
        for (i, v) in [
            0x5046484f,
            0x31303030,
            1,
            self.width,
            self.height,
            nr as u32,
            np as u32,
            nd as u32,
            nb as u32,
            ro as u32,
            po as u32,
            doff as u32,
            bo as u32,
            data as u32,
            0,
            0,
        ]
        .into_iter()
        .enumerate()
        {
            u32_at(&mut out, i * 4, v);
        }
        let mut indexes = BTreeMap::new();
        for (i, r) in self.resources.iter().enumerate() {
            indexes.insert(r.version, i as u32);
            let off = out.len();
            out.extend_from_slice(&r.bytes);
            while out.len() % 8 != 0 {
                out.push(0);
            }
            let at = ro + i * 24;
            u32_at(&mut out, at, r.version.id);
            u64_at(&mut out, at + 8, r.version.generation);
            u32_at(&mut out, at + 16, off as u32);
            u32_at(&mut out, at + 20, r.bytes.len() as u32);
        }
        let mut draw_index = 0;
        let mut binding_index = 0;
        for (pi, pass) in self.passes.iter().enumerate() {
            let at = po + pi * 64;
            for (i, value) in pass.clear_color.unwrap_or([0.0; 4]).into_iter().enumerate() {
                u64_at(&mut out, at + i * 8, value.to_bits());
            }
            u64_at(&mut out, at + 32, pass.clear_depth.unwrap_or(0.0).to_bits());
            u32_at(&mut out, at + 40, draw_index);
            u32_at(&mut out, at + 44, pass.draws.len() as u32);
            u32_at(
                &mut out,
                at + 48,
                u32::from(pass.clear_color.is_some()) | u32::from(pass.clear_depth.is_some()) << 1,
            );
            for d in &pass.draws {
                let at = doff + draw_index as usize * 128;
                for (i, v) in d.state.viewport_bits.into_iter().chain(d.state.scissor).enumerate() {
                    u32_at(&mut out, at + i * 4, v);
                }
                for (i, v) in d.state.blend_bits.into_iter().enumerate() {
                    u64_at(&mut out, at + 40 + i * 8, v);
                }
                u32_at(&mut out, at + 72, d.state.stencil_reference);
                for (i, v) in [
                    d.topology as u32,
                    d.count,
                    binding_index,
                    d.bindings.len() as u32,
                    u32::from(d.blend) | u32::from(d.depth_less_write) << 1,
                    d.indices.map_or(0, |i| indexes[&i.slice.version] + 1),
                    d.indices.map_or(0, |i| i.stride),
                    d.indices.map_or(0, |i| i.slice.offset),
                    d.indices.map_or(0, |i| i.slice.bytes),
                ]
                .into_iter()
                .enumerate()
                {
                    u32_at(&mut out, at + 80 + i * 4, v);
                }
                for b in &d.bindings {
                    let at = bo + binding_index as usize * 32;
                    for (i, v) in [b.group, b.binding, indexes[&b.slice.version], 0].into_iter().enumerate() {
                        u32_at(&mut out, at + i * 4, v);
                    }
                    u64_at(&mut out, at + 16, u64::from(b.slice.offset));
                    u64_at(&mut out, at + 24, u64::from(b.slice.bytes));
                    binding_index += 1;
                }
                draw_index += 1;
            }
        }
        let len = out.len() as u32;
        u32_at(&mut out, 56, len);
        out
    }
}

#[cfg(test)]
#[path = "frame_plan_tests.rs"]
mod tests;
