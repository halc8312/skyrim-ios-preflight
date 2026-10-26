//! Texture conversion and immutable generations in the SAME command stream as
//! guest stores and draws. No CPU readback, mutable mirror handle or WebGPU
//! object crosses this boundary. Publication follows the whole frame commit.
use super::guest_memory::{CapturedRange, MemoryFrame};
use super::*;
use crate::{
    bindings::{Entry, Group, Resource, Sampler},
    compute::SharedComputePipeline,
    render::BoundGroup,
};
use xenos_texture::gpu_load::{self, Plan};
use xenos_texture::{
    DataDimension, HostCaps, HostFormat, HostMapping, TextureFetchConstant, TextureGuestInfo,
};

const CACHE_BYTES: u64 = 128 << 20;
const CACHE_ENTRIES: usize = 256;

#[derive(Clone)]
pub struct GuestTexture {
    view: TextureView<Platform>,
    mapping: HostMapping,
    publication: Rc<Cell<resolve::Submission>>,
    fetch: [u32; 6],
    signed: bool,
    versions: Vec<(u32, u64)>,
    views: Rc<RefCell<HashMap<types::TextureViewDimension, TextureView<Platform>>>>,
}
impl GuestTexture {
    pub(crate) fn front(
        &self,
        gpu: &Gpu<Platform>,
        display_size: [u32; 2],
    ) -> Result<crate::presentation::Front<Platform>> {
        gpu.owner.healthy()?;
        if !self.view.belongs_to(&gpu.owner)
            || self.signed
            || !matches!(self.publication.get(), resolve::Submission::Submitted(_))
            || self.descriptor().dimension != types::TextureDimension::D2
            || self.descriptor().size.depth_or_array_layers != 1
        {
            return Err(Error::Rejected("guest front buffer publication"));
        }
        let view = gpu.texture_view(
            self.view.texture(),
            types::TextureViewDimension::D2,
            0..1,
            0..1,
        )?;
        let fetch = TextureFetchConstant::decode(self.fetch);
        Ok(crate::presentation::Front {
            view,
            display_size,
            swizzle: fetch.swizzle.map(|s| s as u32),
        })
    }
    pub fn mapping(&self) -> HostMapping {
        self.mapping
    }
    pub fn id(&self) -> u64 {
        self.view.id()
    }
    pub fn descriptor(&self) -> &TextureDescriptor {
        self.view.texture().descriptor()
    }
    pub(super) fn view(&self, frame: &Frame<'_>) -> Result<&TextureView<Platform>> {
        frame.healthy()?;
        if !self.view.belongs_to(&frame.gpu.owner) {
            return Err(Error::Rejected("guest texture device"));
        }
        match self.publication.get() {
            resolve::Submission::Submitted(_) => Ok(&self.view),
            resolve::Submission::Staged if Rc::ptr_eq(&self.publication, &frame.resolve_status) => {
                Ok(&self.view)
            }
            _ => Err(Error::Rejected("guest texture transaction not accepted")),
        }
    }
    /// Oracle only: returns the complete row-padded mip after verified execution.
    #[cfg(feature = "fault-injection")]
    pub fn test_read(
        &self,
        gpu: &mut Gpu<Platform>,
        mip: u32,
        timeout: std::time::Duration,
    ) -> Result<Vec<u8>> {
        if !self.view.belongs_to(&gpu.owner) {
            return Err(Error::Rejected("guest texture device"));
        }
        let serial = match self.publication.get() {
            resolve::Submission::Submitted(s) => s,
            _ => return Err(Error::Rejected("guest texture transaction not accepted")),
        };
        gpu.wait(serial, timeout)?;
        let texture = self.view.texture();
        let size = texture
            .mip_size(mip)?
            .physical_size(texture.descriptor().format);
        let (bw, bh) = texture.descriptor().format.block_dimensions();
        let unit = texture
            .descriptor()
            .format
            .block_copy_size(None)
            .ok_or(Error::Rejected("guest texture readback format"))?;
        let row = (size.width / bw * unit).next_multiple_of(256);
        let rows = size.height / bh;
        let bytes = u64::from(row) * u64::from(rows) * u64::from(size.depth_or_array_layers);
        let buffer = gpu.create_buffer("guest texture oracle", bytes, U::COPY_DST | U::MAP_READ)?;
        let mut encoder = gpu.encoder()?;
        encoder.copy_texture_to_buffer(
            texture,
            mip,
            types::Origin3d::ZERO,
            size,
            &buffer,
            types::TexelCopyBufferLayout {
                offset: 0,
                bytes_per_row: Some(row),
                rows_per_image: Some(rows),
            },
        )?;
        let serial = gpu.submit(vec![encoder.finish()?])?;
        gpu.wait(serial, timeout)?;
        gpu.read_buffer(&buffer, 0..bytes, timeout)
    }
}

