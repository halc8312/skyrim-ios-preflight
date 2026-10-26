use crate::*;
use native::{render as r, sink::PassSink};
use resources::Dependency;
pub struct CommandEncoder {
    pub(crate) owner: device::Owner,
    native: Option<native::Encoder<Platform>>,
    error: Option<Error>,
    dependencies: std::collections::HashMap<usize, Dependency>,
}
pub struct CommandBuffer {
    pub(crate) owner: device::Owner,
    pub(crate) native: Result<native::Commands<Platform>>,
    pub(crate) dependencies: std::collections::HashMap<usize, Dependency>,
}

pub(crate) fn submission_commands(
    owner: &device::Owner,
    buffers: Vec<CommandBuffer>,
) -> Result<Vec<native::Commands<Platform>>> {
    owner.borrow().check()?;
    for b in &buffers {
        if !Rc::ptr_eq(owner, &b.owner) {
            return Err(Error::Invalid("cross-device command submission".into()));
        }
        if let Err(e) = &b.native {
            return Err(e.clone());
        }
        for d in b.dependencies.values() {
            d.valid()?;
        }
    }
    buffers.into_iter().map(|b| b.native).collect()
}
impl std::fmt::Debug for CommandEncoder {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("NativeEncoder")
            .field("error", &self.error)
            .finish()
    }
}
impl std::fmt::Debug for CommandBuffer {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("NativeCommands")
            .field("valid", &self.native.is_ok())
            .finish()
    }
}
impl Device {
    pub fn create_command_encoder(&self, _label: &str) -> CommandEncoder {
        let result = self.resource(|g| g.encoder().map_err(convert::err));
        let (native, error) = match result {
            Ok(e) => (Some(e), None),
            Err(e) => (None, Some(e)),
        };
        CommandEncoder {
            owner: self.owner.clone(),
            native,
            error,
            dependencies: std::collections::HashMap::new(),
        }
    }
}
impl CommandEncoder {
    fn dependency(&mut self, d: Dependency) {
        self.dependencies.entry(d.identity()).or_insert(d);
    }
    fn group(&mut self, group: &BindGroup) {
        self.dependencies
            .entry(group.identity())
            .or_insert_with(|| Dependency::Group(group.clone()));
    }
    fn failed(&mut self, e: Error) {
        if self.error.is_none() {
            self.owner.borrow_mut().report(e.clone(), false);
            self.error = Some(e);
        }
    }
    fn encode(&mut self, f: impl FnOnce(&mut native::Encoder<Platform>) -> Result<()>) {
        if self.error.is_some() {
            return;
        }
        let result = self
            .native
            .as_mut()
            .ok_or_else(|| Error::Invalid("encoder absent".into()))
            .and_then(f);
        if let Err(e) = result {
            self.failed(e);
        }
    }
    fn buffer(&mut self, b: &Buffer) {
        self.dependency(Dependency::Buffer(b.clone()));
    }
    fn texture(&mut self, t: &Texture) {
        self.dependency(Dependency::WholeTexture(t.clone()));
    }
    pub fn copy_buffer_to_buffer(
        &mut self,
        src: &Buffer,
        src_offset: u64,
        dst: &Buffer,
        dst_offset: u64,
        size: u64,
    ) {
        self.buffer(src);
        self.buffer(dst);
        self.encode(|e| {
            e.copy_buffer(src.native()?, src_offset, dst.native()?, dst_offset, size)
                .map_err(convert::err)
        });
        counters::add(&counters::count::copies, 1);
    }
    pub fn clear_buffer(&mut self, b: &Buffer, offset: u64, size: Option<u64>) {
        self.buffer(b);
        self.encode(|e| {
            let end = match size {
                Some(n) => offset
                    .checked_add(n)
                    .ok_or_else(|| Error::Invalid("clear range overflow".into()))?,
                None => b.size(),
            };
            e.clear_buffer(b.native()?, offset..end)
                .map_err(convert::err)
        });
    }
    pub fn copy_buffer_to_texture(
        &mut self,
        src: &TexelCopyBufferInfo<'_>,
        dst: &TexelCopyTextureInfo<'_>,
        size: Extent3d,
    ) {
        self.buffer(src.buffer);
        self.texture(dst.texture);
        self.encode(|e| {
            color_aspect(dst.aspect)?;
            e.copy_buffer_to_texture(
                src.buffer.native()?,
                layout(src, dst.texture, size)?,
                dst.texture.native()?,
                dst.mip_level,
                convert::origin(dst.origin),
                convert::size(size),
            )
            .map_err(convert::err)
        });
        counters::add(&counters::count::copies, 1);
    }
    pub fn copy_texture_to_buffer(
        &mut self,
        src: &TexelCopyTextureInfo<'_>,
        dst: &TexelCopyBufferInfo<'_>,
        size: Extent3d,
    ) {
        self.texture(src.texture);
        self.buffer(dst.buffer);
        self.encode(|e| {
            color_aspect(src.aspect)?;
            e.copy_texture_to_buffer(
                src.texture.native()?,
                src.mip_level,
                convert::origin(src.origin),
                convert::size(size),
                dst.buffer.native()?,
                layout(dst, src.texture, size)?,
            )
            .map_err(convert::err)
        });
        counters::add(&counters::count::copies, 1);
    }
    pub fn copy_texture_to_texture(
        &mut self,
        src: &TexelCopyTextureInfo<'_>,
        dst: &TexelCopyTextureInfo<'_>,
        size: Extent3d,
    ) {
        self.texture(src.texture);
        self.texture(dst.texture);
        self.encode(|e| {
            color_aspect(src.aspect)?;
            color_aspect(dst.aspect)?;
            e.copy_texture(
                src.texture.native()?,
                src.mip_level,
                convert::origin(src.origin),
                dst.texture.native()?,
                dst.mip_level,
                convert::origin(dst.origin),
                convert::size(size),
            )
            .map_err(convert::err)
        });
        counters::add(&counters::count::copies, 1);
    }
    pub fn begin_render_pass<'e>(
        &'e mut self,
        _label: &str,
        colors: &[Option<RenderPassColorAttachment<'_>>],
        depth: Option<&RenderPassDepthStencilAttachment<'_>>,
    ) -> RenderPass<'e> {
        let result = (|| {
            if let Some(e) = &self.error {
                return Err(e.clone());
            }
            let resolves = colors
                .iter()
                .map(|c| {
                    c.as_ref()
                        .and_then(|c| c.resolve_target)
                        .map(|v| v.native())
                        .transpose()
                })
                .collect::<Result<Vec<_>>>()?;
            let colors = colors
                .iter()
                .map(|c| {
                    c.as_ref()
                        .map(|c| {
                            if c.depth_slice.is_some() || c.store_op != StoreOp::Store {
                                return Err(Error::Unsupported(
                                    "3D color attachment/discard store",
                                ));
                            }
                            Ok(r::ColorAttachment {
                                view: c.view.native()?,
                                clear: load(c.load_op)?.then(|| convert::color(c.clear_value)),
                            })
                        })
                        .transpose()
                })
                .collect::<Result<Vec<_>>>()?;
            let depth = depth
                .as_ref()
                .map(|d| {
                    let format = d.view.texture.native()?.descriptor().format;
                    if (format.has_depth_aspect() && d.depth_read_only)
                        || (format.has_stencil_aspect() && d.stencil_read_only)
                    {
                        return Err(Error::Unsupported("read-only depth/stencil pass"));
                    }
                    Ok(r::DepthAttachment {
                        view: d.view.native()?,
                        clear_depth: if format.has_depth_aspect() {
                            load(d.depth_load_op)?.then_some(d.depth_clear_value)
                        } else {
                            None
                        },
                        clear_stencil: if format.has_stencil_aspect() {
                            load(d.stencil_load_op)?.then_some(d.stencil_clear_value)
                        } else {
                            None
                        },
                    })
                })
                .transpose()?;
            self.owner
                .borrow()
                .gpu
                .render_recorder(&colors, depth, &resolves)
                .map_err(convert::err)
        })();
        for c in colors.iter().flatten() {
            self.dependency(Dependency::Texture(c.view.clone()));
            if let Some(v) = c.resolve_target {
                self.dependency(Dependency::Texture(v.clone()));
            }
        }
        if let Some(d) = depth {
            self.dependency(Dependency::Texture(d.view.clone()));
        }
        let recorder = match result {
            Ok(r) => Some(r),
            Err(e) => {
                self.failed(e);
                None
            }
        };
        counters::add(&counters::count::render_passes, 1);
        RenderPass {
            encoder: self,
            recorder,
        }
    }
    pub fn begin_compute_pass<'e>(&'e mut self, _label: &str) -> ComputePass<'e> {
        counters::add(&counters::count::compute_passes, 1);
        ComputePass {
            encoder: self,
            pipeline: None,
            groups: Vec::new(),
            dispatches: Vec::new(),
        }
    }
    pub fn finish(mut self, _label: &str) -> CommandBuffer {
        let result = match self.error.take() {
            Some(e) => Err(e),
            None => self
                .native
                .take()
                .ok_or_else(|| Error::Invalid("encoder absent".into()))
                .and_then(|e| e.finish().map_err(convert::err)),
        };
        CommandBuffer {
            owner: self.owner.clone(),
            native: result,
            dependencies: std::mem::take(&mut self.dependencies),
        }
    }
}
fn load(op: LoadOp) -> Result<bool> {
    match op {
        LoadOp::Load => Ok(false),
        LoadOp::Clear => Ok(true),
        _ => Err(Error::Invalid("attachment load operation".into())),
    }
}
fn color_aspect(a: TextureAspect) -> Result<()> {
    if a == TextureAspect::All {
        Ok(())
    } else {
        Err(Error::Unsupported("non-color texture copy"))
    }
}
fn layout(
    b: &TexelCopyBufferInfo<'_>,
    texture: &Texture,
    size: Extent3d,
) -> Result<types::TexelCopyBufferLayout> {
    // WebGPU permits omitted rows_per_image for a single image. The direct
    // backend requires the already-normalized block-row count explicitly.
    let rows = match b.rows_per_image {
        Some(rows) => rows,
        None if size.depthOrArrayLayers == 1 => {
            let (_, height) = texture.native()?.descriptor().format.block_dimensions();
            size.height.div_ceil(height)
        }
        None => {
            return Err(Error::Invalid(
                "array texture copy requires rows_per_image".into(),
            ));
        }
    };
    Ok(types::TexelCopyBufferLayout {
        offset: b.offset,
        bytes_per_row: Some(b.bytes_per_row),
        rows_per_image: Some(rows),
    })
}

