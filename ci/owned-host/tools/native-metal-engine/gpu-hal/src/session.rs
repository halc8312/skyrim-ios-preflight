//! One native device session over immutable, CPU-owned prepared commands.
//!
//! This is the hand-off boundary, not a claim that the legacy guest adapter
//! provides coherent captures. In particular, dirty::range_version is NOT a
//! valid GuestPage generation. No native handle is accepted from the producer.
use crate::*;
use crate::{
    edram::targets::{
        ConstantDraw, DrawAdmission, DrawArena, GuestMemory, GuestPage, GuestTextureBinding,
        TargetCache, TargetOptions, TextureAdmission,
    },
    guest_shader::{GuestPipeline, GuestShader},
    pipeline::PipelineCache,
    render::{Indices, Viewport},
};
use std::{collections::HashMap, sync::Arc};
use xenos_shader::{ShaderStage, TranslationOptions};
use xenos_state::{edram::RenderTargetSetup, resolve::ResolveInfo};
mod worker;
pub use worker::{NativeWorker, OfferError, Ticket, WaitError};

/// Exact source and translation options, not a caller-assigned shader ID.
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct Program {
    pub code: Arc<[u32]>,
    pub options: TranslationOptions,
}
#[derive(Clone)]
pub struct IndexData {
    pub bytes: Arc<[u8]>,
    pub format: types::IndexFormat,
    pub base_vertex: i32,
}
#[derive(Clone)]
pub struct DrawPacket {
    pub vertex: Program,
    pub pixel: Option<Program>,
    pub pipeline: xenos_gfx::pipeline::Pipeline<types::TextureFormat>,
    pub targets: RenderTargetSetup,
    pub min_height: u32,
    pub constants: [Arc<[u8]>; 5],
    /// Must satisfy GuestPage's coherent capture contract. Retained unchanged
    /// through submission-pressure retries; the owner never rereads live RAM.
    pub pages: Arc<[GuestPage]>,
    pub indices: Option<IndexData>,
    pub vertices: Range<u32>,
    pub instances: Range<u32>,
    pub viewport: Option<Viewport>,
    pub scissor: Option<[u32; 4]>,
    pub blend: types::Color,
    pub stencil_reference: u32,
}
#[derive(Clone)]
pub enum Packet {
    Draw(Box<DrawPacket>),
    Store {
        info: ResolveInfo,
        pages: Arc<[GuestPage]>,
    },
    /// Explicit readback/diagnostic consumer; not a substitute for presentation.
    ReadPixels(ResolveInfo),
    /// Prepare the ordered swap source. Actual display acquisition/presentation
    /// stays on the same owner via present_to_surface, separately from fences.
    FrontBuffer {
        fetch: [u32; 6],
        pages: Arc<[GuestPage]>,
        display_size: [u32; 2],
        options: crate::presentation::Options,
    },
}
pub struct Batch {
    packets: Vec<Packet>,
}
impl Batch {
    /// Bound the pending CPU snapshot separately from GPU staging/residency.
    /// Shared allocations are deliberately charged per reference (conservative).
    pub fn new(packets: Vec<Packet>) -> Result<Self> {
        if packets.is_empty() || packets.len() > 256 {
            return Err(Error::Rejected("native batch command count"));
        }
        let mut bytes = 0usize;
        for packet in &packets {
            let add = match packet {
                Packet::Draw(p) => {
                    if p.pages.len() > 4096
                        || p.constants.iter().any(|b| b.len() > 4096)
                        || p.vertex.code.len() > 65536
                        || p.pixel.as_ref().is_some_and(|p| p.code.len() > 65536)
                        || p.indices.as_ref().is_some_and(|i| i.bytes.len() > 16 << 20)
                    {
                        return Err(Error::Rejected("native packet input budget"));
                    }
                    let shaders =
                        p.vertex.code.len() + p.pixel.as_ref().map_or(0, |p| p.code.len());
                    if shaders > 65536 {
                        return Err(Error::Rejected("native packet shader size"));
                    }
                    p.pages.len() * 4096
                        + p.constants.iter().map(|b| b.len()).sum::<usize>()
                        + shaders * 4
                        + p.indices.as_ref().map_or(0, |i| i.bytes.len())
                }
                Packet::Store { pages, .. } | Packet::FrontBuffer { pages, .. } => {
                    if pages.len() > 4096 {
                        return Err(Error::Rejected("native store page budget"));
                    }
                    pages.len() * 4096
                }
                Packet::ReadPixels(_) => 0,
            };
            bytes = bytes
                .checked_add(add)
                .ok_or(Error::Rejected("native batch size overflow"))?;
            if bytes > 32 << 20 {
                return Err(Error::Rejected("native batch snapshot budget"));
            }
        }
        Ok(Self { packets })
    }
    pub fn len(&self) -> usize {
        self.packets.len()
    }
    pub fn is_empty(&self) -> bool {
        self.packets.is_empty()
    }
}

