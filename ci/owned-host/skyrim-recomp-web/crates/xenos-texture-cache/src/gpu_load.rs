//! Texture loading from a GPU copy of guest memory, for textures whose data only exists there
//! (EDRAM resolves stored straight into GPU memory leave guest memory itself unchanged).
//!
//! A compute shader does what `xenos_texture::load_texture` does on the CPU — address the guest
//! blocks (tiled 2D/3D or linear, packed mips), apply the fetch constant's endian swap, convert
//! to the host format — writing rows ready for `copy_buffer_to_texture`. Only the conversions
//! resolves can produce are supported; [`plan`] returns `None` for the rest.

use gpu_webgpu::*;
use xenos_texture::{GuestMemory, GuestRange};

/// Guest physical memory mirrored on the GPU.
pub trait GpuSource {
    /// Whether the GPU copy is the up-to-date version of all of `ranges` (and guest memory may
    /// be stale there), so the texture must be loaded from it.
    fn covers(&mut self, ranges: &[GuestRange]) -> bool;
    /// Brings the GPU copy of `ranges` up to date with guest memory, so the texture can be
    /// loaded from it (only what changed since it was last brought up to date is uploaded,
    /// rather than the whole texture). `false` if that isn't possible right now.
    fn sync(&mut self, ranges: &[GuestRange]) -> bool;
    /// Whether CPU fallback can read a current copy. Mixed GPU ownership is
    /// not equivalent to covers=false. Default sources keep their prior contract.
    fn cpu_read_allowed(&mut self, _ranges: &[GuestRange]) -> bool { true }
    /// Encodes copies of physical bytes `[address, address + len)` (both multiples of 4) of the
    /// GPU copy into `dst` at `dst_offset`. `false` (with nothing encoded) if part of the range
    /// isn't mirrored.
    fn gather(&self, encoder: &mut CommandEncoder, address: u32, len: u32, dst: &Buffer, dst_offset: u64) -> bool;
    /// The encoder a load records into, after the work recorded so far; handed back with
    /// [`GpuSource::return_encoder`].
    fn take_encoder(&mut self, device: &Device) -> CommandEncoder;
    /// Takes back the encoder with the load recorded: it's submitted right away, or with the
    /// source's batch (see [`GpuSource::batched`]).
    fn return_encoder(&mut self, device: &Device, encoder: CommandEncoder);
    /// Whether loads go with a batch the source submits later (one submission for many loads,
    /// in order with the draws around them). The source must then call
    /// `TextureCache::flush_loads` before submitting it: the loads' parameters are uploaded
    /// together then.
    fn batched(&self) -> bool;
}

pub(crate) use xenos_texture::gpu_load::plan;
use xenos_texture::gpu_load::{Plan, upload_offsets, SHADER, shader_variant};

/// Parameter slots (one per level, `PARAMS_STRIDE` bytes apart) the loader's uniform buffer
/// starts with. Each load takes slots of its own until [`GpuLoader::end_batch`] (loads queued in
/// one submission all read their parameters as written before it), growing the buffer if need be.
const PARAM_SLOTS: usize = 64;
/// Uniform binding offsets must be multiples of 256.
const PARAMS_STRIDE: u64 = 256;

/// The loader's buffers live as long as it does and are rewritten for every load (with
/// `Queue::write_buffer`, ordered after earlier submissions): creating buffers and bind groups is
/// expensive in browsers (a fraction of a millisecond each), and a frame loads dozens of
/// textures.
#[derive(Default)]
pub(crate) struct GpuLoader {
    /// Plain-store and atomic variants.
    pipelines: [Option<(BindGroupLayout, ComputePipeline)>; 2],
    /// Per-level parameters, in slots.
    params: Option<Buffer>,
    /// Next free parameter slot.
    param_cursor: usize,
    /// Parameters of loads recorded but not uploaded yet (one upload per batch rather than per
    /// load), for the slots from `pending_from` on.
    pending: Vec<u8>,
    pending_from: usize,
    /// Buffers replaced by bigger ones, destroyed at [`GpuLoader::end_batch`] (queued loads may
    /// still use them).
    retired: Vec<Buffer>,
    /// Converted texels, before the copy into the texture (grows as needed).
    out: Option<Buffer>,
    /// Raw guest bytes for loads from guest memory (grows as needed).
    upload: Option<Buffer>,
    /// Bind groups by (atomic, source buffer, output buffer); the parameter slot is a dynamic
    /// offset.
    groups: rustc_hash::FxHashMap<(bool, usize, usize), BindGroup>,
}

/// A buffer of at least `size` bytes in `slot`: the existing one if big enough, else a new one
/// (rounded up to a power of two; the old one goes to `retired`). Returns whether it was
/// replaced.
fn ensure(
    slot: &mut Option<Buffer>,
    retired: &mut Vec<Buffer>,
    device: &Device,
    label: &str,
    usage: BufferUsage,
    size: u64,
) -> bool {
    if slot.as_ref().is_some_and(|b| b.size() >= size) {
        return false;
    }
    if let Some(old) = slot.take() {
        retired.push(old);
    }
    *slot = Some(device.create_buffer(&BufferDescriptor {
        label,
        size: size.max(1 << 16).next_power_of_two(),
        usage,
        mapped_at_creation: false,
    }));
    true
}

