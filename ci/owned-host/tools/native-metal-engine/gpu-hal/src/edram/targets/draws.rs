//! Exclusive draw/constant/EDRAM owner. A constant rewind encodes every prior
//! consumer before reusing offsets, without submitting or ending texture/index
//! generations. No constant buffer, group or mutable attachment escapes here.
use super::guest_memory::{GuestMemory, GuestPage, MemoryFrame};
use super::*;
use crate::{
    UploadReservation,
    bindings::{Entry, Group, GroupLayout, Resource},
    constants::ConstantStream,
    render::{BoundGroup, Indices, PassRecorder, Pipeline, Viewport},
    sink::PassSink,
};
use xenos_gfx::constant_segments::{AdmissionError, PatchSegment, PushError, StageConstants};

#[derive(Clone, Copy, PartialEq, Eq)]
pub struct ConstantSlot {
    pub binding: u32,
    pub visibility: types::ShaderStages,
    pub storage: bool,
    pub bytes: u32,
}
pub struct DrawArena {
    owner: Rc<Owner<Platform>>,
    stream: ConstantStream<Platform>,
    segment: PatchSegment,
    layout: GroupLayout<Platform>,
    group: Group<Platform>,
    slots: [ConstantSlot; 5],
    dynamic_order: [usize; 5],
}
/// Actual selected attachment formats; native views remain inside the owner.
#[derive(Clone, Copy, Debug)]
pub struct TargetInfo {
    pub colors: [Option<F>; 4],
    pub depth: Option<F>,
    pub extent: [u32; 2],
    pub owned_extent: [u32; 2],
    pub samples: u32,
    pub sample_mask: u32,
}
pub struct ConstantDraw<'a> {
    pub pipeline: &'a Pipeline<Platform>,
    /// Raw draw: Groups1.. in pipeline order. Translated guest draw: Groups2..
    /// only; Group1 is bound internally from the current captured memory.
    /// Group0 always belongs exclusively to this arena.
    pub groups: &'a [BoundGroup<'a, Platform>],
    pub constants: [&'a [u8]; 5],
    pub vertices: Range<u32>,
    pub instances: Range<u32>,
    pub indices: Option<Indices<'a, Platform>>,
    pub viewport: Option<Viewport>,
    pub scissor: Option<[u32; 4]>,
    pub blend: types::Color,
    pub stencil_reference: u32,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DrawAdmission {
    Accepted,
    /// This draw changed no CPU snapshot/dirty state and was not recorded.
    /// Submit previous accepted work (including memory preparation), retire as
    /// needed, then retry the SAME captured input in a new frame. A request
    /// exceeding the absolute budget is a hard error, never this result.
    NeedsSubmit,
}
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct DrawStats {
    pub admitted: u64,
    pub passes: u64,
    pub constant_dispatches: u64,
    pub output_rewinds: u64,
    pub parameter_seals: u64,
    pub snapshot_bytes: u64,
    pub submission_requests: u64,
    pub guest_upload_bytes: u64,
    pub guest_changed_pages: u64,
    pub guest_evicted_blocks: u64,
}
pub struct ConstantFrame<'a, 'b> {
    frame: Frame<'a>,
    arena: &'b mut DrawArena,
    reservation: Option<UploadReservation<Platform>>,
    memory: Option<MemoryFrame<'b>>,
    pass: Option<PassRecorder<Platform>>,
    targets: Option<DrawTargets>,
    stats: DrawStats,
    #[cfg(feature = "fault-injection")]
    unwind_after_admission: bool,
}
// A caller catching an unexpected record-time panic cannot submit a partially
// recorded transaction, nor continue on an owner whose operation was unwound.
struct Unwind(Rc<Owner<Platform>>);
impl Drop for Unwind {
    fn drop(&mut self) {
        if std::thread::panicking() {
            let _ = self.0.fail_next("native draw transaction unwound".into());
        }
    }
}
impl Gpu<Platform> {
    pub fn draw_arena(&mut self, capacity: u64, slots: [ConstantSlot; 5]) -> Result<DrawArena> {
        if slots
            .iter()
            .any(|s| s.bytes == 0 || s.bytes > 4096 || s.bytes % 16 != 0)
        {
            return Err(Error::Rejected("constant slot size"));
        }
        let mut dynamic_order = [0, 1, 2, 3, 4];
        dynamic_order.sort_by_key(|&i| slots[i].binding);
        if dynamic_order
            .windows(2)
            .any(|w| slots[w[0]].binding == slots[w[1]].binding)
        {
            return Err(Error::Rejected("duplicate constant slot binding"));
        }
        let stream = self.constant_stream(capacity)?;
        let mut offset = 0u64;
        for slot in slots {
            offset = offset.next_multiple_of(stream.alignment()) + u64::from(slot.bytes);
        }
        if offset > capacity {
            return Err(Error::Rejected("one draw exceeds constant arena"));
        }
        let entries = slots.map(|s| types::BindGroupLayoutEntry {
            binding: s.binding,
            visibility: s.visibility,
            ty: types::BindingType::Buffer {
                ty: if s.storage {
                    types::BufferBindingType::Storage { read_only: true }
                } else {
                    types::BufferBindingType::Uniform
                },
                has_dynamic_offset: true,
                min_binding_size: NonZeroU64::new(u64::from(s.bytes)),
            },
            count: None,
        });
        let layout = self.group_layout(&entries)?;
        let entries = slots.map(|s| Entry {
            binding: s.binding,
            resource: Resource::Buffer {
                buffer: stream.output(),
                offset: 0,
                size: u64::from(s.bytes),
            },
        });
        let group = self.bind_group(&layout, &entries)?;
        let segment = PatchSegment::new(capacity, stream.alignment())
            .map_err(|_| Error::Rejected("constant arena configuration"))?;
        Ok(DrawArena {
            owner: self.owner.clone(),
            stream,
            segment,
            layout,
            group,
            slots,
            dynamic_order,
        })
    }
}
impl DrawArena {
    pub fn layout(&self) -> &GroupLayout<Platform> {
        &self.layout
    }
    pub(crate) fn is_xenos(&self) -> bool {
        self.slots == crate::guest_shader::xenos_slots()
    }
}
impl<'a> Frame<'a> {
    pub fn with_constants<'b>(self, arena: &'b mut DrawArena) -> Result<ConstantFrame<'a, 'b>> {
        self.healthy()?;
        if !Rc::ptr_eq(&self.gpu.owner, &arena.owner) {
            return Err(Error::Rejected("constant arena device"));
        }
        arena
            .segment
            .reset()
            .map_err(|_| Error::Rejected("constant arena identity exhaustion"))?;
        Ok(ConstantFrame {
            frame: self,
            arena,
            reservation: None,
            memory: None,
            pass: None,
            targets: None,
            stats: DrawStats::default(),
            #[cfg(feature = "fault-injection")]
            unwind_after_admission: false,
        })
    }
}
impl<'a, 'b> ConstantFrame<'a, 'b> {
    pub fn with_guest_memory(mut self, memory: &'b mut GuestMemory) -> Result<Self> {
        self.frame.healthy()?;
        if self.memory.is_some() || self.stats.admitted != 0 {
            return Err(Error::Rejected("guest mirror must attach before draws"));
        }
        self.memory = Some(memory.begin(&self.arena.owner)?);
        Ok(self)
    }
    /// Diagnostics expose identity only, never a reusable mirror descriptor.
    #[cfg(feature = "fault-injection")]
    pub fn test_guest_group_id(&self) -> Result<u64> {
        self.frame.healthy()?;
        Ok(self
            .memory
            .as_ref()
            .ok_or(Error::Rejected("guest mirror absent"))?
            .group()
            .id())
    }
    /// Capture bytes must remain the SAME on a NeedsSubmit retry. This never
    /// re-reads live game memory, never publishes speculative mapping changes,
    /// and never writes new bytes before pending draws have consumed old bytes.
    pub fn sync_guest_pages(&mut self, pages: &[GuestPage]) -> Result<DrawAdmission> {
        let _unwind = Unwind(self.arena.owner.clone());
        self.frame.healthy()?;
        let result = self.sync_guest_pages_inner(pages);
        self.latch(result)
    }
    fn sync_guest_pages_inner(&mut self, pages: &[GuestPage]) -> Result<DrawAdmission> {
        let memory = self
            .memory
            .as_ref()
            .ok_or(Error::Rejected("guest mirror absent"))?;
        let plan = memory.prepare(self.frame.gpu, pages)?;
        if plan.bytes != 0 {
            let reserved = self
                .frame
                .gpu
                .reserve_ordered_upload(self.frame.encoder.as_mut().unwrap(), plan.bytes);
            let mut token = match reserved {
                Ok(token) => token,
                Err(error) => {
                    if is_pressure(&error) && self.can_release_upload_pressure(plan.bytes) {
                        self.stats.submission_requests += 1;
                        return Ok(DrawAdmission::NeedsSubmit);
                    }
                    return Err(error);
                }
            };
            self.flush_pass(false)?;
            let encoder = self.frame.encoder.as_mut().unwrap();
            for (buffer, range) in &plan.clears {
                encoder.clear_buffer(buffer, range.clone())?;
            }
            let uploads: Vec<_> = plan
                .writes
                .iter()
                .map(|w| crate::OrderedUpload {
                    destination: &w.buffer,
                    offset: w.offset,
                    data: w.bytes.as_slice(),
                })
                .collect();
            self.frame
                .gpu
                .write_buffers_reserved(encoder, &mut token, &uploads)?;
        }
        self.stats.guest_upload_bytes += plan.bytes;
        self.stats.guest_changed_pages += plan.changed_pages;
        self.stats.guest_evicted_blocks += plan.evictions;
        self.memory.as_mut().unwrap().apply(plan);
        Ok(DrawAdmission::Accepted)
    }
    pub fn gpu(&self) -> &Gpu<Platform> {
        self.frame.gpu()
    }
    pub fn stats(&self) -> DrawStats {
        self.stats
    }
    /// Session-owned immutable index packet. Admission and the actual upload
    /// share this encoder's reservation ledger; no queue-prefix mutation.
    pub(crate) fn packet_indices(&mut self, bytes: &[u8]) -> Result<Option<Buffer<Platform>>> {
        let _unwind = Unwind(self.arena.owner.clone());
        self.frame.healthy()?;
        let result = (|| {
            if bytes.is_empty() || bytes.len() > 16 << 20 {
                return Err(Error::Rejected("native packet index size"));
            }
            let size = (bytes.len() as u64).next_multiple_of(4);
            let mut token = match self
                .frame
                .gpu
                .reserve_ordered_upload(self.frame.encoder.as_mut().unwrap(), size)
            {
                Ok(token) => token,
                Err(error) if is_pressure(&error) && self.can_release_upload_pressure(size) => {
                    return Ok(None);
                }
                Err(error) => return Err(error),
            };
            let buffer = self.frame.gpu.raw_buffer(
                "native immutable packet indices",
                size,
                U::INDEX | U::COPY_DST,
                false,
            )?;
            let mut padded = Vec::new();
            let data = if size == bytes.len() as u64 {
                bytes
            } else {
                padded.extend_from_slice(bytes);
                padded.resize(size as usize, 0);
                &padded
            };
            // This buffer is fresh, so no earlier draw can consume it. Its
            // copy may precede a pending pass without flushing that pass.
            self.frame.gpu.write_buffers_reserved(
                self.frame.encoder.as_mut().unwrap(),
                &mut token,
                &[crate::OrderedUpload {
                    destination: &buffer,
                    offset: 0,
                    data,
                }],
            )?;
            Ok(Some(buffer))
        })();
        self.latch(result)
    }
    #[cfg(feature = "fault-injection")]
    pub fn test_unwind_next_draw(&mut self) {
        self.unwind_after_admission = true;
    }
    fn latch<T>(&mut self, result: Result<T>) -> Result<T> {
        if result.is_err() {
            self.frame.failed = true;
        }
        result
    }
    pub fn targets(&mut self, setup: &RenderTargetSetup, min_height: u32) -> Result<TargetInfo> {
        let _unwind = Unwind(self.arena.owner.clone());
        self.frame.healthy()?;
        let result = self.targets_inner(setup, min_height);
        self.latch(result)
    }
    fn targets_inner(&mut self, setup: &RenderTargetSetup, min_height: u32) -> Result<TargetInfo> {
        self.flush_pass(false)?;
        let targets = self.frame.targets(setup, min_height)?;
        if targets.is_empty() {
            return Err(Error::Rejected("draw arena has no attachments"));
        }
        let mut info = TargetInfo {
            colors: [None; 4],
            depth: None,
            extent: [targets.width, targets.height],
            owned_extent: [targets.width, targets.owned_height],
            samples: targets.samples,
            sample_mask: targets.sample_mask,
        };
        for selected in &targets.selected {
            match selected.binding.slot {
                RenderTargetSlot::Color(n) => info.colors[n as usize] = Some(selected.format),
                RenderTargetSlot::Depth => info.depth = Some(selected.format),
            }
        }
        self.targets = Some(targets);
        self.reset_pass()?;
        Ok(info)
    }
    fn reset_pass(&mut self) -> Result<()> {
        let Some(targets) = &self.targets else {
            return Ok(());
        };
        let mut colors = [None, None, None, None];
        let mut last = 0;
        let mut depth = None;
        for selected in &targets.selected {
            match selected.binding.slot {
                RenderTargetSlot::Color(n) => {
                    colors[n as usize] = Some(ColorAttachment {
                        view: &selected.view,
                        clear: None,
                    });
                    last = last.max(n as usize + 1);
                }
                RenderTargetSlot::Depth => {
                    depth = Some(DepthAttachment {
                        view: &selected.view,
                        clear_depth: None,
                        clear_stencil: None,
                    })
                }
            }
        }
        if let Some(pass) = &mut self.pass {
            pass.reset(&colors[..last], depth, &[])?;
        } else {
            self.pass = Some(
                self.frame
                    .gpu
                    .render_recorder(&colors[..last], depth, &[])?,
            );
        }
        self.pass
            .as_mut()
            .unwrap()
            .set_group(0, &self.arena.group, &[0; 5])?;
        Ok(())
    }
    pub fn draw(&mut self, draw: &ConstantDraw<'_>) -> Result<DrawAdmission> {
        let _unwind = Unwind(self.arena.owner.clone());
        self.frame.healthy()?;
        let result = self.draw_inner(draw, None);
        self.latch(result)
    }
    pub fn draw_guest(
        &mut self,
        program: &crate::guest_shader::GuestPipeline,
        draw: &ConstantDraw<'_>,
    ) -> Result<DrawAdmission> {
        let _unwind = Unwind(self.arena.owner.clone());
        self.frame.healthy()?;
        if !self.arena.is_xenos()
            || program.pipeline().id() != draw.pipeline.id()
            || !program.textures.is_empty()
        {
            self.frame.failed = true;
            return Err(Error::Rejected("shader proof does not match draw pipeline"));
        }
        let result = self.draw_guest_inner(program, draw);
        self.latch(result)
    }
    /// Textured translated draws only accept immutable texture tickets produced
    /// by this owner. Raw groups cannot impersonate a captured guest texture.
    pub fn draw_guest_textured(
        &mut self,
        program: &crate::guest_shader::GuestPipeline,
        draw: &ConstantDraw<'_>,
        textures: &[GuestTextureBinding<'_>],
    ) -> Result<DrawAdmission> {
        let _unwind = Unwind(self.arena.owner.clone());
        self.frame.healthy()?;
        let result = (|| {
            if !self.arena.is_xenos()
                || program.pipeline().id() != draw.pipeline.id()
                || !draw.groups.is_empty()
            {
                return Err(Error::Rejected("textured guest draw pipeline/groups"));
            }
            let memory = self
                .memory
                .as_ref()
                .ok_or(Error::Rejected("guest mirror absent"))?;
            let group =
                self.frame
                    .guest_texture_group(program, memory, draw.constants[3], textures)?;
            let groups = [BoundGroup {
                group: &group,
                dynamic_offsets: &[],
            }];
            let bound = ConstantDraw {
                groups: &groups,
                pipeline: draw.pipeline,
                constants: draw.constants,
                vertices: draw.vertices.clone(),
                instances: draw.instances.clone(),
                indices: draw.indices.as_ref().map(|i| Indices {
                    buffer: i.buffer,
                    format: i.format,
                    offset: i.offset,
                    size: i.size,
                    base_vertex: i.base_vertex,
                }),
                viewport: draw.viewport,
                scissor: draw.scissor,
                blend: draw.blend,
                stencil_reference: draw.stencil_reference,
            };
            self.draw_guest_inner(program, &bound)
        })();
        self.latch(result)
    }
    fn draw_guest_inner(
        &mut self,
        program: &crate::guest_shader::GuestPipeline,
        draw: &ConstantDraw<'_>,
    ) -> Result<DrawAdmission> {
        let memory = self
            .memory
            .as_ref()
            .ok_or(Error::Rejected("guest draw requires owned memory"))?;
        if !memory.has_capture() {
            return Err(Error::Rejected("guest draw requires a fresh capture"));
        }
        program.validate_captured_reads(draw, |page| memory.captured(page))?;
        if draw.groups.len() > 7 {
            return Err(Error::Rejected("guest draw binding count"));
        }
        let shared = memory.group();
        let mut groups: [BoundGroup<'_, Platform>; 8] = std::array::from_fn(|_| BoundGroup {
            group: &shared,
            dynamic_offsets: &[],
        });
        for (i, group) in draw.groups.iter().enumerate() {
            groups[i + 1] = BoundGroup {
                group: group.group,
                dynamic_offsets: group.dynamic_offsets,
            };
        }
        let owned = ConstantDraw {
            pipeline: draw.pipeline,
            groups: &groups[..draw.groups.len() + 1],
            constants: draw.constants,
            vertices: draw.vertices.clone(),
            instances: draw.instances.clone(),
            indices: draw.indices.as_ref().map(|i| Indices {
                buffer: i.buffer,
                format: i.format,
                offset: i.offset,
                size: i.size,
                base_vertex: i.base_vertex,
            }),
            viewport: draw.viewport,
            scissor: draw.scissor,
            blend: draw.blend,
            stencil_reference: draw.stencil_reference,
        };
        let result = self.draw_inner(&owned, Some(program.constant_bytes()))?;
        if result == DrawAdmission::Accepted {
            self.memory.as_mut().unwrap().consume_capture();
        }
        Ok(result)
    }
    fn draw_inner(
        &mut self,
        draw: &ConstantDraw<'_>,
        required: Option<[usize; 5]>,
    ) -> Result<DrawAdmission> {
        let targets = self
            .targets
            .as_ref()
            .ok_or(Error::Rejected("draw before target selection"))?;
        let layout = draw.pipeline.layout();
        if layout
            .0
            .groups
            .first()
            .is_none_or(|g| !g.compatible(&self.arena.layout))
            || draw.groups.len() + 1 != layout.0.groups.len()
        {
            return Err(Error::Rejected("draw pipeline constant/group contract"));
        }
        // Arbitrary native pipelines require full binding spans. Only a
        // matching immutable translated GuestPipeline can authorize prefixes.
        let required = required.unwrap_or(self.arena.slots.map(|s| s.bytes as usize));
        if draw
            .constants
            .iter()
            .zip(required)
            .any(|(data, bytes)| data.len() != bytes)
        {
            return Err(Error::Rejected(
                "constant binding requires its complete proven span",
            ));
        }
        let extent = [targets.width, targets.owned_height];
        let scissor = draw.scissor.unwrap_or([0, 0, extent[0], extent[1]]);
        if scissor[0]
            .checked_add(scissor[2])
            .is_none_or(|x| x > extent[0])
            || scissor[1]
                .checked_add(scissor[3])
                .is_none_or(|y| y > extent[1])
        {
            return Err(Error::Rejected("draw exceeds owned target extent"));
        }
        self.prepare_draw(draw, extent, scissor)?;
        let stages = std::array::from_fn::<_, 5, _>(|stage| StageConstants {
            stage,
            data: draw.constants[stage],
            window: self.arena.slots[stage].bytes,
        });
        // A stage can report parameter pressure before a later stage reports
        // output pressure. Allow each remedy once, including sealing an old
        // reservation when acquiring its larger replacement hits the budget.
        // No remedy can repeat, even when one draw cannot fit an empty budget.
        let mut sealed = false;
        let mut rewound = false;
        let blocks = loop {
            let frame = &mut self.frame;
            let reservation = &mut self.reservation;
            let mut requested_bytes = 0;
            let admission = self
                .arena
                .segment
                .push_draw_with_admission(&stages, |demand| {
                    requested_bytes = demand.upload_bytes();
                    if demand.upload_bytes() == 0 {
                        return Ok(());
                    }
                    let encoder = frame.encoder.as_mut().unwrap();
                    if let Some(token) = reservation.as_mut() {
                        frame
                            .gpu
                            .grow_ordered_upload(encoder, token, demand.upload_bytes())
                    } else {
                        *reservation = Some(
                            frame
                                .gpu
                                .reserve_ordered_upload(encoder, demand.upload_bytes())?,
                        );
                        Ok(())
                    }
                });
            match admission {
                Ok(blocks) => break blocks,
                Err(AdmissionError::Draw(PushError::Full(why))) => {
                    if why.rewind_required() && !rewound {
                        self.flush_pass(true)?;
                        rewound = true;
                    } else if !why.rewind_required() && !sealed {
                        self.seal_parameters()?;
                        sealed = true;
                    } else {
                        return Err(Error::Rejected("draw constant capacity/shape"));
                    }
                    // Output rewind resets recorder state. Parameter sealing
                    // preserves it; the same setters simply deduplicate there.
                    self.prepare_draw(draw, extent, scissor)?;
                }
                Err(AdmissionError::Reservation(error)) => {
                    // Only a budget preflight is retryable. Backend failures or
                    // owner failure are not translated into capacity pressure.
                    let budget = is_pressure(&error);
                    if budget && !sealed && self.arena.segment.record_count() != 0 {
                        self.seal_parameters()?;
                        sealed = true;
                        continue;
                    }
                    if budget && self.can_release_upload_pressure(requested_bytes) {
                        self.stats.submission_requests += 1;
                        return Ok(DrawAdmission::NeedsSubmit);
                    }
                    return Err(error);
                }
                Err(_) => return Err(Error::Rejected("draw constant capacity/shape")),
            }
        };
        #[cfg(feature = "fault-injection")]
        if std::mem::take(&mut self.unwind_after_admission) {
            panic!("injected draw admission unwind");
        }
        let offsets = self
            .arena
            .dynamic_order
            .map(|stage| blocks[stage].unwrap().offset());
        let pass = self.pass.as_mut().unwrap();
        pass.set_group(0, &self.arena.group, &offsets)?;
        if let Some(index) = &draw.indices {
            pass.draw_indexed(
                draw.vertices.clone(),
                index.base_vertex,
                draw.instances.clone(),
            )?;
        } else {
            pass.draw(draw.vertices.clone(), draw.instances.clone())?;
        }
        self.stats.admitted += 1;
        Ok(DrawAdmission::Accepted)
    }
    fn prepare_draw(
        &mut self,
        draw: &ConstantDraw<'_>,
        extent: [u32; 2],
        scissor: [u32; 4],
    ) -> Result<()> {
        let pass = self.pass.as_mut().unwrap();
        pass.set_pipeline(draw.pipeline)?;
        for (n, group) in draw.groups.iter().enumerate() {
            pass.set_group(n as u32 + 1, group.group, group.dynamic_offsets)?;
        }
        let v = draw.viewport.unwrap_or(Viewport {
            x: 0.,
            y: 0.,
            width: extent[0] as f32,
            height: extent[1] as f32,
            min_depth: 0.,
            max_depth: 1.,
        });
        pass.set_viewport([v.x, v.y, v.width, v.height, v.min_depth, v.max_depth])?;
        pass.set_scissor(scissor)?;
        pass.set_blend([draw.blend.r, draw.blend.g, draw.blend.b, draw.blend.a])?;
        pass.set_stencil(draw.stencil_reference)?;
        if let Some(index) = &draw.indices {
            pass.set_index(index.buffer, index.format, index.offset, Some(index.size))?;
        }
        pass.preflight_draw(
            &draw.vertices,
            &draw.instances,
            draw.indices.as_ref().map(|i| i.base_vertex),
            5,
        )
    }
    fn encode_constants(&mut self) -> Result<()> {
        let sealed = self
            .arena
            .segment
            .seal()
            .map_err(|_| Error::Rejected("invalid sealed constants"))?;
        let bytes = if sealed.records()[0] == 0 {
            0
        } else {
            (sealed.records().len() * 4 + sealed.values().len()) as u64
        };
        self.arena.stream.encode_reserved(
            self.frame.gpu,
            self.frame.encoder.as_mut().unwrap(),
            &sealed,
            &mut self.reservation,
        )?;
        if bytes != 0 {
            self.stats.constant_dispatches += 1;
        }
        self.stats.snapshot_bytes += bytes;
        Ok(())
    }
    fn seal_parameters(&mut self) -> Result<()> {
        self.encode_constants()?;
        self.arena
            .segment
            .continue_after_seal()
            .map_err(|_| Error::Rejected("constant dispatch exhausted"))?;
        self.stats.parameter_seals += 1;
        Ok(())
    }
    fn flush_pass(&mut self, rewind: bool) -> Result<()> {
        let pending = self
            .pass
            .as_ref()
            .is_some_and(|p| p.stats().draws + p.stats().skipped_empty > 0);
        let records = self.arena.segment.record_count() != 0;
        if !pending && !records && !rewind {
            return Ok(());
        }
        if pending || records {
            self.encode_constants()?;
        }
        if pending {
            self.frame.render_recorded(self.pass.as_ref().unwrap())?;
            self.stats.passes += 1;
        }
        if rewind {
            self.arena
                .segment
                .reset()
                .map_err(|_| Error::Rejected("constant identity exhausted"))?;
            self.stats.output_rewinds += 1;
        } else {
            self.arena
                .segment
                .continue_after_seal()
                .map_err(|_| Error::Rejected("constant dispatch exhausted"))?;
        }
        self.reset_pass()
    }
    fn can_release_upload_pressure(&self, bytes: u64) -> bool {
        let gpu = &self.frame.gpu;
        let config = &gpu.owner.config;
        if bytes == 0
            || bytes > config.max_pending_upload_bytes
            || bytes > config.max_staging_buffer_bytes
            || bytes > gpu.owner.limits.max_buffer_size
        {
            return false;
        }
        // No draw is required: earlier page preparation can consume all of an
        // encoder's staging budget. A submit also retires queue-prefix uploads.
        // Work on unrelated, unsubmitted encoders is deliberately not counted.
        self.stats.admitted != 0
            || self
                .frame
                .encoder
                .as_ref()
                .is_some_and(|e| e.ordered_staging_bytes != 0)
            || !gpu.pending_staging.is_empty()
            || gpu.flights.iter().any(|f| !f.staging.is_empty())
    }
    pub fn resolve_pixels(
        &mut self,
        info: &xenos_state::resolve::ResolveInfo,
    ) -> Result<Option<ResolvePixels>> {
        let _unwind = Unwind(self.arena.owner.clone());
        self.frame.healthy()?;
        let result = self
            .flush_pass(false)
            .and_then(|_| self.frame.resolve_pixels(info));
        if info.is_clearing_color() || info.is_clearing_depth() {
            self.targets = None;
        }
        self.latch(result)
    }
    /// Encode a resolve directly into the owned GPU mirror. The caller must
    /// first capture every destination page with CPU write generations. Returns
    /// the conservative written extent for texture invalidation. No CPU wait or
    /// readback occurs; the next guest draw must arm its own fresh capture.
    pub fn resolve_guest(
        &mut self,
        info: &xenos_state::resolve::ResolveInfo,
    ) -> Result<Option<Range<u32>>> {
        let _unwind = Unwind(self.arena.owner.clone());
        self.frame.healthy()?;
        let result = (|| {
            self.flush_pass(false)?;
            let memory = self
                .memory
                .as_mut()
                .ok_or(Error::Rejected("guest mirror absent"))?;
            self.frame
                .resolve_guest_inner(info, memory, false)
                .map(|r| r.0)
        })();
        if info.is_clearing_color() || info.is_clearing_depth() {
            self.targets = None;
        }
        self.latch(result)
    }
    /// Exact immutable written-byte runs for the upcoming host visibility
    /// adapter. Normal GPU-to-GPU resolves use resolve_guest without this
    /// snapshot cost. The same strict destination capture contract still applies.
    pub fn resolve_guest_journal(
        &mut self,
        info: &xenos_state::resolve::ResolveInfo,
    ) -> Result<Option<GuestWriteJournal>> {
        let _unwind = Unwind(self.arena.owner.clone());
        self.frame.healthy()?;
        let result = (|| {
            self.flush_pass(false)?;
            let memory = self
                .memory
                .as_mut()
                .ok_or(Error::Rejected("guest mirror absent"))?;
            self.frame
                .resolve_guest_inner(info, memory, true)
                .map(|r| r.1)
        })();
        if info.is_clearing_color() || info.is_clearing_depth() {
            self.targets = None;
        }
        self.latch(result)
    }
    pub fn submit(mut self) -> Result<(Serial, FrameStats, DrawStats)> {
        let _unwind = Unwind(self.arena.owner.clone());
        self.frame.healthy()?;
        let result = self.flush_pass(false);
        self.latch(result)?;
        let (serial, frame) = self.frame.submit()?;
        if let Some(memory) = self.memory {
            memory.commit(serial);
        }
        Ok((serial, frame, self.stats))
    }