/// Adapter for the existing strict whole-page writeback contract. A successful
/// callback must atomically compare BOTH bytes and every-write generation, then
/// replace both, excluding actual CPU writers. A renderer-only mutex cannot do
/// this for live guest stores. False never publishes successful completion.
pub trait CpuCommit: Send + Sync {
    fn compare_exchange(&self, expected: &GuestPage, replacement: &GuestPage) -> bool;
}
/// Suitable for read-only sessions; a GPU store cannot silently become visible.
pub struct NoCpuCommit;
impl CpuCommit for NoCpuCommit {
    fn compare_exchange(&self, _: &GuestPage, _: &GuestPage) -> bool {
        false
    }
}
#[derive(Default, Debug)]
pub struct CompletedBatch {
    pub serial: Serial,
    pub submissions: u64,
    pub draws: u64,
    pub output_rewinds: u64,
    pub pressure_retries: u64,
    pub cpu_pages_written: u32,
    pub front_buffer: Option<[u32; 2]>,
    pub pixels: Vec<PixelResult>,
}
#[derive(Debug)]
pub struct PixelResult {
    pub packet_index: usize,
    pub words: Option<Vec<u32>>,
}

#[derive(Clone, PartialEq, Eq, Hash)]
struct PipelineKey {
    vertex: Program,
    pixel: Option<Program>,
    plan: xenos_gfx::pipeline::Pipeline<types::TextureFormat>,
}
struct Prepared {
    program: Rc<GuestPipeline>,
    indices: Option<Buffer<backend::Platform>>,
}
fn trim_cache<K: Eq + std::hash::Hash + Clone, T>(
    cache: &mut HashMap<K, (Rc<T>, u64)>,
    limit: usize,
) -> Result<()> {
    if cache.len() < limit {
        return Ok(());
    }
    let victim = cache
        .iter()
        .filter(|(_, (value, _))| Rc::strong_count(value) == 1)
        .min_by_key(|(_, (_, used))| *used)
        .map(|(key, _)| key.clone())
        .ok_or(Error::Rejected(
            "native prepared working set exceeds cache budget",
        ))?;
    cache.remove(&victim);
    Ok(())
}
pub struct NativeSession {
    gpu: Gpu<backend::Platform>,
    targets: TargetCache,
    arena: DrawArena,
    memory: GuestMemory,
    shaders: HashMap<(ShaderStage, Program), (Rc<GuestShader>, u64)>,
    programs: HashMap<PipelineKey, (Rc<GuestPipeline>, u64)>,
    clock: u64,
    shader_limit: usize,
    program_limit: usize,
    pipelines: PipelineCache<backend::Platform>,
    presenter: crate::presentation::Presenter<backend::Platform>,
    failed: Option<Error>,
}
impl NativeSession {
    #[cfg(feature = "fault-injection")]
    pub fn test_cache_limits(&mut self, shaders: usize, programs: usize) -> Result<()> {
        if shaders < 2 || programs < 1 || !self.programs.is_empty() {
            return Err(Error::Rejected("session test limits"));
        }
        self.shader_limit = shaders;
        self.program_limit = programs;
        Ok(())
    }
    #[cfg(feature = "fault-injection")]
    pub fn test_cache_counts(&self) -> (usize, usize) {
        (self.shaders.len(), self.programs.len())
    }