impl GpuLoader {
    fn pipeline(&mut self, device: &Device, atomic: bool) -> &(BindGroupLayout, ComputePipeline) {
        self.pipelines[atomic as usize].get_or_insert_with(|| {
            let entry = |binding, ty| BindGroupLayoutEntry {
                binding,
                visibility: ShaderStage::Compute,
                ty: BindingType::Buffer {
                    ty,
                    has_dynamic_offset: ty == BufferBindingType::Uniform,
                    min_binding_size: 0,
                },
            };
            let bgl = device.create_bind_group_layout(
                "texture gpu load",
                &[
                    entry(0, BufferBindingType::Uniform),
                    entry(1, BufferBindingType::ReadOnlyStorage),
                    entry(2, BufferBindingType::Storage),
                ],
            );
            let layout = device.create_pipeline_layout("texture gpu load", &[&bgl]);
            let module = device.create_shader_module_wgsl("texture gpu load", &shader_variant(SHADER, atomic));
            let pipeline = device.create_compute_pipeline(&ComputePipelineDescriptor {
                label: "texture gpu load",
                layout: Some(&layout),
                stage: ProgrammableStage::new(&module, "main"),
            });
            (bgl, pipeline)
        })
    }

    /// Fills `texture` from the GPU copy of guest memory and submits the work: each level's raw
    /// bytes are gathered (copied on the GPU) into the upload buffer and converted there. Returns
    /// `false` (nothing done) if part of the data isn't mirrored.
    pub(crate) fn encode(
        &mut self,
        device: &Device,
        source: &mut dyn GpuSource,
        plan: &Plan,
        texture: &Texture,
    ) -> bool {
        let (offsets, total) = upload_offsets(plan);
        let usage = BufferUsage::Storage | BufferUsage::CopyDst;
        if ensure(&mut self.upload, &mut self.retired, device, "texture upload", usage, total) {
            self.groups.clear();
        }
        let upload = self.upload.clone().expect("ensured above");
        let mut encoder = source.take_encoder(device);
        let mut sources = Vec::with_capacity(plan.levels.len());
        for (level, &offset) in plan.levels.iter().zip(&offsets) {
            let (address, len) = level.source;
            // Rounded out to whole dwords; the extra bytes are never read as texels.
            let start = address & !3;
            let end = (u64::from(address) + u64::from(len.max(1))).next_multiple_of(4);
            if !source.gather(&mut encoder, start, (end - u64::from(start)) as u32, &upload, offset) {
                // Copies gathered so far only fill the upload buffer, which nothing reads yet.
                source.return_encoder(device, encoder);
                return false;
            }
            sources.push((upload.clone(), start.wrapping_sub(offset as u32)));
        }
        // The conversion follows the copies.
        self.dispatch(device, &mut encoder, &sources, plan, texture);
        if !source.batched() {
            self.flush_params(device);
        }
        source.return_encoder(device, encoder);
        true
    }

    /// Fills `texture` from guest memory: each level's raw bytes are copied into the upload
    /// buffer and converted there (instead of texel by texel on the CPU).
    pub(crate) fn encode_from_memory(
        &mut self,
        device: &Device,
        memory: &dyn GuestMemory,
        plan: &Plan,
        texture: &Texture,
    ) -> u64 {
        let (offsets, total) = upload_offsets(plan);
        let usage = BufferUsage::Storage | BufferUsage::CopyDst;
        if ensure(&mut self.upload, &mut self.retired, device, "texture upload", usage, total) {
            self.groups.clear();
        }
        let upload = self.upload.clone().expect("ensured above");
        let mut uploaded = 0u64;
        let mut bytes = Vec::new();
        let sources: Vec<(Buffer, u32)> = plan
            .levels
            .iter()
            .zip(&offsets)
            .map(|(level, &offset)| {
                let (address, len) = level.source;
                bytes.clear();
                bytes.resize((len as usize).next_multiple_of(4).max(4), 0);
                memory.read(address, &mut bytes);
                device.queue().write_buffer(&upload, offset, &bytes);
                uploaded += bytes.len() as u64;
                // The shader indexes the source from the guest address `base` on: the level's
                // address sits `offset` bytes into the buffer.
                (upload.clone(), address.wrapping_sub(offset as u32))
            })
            .collect();
        // Submitted at once: the upload is written for the next submission, and loads queued
        // into one would all see the last load's bytes.
        let mut encoder = device.create_command_encoder("texture gpu load");
        self.dispatch(device, &mut encoder, &sources, plan, texture);
        self.flush_params(device);
        device.queue().submit([encoder.finish("texture gpu load")]);
        uploaded
    }