pub struct RenderPass<'e> {
    encoder: &'e mut CommandEncoder,
    recorder: Option<r::PassRecorder<Platform>>,
}
impl RenderPass<'_> {
    fn record(&mut self, f: impl FnOnce(&mut r::PassRecorder<Platform>) -> Result<()>) {
        if self.encoder.error.is_some() {
            return;
        }
        let result = self
            .recorder
            .as_mut()
            .ok_or_else(|| Error::Invalid("render recorder absent".into()))
            .and_then(f);
        if let Err(e) = result {
            self.encoder.failed(e);
        }
    }
    pub fn set_pipeline(&mut self, p: &RenderPipeline) {
        self.record(|r| r.set_pipeline(p.native()?).map_err(convert::err));
        counters::add(&counters::count::set_pipelines, 1);
    }
    pub fn set_bind_group(&mut self, slot: u32, g: &BindGroup, offsets: &[u32]) {
        self.encoder.group(g);
        self.record(|r| {
            r.set_group(slot, g.native()?, offsets)
                .map_err(convert::err)
        });
        counters::add(&counters::count::set_bind_groups, 1);
    }
    pub fn set_index_buffer(&mut self, b: &Buffer, f: IndexFormat, offset: u64, size: Option<u64>) {
        self.encoder.buffer(b);
        self.record(|r| {
            r.set_index(b.native()?, convert::index(f)?, offset, size)
                .map_err(convert::err)
        });
    }
    pub fn set_vertex_buffer(&mut self, _slot: u32, _b: &Buffer, _offset: u64, _size: Option<u64>) {
        self.encoder
            .failed(Error::Unsupported("vertex buffer input"));
    }
    pub fn set_viewport(&mut self, x: f32, y: f32, w: f32, h: f32, z0: f32, z1: f32) {
        self.record(|r| r.set_viewport([x, y, w, h, z0, z1]).map_err(convert::err));
    }
    pub fn set_scissor_rect(&mut self, x: u32, y: u32, w: u32, h: u32) {
        self.record(|r| r.set_scissor([x, y, w, h]).map_err(convert::err));
    }
    pub fn set_blend_constant(&mut self, c: Color) {
        self.record(|r| r.set_blend([c.r, c.g, c.b, c.a]).map_err(convert::err));
    }
    pub fn set_stencil_reference(&mut self, s: u32) {
        self.record(|r| r.set_stencil(s).map_err(convert::err));
    }
    pub fn draw(&mut self, vertices: u32, instances: u32, first: u32, first_instance: u32) {
        self.record(|r| {
            r.draw(range(first, vertices)?, range(first_instance, instances)?)
                .map_err(convert::err)
        });
        counters::add(&counters::count::draws, 1);
    }
    pub fn draw_indexed(
        &mut self,
        indices: u32,
        instances: u32,
        first: u32,
        base: i32,
        first_instance: u32,
    ) {
        self.record(|r| {
            r.draw_indexed(
                range(first, indices)?,
                base,
                range(first_instance, instances)?,
            )
            .map_err(convert::err)
        });
        counters::add(&counters::count::draws, 1);
    }
    pub fn end(self) {}
}
impl Drop for RenderPass<'_> {
    fn drop(&mut self) {
        if std::thread::panicking() {
            self.encoder
                .failed(Error::Invalid("render recording unwound".into()));
            return;
        }
        if let Some(r) = self.recorder.take() {
            self.encoder
                .encode(|e| e.render_recorded(&r).map(|_| ()).map_err(convert::err));
        }
    }
}
fn range(start: u32, count: u32) -> Result<std::ops::Range<u32>> {
    Ok(start
        ..start
            .checked_add(count)
            .ok_or_else(|| Error::Invalid("draw range overflow".into()))?)
}