pub enum TextureAdmission {
    Loaded(GuestTexture),
    NeedsSubmit,
}
pub struct GuestTextureBinding<'a> {
    pub fetch_constant: u32,
    pub texture: &'a GuestTexture,
}
pub(super) struct LoadError {
    pub error: Error,
    pub retry_bytes: Option<u64>,
}
impl From<Error> for LoadError {
    fn from(error: Error) -> Self {
        Self {
            error,
            retry_bytes: None,
        }
    }
}
#[derive(Clone, PartialEq, Eq, Hash)]
struct Key {
    fetch: [u32; 6],
    signed: bool,
}
#[derive(Clone)]
struct EntryState {
    texture: GuestTexture,
    versions: Vec<(u32, u64)>,
    bytes: u64,
    used: u64,
}
#[derive(Default, Clone)]
pub(super) struct TextureState {
    entries: HashMap<Key, EntryState>,
}
#[derive(Clone)]
struct Kernel {
    layout: GroupLayout<Platform>,
    pipeline: SharedComputePipeline<Platform>,
}
#[derive(Default)]
pub(super) struct TextureKernels {
    variants: [Option<Kernel>; 2],
    samplers: HashMap<SamplerKey, Sampler<Platform>>,
    dummies: HashMap<types::TextureViewDimension, TextureView<Platform>>,
}
#[derive(PartialEq, Eq, Hash)]
struct SamplerKey {
    fetch: [u32; 6],
    filters: xenos_texture::FetchInstructionFilters,
    filterable: bool,
}
impl TextureKernels {
    fn get(&mut self, gpu: &Gpu<Platform>, atomic: bool) -> Result<Kernel> {
        if let Some(k) = &self.variants[atomic as usize] {
            return Ok(k.clone());
        }
        let entries = [0, 1, 2].map(|binding| types::BindGroupLayoutEntry {
            binding,
            visibility: S::COMPUTE,
            ty: types::BindingType::Buffer {
                ty: if binding == 0 {
                    types::BufferBindingType::Uniform
                } else {
                    types::BufferBindingType::Storage {
                        read_only: binding == 1,
                    }
                },
                has_dynamic_offset: binding == 0,
                min_binding_size: NonZeroU64::new(if binding == 0 { 128 } else { 4 }),
            },
            count: None,
        });
        let layout = gpu.group_layout(&entries)?;
        let shader = gpu.shader(&gpu_load::shader_variant(gpu_load::SHADER, atomic))?;
        let pipeline =
            gpu.shared_compute_pipeline(&gpu.pipeline_layout(&[layout.clone()])?, &shader, "main")?;
        let k = Kernel { layout, pipeline };
        self.variants[atomic as usize] = Some(k.clone());
        Ok(k)
    }
}

