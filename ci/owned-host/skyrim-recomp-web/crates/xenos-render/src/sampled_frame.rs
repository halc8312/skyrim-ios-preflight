//! Owned sampled-resource extension of the existing immutable pass plan.
//! Unsigned, linear RGBA8 2D data only: Xenos gamma/swizzle/sign/LOD bias stay
//! in the translated shader. No fallback or implicit mip generation is allowed.
//! This conformance packet is not the production upload/cache strategy.
//! Version identities are scoped to one frame. Cross-frame caching needs a
//! resource-owner epoch/content identity; fixtures intentionally reuse IDs.
use crate::frame_plan::{Error, FramePlan, MAX_PACKET, Version};
use std::{collections::BTreeSet, sync::Arc};
pub use xenos_texture::sampler::{AddressMode, FilterMode};

pub const MAX_TEXTURES: usize = 16;
pub const MAX_SAMPLERS: usize = 16;
pub const MAX_SAMPLED_BINDINGS: usize = 4096;

#[derive(Clone, Debug)]
pub struct Mip {
    pub bytes_per_row: u32,
    pub bytes: Arc<[u8]>,
}
#[derive(Clone, Debug)]
pub struct TextureVersion {
    pub version: Version,
    pub width: u32,
    pub height: u32,
    /// Levels 0..N, each fully initialized, including any captured row padding.
    pub mips: Vec<Mip>,
}
impl TextureVersion {
    /// Reuse the existing CPU decoder, preserving its endian/tiling/conversion.
    /// Incomplete mip sets, other dimensions/formats and lossy conversions refuse.
    pub fn from_loaded(version: Version, loaded: xenos_texture::LoadedTexture) -> Result<Self, Error> {
        use xenos_texture::{HostChannel, HostDimension, HostFormat};
        if !(1..=1024).contains(&loaded.width)
            || !(1..=1024).contains(&loaded.height)
            || loaded.levels.is_empty()
            || loaded.levels.len() > 11
        {
            return Err(Error::Limit);
        }
        if loaded.dimension != HostDimension::D2
            || loaded.depth_or_array_layers != 1
            || loaded.mapping.host_format != HostFormat::Rgba8Unorm
            || loaded.mapping.lossy.is_some()
            || loaded.mapping.component_map != [HostChannel::R, HostChannel::G, HostChannel::B, HostChannel::A]
            || loaded.levels.len() != loaded.mip_level_count as usize
        {
            return Err(Error::Binding);
        }
        let mut mips = Vec::new();
        for (i, level) in loaded.levels.into_iter().enumerate() {
            let w = loaded.width.checked_shr(i as u32).unwrap_or(0).max(1);
            let h = loaded.height.checked_shr(i as u32).unwrap_or(0).max(1);
            if level.level != i as u32
                || level.width != w
                || level.height != h
                || level.copy_width != w
                || level.copy_height != h
                || level.rows_per_image != h
                || level.depth_or_array_layers != 1
            {
                return Err(Error::Range);
            }
            mips.push(Mip { bytes_per_row: level.bytes_per_row, bytes: Arc::from(level.data) });
        }
        let t = Self { version, width: loaded.width, height: loaded.height, mips };
        t.validate()?;
        Ok(t)
    }
    fn validate(&self) -> Result<(), Error> {
        if self.version.generation == 0 {
            return Err(Error::Identity);
        }
        if !(1..=1024).contains(&self.width)
            || !(1..=1024).contains(&self.height)
            || self.mips.is_empty()
            || self.mips.len() > (32 - self.width.max(self.height).leading_zeros()) as usize
        {
            return Err(Error::Limit);
        }
        for (i, mip) in self.mips.iter().enumerate() {
            let w = (self.width >> i).max(1);
            let h = (self.height >> i).max(1);
            if mip.bytes_per_row % 4 != 0
                || mip.bytes_per_row < w * 4
                || u64::from(mip.bytes_per_row) * u64::from(h) != mip.bytes.len() as u64
                || mip.bytes.len() > MAX_PACKET
            {
                return Err(Error::Range);
            }
        }
        Ok(())
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SamplerState {
    pub address: [AddressMode; 3],
    /// minification, magnification, then mip interpolation.
    pub filters: [FilterMode; 3],
    pub lod: [f32; 2],
    pub anisotropy: u16,
}
impl Default for SamplerState {
    fn default() -> Self {
        Self {
            address: [AddressMode::ClampToEdge; 3],
            filters: [FilterMode::Nearest; 3],
            lod: [0., 32.],
            anisotropy: 1,
        }
    }
}
impl SamplerState {
    /// Bias is already applied by the Xenos shader and is not applied twice.
    /// Approximate border/mirror-once mappings must not silently become success.
    pub fn from_host(s: &xenos_texture::SamplerDescriptor) -> Result<Self, Error> {
        if !s.losses.is_empty() || s.border_color.is_some() || !s.lod_bias.is_finite() {
            return Err(Error::Binding);
        }
        let state = Self {
            address: [s.address_mode_u, s.address_mode_v, s.address_mode_w],
            filters: [s.min_filter, s.mag_filter, s.mipmap_filter],
            lod: [s.lod_min_clamp, s.lod_max_clamp],
            anisotropy: s.max_anisotropy,
        };
        state.validate()?;
        Ok(state)
    }
    fn validate(&self) -> Result<(), Error> {
        if self.lod.iter().any(|v| !v.is_finite() || !(0.0..=32.0).contains(v))
            || self.lod[0] > self.lod[1]
            || !matches!(self.anisotropy, 1 | 2 | 4 | 8 | 16)
            || (self.anisotropy > 1 && self.filters != [FilterMode::Linear; 3])
        {
            return Err(Error::State);
        }
        Ok(())
    }
}
#[derive(Clone, Debug)]
pub struct SamplerVersion {
    pub version: Version,
    pub state: SamplerState,
}
#[derive(Clone, Copy, Debug)]
pub enum Resource {
    Texture(Version),
    Sampler(Version),
}
#[derive(Clone, Copy, Debug)]
pub struct Binding {
    pub binding: u32,
    pub resource: Resource,
}

#[derive(Debug)]
pub struct SampledFrame {
    geometry: FramePlan,
    textures: Vec<TextureVersion>,
    samplers: Vec<SamplerVersion>,
    /// One group-2 binding set per flattened draw, retaining original pass order.
    bindings: Vec<Vec<Binding>>,
}
impl SampledFrame {
    pub fn new(
        geometry: FramePlan,
        textures: Vec<TextureVersion>,
        samplers: Vec<SamplerVersion>,
        bindings: Vec<Vec<Binding>>,
    ) -> Result<Self, Error> {
        let s = Self { geometry, textures, samplers, bindings };
        s.validate(true)?;
        Ok(s)
    }
    /// Resource bundle embedded only in the target-frame contract. Empty input
    /// tables and contextual geometry are legal there: the outer operations
    /// prove initialization. Standalone OHST keeps its first-clear requirement
    /// and still requires a texture and sampler.
    pub(crate) fn inputs(
        geometry: FramePlan,
        textures: Vec<TextureVersion>,
        samplers: Vec<SamplerVersion>,
    ) -> Result<Self, Error> {
        let count = geometry.passes().iter().map(|p| p.draws.len()).sum();
        let s = Self { geometry, textures, samplers, bindings: vec![vec![]; count] };
        s.validate(false)?;
        Ok(s)
    }
    pub fn geometry(&self) -> &FramePlan {
        &self.geometry
    }
    pub fn textures(&self) -> &[TextureVersion] {
        &self.textures
    }
    pub fn samplers(&self) -> &[SamplerVersion] {
        &self.samplers
    }
    pub fn bindings(&self) -> &[Vec<Binding>] {
        &self.bindings
    }
    fn validate(&self, require_sampled: bool) -> Result<(), Error> {
        if require_sampled {
            self.geometry.require_first_clear()?;
        }
        if (require_sampled && self.textures.is_empty())
            || self.textures.len() > MAX_TEXTURES
            || (require_sampled && self.samplers.is_empty())
            || self.samplers.len() > MAX_SAMPLERS
            || self.bindings.len() != self.geometry.passes().iter().map(|p| p.draws.len()).sum::<usize>()
        {
            return Err(Error::Limit);
        }
        let mut textures = BTreeSet::new();
        let mut samplers = BTreeSet::new();
        let mut total = 80 + self.geometry.packet().len() + self.textures.len() * 40 + self.samplers.len() * 64;
        for t in &self.textures {
            t.validate()?;
            if !textures.insert(t.version) {
                return Err(Error::Identity);
            }
            for m in &t.mips {
                total = total.checked_add(24 + m.bytes.len() + 7).ok_or(Error::Limit)?;
            }
        }
        for s in &self.samplers {
            s.state.validate()?;
            if s.version.generation == 0 || !samplers.insert(s.version) {
                return Err(Error::Identity);
            }
        }
        let mut count = 0;
        for bindings in &self.bindings {
            if bindings.len() > 32 {
                return Err(Error::Limit);
            }
            let mut slots = BTreeSet::new();
            for b in bindings {
                if b.binding > 127 || !slots.insert(b.binding) {
                    return Err(Error::Binding);
                }
                let exists = match b.resource {
                    Resource::Texture(v) => textures.contains(&v),
                    Resource::Sampler(v) => samplers.contains(&v),
                };
                if !exists {
                    return Err(Error::MissingVersion);
                }
            }
            count += bindings.len();
        }
        total = total.checked_add(count * 16).ok_or(Error::Limit)?;
        if count > MAX_SAMPLED_BINDINGS || total > MAX_PACKET {
            return Err(Error::Limit);
        }
        Ok(())
    }
    /// OHST0001: 80-byte header, embedded OHFP0001, texture40/mip24/sampler64/
    /// binding16 tables, then 8-aligned mip bytes. No handles or native structs.
    pub fn packet(&self) -> Vec<u8> {
        let base = self.geometry.packet();
        debug_assert_eq!(base.len() % 8, 0);
        let nt = self.textures.len();
        let nm = self.textures.iter().map(|t| t.mips.len()).sum::<usize>();
        let ns = self.samplers.len();
        let nb = self.bindings.iter().map(Vec::len).sum::<usize>();
        let to = 80 + base.len();
        let mo = to + nt * 40;
        let so = mo + nm * 24;
        let bo = so + ns * 64;
        let data = bo + nb * 16;
        let mut out = vec![0; data];
        out[80..to].copy_from_slice(&base);
        let u32_at = |b: &mut [u8], o: usize, v: u32| b[o..o + 4].copy_from_slice(&v.to_le_bytes());
        let u64_at = |b: &mut [u8], o: usize, v: u64| b[o..o + 8].copy_from_slice(&v.to_le_bytes());
        for (i, v) in [
            0x5453484f,
            0x31303030,
            1,
            0,
            80,
            base.len() as u32,
            nt as u32,
            nm as u32,
            ns as u32,
            nb as u32,
            to as u32,
            mo as u32,
            so as u32,
            bo as u32,
            data as u32,
            0,
            0,
            0,
            0,
            0,
        ]
        .into_iter()
        .enumerate()
        {
            u32_at(&mut out, i * 4, v);
        }
        let mut mi = 0;
        for (ti, t) in self.textures.iter().enumerate() {
            let a = to + ti * 40;
            u32_at(&mut out, a, t.version.id);
            u32_at(&mut out, a + 4, 1);
            u64_at(&mut out, a + 8, t.version.generation);
            for (i, v) in [t.width, t.height, mi as u32, t.mips.len() as u32].into_iter().enumerate() {
                u32_at(&mut out, a + 16 + i * 4, v);
            }
            for (level, m) in t.mips.iter().enumerate() {
                let a = mo + mi * 24;
                let start = out.len();
                out.extend_from_slice(&m.bytes);
                while out.len() % 8 != 0 {
                    out.push(0);
                }
                for (i, v) in [ti as u32, level as u32, m.bytes_per_row, start as u32, m.bytes.len() as u32, 0]
                    .into_iter()
                    .enumerate()
                {
                    u32_at(&mut out, a + i * 4, v);
                }
                mi += 1;
            }
        }
        for (si, s) in self.samplers.iter().enumerate() {
            let a = so + si * 64;
            u32_at(&mut out, a, s.version.id);
            u64_at(&mut out, a + 8, s.version.generation);
            for (i, v) in s
                .state
                .address
                .into_iter()
                .map(address_code)
                .chain(s.state.filters.into_iter().map(filter_code))
                .enumerate()
            {
                u32_at(&mut out, a + 16 + i * 4, v);
            }
            u32_at(&mut out, a + 40, s.state.lod[0].to_bits());
            u32_at(&mut out, a + 44, s.state.lod[1].to_bits());
            u32_at(&mut out, a + 48, u32::from(s.state.anisotropy));
        }
        let mut bi = 0;
        for (di, bindings) in self.bindings.iter().enumerate() {
            for b in bindings {
                let (kind, index) = match b.resource {
                    Resource::Texture(v) => (1, self.textures.iter().position(|t| t.version == v).unwrap()),
                    Resource::Sampler(v) => (2, self.samplers.iter().position(|s| s.version == v).unwrap()),
                };
                for (i, v) in [di as u32, b.binding, kind, index as u32].into_iter().enumerate() {
                    u32_at(&mut out, bo + bi * 16 + i * 4, v);
                }
                bi += 1;
            }
        }
        let n = out.len() as u32;
        u32_at(&mut out, 12, n);
        out
    }
}
pub(crate) fn address_code(a: AddressMode) -> u32 {
    match a {
        AddressMode::ClampToEdge => 0,
        AddressMode::Repeat => 1,
        AddressMode::MirrorRepeat => 2,
    }
}
pub(crate) fn filter_code(f: FilterMode) -> u32 {
    match f {
        FilterMode::Nearest => 0,
        FilterMode::Linear => 1,
    }
}

#[cfg(test)]
#[path = "sampled_frame_tests.rs"]
mod tests;