    /// Uploads the parameters of the loads recorded since the last call (before the commands
    /// using them are submitted).
    pub(crate) fn flush_params(&mut self, device: &Device) {
        if let Some(params) = self.params.as_ref().filter(|_| !self.pending.is_empty()) {
            device.queue().write_buffer(params, self.pending_from as u64 * PARAMS_STRIDE, &self.pending);
        }
        self.pending.clear();
        self.pending_from = self.param_cursor;
    }

    /// Converts each level from its source buffer (holding guest bytes from address `base` on)
    /// into `texture`, recording into `encoder` (after anything already in it).
    fn dispatch(
        &mut self,
        device: &Device,
        encoder: &mut CommandEncoder,
        sources: &[(Buffer, u32)],
        plan: &Plan,
        texture: &Texture,
    ) {
        let mut replaced = ensure(
            &mut self.out,
            &mut self.retired,
            device,
            "texture gpu load",
            BufferUsage::Storage | BufferUsage::CopySrc,
            plan.out_bytes.max(4),
        );
        let slots = (self.param_cursor + plan.levels.len()).max(PARAM_SLOTS);
        let params_usage = BufferUsage::Uniform | BufferUsage::CopyDst;
        if self.params.as_ref().is_some_and(|b| b.size() < slots as u64 * PARAMS_STRIDE) {
            // Pending parameters belong in the buffer the loads recorded so far bind.
            self.flush_params(device);
        }
        if ensure(
            &mut self.params,
            &mut self.retired,
            device,
            "texture gpu load params",
            params_usage,
            slots as u64 * PARAMS_STRIDE,
        ) {
            // Slots of the old buffer stay as written; a new one starts over.
            self.param_cursor = 0;
            self.pending_from = 0;
            replaced = true;
        }
        if replaced {
            self.groups.clear();
        }
        let (out, params) = (self.out.clone().expect("ensured"), self.params.clone().expect("ensured"));
        // Host texels narrower than a dword share dwords between invocations.
        let atomic = plan.levels.first().is_some_and(|l| l.params[19] < 4);
        let (bgl, pipeline) = self.pipeline(device, atomic).clone();
        let mut groups = Vec::with_capacity(plan.levels.len());
        // The levels' parameters, in consecutive slots.
        let first_slot = self.param_cursor;
        debug_assert_eq!(self.pending_from + self.pending.len() / PARAMS_STRIDE as usize, first_slot);
        for (level, (_, base)) in plan.levels.iter().zip(sources) {
            let mut words = level.params;
            words[21] = *base;
            words[25] = (level.out_offset / 4) as u32;
            self.pending.extend(words.iter().flat_map(|w| w.to_le_bytes()));
            self.pending.resize(self.pending.len().next_multiple_of(PARAMS_STRIDE as usize), 0);
        }
        self.param_cursor += plan.levels.len();
        for (_, (buffer, _)) in plan.levels.iter().zip(sources) {
            let key = (atomic, buffer.identity(), out.identity());
            let group = self.groups.entry(key).or_insert_with(|| {
                device.create_bind_group(
                    "texture gpu load",
                    &bgl,
                    &[
                        BindGroupEntry {
                            binding: 0,
                            resource: BindingResource::Buffer { buffer: &params, offset: 0, size: Some(32 * 4) },
                        },
                        BindGroupEntry {
                            binding: 1,
                            resource: BindingResource::Buffer { buffer, offset: 0, size: None },
                        },
                        BindGroupEntry {
                            binding: 2,
                            resource: BindingResource::Buffer { buffer: &out, offset: 0, size: None },
                        },
                    ],
                )
            });
            groups.push(group.clone());
        }
        {
            let mut pass = encoder.begin_compute_pass("texture gpu load");
            pass.set_pipeline(&pipeline);
            for (n, (level, group)) in plan.levels.iter().zip(&groups).enumerate() {
                let (x, y, z) = level.blocks;
                pass.set_bind_group(0, group, &[((first_slot + n) as u64 * PARAMS_STRIDE) as u32]);
                pass.dispatch_workgroups(x.div_ceil(8), y.div_ceil(8), z);
            }
            pass.end();
        }
        for level in &plan.levels {
            encoder.copy_buffer_to_texture(
                &TexelCopyBufferInfo {
                    buffer: &out,
                    offset: level.out_offset,
                    bytes_per_row: level.bytes_per_row,
                    rows_per_image: Some(level.rows_per_image),
                },
                &TexelCopyTextureInfo {
                    texture,
                    mip_level: level.level,
                    origin: Origin3d { x: 0, y: 0, z: 0 },
                    aspect: TextureAspect::All,
                },
                extent(level.copy.0, level.copy.1, level.copy.2),
            );
        }
    }

    /// Everything recorded with the loader so far was submitted: its parameter slots can be
    /// reused and the buffers it replaced freed.
    pub(crate) fn end_batch(&mut self) {
        debug_assert!(self.pending.is_empty(), "texture loads submitted before flush_loads");
        self.param_cursor = 0;
        self.pending_from = 0;
        for buffer in self.retired.drain(..) {
            buffer.destroy();
        }
    }
}