struct Prepared {
    plan: Option<Plan>,
    mapping: HostMapping,
    descriptor: TextureDescriptor,
    dimension: types::TextureViewDimension,
    sources: Vec<CapturedRange>,
    versions: Vec<(u32, u64)>,
    offsets: Vec<u64>,
    raw_bytes: u64,
}
fn prepare(
    gpu: &Gpu<Platform>,
    memory: &MemoryFrame<'_>,
    fetch: &TextureFetchConstant,
    signed: bool,
) -> Result<Prepared> {
    // The public decoder is the canonical field/range check. Reject aliases and
    // malformed manually constructed fields instead of truncating them silently.
    if TextureFetchConstant::decode(fetch.encode()) != *fetch
        || fetch.fetch_type != xenos_texture::FetchConstantType::Texture
        || fetch.base_page >= (512 << 20) / 4096
        || fetch.mip_page >= (512 << 20) / 4096
        || fetch.width > 8192
        || fetch.height > 8192
        || fetch.depth > 2048
    {
        return Err(Error::Rejected("guest texture fetch shape/address"));
    }
    let info = TextureGuestInfo::from_fetch(fetch);
    let (base_bytes, mip_bytes) = info.total_sizes();
    if u64::from(fetch.base_page) * 4096 + u64::from(base_bytes) > 512 << 20
        || u64::from(fetch.mip_page) * 4096 + u64::from(mip_bytes) > 512 << 20
    {
        return Err(Error::Rejected("guest texture physical extent wraps"));
    }
    let sub = &info.subresources;
    let caps = HostCaps {
        texture_compression_bc: gpu
            .owner
            .features
            .contains(types::Features::TEXTURE_COMPRESSION_BC),
        norm16: gpu
            .owner
            .features
            .contains(types::Features::TEXTURE_FORMAT_16BIT_NORM),
    };
    let mapping = xenos_texture::host_mapping(info.format, signed, caps, sub.width, sub.height)
        .map_err(backend_error)?;
    if matches!(
        mapping.host_format,
        HostFormat::R32Uint | HostFormat::Depth32Float | HostFormat::Depth24PlusStencil8
    ) {
        return Err(Error::Rejected("guest texture non-float sampling"));
    }
    let is_3d = sub.dimension == DataDimension::D3;
    let layers = sub.depth_or_array_size.max(1);
    let longest = sub
        .width
        .max(sub.height)
        .max(if is_3d { layers } else { 1 });
    let levels = (info.layout.max_level + 1).clamp(1, 32 - longest.leading_zeros());
    // Checked before a CPU decompression can allocate output. Include row and
    // layer padding, and all mips, not just the logical base image size.
    let host_block = mapping.host_format.block_size();
    let unit = mapping.host_format.bytes_per_block();
    let mut output_bound = 0u64;
    for level in 0..levels {
        let width = (sub.width >> level).max(1);
        let height = (sub.height >> level).max(1);
        let slices = if is_3d {
            (layers >> level).max(1)
        } else {
            layers
        };
        output_bound += u64::from((width.div_ceil(host_block) * unit).next_multiple_of(256))
            * u64::from(height.div_ceil(host_block))
            * u64::from(slices);
    }
    if output_bound > CACHE_BYTES {
        return Err(Error::Rejected("guest texture conversion budget"));
    }
    let plan = gpu_load::plan(fetch, &info, &mapping, levels);
    if plan
        .as_ref()
        .is_some_and(|p| p.out_bytes > CACHE_BYTES || p.host_bytes() > CACHE_BYTES)
    {
        return Err(Error::Rejected("guest texture conversion budget"));
    }
    let (offsets, raw_bytes) = plan
        .as_ref()
        .map(gpu_load::upload_offsets)
        .unwrap_or_default();
    if raw_bytes > CACHE_BYTES {
        return Err(Error::Rejected("guest texture source budget"));
    }
    let mut sources = Vec::new();
    let mut versions = BTreeMap::new();
    let ranges: Vec<_> = plan
        .as_ref()
        .map(|p| p.levels.iter().map(|l| l.source).collect())
        .unwrap_or_else(|| {
            info.memory_ranges()
                .iter()
                .map(|r| (r.address, r.length))
                .collect()
        });
    for (address, bytes) in ranges {
        let end = (u64::from(address) + u64::from(bytes.max(1))).next_multiple_of(4);
        if end > 512 << 20 {
            return Err(Error::Rejected("guest texture physical extent"));
        }
        let source = memory.capture_range((address & !3)..end as u32)?;
        versions.extend(source.versions.iter().copied());
        sources.push(source);
    }
    let descriptor = TextureDescriptor {
        size: types::Extent3d {
            width: sub.width,
            height: sub.height,
            depth_or_array_layers: layers,
        },
        mip_levels: levels,
        sample_count: 1,
        dimension: if is_3d {
            types::TextureDimension::D3
        } else {
            types::TextureDimension::D2
        },
        format: format(mapping.host_format),
        usage: types::TextureUses::RESOURCE
            | types::TextureUses::COPY_DST
            | types::TextureUses::COPY_SRC,
    };
    let dimension = if is_3d {
        types::TextureViewDimension::D3
    } else if sub.dimension == DataDimension::Cube {
        types::TextureViewDimension::Cube
    } else {
        types::TextureViewDimension::D2Array
    };
    Ok(Prepared {
        plan,
        mapping,
        descriptor,
        dimension,
        sources,
        versions: versions.into_iter().collect(),
        offsets,
        raw_bytes,
    })
}
impl Frame<'_> {
    pub(super) fn load_guest_texture_inner(
        &mut self,
        memory: &MemoryFrame<'_>,
        fetch: &TextureFetchConstant,
        signed: bool,
    ) -> std::result::Result<GuestTexture, LoadError> {
        let key = Key {
            fetch: fetch.encode(),
            signed,
        };
        // A real cache hit needs no rebuilt layout, gather spans, descriptor or
        // temporary version map. Canonicality prevents truncated malformed
        // inputs from borrowing the ticket for a different valid fetch.
        if TextureFetchConstant::decode(key.fetch) != *fetch {
            return Err(Error::Rejected("guest texture fetch shape/address").into());
        }
        if let Some(entry) = self.state.textures.entries.get_mut(&key)
            && memory.validate_versions(&entry.versions).is_ok()
        {
            entry.used = self.state.epoch;
            return Ok(entry.texture.clone());
        }
        let p = prepare(self.gpu, memory, fetch, signed)?;
        let Some(plan) = p.plan.as_ref() else {
            return self.load_guest_texture_cpu(memory, fetch, signed, key, p);
        };
        let parameter_bytes = plan.levels.len() as u64 * 256;
        // Admit staging before allocating texture/buffers or changing cache
        // state. NeedsSubmit keeps the caller's capture intact for one retry.
        let mut reservation = self
            .gpu
            .reserve_ordered_upload(self.encoder.as_mut().unwrap(), parameter_bytes)
            .map_err(|error| LoadError {
                error,
                retry_bytes: Some(parameter_bytes),
            })?;
        let kernel = self
            .cache
            .texture_kernels
            .get(self.gpu, plan.levels[0].params[19] < 4)?;
        let texture = self.gpu.create_texture_recorded(
            self.encoder.as_mut().unwrap(),
            "guest converted texture",
            p.descriptor.clone(),
        )?;
        let view = self.gpu.texture_view(
            &texture,
            p.dimension,
            0..texture.descriptor().mip_levels,
            0..if p.dimension == types::TextureViewDimension::D3 {
                1
            } else {
                texture.descriptor().size.depth_or_array_layers
            },
        )?;
        // Private scratch is completely initialized on this encoder. It is not
        // an eager queue-prefix allocation surviving an abandoned transaction.
        let upload = self.gpu.raw_buffer(
            "guest texture gather",
            p.raw_bytes,
            U::COPY_DST | U::STORAGE_READ_ONLY,
            false,
        )?;
        let out = self.gpu.raw_buffer(
            "guest texture converted rows",
            plan.out_bytes,
            U::COPY_DST | U::COPY_SRC | U::STORAGE_READ_WRITE,
            false,
        )?;
        let params = self.gpu.raw_buffer(
            "guest texture params",
            parameter_bytes,
            U::COPY_DST | U::UNIFORM,
            false,
        )?;
        let encoder = self.encoder.as_mut().unwrap();
        encoder.clear_buffer(&upload, 0..upload.size())?;
        encoder.clear_buffer(&out, 0..out.size())?;
        let mut words = vec![0u8; parameter_bytes as usize];
        for (n, (level, source)) in plan.levels.iter().zip(&p.sources).enumerate() {
            let mut cursor = p.offsets[n];
            for (buffer, offset, bytes) in &source.copies {
                encoder.copy_buffer(buffer, *offset, &upload, cursor, *bytes)?;
                cursor += bytes;
            }
            let mut constants = level.params;
            constants[21] = (level.source.0 & !3).wrapping_sub(p.offsets[n] as u32);
            constants[25] = (level.out_offset / 4) as u32;
            for (i, word) in constants.iter().enumerate() {
                words[n * 256 + i * 4..n * 256 + i * 4 + 4].copy_from_slice(&word.to_le_bytes());
            }
        }
        self.gpu.write_buffers_reserved(
            encoder,
            &mut reservation,
            &[crate::OrderedUpload {
                destination: &params,
                offset: 0,
                data: &words,
            }],
        )?;
        let group = self.gpu.bind_group(
            &kernel.layout,
            &[(&params, 128), (&upload, upload.size()), (&out, out.size())]
                .iter()
                .enumerate()
                .map(|(binding, (buffer, size))| Entry {
                    binding: binding as u32,
                    resource: Resource::Buffer {
                        buffer,
                        offset: 0,
                        size: *size,
                    },
                })
                .collect::<Vec<_>>(),
        )?;
        let offsets: Vec<[u32; 1]> = (0..plan.levels.len()).map(|n| [(n * 256) as u32]).collect();
        let groups: Vec<_> = offsets
            .iter()
            .map(|offset| {
                [BoundGroup {
                    group: &group,
                    dynamic_offsets: offset,
                }]
            })
            .collect();
        let dispatches: Vec<_> = plan
            .levels
            .iter()
            .zip(&groups)
            .map(|(level, groups)| crate::compute::ComputeDispatch {
                pipeline: &kernel.pipeline,
                groups,
                workgroups: [
                    level.blocks.0.div_ceil(8),
                    level.blocks.1.div_ceil(8),
                    level.blocks.2,
                ],
            })
            .collect();
        encoder.dispatch_shared_batch(&dispatches)?;
        for level in &plan.levels {
            encoder.copy_buffer_to_texture(
                &out,
                types::TexelCopyBufferLayout {
                    offset: level.out_offset,
                    bytes_per_row: Some(level.bytes_per_row),
                    rows_per_image: Some(level.rows_per_image),
                },
                &texture,
                level.level,
                types::Origin3d::ZERO,
                types::Extent3d {
                    width: level.copy.0,
                    height: level.copy.1,
                    depth_or_array_layers: level.copy.2,
                },
            )?;
        }
        let bytes = plan.host_bytes();
        self.publish_texture(key, p, view, bytes)
            .map_err(Into::into)
    }
    fn publish_texture(
        &mut self,
        key: Key,
        p: Prepared,
        view: TextureView<Platform>,
        bytes: u64,
    ) -> Result<GuestTexture> {
        let result = GuestTexture {
            view,
            mapping: p.mapping,
            publication: self.resolve_status.clone(),
            fetch: key.fetch,
            signed: key.signed,
            versions: p.versions.clone(),
            views: Default::default(),
        };
        self.state.textures.entries.remove(&key);
        while self.state.textures.entries.len() >= CACHE_ENTRIES
            || self
                .state
                .textures
                .entries
                .values()
                .map(|e| e.bytes)
                .sum::<u64>()
                + bytes
                > CACHE_BYTES
        {
            let victim = self
                .state
                .textures
                .entries
                .iter()
                .min_by_key(|(_, e)| e.used)
                .map(|(k, _)| k.clone())
                .ok_or(Error::Rejected("guest texture cache budget"))?;
            self.state.textures.entries.remove(&victim);
        }
        self.state.textures.entries.insert(
            key,
            EntryState {
                texture: result.clone(),
                versions: p.versions,
                bytes,
                used: self.state.epoch,
            },
        );
        Ok(result)
    }

    fn load_guest_texture_cpu(
        &mut self,
        memory: &MemoryFrame<'_>,
        fetch: &TextureFetchConstant,
        signed: bool,
        key: Key,
        p: Prepared,
    ) -> std::result::Result<GuestTexture, LoadError> {
        // Every decoder read uses an immutable owned snapshot. The decoder is
        // never permitted to consult the live CPU baseline of GPU-newer bytes.
        struct Snapshot(BTreeMap<u32, std::sync::Arc<[u8; 4096]>>);
        impl xenos_texture::GuestMemory for Snapshot {
            fn read(&self, address: u32, out: &mut [u8]) {
                let mut address = address;
                let mut done = 0;
                while done < out.len() {
                    let offset = (address % 4096) as usize;
                    let n = (4096 - offset).min(out.len() - done);
                    let page = self
                        .0
                        .get(&(address / 4096 * 4096))
                        .expect("validated CPU texture decoder extent");
                    out[done..done + n].copy_from_slice(&page[offset..offset + n]);
                    done += n;
                    address += n as u32;
                }
            }
        }
        let mut snapshot = Snapshot(BTreeMap::new());
        for &(address, _) in &p.versions {
            snapshot.0.insert(address, memory.cpu_page(address)?);
        }
        let caps = HostCaps {
            texture_compression_bc: self
                .gpu
                .owner
                .features
                .contains(types::Features::TEXTURE_COMPRESSION_BC),
            norm16: self
                .gpu
                .owner
                .features
                .contains(types::Features::TEXTURE_FORMAT_16BIT_NORM),
        };
        let loaded = xenos_texture::load_texture(
            fetch,
            &snapshot,
            &xenos_texture::LoadOptions {
                caps,
                signed,
                row_alignment: 256,
            },
        )
        .map_err(backend_error)?;
        if loaded.mapping != p.mapping || loaded.mip_level_count != p.descriptor.mip_levels {
            return Err(Error::Rejected("guest CPU texture plan mismatch").into());
        }
        let bytes = loaded
            .levels
            .iter()
            .map(|l| l.data.len() as u64)
            .sum::<u64>();
        if bytes == 0 || bytes > CACHE_BYTES {
            return Err(Error::Rejected("guest CPU texture upload budget").into());
        }
        let mut reservation = self
            .gpu
            .reserve_ordered_upload(self.encoder.as_mut().unwrap(), bytes)
            .map_err(|error| LoadError {
                error,
                retry_bytes: Some(bytes),
            })?;
        let texture = self.gpu.create_texture_recorded(
            self.encoder.as_mut().unwrap(),
            "CPU decoded guest texture",
            p.descriptor.clone(),
        )?;
        let view = self.gpu.texture_view(
            &texture,
            p.dimension,
            0..p.descriptor.mip_levels,
            0..if p.dimension == types::TextureViewDimension::D3 {
                1
            } else {
                p.descriptor.size.depth_or_array_layers
            },
        )?;
        let upload = self.gpu.raw_buffer(
            "owned CPU texture upload",
            bytes,
            U::COPY_DST | U::COPY_SRC,
            false,
        )?;
        let mut offset = 0u64;
        let uploads: Vec<_> = loaded
            .levels
            .iter()
            .map(|l| {
                let at = offset;
                offset += l.data.len() as u64;
                crate::OrderedUpload {
                    destination: &upload,
                    offset: at,
                    data: &l.data,
                }
            })
            .collect();
        let encoder = self.encoder.as_mut().unwrap();
        self.gpu
            .write_buffers_reserved(encoder, &mut reservation, &uploads)?;
        offset = 0;
        for l in &loaded.levels {
            encoder.copy_buffer_to_texture(
                &upload,
                types::TexelCopyBufferLayout {
                    offset,
                    bytes_per_row: Some(l.bytes_per_row),
                    rows_per_image: Some(l.rows_per_image),
                },
                &texture,
                l.level,
                types::Origin3d::ZERO,
                types::Extent3d {
                    width: l.copy_width,
                    height: l.copy_height,
                    depth_or_array_layers: l.depth_or_array_layers,
                },
            )?;
            offset += l.data.len() as u64;
        }
        self.publish_texture(key, p, view, bytes)
            .map_err(Into::into)
    }
}