    pub fn load_guest_texture(
        &mut self,
        fetch: &xenos_texture::TextureFetchConstant,
        signed: bool,
    ) -> Result<TextureAdmission> {
        let _unwind = Unwind(self.arena.owner.clone());
        self.frame.healthy()?;
        let result: std::result::Result<GuestTexture, super::guest_texture::LoadError> = (|| {
            // End old consumers before any source conversion/cache generation.
            // This records commands only; it does not submit or wait on the CPU.
            self.flush_pass(false)?;
            let memory = self
                .memory
                .as_ref()
                .ok_or(Error::Rejected("guest mirror absent"))?;
            self.frame.load_guest_texture_inner(memory, fetch, signed)
        })(
        );
        match result {
            Ok(texture) => Ok(TextureAdmission::Loaded(texture)),
            Err(error)
                if is_pressure(&error.error)
                    && error
                        .retry_bytes
                        .is_some_and(|bytes| self.can_release_upload_pressure(bytes)) =>
            {
                self.stats.submission_requests += 1;
                Ok(TextureAdmission::NeedsSubmit)
            }
            Err(error) => self.latch(Err(error.error)),
        }
    }
}

fn is_pressure(error: &Error) -> bool {
    matches!(
        error,
        Error::Rejected(
            "ordered upload encoder byte budget" | "staging buffer budget" | "buffer object budget"
        )
    )
}
