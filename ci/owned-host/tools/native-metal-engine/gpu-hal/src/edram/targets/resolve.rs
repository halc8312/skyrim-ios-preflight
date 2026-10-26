//! Native targets -> canonical EDRAM -> converted pixels -> readback OR GPU
//! guest store. CPU tickets require accepted submission and verified completion.
//! Guest stores and post-copy clears share the transactional frame.
use super::*;
use crate::{
    bindings::Group,
    compute::{ComputeDispatch, SharedComputePipeline},
};
use std::time::{Duration, Instant};
use xenos_gfx::edram_resolve::{copy_encoding, copy_words};
use xenos_state::{
    resolve::ResolveInfo,
    xenos::{ColorRenderTargetFormat, CopyCommand, MsaaSamples},
};

const OUTPUT_BUDGET: u64 = 64 << 20;
#[derive(Clone, Copy, PartialEq, Eq)]
pub(super) enum Submission {
    Staged,
    Submitted(Serial),
    Abandoned,
}

pub struct ResolvePixels {
    owner: Rc<Owner<Platform>>,
    submission: Rc<Cell<Submission>>,
    readback: Buffer<Platform>,
    info: ResolveInfo,
    words_per_pixel: u32,
    bytes: u64,
}
impl ResolvePixels {
    pub fn info(&self) -> &ResolveInfo {
        &self.info
    }
    pub fn words_per_pixel(&self) -> u32 {
        self.words_per_pixel
    }
    pub fn serial(&self) -> Option<Serial> {
        match self.submission.get() {
            Submission::Submitted(s) => Some(s),
            _ => None,
        }
    }
    pub fn read(&self, gpu: &mut Gpu<Platform>, timeout: Duration) -> Result<Vec<u32>> {
        if !Rc::ptr_eq(&self.owner, &gpu.owner) {
            return Err(Error::Rejected("resolve ticket device"));
        }
        let serial = match self.submission.get() {
            Submission::Submitted(s) => s,
            _ => return Err(Error::Rejected("resolve frame was not submitted")),
        };
        let start = Instant::now();
        gpu.wait(serial, timeout)?;
        let left = timeout
            .checked_sub(start.elapsed())
            .ok_or(Error::Timeout(serial))?;
        let bytes = gpu.read_buffer(&self.readback, 0..self.bytes, left)?;
        Ok(bytes
            .chunks_exact(4)
            .map(|b| u32::from_le_bytes(b.try_into().unwrap()))
            .collect())
    }
}