struct Dispatch {
    pipeline: ComputePipeline,
    groups: Vec<(BindGroup, Vec<u32>)>,
    workgroups: [u32; 3],
}
pub struct ComputePass<'e> {
    encoder: &'e mut CommandEncoder,
    pipeline: Option<ComputePipeline>,
    groups: Vec<Option<(BindGroup, Vec<u32>)>>,
    dispatches: Vec<Dispatch>,
}
impl ComputePass<'_> {
    pub fn set_pipeline(&mut self, p: &ComputePipeline) {
        if let Err(e) = p.native() {
            self.encoder.failed(e);
            return;
        }
        self.pipeline = Some(p.clone());
        counters::add(&counters::count::set_pipelines, 1);
    }
    pub fn set_bind_group(&mut self, slot: u32, g: &BindGroup, offsets: &[u32]) {
        if slot >= 32 {
            self.encoder
                .failed(Error::Invalid("compute group slot".into()));
            return;
        }
        if let Err(e) = g.native() {
            self.encoder.failed(e);
            return;
        }
        if self.groups.len() <= slot as usize {
            self.groups.resize(slot as usize + 1, None);
        }
        self.groups[slot as usize] = Some((g.clone(), offsets.to_vec()));
        self.encoder.group(g);
        counters::add(&counters::count::set_bind_groups, 1);
    }
    pub fn dispatch_workgroups(&mut self, x: u32, y: u32, z: u32) {
        if self.encoder.error.is_some() {
            return;
        }
        let result = (|| {
            let p = self
                .pipeline
                .as_ref()
                .ok_or_else(|| Error::Invalid("dispatch without pipeline".into()))?;
            let n = p
                .layout
                .as_ref()
                .ok_or_else(|| Error::Invalid("compute layout absent".into()))?
                .groups
                .len();
            if self.dispatches.len() >= 8192 {
                return Err(Error::Invalid("compute pass capacity".into()));
            }
            let groups = (0..n)
                .map(|i| {
                    self.groups
                        .get(i)
                        .cloned()
                        .flatten()
                        .ok_or_else(|| Error::Invalid("missing compute group".into()))
                })
                .collect::<Result<Vec<_>>>()?;
            Ok(Dispatch {
                pipeline: p.clone(),
                groups,
                workgroups: [x, y, z],
            })
        })();
        match result {
            Ok(d) => self.dispatches.push(d),
            Err(e) => self.encoder.failed(e),
        };
        counters::add(&counters::count::dispatches, 1);
    }
    pub fn end(self) {}
}
impl Drop for ComputePass<'_> {
    fn drop(&mut self) {
        if self.dispatches.is_empty() {
            return;
        }
        if std::thread::panicking() {
            self.encoder
                .failed(Error::Invalid("compute recording unwound".into()));
            return;
        }
        self.encoder.encode(|e| {
            let groups = self
                .dispatches
                .iter()
                .map(|d| {
                    d.groups
                        .iter()
                        .map(|(g, o)| {
                            Ok(r::BoundGroup {
                                group: g.native()?,
                                dynamic_offsets: o,
                            })
                        })
                        .collect::<Result<Vec<_>>>()
                })
                .collect::<Result<Vec<_>>>()?;
            let dispatches = self
                .dispatches
                .iter()
                .zip(&groups)
                .map(|(d, g)| {
                    Ok(native::compute::ComputeDispatch {
                        pipeline: d.pipeline.native()?,
                        groups: g,
                        workgroups: d.workgroups,
                    })
                })
                .collect::<Result<Vec<_>>>()?;
            e.dispatch_shared_batch(&dispatches)
                .map(|_| ())
                .map_err(convert::err)
        });
    }
}