impl Frame<'_> {
    pub(super) fn guest_texture_group(
        &mut self,
        program: &crate::guest_shader::GuestPipeline,
        memory: &MemoryFrame<'_>,
        fetch_bytes: &[u8],
        textures: &[GuestTextureBinding<'_>],
    ) -> Result<Group<Platform>> {
        use types::TextureViewDimension as V;
        use xenos_shader::TextureBindingDimension as D;
        if fetch_bytes.len() != 768 || textures.len() > 32 || program.textures.is_empty() {
            return Err(Error::Rejected("guest texture bindings/fetch size"));
        }
        let mut supplied = BTreeMap::new();
        for binding in textures {
            if binding.fetch_constant >= 32
                || supplied
                    .insert(binding.fetch_constant, binding.texture)
                    .is_some()
            {
                return Err(Error::Rejected("duplicate/invalid guest texture slot"));
            }
        }
        let mut used = std::collections::BTreeSet::new();
        let mut views = Vec::new();
        let mut samplers = Vec::new();
        for binding in &program.textures {
            let texture = *supplied
                .get(&binding.fetch_constant)
                .ok_or(Error::Rejected("missing guest texture slot"))?;
            used.insert(binding.fetch_constant);
            texture.view(self)?;
            memory.validate_versions(&texture.versions)?;
            let offset = binding.fetch_constant as usize * 24;
            let words = std::array::from_fn(|i| {
                u32::from_le_bytes(
                    fetch_bytes[offset + i * 4..offset + i * 4 + 4]
                        .try_into()
                        .unwrap(),
                )
            });
            if words != texture.fetch || texture.signed {
                return Err(Error::Rejected("guest texture fetch/signed view mismatch"));
            }
            let fetch = TextureFetchConstant::decode(words);
            let dimension = match binding.dimension {
                D::D2 => V::D2,
                D::Cube => V::Cube,
                D::D3OrStacked => V::D3,
            };
            let is3d = texture.descriptor().dimension == types::TextureDimension::D3;
            let view = |frame: &mut Self, kind: V, dummy: bool| -> Result<TextureView<Platform>> {
                if dummy {
                    if let Some(v) = frame.cache.texture_kernels.dummies.get(&kind) {
                        return Ok(v.clone());
                    }
                    let t = frame.gpu.create_texture(
                        "unused guest texture dimension",
                        TextureDescriptor {
                            size: types::Extent3d {
                                width: 1,
                                height: 1,
                                depth_or_array_layers: 1,
                            },
                            mip_levels: 1,
                            sample_count: 1,
                            dimension: if kind == V::D3 {
                                types::TextureDimension::D3
                            } else {
                                types::TextureDimension::D2
                            },
                            format: F::Rgba8Unorm,
                            usage: types::TextureUses::RESOURCE | types::TextureUses::COPY_DST,
                        },
                    )?;
                    let v = frame.gpu.texture_view(&t, kind, 0..1, 0..1)?;
                    frame.cache.texture_kernels.dummies.insert(kind, v.clone());
                    return Ok(v);
                }
                if let Some(v) = texture.views.borrow().get(&kind) {
                    return Ok(v.clone());
                }
                let layers = match kind {
                    V::D3 | V::D2 => 1,
                    V::Cube => 6,
                    _ => texture.descriptor().size.depth_or_array_layers,
                };
                let v = frame.gpu.texture_view(
                    texture.view.texture(),
                    kind,
                    0..texture.descriptor().mip_levels,
                    0..layers,
                )?;
                texture.views.borrow_mut().insert(kind, v.clone());
                Ok(v)
            };
            views.push((
                binding.texture_binding,
                view(
                    self,
                    dimension,
                    binding.dimension == D::D3OrStacked && !is3d,
                )?,
            ));
            if binding.dimension == D::D3OrStacked {
                let stacked = binding
                    .stacked_texture_binding
                    .ok_or(Error::Rejected("guest stacked texture binding"))?;
                views.push((stacked, view(self, V::D2Array, is3d)?));
            }
            let filter = |f| match f {
                xenos_shader::parsed::TextureFilter::Point => xenos_texture::TextureFilter::Point,
                xenos_shader::parsed::TextureFilter::Linear => xenos_texture::TextureFilter::Linear,
                xenos_shader::parsed::TextureFilter::BaseMap => {
                    xenos_texture::TextureFilter::BaseMap
                }
                xenos_shader::parsed::TextureFilter::UseFetchConst => {
                    xenos_texture::TextureFilter::UseFetchConst
                }
            };
            let filters = xenos_texture::FetchInstructionFilters {
                mag_filter: filter(binding.mag_filter),
                min_filter: filter(binding.min_filter),
                mip_filter: filter(binding.mip_filter),
                aniso_filter: match binding.aniso_filter {
                    xenos_shader::parsed::AnisoFilter::Disabled => {
                        xenos_texture::AnisoFilter::Disabled
                    }
                    xenos_shader::parsed::AnisoFilter::Max1To1 => {
                        xenos_texture::AnisoFilter::Max1to1
                    }
                    xenos_shader::parsed::AnisoFilter::Max2To1 => {
                        xenos_texture::AnisoFilter::Max2to1
                    }
                    xenos_shader::parsed::AnisoFilter::Max4To1 => {
                        xenos_texture::AnisoFilter::Max4to1
                    }
                    xenos_shader::parsed::AnisoFilter::Max8To1 => {
                        xenos_texture::AnisoFilter::Max8to1
                    }
                    xenos_shader::parsed::AnisoFilter::Max16To1 => {
                        xenos_texture::AnisoFilter::Max16to1
                    }
                    xenos_shader::parsed::AnisoFilter::Invalid6 => {
                        xenos_texture::AnisoFilter::Reserved6
                    }
                    xenos_shader::parsed::AnisoFilter::UseFetchConst => {
                        xenos_texture::AnisoFilter::UseFetchConst
                    }
                },
            };
            let filterable = texture.mapping.host_format.is_filterable()
                || self
                    .gpu
                    .owner
                    .features
                    .contains(types::Features::FLOAT32_FILTERABLE);
            let key = SamplerKey {
                fetch: words,
                filters,
                filterable,
            };
            if !self.cache.texture_kernels.samplers.contains_key(&key) {
                let d = xenos_texture::sampler_descriptor(&fetch, &filters);
                let filter = |f| {
                    if !filterable || f == xenos_texture::FilterMode::Nearest {
                        types::FilterMode::Nearest
                    } else {
                        types::FilterMode::Linear
                    }
                };
                let address = |a| match a {
                    xenos_texture::AddressMode::ClampToEdge => types::AddressMode::ClampToEdge,
                    xenos_texture::AddressMode::Repeat => types::AddressMode::Repeat,
                    xenos_texture::AddressMode::MirrorRepeat => types::AddressMode::MirrorRepeat,
                };
                let all_linear = filterable
                    && [d.mag_filter, d.min_filter, d.mipmap_filter]
                        .iter()
                        .all(|f| *f == xenos_texture::FilterMode::Linear);
                let sampler = self.gpu.sampler(&types::SamplerDescriptor {
                    label: Some("native guest sampler"),
                    address_mode_u: address(d.address_mode_u),
                    address_mode_v: address(d.address_mode_v),
                    address_mode_w: address(d.address_mode_w),
                    mag_filter: filter(d.mag_filter),
                    min_filter: filter(d.min_filter),
                    mipmap_filter: if !filterable
                        || d.mipmap_filter == xenos_texture::FilterMode::Nearest
                    {
                        types::MipmapFilterMode::Nearest
                    } else {
                        types::MipmapFilterMode::Linear
                    },
                    lod_min_clamp: d.lod_min_clamp,
                    lod_max_clamp: d.lod_max_clamp,
                    anisotropy_clamp: if all_linear {
                        d.max_anisotropy.clamp(1, 16)
                    } else {
                        1
                    },
                    ..Default::default()
                })?;
                if !d.losses.is_empty() {
                    log::debug!("native guest sampler approximations: {:?}", d.losses);
                }
                if self.cache.texture_kernels.samplers.len() >= 1024 {
                    self.cache.texture_kernels.samplers.clear();
                }
                self.cache
                    .texture_kernels
                    .samplers
                    .insert(key, sampler.clone());
                samplers.push((binding.sampler_binding, sampler));
            } else {
                samplers.push((
                    binding.sampler_binding,
                    self.cache.texture_kernels.samplers[&key].clone(),
                ));
            }
        }
        if used.len() != supplied.len() {
            return Err(Error::Rejected("unused guest texture slot"));
        }
        let mut entries: Vec<_> = views
            .iter()
            .map(|(binding, view)| Entry {
                binding: *binding,
                resource: Resource::Texture(view),
            })
            .collect();
        entries.extend(samplers.iter().map(|(binding, sampler)| Entry {
            binding: *binding,
            resource: Resource::Sampler(sampler),
        }));
        self.gpu.bind_group(
            program
                .texture_layout
                .as_ref()
                .ok_or(Error::Rejected("guest texture layout absent"))?,
            &entries,
        )
    }
}

fn format(h: HostFormat) -> F {
    macro_rules! formats { ($($v:ident),* $(,)?) => { match h { $(HostFormat::$v => F::$v,)* } }; }
    formats!(
        R8Unorm,
        R8Snorm,
        Rg8Unorm,
        Rg8Snorm,
        Rgba8Unorm,
        Rgba8UnormSrgb,
        Rgba8Snorm,
        Bgra8Unorm,
        Bgra8UnormSrgb,
        R16Unorm,
        R16Snorm,
        Rg16Unorm,
        Rg16Snorm,
        Rgba16Unorm,
        Rgba16Snorm,
        R16Float,
        Rg16Float,
        Rgba16Float,
        R32Uint,
        R32Float,
        Rg32Float,
        Rgba32Float,
        Rgb10a2Unorm,
        Rg11b10Ufloat,
        Depth32Float,
        Depth24PlusStencil8,
        Bc1RgbaUnorm,
        Bc1RgbaUnormSrgb,
        Bc2RgbaUnorm,
        Bc2RgbaUnormSrgb,
        Bc3RgbaUnorm,
        Bc3RgbaUnormSrgb,
        Bc4RUnorm,
        Bc4RSnorm,
        Bc5RgUnorm,
        Bc5RgSnorm
    )
}