#[derive(Clone)]
struct Kernel {
    layout: GroupLayout<Platform>,
    pipeline: SharedComputePipeline<Platform>,
}
#[derive(Default)]
pub(super) struct Pipelines {
    dumps: HashMap<kernels::SrcKind, Kernel>,
    resolve: Option<Kernel>,
    stores: HashMap<(bool, bool), Kernel>,
}
fn uniform() -> types::BindGroupLayoutEntry {
    types::BindGroupLayoutEntry {
        binding: 0,
        visibility: S::COMPUTE,
        ty: types::BindingType::Buffer {
            ty: types::BufferBindingType::Uniform,
            has_dynamic_offset: true,
            min_binding_size: NonZeroU64::new(96),
        },
        count: None,
    }
}
fn storage(binding: u32, read_only: bool) -> types::BindGroupLayoutEntry {
    types::BindGroupLayoutEntry {
        binding,
        visibility: S::COMPUTE,
        ty: types::BindingType::Buffer {
            ty: types::BufferBindingType::Storage { read_only },
            has_dynamic_offset: false,
            min_binding_size: NonZeroU64::new(4),
        },
        count: None,
    }
}
fn buffer<'a>(binding: u32, b: &'a Buffer<Platform>, size: u64) -> Entry<'a, Platform> {
    Entry {
        binding,
        resource: Resource::Buffer {
            buffer: b,
            offset: 0,
            size,
        },
    }
}
impl Pipelines {
    fn dump(&mut self, gpu: &Gpu<Platform>, kind: kernels::SrcKind, scale: u32) -> Result<Kernel> {
        if let Some(k) = self.dumps.get(&kind) {
            return Ok(k.clone());
        }
        let sample = |binding, kind_sample| types::BindGroupLayoutEntry {
            binding,
            visibility: S::COMPUTE,
            ty: types::BindingType::Texture {
                sample_type: kind_sample,
                view_dimension: V::D2,
                multisampled: kind.multisampled(),
            },
            count: None,
        };
        let depth = matches!(kind, kernels::SrcKind::Depth { .. });
        let mut entries = vec![
            uniform(),
            sample(
                1,
                if depth {
                    Sample::Depth
                } else {
                    Sample::Float { filterable: false }
                },
            ),
            storage(3, false),
        ];
        if depth {
            entries.push(sample(2, Sample::Uint));
        }
        let layout = gpu.group_layout(&entries)?;
        let pl = gpu.pipeline_layout(&[layout.clone()])?;
        let shader = gpu.shader(&kernels::dump_module(kind, scale))?;
        let pipeline = gpu.shared_compute_pipeline(&pl, &shader, "main")?;
        let result = Kernel { layout, pipeline };
        self.dumps.insert(kind, result.clone());
        Ok(result)
    }
    fn store(&mut self, gpu: &Gpu<Platform>, atomic: bool, masked: bool) -> Result<Kernel> {
        if let Some(k) = self.stores.get(&(atomic, masked)) {
            return Ok(k.clone());
        }
        let mut params = uniform();
        params.ty = types::BindingType::Buffer {
            ty: types::BufferBindingType::Uniform,
            has_dynamic_offset: true,
            min_binding_size: NonZeroU64::new(64),
        };
        let mut entries = vec![
            params,
            storage(1, true),
            storage(2, true),
            storage(3, false),
            storage(4, false),
            storage(5, false),
            storage(6, false),
        ];
        if masked {
            entries.push(storage(7, false));
        }
        let layout = gpu.group_layout(&entries)?;
        let pl = gpu.pipeline_layout(&[layout.clone()])?;
        let shader = gpu.shader(&if masked {
            kernels::store_masked_module(atomic)
        } else {
            kernels::store_module(atomic)
        })?;
        let pipeline = gpu.shared_compute_pipeline(&pl, &shader, "main")?;
        let result = Kernel { layout, pipeline };
        self.stores.insert((atomic, masked), result.clone());
        Ok(result)
    }
    fn resolve(&mut self, gpu: &Gpu<Platform>, scale: u32) -> Result<Kernel> {
        if let Some(k) = &self.resolve {
            return Ok(k.clone());
        }
        let layout = gpu.group_layout(&[uniform(), storage(1, true), storage(2, false)])?;
        let pl = gpu.pipeline_layout(&[layout.clone()])?;
        let shader = gpu.shader(&kernels::resolve_module(scale))?;
        let pipeline = gpu.shared_compute_pipeline(&pl, &shader, "main")?;
        let result = Kernel { layout, pipeline };
        self.resolve = Some(result.clone());
        Ok(result)
    }
}
struct Dispatch {
    kernel: Kernel,
    group: Group<Platform>,
    offset: u32,
    grid: [u32; 3],
}
struct Plan {
    info: ResolveInfo,
    params: [u32; 24],
    spans: Vec<(u32, u32)>,
    words: u32,
    bytes: u64,
}
impl Plan {
    fn new(info: &ResolveInfo, max_bytes: u64) -> Result<Option<Self>> {
        if info.rect.is_empty() {
            return Ok(None);
        }
        if info.copy_dest_extent_length == 0 {
            return Ok(None);
        }
        let Some(source) = info.source_edram() else {
            return Ok(None);
        };
        if source.msaa_samples.count().is_none()
            || source.is_depth != info.is_copying_depth()
            || !(1..=2048).contains(&source.pitch_tiles)
            || source.base_tiles > 0x00ff_ffff
            || source.format_is_64bpp && source.pitch_tiles % 2 != 0
            || info.copy_control.copy_command == CopyCommand::Null
            || info.edram_offset_x_div_8 > 9
            || info.edram_offset_y_div_8 > 1
            || [info.rect.width, info.rect.height]
                .iter()
                .any(|&v| v > 8192 || v % 8 != 0)
            || info.copy_sample_select.raw() > 6
            || source.msaa_samples == MsaaSamples::X1 && info.copy_sample_select.raw() != 0
            || source.msaa_samples == MsaaSamples::X2
                && !matches!(info.copy_sample_select.raw(), 0 | 1 | 4)
            || info.copy_dest_extent_length > 512 << 20
            || (info.copy_dest_extent_start & 0x1fff_ffff)
                .checked_add(info.copy_dest_extent_length)
                .is_none_or(|n| n > 512 << 20)
        {
            return Err(Error::Rejected(
                "native resolve source/rectangle/destination contract",
            ));
        }
        if source.is_depth {
            if source.format > 1 || source.format_is_64bpp {
                return Err(Error::Rejected("native resolve depth format"));
            }
        } else {
            let format = ColorRenderTargetFormat::from_raw(source.format);
            if color_storage(format).is_none() || format.is_64bpp() != source.format_is_64bpp {
                return Err(Error::Rejected("native resolve color format"));
            }
        }
        let (kind, _) = info
            .copy_kind(1, 1)
            .ok_or(Error::Rejected("native resolve conversion"))?;
        let (_, words, _) = copy_encoding(kind);
        let bytes = u64::from(info.rect.width) * u64::from(info.rect.height) * u64::from(words) * 4;
        if bytes > max_bytes {
            return Err(Error::Rejected("native resolve output budget"));
        }
        let (base, used, rows, pitch) = info
            .copy_edram_tile_span()
            .ok_or(Error::Rejected("native resolve tile span"))?;
        let spans = if used >= pitch {
            vec![(base, (rows * pitch).min(EDRAM_TILE_COUNT))]
        } else {
            (0..rows)
                .map(|r| (base.wrapping_add(r * pitch), used))
                .collect()
        };
        Ok(Some(Self {
            info: info.clone(),
            params: copy_words(info, kind, [0, 0], 1)
                .ok_or(Error::Rejected("native resolve parameter contract"))?,
            spans,
            words,
            bytes,
        }))
    }
}
impl Frame<'_> {
    /// The established resolve kernel yields linear words. This is the checked
    /// readback/reference route; a GPU guest-memory store consumes the same
    /// words without this readback. Requested clears run after the copy.
    pub fn resolve_pixels(&mut self, info: &ResolveInfo) -> Result<Option<ResolvePixels>> {
        self.healthy()?;
        let result = self.resolve_pixels_inner(info);
        if result.is_err() {
            self.failed = true;
        }
        result
    }
    fn resolve_plan(&self, info: &ResolveInfo) -> Result<Option<Plan>> {
        let cap = (OUTPUT_BUDGET - self.resolve_bytes)
            .min(self.gpu.owner.limits.max_storage_buffer_binding_size);
        Plan::new(info, cap)
    }
    fn resolve_pixels_inner(&mut self, info: &ResolveInfo) -> Result<Option<ResolvePixels>> {
        let clears = self.plan_clears(info)?;
        let Some(plan) = self.resolve_plan(info)? else {
            self.encode_clears(&clears)?;
            return Ok(None);
        };
        let resolved = self.resolve_linear(plan)?;
        #[cfg(feature = "fault-injection")]
        let poison = self.gpu.owner.faults.poison_resolve.get();
        #[cfg(not(feature = "fault-injection"))]
        let poison = false;
        let readback = if poison {
            self.gpu.create_buffer_init(
                "poisoned resolve readback",
                &vec![0x5a; resolved.bytes as usize],
                U::COPY_DST | U::MAP_READ,
            )?
        } else {
            self.gpu.create_buffer(
                "native resolve readback",
                resolved.bytes,
                U::COPY_DST | U::MAP_READ,
            )?
        };
        self.encoder.as_mut().unwrap().copy_buffer(
            &resolved.output,
            0,
            &readback,
            0,
            resolved.bytes,
        )?;
        self.encode_clears(&clears)?;
        Ok(Some(ResolvePixels {
            owner: self.gpu.owner.clone(),
            submission: self.resolve_status.clone(),
            readback,
            info: resolved.info,
            words_per_pixel: resolved.words,
            bytes: resolved.bytes,
        }))
    }
    /// Full pages of the destination extent must have a fresh, tracked CPU
    /// capture. The store remains on the GPU and creates no CPU readback buffer.
    pub(super) fn resolve_guest_inner(
        &mut self,
        info: &ResolveInfo,
        memory: &mut super::guest_memory::MemoryFrame<'_>,
        journaled: bool,
    ) -> Result<(Option<Range<u32>>, Option<GuestWriteJournal>)> {
        let clears = self.plan_clears(info)?;
        if !info.rect.is_empty()
            && info.copy_dest_extent_length != 0
            && info.source_edram().is_none()
        {
            return Err(Error::Rejected("native guest store source missing"));
        }
        let Some(plan) = self.resolve_plan(info)? else {
            self.encode_clears(&clears)?;
            memory.consume_capture();
            return Ok((None, None));
        };
        let start = info.copy_dest_extent_start & 0x1fff_ffff;
        let range = start..start + info.copy_dest_extent_length;
        memory.validate_store(&range)?;
        let (kind, _) = info
            .copy_kind(1, 1)
            .ok_or(Error::Rejected("native guest store kind"))?;
        let (_, words, bits) = copy_encoding(kind);
        let packed = kind == xenos_state::resolve::ResolveCopyKind::Dxt3aAs1111;
        let bpe = bits / 8;
        let dest = info.copy_dest_info;
        validate_store_footprint(info, bpe, packed, &range)?;
        let kernel = self
            .cache
            .resolve_kernels
            .store(self.gpu, bpe < 4, journaled)?;
        let journal = if journaled {
            let aligned = range.start & !3..range.end.next_multiple_of(4);
            let capture = memory.capture_range(aligned.clone())?;
            let data_bytes = u64::from(aligned.end - aligned.start);
            let mask_bytes = u64::from((range.end - range.start).div_ceil(32)) * 4;
            let total = data_bytes + mask_bytes;
            if self.journal_bytes + total > 32 << 20 {
                return Err(Error::Rejected("guest journal frame budget"));
            }
            let mask = self.gpu.raw_buffer(
                "exact guest write mask",
                mask_bytes,
                U::STORAGE_READ_WRITE | U::COPY_SRC | U::COPY_DST,
                false,
            )?;
            let readback = self.gpu.raw_buffer(
                "immutable guest write journal",
                total,
                U::COPY_DST | U::MAP_READ,
                false,
            )?;
            self.encoder
                .as_mut()
                .unwrap()
                .clear_buffer(&mask, 0..mask_bytes)?;
            self.journal_bytes += total;
            Some((
                mask,
                capture,
                GuestWriteJournal {
                    owner: self.gpu.owner.clone(),
                    submission: self.resolve_status.clone(),
                    readback,
                    range: range.clone(),
                    aligned_start: aligned.start,
                    snapshot_bytes: data_bytes,
                    mask_bytes,
                },
            ))
        } else {
            None
        };
        let params = [
            info.copy_dest_base & 0x1fff_ffff,
            info.dest_pitch_aligned,
            info.dest_height_aligned,
            u32::from(dest.copy_dest_array),
            dest.copy_dest_slice,
            info.dest_offset_x_div_8 << 3,
            info.dest_offset_y_div_8 << 3,
            bpe,
            dest.copy_dest_endian.raw(),
            info.rect.width,
            info.rect.height,
            words,
            u32::from(packed),
            (xenos_shader::bindings::SHARED_MEMORY_BLOCK_BYTES / 4).ilog2()
                | ((xenos_shader::bindings::SHARED_MEMORY_CHUNK_BYTES / 4).ilog2() << 8),
            range.start,
            range.end - range.start,
        ];
        let params = self.gpu.create_buffer_init(
            "immutable guest store parameters",
            bytemuck::cast_slice(&params),
            U::UNIFORM | U::COPY_DST,
        )?;
        let resolved = self.resolve_linear(plan)?;
        let (table, chunks) = memory.store_buffers();
        let mut entries = vec![
            buffer(0, &params, 64),
            buffer(1, &resolved.output, resolved.bytes),
            buffer(2, table, table.size()),
            buffer(3, &chunks[0], chunks[0].size()),
            buffer(4, &chunks[1], chunks[1].size()),
            buffer(5, &chunks[2], chunks[2].size()),
            buffer(6, &chunks[3], chunks[3].size()),
        ];
        if let Some((mask, _, _)) = &journal {
            entries.push(buffer(7, mask, mask.size()));
        }
        let group = self.gpu.bind_group(&kernel.layout, &entries)?;
        let divisor = if packed { 4 } else { 1 };
        let encoded = self
            .encoder
            .as_mut()
            .unwrap()
            .dispatch_shared_batch(&[ComputeDispatch {
                pipeline: &kernel.pipeline,
                groups: &[BoundGroup {
                    group: &group,
                    dynamic_offsets: &[0],
                }],
                workgroups: [
                    (info.rect.width / divisor).div_ceil(8),
                    (info.rect.height / divisor).div_ceil(8),
                    1,
                ],
            }])?;
        if encoded.dispatches != 1 {
            return Err(Error::Rejected("native guest store dispatch coverage"));
        }
        let journal = if let Some((mask, capture, ticket)) = journal {
            let encoder = self.encoder.as_mut().unwrap();
            let mut at = 0;
            for (source, offset, bytes) in capture.copies {
                encoder.copy_buffer(&source, offset, &ticket.readback, at, bytes)?;
                at += bytes;
            }
            if at != ticket.snapshot_bytes {
                return Err(Error::Rejected("guest journal capture coverage"));
            }
            encoder.copy_buffer(&mask, 0, &ticket.readback, at, ticket.mask_bytes)?;
            Some(ticket)
        } else {
            None
        };
        memory.mark_gpu_written(&range)?;
        self.encode_clears(&clears)?;
        Ok((Some(range), journal))
    }
    fn resolve_linear(&mut self, plan: Plan) -> Result<Resolved> {
        let info = &plan.info;
        let scale = self.cache.options.scale;
        let tile_words = 1280 * scale * scale;
        let shadow_bytes = u64::from(tile_words) * u64::from(EDRAM_TILE_COUNT) * 4;
        let shadow = if let Some(shadow) = &self.state.resolve_shadow {
            shadow.clone()
        } else {
            let shadow = self.gpu.create_buffer(
                "native canonical EDRAM",
                shadow_bytes,
                U::STORAGE_READ_WRITE | U::STORAGE_READ_ONLY,
            )?;
            self.state.resolve_shadow = Some(shadow.clone());
            shadow
        };
        let mut sources = Vec::new();
        for &(base, count) in &plan.spans {
            for range in self.state.ownership.owners_in(base, count) {
                let target = self
                    .state
                    .targets
                    .get(&range.owner)
                    .ok_or(Error::Rejected("resolve owner missing"))?
                    .clone();
                if sources.len() >= 4096 {
                    return Err(Error::Rejected("native resolve dump budget"));
                }
                sources.push((target, range));
            }
        }
        let alignment = self
            .gpu
            .owner
            .limits
            .min_uniform_buffer_offset_alignment
            .max(1);
        let stride = 96u32.next_multiple_of(alignment);
        let params_bytes = (sources.len() + 1) as u64 * u64::from(stride);

        let mut data = vec![0; params_bytes as usize];
        for (i, (target, range)) in sources.iter().enumerate() {
            let k = target.key;
            let words = [
                range.start * tile_words,
                (range.end - range.start) * tile_words,
                k.base_tiles,
                k.pitch_tiles(),
                k.msaa_samples.raw(),
                u32::from(k.is_depth) | (u32::from(k.is_64bpp()) << 1),
                k.format + if k.is_depth { 16 } else { 0 },
                target.width,
                target.height,
                0,
                0,
                0,
            ];
            for (j, v) in words.iter().enumerate() {
                let p = i * stride as usize + j * 4;
                data[p..p + 4].copy_from_slice(&v.to_le_bytes());
            }
        }
        for (j, v) in plan.params.iter().enumerate() {
            let p = sources.len() * stride as usize + j * 4;
            data[p..p + 4].copy_from_slice(&v.to_le_bytes());
        }
        let params = self.gpu.create_buffer_init(
            "immutable resolve parameters",
            &data,
            U::UNIFORM | U::COPY_DST,
        )?;
        // A test-only nonzero poison proves the kernels and copy actually cover
        // every output word, including legitimate never-owned zero EDRAM.
        #[cfg(feature = "fault-injection")]
        let poison = self.gpu.owner.faults.poison_resolve.get();
        #[cfg(not(feature = "fault-injection"))]
        let poison = false;
        let output = if poison {
            self.gpu.create_buffer_init(
                "poisoned resolve output",
                &vec![0xa5; plan.bytes as usize],
                U::STORAGE_READ_WRITE | U::STORAGE_READ_ONLY | U::COPY_SRC | U::COPY_DST,
            )?
        } else {
            self.gpu.create_buffer(
                "native resolve linear output",
                plan.bytes,
                U::STORAGE_READ_WRITE | U::STORAGE_READ_ONLY | U::COPY_SRC,
            )?
        };
        let mut dispatches = Vec::with_capacity(sources.len() + 1);
        for (i, (target, range)) in sources.iter().enumerate() {
            let kind = if target.key.is_depth {
                kernels::SrcKind::Depth {
                    multisampled: target.samples > 1,
                }
            } else {
                kernels::SrcKind::Color {
                    multisampled: target.samples > 1,
                }
            };
            let kernel = self.cache.resolve_kernels.dump(self.gpu, kind, scale)?;
            let mut entries = vec![
                buffer(0, &params, 96),
                Entry {
                    binding: 1,
                    resource: Resource::Texture(&target.read),
                },
                buffer(3, &shadow, shadow_bytes),
            ];
            if let Some(stencil) = &target.stencil {
                entries.push(Entry {
                    binding: 2,
                    resource: Resource::Texture(stencil),
                });
            }
            let group = self.gpu.bind_group(&kernel.layout, &entries)?;
            let groups = ((range.end - range.start) * tile_words).div_ceil(256);
            dispatches.push(Dispatch {
                kernel,
                group,
                offset: i as u32 * stride,
                grid: [groups.min(65535), groups.div_ceil(65535), 1],
            });
        }
        let kernel = self.cache.resolve_kernels.resolve(self.gpu, scale)?;
        let group = self.gpu.bind_group(
            &kernel.layout,
            &[
                buffer(0, &params, 96),
                buffer(1, &shadow, shadow_bytes),
                buffer(2, &output, plan.bytes),
            ],
        )?;
        dispatches.push(Dispatch {
            kernel,
            group,
            offset: sources.len() as u32 * stride,
            grid: [info.rect.width.div_ceil(8), info.rect.height.div_ceil(8), 1],
        });
        // All resources, layouts and parameters are ready before the first
        // dispatch. Any remaining encoder error latches the entire frame failed.
        let encoder = self.encoder.as_mut().unwrap();
        let groups: Vec<_> = dispatches
            .iter()
            .map(|d| {
                [BoundGroup {
                    group: &d.group,
                    dynamic_offsets: std::slice::from_ref(&d.offset),
                }]
            })
            .collect();
        let batch: Vec<_> = dispatches
            .iter()
            .zip(&groups)
            .map(|(d, g)| ComputeDispatch {
                pipeline: &d.kernel.pipeline,
                groups: g,
                workgroups: d.grid,
            })
            .collect();
        let encoded = encoder.dispatch_shared_batch(&batch)?;
        if encoded.dispatches != batch.len() as u64 {
            return Err(Error::Rejected("native resolve dispatch coverage"));
        }
        self.resolve_bytes += plan.bytes;
        self.stats.resolves += 1;
        self.stats.dump_dispatches += sources.len() as u64;
        Ok(Resolved {
            output,
            info: plan.info,
            words: plan.words,
            bytes: plan.bytes,
        })
    }
}