impl Queue {
    pub fn submit(&self, buffers: impl IntoIterator<Item = CommandBuffer>) {
        let copies = staging::take_copies();
        self.submit_raw(copies.into_iter().chain(buffers));
        staging::submitted();
    }
    pub(crate) fn submit_raw(&self, buffers: impl IntoIterator<Item = CommandBuffer>) {
        let prepared = submission_commands(&self.owner, buffers.into_iter().collect());
        let mut c = self.owner.borrow_mut();
        let result = prepared.and_then(|commands| c.submit(commands));
        if let Err(e) = result {
            c.report(e, true);
        }
        counters::add(&counters::count::submits, 1);
    }
    pub fn write_buffer(&self, b: &Buffer, offset: u64, data: &[u8]) {
        if let Err(e) = b.gpu_ready() {
            self.owner.borrow_mut().report(e, true);
            return;
        }
        if staging::stage(self, b, offset, data) {
            return;
        }
        let mut c = self.owner.borrow_mut();
        let result = c.check().and_then(|_| {
            if !b.usage().contains(BufferUsage::CopyDst) {
                return Err(Error::Invalid("queue write without COPY_DST".into()));
            }
            c.write(b.native()?, offset, data)
        });
        if let Err(e) = result {
            c.report(e, true);
        }
        counters::add(&counters::count::write_buffers, 1);
        counters::add(&counters::count::write_buffer_bytes, data.len() as u64);
        counters::written(b.label(), data.len() as u64);
    }
    pub fn write_texture(
        &self,
        dst: &TexelCopyTextureInfo<'_>,
        data: &[u8],
        bytes_per_row: u32,
        rows_per_image: Option<u32>,
        size: Extent3d,
    ) {
        let mut c = self.owner.borrow_mut();
        let result = (|| {
            c.check()?;
            color_aspect(dst.aspect)?;
            if !dst.texture.usage().contains(TextureUsage::CopyDst) {
                return Err(Error::Invalid("texture upload without COPY_DST".into()));
            }
            let t = dst.texture.native()?;
            let (block_bytes, block) = format_block_info(dst.texture.format())
                .ok_or(Error::Unsupported("upload texture format"))?;
            let rows = size.height.div_ceil(block);
            let row_bytes = size
                .width
                .div_ceil(block)
                .checked_mul(block_bytes)
                .ok_or_else(|| Error::Invalid("texture row overflow".into()))?;
            let source_rows = rows_per_image.unwrap_or(rows);
            let stride = bytes_per_row;
            if row_bytes == 0
                || rows == 0
                || size.depthOrArrayLayers == 0
                || stride < row_bytes
                || stride % block_bytes != 0
                || source_rows < rows
            {
                return Err(Error::Invalid("texture data layout".into()));
            }
            let needed = (size.depthOrArrayLayers as u64 - 1)
                .checked_mul(source_rows as u64)
                .and_then(|n| n.checked_add(rows as u64 - 1))
                .and_then(|n| n.checked_mul(stride as u64))
                .and_then(|n| n.checked_add(row_bytes as u64))
                .ok_or_else(|| Error::Invalid("texture upload source range overflow".into()))?;
            if needed > data.len() as u64 {
                return Err(Error::Invalid("texture upload source bounds".into()));
            }
            let padded = row_bytes
                .div_ceil(256)
                .checked_mul(256)
                .ok_or_else(|| Error::Invalid("texture pitch overflow".into()))?;
            let total = (padded as u64)
                .checked_mul(rows as u64)
                .and_then(|n| n.checked_mul(size.depthOrArrayLayers as u64))
                .ok_or_else(|| Error::Invalid("texture extent overflow".into()))?;
            if total > 256 << 20 {
                return Err(Error::Invalid("texture staging budget".into()));
            }
            let mut bytes = vec![0; total as usize];
            for layer in 0..size.depthOrArrayLayers as usize {
                for row in 0..rows as usize {
                    let src = (layer * source_rows as usize + row) * stride as usize;
                    let dest = (layer * rows as usize + row) * padded as usize;
                    bytes[dest..dest + row_bytes as usize]
                        .copy_from_slice(&data[src..src + row_bytes as usize]);
                }
            }
            if c.prefix_bytes != 0 && c.prefix_bytes + total > 32 << 20 {
                c.submit(Vec::new())?;
            }
            let staging = c
                .gpu
                .create_buffer(
                    "native texture queue upload",
                    total,
                    types::BufferUses::COPY_SRC | types::BufferUses::COPY_DST,
                )
                .map_err(convert::err)?;
            c.write(&staging, 0, &bytes)?;
            let mut e = match c.texture_uploads.take() {
                Some(e) => e,
                None => c.gpu.encoder().map_err(convert::err)?,
            };
            e.copy_buffer_to_texture(
                &staging,
                types::TexelCopyBufferLayout {
                    offset: 0,
                    bytes_per_row: Some(padded),
                    rows_per_image: Some(rows),
                },
                t,
                dst.mip_level,
                convert::origin(dst.origin),
                convert::size(size),
            )
            .map_err(convert::err)?;
            c.texture_uploads = Some(e);
            c.prefix_bytes += total;
            Ok(())
        })();
        if let Err(e) = result {
            c.report(e, true);
        }
        counters::add(&counters::count::write_textures, 1);
        counters::add(&counters::count::write_texture_bytes, data.len() as u64);
    }
    pub fn on_submitted_work_done(&self) -> Pending<Result<()>> {
        let owner = self.owner.clone();
        let target = owner.borrow().serial;
        let deadline = Instant::now() + Duration::from_secs(15);
        Pending::polling(move || match owner.borrow_mut().poll() {
            Ok(s) if s >= target => Some(Ok(())),
            Ok(_) => {
                if Instant::now() >= deadline {
                    Some(Err(Error::WorkDone("native completion deadline".into())))
                } else {
                    None
                }
            }
            Err(e) => Some(Err(e)),
        })
    }
}