    pub fn new(
        mut gpu: Gpu<backend::Platform>,
        options: TargetOptions,
        constant_bytes: u64,
    ) -> Result<Self> {
        let targets = gpu.edram_target_cache(options)?;
        let arena = gpu.xenos_draw_arena(constant_bytes)?;
        let memory = gpu.guest_memory(4)?;
        let presenter = gpu.presenter(types::TextureFormat::Bgra8Unorm)?;
        Ok(Self {
            gpu,
            targets,
            arena,
            memory,
            shaders: HashMap::new(),
            programs: HashMap::new(),
            clock: 0,
            shader_limit: 2048,
            program_limit: 1024,
            pipelines: PipelineCache::new(1024)?,
            presenter,
            failed: None,
        })
    }
    fn shader(&mut self, stage: ShaderStage, p: &Program) -> Result<Rc<GuestShader>> {
        let key = (stage, p.clone());
        if let Some((shader, used)) = self.shaders.get_mut(&key) {
            *used = self.clock;
            return Ok(shader.clone());
        }
        trim_cache(&mut self.shaders, self.shader_limit)?;
        let shader = Rc::new(self.gpu.guest_shader(stage, &p.code, &p.options)?);
        self.shaders.insert(key, (shader.clone(), self.clock));
        Ok(shader)
    }
    fn prepare(&mut self, p: &DrawPacket) -> Result<Prepared> {
        self.clock = self
            .clock
            .checked_add(1)
            .ok_or(Error::Rejected("native session cache clock"))?;
        let key = PipelineKey {
            vertex: p.vertex.clone(),
            pixel: p.pixel.clone(),
            plan: p.pipeline.clone(),
        };
        let program = if let Some((program, used)) = self.programs.get_mut(&key) {
            *used = self.clock;
            program.clone()
        } else {
            trim_cache(&mut self.programs, self.program_limit)?;
            let vs = self.shader(ShaderStage::Vertex, &p.vertex)?;
            let ps = p
                .pixel
                .as_ref()
                .map(|p| self.shader(ShaderStage::Pixel, p))
                .transpose()?;
            if !vs.metadata().texture_bindings.is_empty() {
                return Err(Error::Rejected("native session vertex texture semantics"));
            }
            let mut groups = vec![self.arena.layout().clone(), self.memory.layout().clone()];
            if let Some(ps) = &ps {
                if !ps.metadata().texture_bindings.is_empty() {
                    let mut entries = Vec::new();
                    use types::TextureViewDimension as V;
                    use xenos_shader::TextureBindingDimension as D;
                    for b in &ps.metadata().texture_bindings {
                        let texture = |binding, view_dimension| types::BindGroupLayoutEntry {
                            binding,
                            visibility: types::ShaderStages::FRAGMENT,
                            ty: types::BindingType::Texture {
                                sample_type: types::TextureSampleType::Float { filterable: true },
                                view_dimension,
                                multisampled: false,
                            },
                            count: None,
                        };
                        entries.push(texture(
                            b.texture_binding,
                            match b.dimension {
                                D::D2 => V::D2,
                                D::Cube => V::Cube,
                                D::D3OrStacked => V::D3,
                            },
                        ));
                        if b.dimension == D::D3OrStacked {
                            entries.push(texture(
                                b.stacked_texture_binding
                                    .ok_or(Error::Rejected("native stacked texture layout"))?,
                                V::D2Array,
                            ));
                        }
                        entries.push(types::BindGroupLayoutEntry {
                            binding: b.sampler_binding,
                            visibility: types::ShaderStages::FRAGMENT,
                            ty: types::BindingType::Sampler(types::SamplerBindingType::Filtering),
                            count: None,
                        });
                    }
                    groups.push(self.gpu.group_layout(&entries)?);
                }
            }
            let layout = self.gpu.pipeline_layout(&groups)?;
            let program = Rc::new(self.arena.guest_pipeline(
                &self.gpu,
                &mut self.pipelines,
                &p.pipeline,
                &layout,
                &vs,
                ps.as_deref(),
            )?);
            self.programs.insert(key, (program.clone(), self.clock));
            program
        };
        if p.constants
            .iter()
            .zip(program.constant_bytes())
            .any(|(b, n)| b.len() != n)
        {
            return Err(Error::Rejected("native packet constant size"));
        }
        if let Some(i) = &p.indices {
            let width = if i.format == types::IndexFormat::Uint16 {
                2
            } else {
                4
            };
            if i.bytes.is_empty()
                || i.bytes.len() % width != 0
                || p.vertices.end as usize > i.bytes.len() / width
            {
                return Err(Error::Rejected("native packet index range"));
            }
        }
        let pages: std::collections::BTreeSet<_> = p.pages.iter().map(|p| p.address).collect();
        let shape = ConstantDraw {
            pipeline: program.pipeline(),
            groups: &[],
            constants: p.constants.each_ref().map(|b| b.as_ref()),
            vertices: p.vertices.clone(),
            instances: p.instances.clone(),
            indices: None,
            viewport: p.viewport,
            scissor: p.scissor,
            blend: p.blend,
            stencil_reference: p.stencil_reference,
        };
        program.validate_capture_parts(
            &shape,
            p.indices.is_some(),
            p.indices.as_ref().map_or(0, |i| i.base_vertex),
            |a| pages.contains(&a),
        )?;
        for binding in &program.textures {
            let offset = binding.fetch_constant as usize * 24;
            let bytes = p.constants[3]
                .get(offset..offset + 24)
                .ok_or(Error::Rejected("native packet fetch constants"))?;
            let words = std::array::from_fn(|i| {
                u32::from_le_bytes(bytes[i * 4..i * 4 + 4].try_into().unwrap())
            });
            if xenos_texture::TextureFetchConstant::decode(words).encode() != words {
                return Err(Error::Rejected("native packet noncanonical texture fetch"));
            }
        }
        Ok(Prepared {
            program,
            indices: None,
        })
    }
    /// Runs a sealed batch on THIS owner thread. No successful return until
    /// GPU completion and all required CPU commits. Any unexpected failure
    /// makes the session unusable; no alternate-backend replay is attempted.
    pub fn execute(
        &mut self,
        batch: Batch,
        commit: &dyn CpuCommit,
        timeout: Duration,
    ) -> Result<CompletedBatch> {
        if let Some(error) = &self.failed {
            return Err(error.clone());
        }
        let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            self.execute_inner(batch, commit, timeout)
        }));
        let result = match result {
            Ok(result) => result,
            Err(payload) => {
                self.failed = Some(Error::Rejected("native session unwound"));
                std::panic::resume_unwind(payload);
            }
        };
        if let Err(error) = &result {
            self.failed = Some(error.clone());
        }
        result
    }
    fn execute_inner(
        &mut self,
        batch: Batch,
        commit: &dyn CpuCommit,
        timeout: Duration,
    ) -> Result<CompletedBatch> {
        let start = Instant::now();
        let mut result = CompletedBatch::default();
        let mut prepared = batch
            .packets
            .iter()
            .map(|packet| match packet {
                Packet::Draw(p) => self.prepare(p).map(Some),
                _ => Ok(None),
            })
            .collect::<Result<Vec<_>>>()?;
        let mut readbacks = Vec::new();
        let mut front_buffer = None;
        let mut cursor = 0usize;
        let mut retries = 0u32;
        while cursor < batch.packets.len() {
            let mut frame = self
                .targets
                .begin(&mut self.gpu)?
                .with_constants(&mut self.arena)?
                .with_guest_memory(&mut self.memory)?;
            let mut progress = 0usize;
            let mut selected: Option<(RenderTargetSetup, u32)> = None;
            loop {
                let accepted = match &batch.packets[cursor] {
                    Packet::FrontBuffer {
                        fetch,
                        pages,
                        display_size,
                        options,
                    } => {
                        if display_size.iter().any(|&n| n == 0 || n > 8192) {
                            return Err(Error::Rejected("native front display size"));
                        }
                        if frame.sync_guest_pages(pages)? == DrawAdmission::NeedsSubmit {
                            false
                        } else {
                            match frame.load_guest_texture(
                                &xenos_texture::TextureFetchConstant::decode(*fetch),
                                false,
                            )? {
                                TextureAdmission::Loaded(texture) => {
                                    front_buffer = Some((texture, *display_size, *options));
                                    true
                                }
                                TextureAdmission::NeedsSubmit => false,
                            }
                        }
                    }
                    Packet::Draw(p) => {
                        let prepared = prepared[cursor].as_mut().unwrap();
                        if frame.sync_guest_pages(&p.pages)? == DrawAdmission::NeedsSubmit {
                            false
                        } else {
                            let mut textures = Vec::new();
                            let mut needs_submit = false;
                            if let Some(indices) =
                                p.indices.as_ref().filter(|_| prepared.indices.is_none())
                            {
                                prepared.indices = frame.packet_indices(&indices.bytes)?;
                                needs_submit = prepared.indices.is_none();
                            }
                            for binding in &prepared.program.textures {
                                if needs_submit {
                                    break;
                                }
                                if textures
                                    .iter()
                                    .any(|(slot, _)| *slot == binding.fetch_constant)
                                {
                                    continue;
                                }
                                let offset = binding.fetch_constant as usize * 24;
                                let bytes = p.constants[3]
                                    .get(offset..offset + 24)
                                    .ok_or(Error::Rejected("native packet fetch constants"))?;
                                let fetch = xenos_texture::TextureFetchConstant::decode(
                                    std::array::from_fn(|i| {
                                        u32::from_le_bytes(
                                            bytes[i * 4..i * 4 + 4].try_into().unwrap(),
                                        )
                                    }),
                                );
                                match frame.load_guest_texture(&fetch, false)? {
                                    TextureAdmission::Loaded(texture) => {
                                        textures.push((binding.fetch_constant, texture))
                                    }
                                    TextureAdmission::NeedsSubmit => {
                                        needs_submit = true;
                                        break;
                                    }
                                }
                            }
                            if needs_submit {
                                false
                            } else {
                                if selected
                                    .as_ref()
                                    .is_none_or(|(s, h)| *s != p.targets || *h < p.min_height)
                                {
                                    frame.targets(&p.targets, p.min_height)?;
                                    selected = Some((p.targets.clone(), p.min_height));
                                }
                                let bindings: Vec<_> = textures
                                    .iter()
                                    .map(|(fetch_constant, texture)| GuestTextureBinding {
                                        fetch_constant: *fetch_constant,
                                        texture,
                                    })
                                    .collect();
                                let draw = ConstantDraw {
                                    pipeline: prepared.program.pipeline(),
                                    groups: &[],
                                    constants: p.constants.each_ref().map(|b| b.as_ref()),
                                    vertices: p.vertices.clone(),
                                    instances: p.instances.clone(),
                                    indices: p.indices.as_ref().zip(prepared.indices.as_ref()).map(
                                        |(i, b)| Indices {
                                            buffer: b,
                                            format: i.format,
                                            offset: 0,
                                            size: i.bytes.len() as u64,
                                            base_vertex: i.base_vertex,
                                        },
                                    ),
                                    viewport: p.viewport,
                                    scissor: p.scissor,
                                    blend: p.blend,
                                    stencil_reference: p.stencil_reference,
                                };
                                (if bindings.is_empty() {
                                    frame.draw_guest(&prepared.program, &draw)?
                                } else {
                                    frame.draw_guest_textured(
                                        &prepared.program,
                                        &draw,
                                        &bindings,
                                    )?
                                }) == DrawAdmission::Accepted
                            }
                        }
                    }
                    Packet::Store { info, pages } => {
                        if frame.sync_guest_pages(pages)? == DrawAdmission::NeedsSubmit {
                            false
                        } else {
                            frame.resolve_guest(info)?;
                            selected = None;
                            true
                        }
                    }
                    Packet::ReadPixels(info) => {
                        readbacks.push((cursor, frame.resolve_pixels(info)?));
                        selected = None;
                        true
                    }
                };
                if !accepted {
                    result.pressure_retries += 1;
                    break;
                }
                cursor += 1;
                progress += 1;
                retries = 0;
                if cursor == batch.packets.len() {
                    break;
                }
            }
            let stats = frame.stats();
            let (serial, _, _) = frame.submit()?;
            result.serial = serial;
            result.submissions += 1;
            result.draws += stats.admitted;
            result.output_rewinds += stats.output_rewinds;
            if progress == 0 {
                self.gpu.wait(
                    serial,
                    timeout
                        .checked_sub(start.elapsed())
                        .ok_or(Error::Timeout(serial))?,
                )?;
                // A single bounded retry may now succeed due to retirement.
                // No fresh snapshot is taken; repeated pressure is a hard error.
                retries += 1;
                if retries > 1 {
                    return Err(Error::Rejected("native packet cannot make progress"));
                }
            }
        }
        let remaining = |serial| {
            timeout
                .checked_sub(start.elapsed())
                .ok_or(Error::Timeout(serial))
        };
        self.gpu.wait(result.serial, remaining(result.serial)?)?;
        for (packet_index, readback) in readbacks {
            let words = readback
                .map(|readback| readback.read(&mut self.gpu, remaining(result.serial)?))
                .transpose()?;
            result.pixels.push(PixelResult {
                packet_index,
                words,
            });
        }
        let pages = self.memory.gpu_newer_pages();
        for pages in pages.chunks(4096) {
            let stats = self.memory.writeback(
                &mut self.gpu,
                pages,
                remaining(result.serial)?,
                |a, b| commit.compare_exchange(a, b),
            )?;
            if stats.conflicted != 0 {
                return Err(Error::Rejected("native session CPU visibility conflict"));
            }
            result.cpu_pages_written += stats.written;
        }
        // Publication of the display source also waits until this whole batch
        // succeeded. A rejected CPU commit cannot show a partially accepted swap.
        if let Some((texture, size, options)) = front_buffer {
            let front = texture.front(&self.gpu, size)?;
            result.front_buffer = self.presenter.swap(&mut self.gpu, Some(front), options)?;
        }
        Ok(result)
    }
    #[cfg(target_vendor = "apple")]
    pub unsafe fn attach_metal_surface(
        &self,
        layer: &objc2_quartz_core::CAMetalLayer,
    ) -> Result<crate::surface::MetalSurface> {
        // Caller inherits the same exclusive layer/lifetime contract.
        unsafe { self.gpu.metal_surface(layer) }
    }
    #[cfg(target_vendor = "apple")]
    pub fn present_to_surface(
        &mut self,
        surface: &mut crate::surface::MetalSurface,
        epoch: crate::surface::SurfaceEpoch,
    ) -> Result<Option<Serial>> {
        if let Some(error) = &self.failed {
            return Err(error.clone());
        }
        let result = surface.present(&mut self.gpu, &self.presenter, epoch);
        if let Err(error) = &result {
            self.failed = Some(error.clone());
        }
        result
    }
    pub fn wait_presentation(&mut self, serial: Serial, timeout: Duration) -> Result<()> {
        if let Some(error) = &self.failed {
            return Err(error.clone());
        }
        let result = self.gpu.wait(serial, timeout);
        if let Err(error) = &result {
            self.failed = Some(error.clone());
        }
        result
    }
    #[cfg(feature = "fault-injection")]
    pub fn test_front_pixels(&mut self, size: [u32; 2], timeout: Duration) -> Result<Vec<u8>> {
        use crate::texture::TextureDescriptor;
        let extent = types::Extent3d {
            width: size[0],
            height: size[1],
            depth_or_array_layers: 1,
        };
        let target = self.gpu.create_texture(
            "session front oracle",
            TextureDescriptor {
                size: extent,
                mip_levels: 1,
                sample_count: 1,
                dimension: types::TextureDimension::D2,
                format: types::TextureFormat::Bgra8Unorm,
                usage: types::TextureUses::COLOR_TARGET | types::TextureUses::COPY_SRC,
            },
        )?;
        let view = self
            .gpu
            .texture_view(&target, types::TextureViewDimension::D2, 0..1, 0..1)?;
        let pitch = (size[0] * 4).next_multiple_of(256);
        let bytes = u64::from(pitch) * u64::from(size[1]);
        let buffer =
            self.gpu
                .create_buffer("session front readback", bytes, U::COPY_DST | U::MAP_READ)?;
        let mut encoder = self.gpu.encoder()?;
        self.presenter.encode(&mut encoder, &view)?;
        encoder.copy_texture_to_buffer(
            &target,
            0,
            types::Origin3d::ZERO,
            extent,
            &buffer,
            types::TexelCopyBufferLayout {
                offset: 0,
                bytes_per_row: Some(pitch),
                rows_per_image: Some(size[1]),
            },
        )?;
        self.gpu.submit(vec![encoder.finish()?])?;
        let data = self.gpu.read_buffer(&buffer, 0..bytes, timeout)?;
        Ok(data
            .chunks(pitch as usize)
            .flat_map(|row| row[..size[0] as usize * 4].iter().copied())
            .collect())
    }
}