struct Resolved {
    output: Buffer<Platform>,
    info: ResolveInfo,
    words: u32,
    bytes: u64,
}

/// Bound the actual tiled footprint before the shader's signed address math.
/// Reuse the same public tiling bounds as the register decoder. This rejects
/// truncation/physical wrap instead of relying on the store shader to clip it.
fn validate_store_footprint(
    info: &ResolveInfo,
    bpe: u32,
    packed: bool,
    range: &Range<u32>,
) -> Result<()> {
    use xenos_texture::address::*;
    let dest = info.copy_dest_info;
    let pitch = info.dest_pitch_aligned;
    let height = info.dest_height_aligned;
    let base = u64::from(info.copy_dest_base & 0x1fff_ffff);
    if pitch == 0
        || pitch % 32 != 0
        || pitch > 8192
        || height % 32 != 0
        || height > 8192
        || dest.copy_dest_array && (height == 0 || packed)
        || dest.copy_dest_slice > 7
        || dest.copy_dest_endian.raw() > 5
        || !matches!(bpe, 1 | 2 | 4 | 8 | 16)
        || base % u64::from(bpe) != 0
        || info.dest_offset_x_div_8 > 1023
        || info.dest_offset_y_div_8 > 1023
    {
        return Err(Error::Rejected("native guest store layout"));
    }
    let divisor = if packed { 4 } else { 1 };
    let left = (info.dest_offset_x_div_8 * 8) / divisor;
    let top = (info.dest_offset_y_div_8 * 8) / divisor;
    let right = left + info.rect.width / divisor;
    let bottom = top + info.rect.height / divisor;
    if right > pitch || bottom > 8192 || dest.copy_dest_array && bottom > height {
        return Err(Error::Rejected("native guest store coordinates"));
    }
    let log = bpe.ilog2();
    let (lo, hi) = if dest.copy_dest_array {
        (
            tiled_address_lower_bound_3d(left, top, dest.copy_dest_slice, pitch, height, log),
            tiled_address_upper_bound_3d(
                right,
                bottom,
                dest.copy_dest_slice + 1,
                pitch,
                height,
                log,
            ),
        )
    } else {
        (
            u64::from(tiled_address_lower_bound_2d(left, top, pitch, log)),
            u64::from(tiled_address_upper_bound_2d(right, bottom, pitch, log)),
        )
    };
    if base + lo < u64::from(range.start)
        || base + hi > u64::from(range.end)
        || base + hi > 512 << 20
    {
        return Err(Error::Rejected(
            "native guest store footprint exceeds extent",
        ));
    }
    Ok(())
}
