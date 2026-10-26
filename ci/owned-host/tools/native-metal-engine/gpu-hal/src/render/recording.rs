//! Reusable checked pass commands. Setters validate state changes; draws reuse
//! that validation. Strong object tables and offset storage outlive recording.
//! Execution never invokes caller code and never reinterprets a mutable source.
use super::*;
use crate::sink::{PassSink, PassTypes};
use std::marker::PhantomData;

pub struct NativeTypes<B: Backend>(PhantomData<B>);
impl<B: Backend> PassTypes for NativeTypes<B> {
    type Pipeline = Pipeline<B>;
    type Group = Group<B>;
    type Buffer = Buffer<B>;
    type IndexFormat = types::IndexFormat;
    fn pipeline_key(p: &Pipeline<B>) -> u64 {
        p.0.id
    }
    fn group_key(g: &Group<B>) -> u64 {
        g.id()
    }
    fn index_bytes(f: types::IndexFormat) -> u64 {
        if f == types::IndexFormat::Uint32 {
            4
        } else {
            2
        }
    }
}
#[derive(Clone, Copy, PartialEq, Eq)]
struct GroupState {
    index: u32,
    start: u32,
    len: u32,
}
#[derive(Clone, Copy, PartialEq, Eq)]
struct IndexState {
    index: u32,
    format: types::IndexFormat,
    offset: u64,
    size: u64,
}
#[derive(Clone, Copy)]
enum Command {
    Pipeline(u32),
    Group(u32, GroupState),
    Index(IndexState),
    Viewport(Viewport),
    Scissor([u32; 4]),
    Blend([f32; 4]),
    Stencil(u32),
    Draw {
        first: u32,
        count: u32,
        first_instance: u32,
        instances: u32,
        base: Option<i32>,
    },
}
struct OwnedColor<B: Backend> {
    view: TextureView<B>,
    clear: Option<types::Color>,
}
struct OwnedDepth<B: Backend> {
    view: TextureView<B>,
    depth: Option<f32>,
    stencil: Option<u32>,
}
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct RecorderStorage {
    pub commands: usize,
    pub offsets: usize,
    pub pipelines: usize,
    pub groups: usize,
    pub buffers: usize,
}

pub struct PassRecorder<B: Backend> {
    owner: Rc<Owner<B>>,
    failed: Option<Error>,
    width: u32,
    height: u32,
    samples: u32,
    formats: Vec<Option<Format>>,
    depth_format: Option<Format>,
    colors: Vec<Option<OwnedColor<B>>>,
    depth: Option<OwnedDepth<B>>,
    resolves: Vec<Option<TextureView<B>>>,
    commands: Vec<Command>,
    offsets: Vec<u32>,
    pipelines: Vec<Pipeline<B>>,
    groups: Vec<Group<B>>,
    indices: Vec<Buffer<B>>,
    pipeline_ids: BTreeMap<u64, u32>,
    group_ids: BTreeMap<u64, u32>,
    index_ids: BTreeMap<u64, u32>,
    buffers: BTreeMap<u64, (Buffer<B>, U)>,
    images: BTreeMap<(u64, u32, u32), (texture::Texture<B>, T)>,
    used_groups: Vec<bool>,
    pipeline: Option<u32>,
    bound: Vec<Option<GroupState>>,
    index: Option<IndexState>,
    dirty_slots: u32,
    viewport: Option<Viewport>,
    scissor: Option<[u32; 4]>,
    blend: Option<[f32; 4]>,
    stencil: u32,
    emitted_pipeline: Option<u32>,
    emitted_layout: Option<u64>,
    emitted_groups: Vec<Option<GroupState>>,
    emitted_index: Option<IndexState>,
    emitted_viewport: Option<Viewport>,
    emitted_scissor: Option<[u32; 4]>,
    emitted_blend: Option<[f32; 4]>,
    emitted_stencil: Option<u32>,
    stats: PassStats,
    draw_extent: [u32; 2],
}
impl<B: Backend> Gpu<B> {
    pub fn render_recorder(
        &self,
        colors: &[Option<ColorAttachment<'_, B>>],
        depth: Option<DepthAttachment<'_, B>>,
        resolves: &[Option<&TextureView<B>>],
    ) -> Result<PassRecorder<B>> {
        let mut r = PassRecorder {
            owner: self.owner.clone(),
            failed: None,
            width: 0,
            height: 0,
            samples: 1,
            formats: Vec::new(),
            depth_format: None,
            colors: Vec::new(),
            depth: None,
            resolves: Vec::new(),
            commands: Vec::new(),
            offsets: Vec::new(),
            pipelines: Vec::new(),
            groups: Vec::new(),
            indices: Vec::new(),
            pipeline_ids: BTreeMap::new(),
            group_ids: BTreeMap::new(),
            index_ids: BTreeMap::new(),
            buffers: BTreeMap::new(),
            images: BTreeMap::new(),
            used_groups: Vec::new(),
            pipeline: None,
            bound: Vec::new(),
            index: None,
            dirty_slots: u32::MAX,
            viewport: None,
            scissor: None,
            blend: None,
            stencil: 0,
            emitted_pipeline: None,
            emitted_layout: None,
            emitted_groups: Vec::new(),
            emitted_index: None,
            emitted_viewport: None,
            emitted_scissor: None,
            emitted_blend: None,
            emitted_stencil: None,
            stats: PassStats::default(),
            draw_extent: [0; 2],
        };
        r.reset(colors, depth, resolves)?;
        Ok(r)
    }
}
impl<B: Backend> PassRecorder<B> {
    /// Ownership caches may accept only the targets selected for this draw.
    /// This checks actual retained view identities without allocating a list.
    pub(crate) fn targets_are_selected(
        &self,
        colors: &[Option<u64>; 4],
        depth: Option<u64>,
    ) -> bool {
        self.colors.len() <= 4
            && self
                .colors
                .iter()
                .enumerate()
                .all(|(slot, c)| c.as_ref().is_none_or(|c| Some(c.view.id()) == colors[slot]))
            && self
                .depth
                .as_ref()
                .is_none_or(|d| Some(d.view.id()) == depth)
            && self.resolves.iter().all(Option::is_none)
    }
    /// Constant-time ownership check; accumulated only from emitted draws.
    pub(crate) fn fits_owned_extent(&self, extent: [u32; 2]) -> bool {
        self.colors.iter().flatten().all(|c| c.clear.is_none())
            && self
                .depth
                .as_ref()
                .is_none_or(|d| d.depth.is_none() && d.stencil.is_none())
            && self.draw_extent[0] <= extent[0]
            && self.draw_extent[1] <= extent[1]
    }
    /// Retains vector capacity. Commands already encoded own separate strong
    /// resource references, so reset never releases in-flight native resources.
    pub fn reset(
        &mut self,
        colors: &[Option<ColorAttachment<'_, B>>],
        depth: Option<DepthAttachment<'_, B>>,
        resolves: &[Option<&TextureView<B>>],
    ) -> Result<()> {
        self.release();
        let info = match validate_attachments(&self.owner, colors, depth.as_ref(), resolves) {
            Ok(info) => info,
            Err(e) => {
                self.failed = Some(e.clone());
                return Err(e);
            }
        };
        self.width = info.width;
        self.height = info.height;
        self.samples = info.samples;
        self.formats = info.formats;
        self.depth_format = info.depth_format;
        self.colors.extend(colors.iter().map(|c| {
            c.as_ref().map(|c| OwnedColor {
                view: c.view.clone(),
                clear: c.clear,
            })
        }));
        self.depth = depth.map(|d| OwnedDepth {
            view: d.view.clone(),
            depth: d.clear_depth,
            stencil: d.clear_stencil,
        });
        self.resolves
            .extend(resolves.iter().map(|v| v.map(Clone::clone)));
        self.images.extend(
            info.images
                .into_iter()
                .map(|(k, (t, u))| (k, (t.clone(), u))),
        );
        self.failed = None;
        Ok(())
    }
    /// Release this pass's ownership after replay or abandonment. Encoded work
    /// owns its own references. Capacity is retained, and replay is disabled
    /// until a successful reset establishes a new attachment contract.
    pub fn release(&mut self) {
        self.colors.clear();
        self.depth = None;
        self.resolves.clear();
        self.formats.clear();
        self.depth_format = None;
        self.commands.clear();
        self.offsets.clear();
        self.pipelines.clear();
        self.groups.clear();
        self.indices.clear();
        self.pipeline_ids.clear();
        self.group_ids.clear();
        self.index_ids.clear();
        self.buffers.clear();
        self.images.clear();
        self.used_groups.clear();
        self.pipeline = None;
        self.bound.clear();
        self.index = None;
        self.dirty_slots = u32::MAX;
        self.viewport = None;
        self.scissor = None;
        self.blend = None;
        self.stencil = 0;
        self.emitted_pipeline = None;
        self.emitted_layout = None;
        self.emitted_groups.clear();
        self.emitted_index = None;
        self.emitted_viewport = None;
        self.emitted_scissor = None;
        self.emitted_blend = None;
        self.emitted_stencil = None;
        self.stats = PassStats::default();
        self.draw_extent = [0; 2];
        self.failed = Some(Error::Rejected("released recorder"));
    }
    pub fn storage(&self) -> RecorderStorage {
        RecorderStorage {
            commands: self.commands.capacity(),
            offsets: self.offsets.capacity(),
            pipelines: self.pipelines.capacity(),
            groups: self.groups.capacity(),
            buffers: self.indices.capacity(),
        }
    }
    pub fn stats(&self) -> PassStats {
        self.stats
    }
    fn healthy(&self) -> Result<()> {
        self.owner.healthy()?;
        if let Some(e) = &self.failed {
            Err(e.clone())
        } else {
            Ok(())
        }
    }
    fn checked(&mut self, f: impl FnOnce(&mut Self) -> Result<()>) -> Result<()> {
        self.healthy()?;
        let result = f(self);
        if let Err(e) = &result {
            self.failed = Some(e.clone());
        }
        result
    }
    fn push(&mut self, c: Command) -> Result<()> {
        if self.commands.len() >= 1_048_576 {
            return Err(Error::Rejected("pass command budget"));
        }
        self.commands.push(c);
        Ok(())
    }
    fn use_group(&mut self, index: u32) -> Result<()> {
        if self.used_groups[index as usize] {
            return Ok(());
        }
        let group = &self.groups[index as usize];
        for (b, u) in &group.0.buffers {
            if !U::INCLUSIVE.contains(*u) {
                return Err(Error::Rejected("writable graphics binding"));
            }
            self.buffers
                .entry(b.id())
                .and_modify(|(_, old)| *old |= *u)
                .or_insert_with(|| (b.clone(), *u));
        }
        for v in &group.0.textures {
            let r = v.range();
            for mip in r.base_mip_level..r.base_mip_level + r.mip_level_count.unwrap() {
                for layer in r.base_array_layer..r.base_array_layer + r.array_layer_count.unwrap() {
                    let key = (v.texture().id(), mip, layer);
                    if self
                        .images
                        .get(&key)
                        .is_some_and(|(_, u)| *u != T::RESOURCE)
                    {
                        return Err(Error::Rejected("sampled attachment aliases target"));
                    }
                    self.images
                        .entry(key)
                        .or_insert_with(|| (v.texture().clone(), T::RESOURCE));
                }
            }
        }
        self.used_groups[index as usize] = true;
        self.stats.unique_groups += 1;
        Ok(())
    }
    fn validate_draw_state(
        &mut self,
        vertices: &Range<u32>,
        instances: &Range<u32>,
        base: Option<i32>,
    ) -> Result<(u32, u64, usize)> {
        if self.stats.draws + self.stats.skipped_empty >= 65536 {
            return Err(Error::Rejected("pass draw budget"));
        }
        if vertices.start > vertices.end || instances.start > instances.end {
            return Err(Error::Rejected("draw ranges"));
        }
        let pi = self
            .pipeline
            .ok_or(Error::Rejected("draw without pipeline"))?;
        let p = &self.pipelines[pi as usize].0;
        let layout_id = p.layout.id();
        let group_count = p.layout.0.groups.len();
        if (instances.start != 0 || base.is_some_and(|b| b != 0))
            && !self
                .owner
                .downlevel
                .contains(types::DownlevelFlags::BASE_VERTEX)
        {
            return Err(Error::Rejected("base vertex/instance capability"));
        }
        if let Some(_) = base {
            let i = self
                .index
                .ok_or(Error::Rejected("indexed draw without buffer"))?;
            if u64::from(vertices.end) * NativeTypes::<B>::index_bytes(i.format) > i.size
                || p.primitive
                    .strip_index_format
                    .is_some_and(|f| f != i.format)
            {
                return Err(Error::Rejected("index draw bounds/format"));
            }
        }
        let active_slots = 1u32
            .checked_shl(group_count as u32)
            .unwrap_or(0)
            .wrapping_sub(1);
        if self.dirty_slots & active_slots != 0 {
            for n in 0..group_count {
                if self.dirty_slots & (1u32 << n) == 0 {
                    continue;
                }
                let g = self
                    .bound
                    .get(n)
                    .copied()
                    .flatten()
                    .ok_or(Error::Rejected("draw missing group"))?;
                if !self.pipelines[pi as usize].0.layout.0.groups[n]
                    .compatible(&self.groups[g.index as usize].0.layout)
                {
                    return Err(Error::Rejected("draw group layout mismatch"));
                }
                self.groups[g.index as usize].check_sizes(n as u32, &self.pipelines[pi as usize].0.required_sizes)?;
                self.use_group(g.index)?;
                self.dirty_slots &= !(1u32 << n);
            }
        }
        Ok((pi, layout_id, group_count))
    }
    /// Check the complete resource/state contract without appending a draw.
    /// A strict owner uses this before committing CPU constants/reservations.
    /// Cached immutable resource uses may be retained, but no GPU work or draw
    /// command is emitted. Ignored validation errors still latch this recorder.
    pub(crate) fn preflight_draw(
        &mut self,
        vertices: &Range<u32>,
        instances: &Range<u32>,
        base: Option<i32>,
        additional_offsets: usize,
    ) -> Result<()> {
        self.checked(|s| {
            let (_, _, groups) = s.validate_draw_state(vertices, instances, base)?;
            if s.commands
                .len()
                .checked_add(groups + 7)
                .is_none_or(|n| n > 1_048_576)
                || s.offsets
                    .len()
                    .checked_add(additional_offsets)
                    .is_none_or(|n| n > 1_048_576)
            {
                return Err(Error::Rejected("draw recorder storage budget"));
            }
            Ok(())
        })
    }
    fn record_draw(
        &mut self,
        vertices: Range<u32>,
        instances: Range<u32>,
        base: Option<i32>,
    ) -> Result<()> {
        let (pi, layout_id, group_count) = self.validate_draw_state(&vertices, &instances, base)?;
        let scissor = self.scissor.unwrap_or([0, 0, self.width, self.height]);
        if vertices.is_empty()
            || instances.is_empty()
            || scissor[2] == 0
            || scissor[3] == 0
            || self
                .viewport
                .is_some_and(|v| v.width == 0. || v.height == 0.)
        {
            self.stats.skipped_empty += 1;
            return Ok(());
        }
        if self.emitted_pipeline != Some(pi) {
            self.push(Command::Pipeline(pi))?;
            self.emitted_pipeline = Some(pi);
            self.stats.pipeline_binds += 1;
        }
        if self.emitted_layout != Some(layout_id) {
            self.emitted_groups.clear();
            self.emitted_groups.resize(group_count, None);
            self.emitted_layout = Some(layout_id);
        }
        for n in 0..group_count {
            let g = self.bound[n].unwrap();
            if self.emitted_groups[n] != Some(g) {
                self.push(Command::Group(n as u32, g))?;
                self.emitted_groups[n] = Some(g);
                self.stats.group_binds += 1;
            }
        }
        let viewport = self.viewport.unwrap_or(Viewport {
            x: 0.,
            y: 0.,
            width: self.width as f32,
            height: self.height as f32,
            min_depth: 0.,
            max_depth: 1.,
        });
        if self.emitted_viewport != Some(viewport) {
            self.push(Command::Viewport(viewport))?;
            self.emitted_viewport = Some(viewport);
        }
        if self.emitted_scissor != Some(scissor) {
            self.push(Command::Scissor(scissor))?;
            self.emitted_scissor = Some(scissor);
        }
        let blend = self.blend.unwrap_or([0.; 4]);
        if self.emitted_blend != Some(blend) {
            self.push(Command::Blend(blend))?;
            self.emitted_blend = Some(blend);
        }
        if self.emitted_stencil != Some(self.stencil) {
            self.push(Command::Stencil(self.stencil))?;
            self.emitted_stencil = Some(self.stencil);
        }
        if base.is_some() {
            let i = self.index.unwrap();
            if self.emitted_index != Some(i) {
                self.push(Command::Index(i))?;
                self.emitted_index = Some(i);
                let b = &self.indices[i.index as usize];
                self.buffers
                    .entry(b.id())
                    .and_modify(|(_, old)| *old |= U::INDEX)
                    .or_insert_with(|| (b.clone(), U::INDEX));
            }
        }
        self.push(Command::Draw {
            first: vertices.start,
            count: vertices.end - vertices.start,
            first_instance: instances.start,
            instances: instances.end - instances.start,
            base,
        })?;
        self.draw_extent[0] = self.draw_extent[0].max(scissor[0] + scissor[2]);
        self.draw_extent[1] = self.draw_extent[1].max(scissor[1] + scissor[3]);
        self.stats.draws += 1;
        Ok(())
    }
}
impl<B: Backend> PassSink<NativeTypes<B>> for PassRecorder<B> {
    type Error = Error;
    fn set_pipeline(&mut self, p: &Pipeline<B>) -> Result<()> {
        self.checked(|s| {
            if !Rc::ptr_eq(&s.owner, &p.0.owner) {
                return Err(Error::Rejected("pipeline device"));
            }
            let index = if let Some(&i) = s.pipeline_ids.get(&p.0.id) {
                i
            } else {
                if p.0.colors != s.formats
                    || p.0.depth != s.depth_format
                    || p.0.samples != s.samples
                {
                    return Err(Error::Rejected("pipeline/pass format or samples"));
                }
                let i = s.pipelines.len() as u32;
                s.pipelines.push(p.clone());
                s.pipeline_ids.insert(p.0.id, i);
                i
            };
            if s.pipeline != Some(index) {
                if s.pipeline
                    .is_none_or(|old| s.pipelines[old as usize].0.layout.id() != p.0.layout.id())
                {
                    s.dirty_slots = u32::MAX;
                }
                s.dirty_slots = u32::MAX;
                s.pipeline = Some(index);
            }
            Ok(())
        })
    }
    fn set_group(&mut self, slot: u32, g: &Group<B>, offsets: &[u32]) -> Result<()> {
        self.checked(|s| {
            if slot >= s.owner.limits.max_bind_groups || slot >= 32 || !g.belongs_to(&s.owner) {
                return Err(Error::Rejected("group slot/device"));
            }
            let old = s.bound.get(slot as usize).copied().flatten();
            let same = old.filter(|old| s.groups[old.index as usize].id() == g.id());
            if let Some(old) = same {
                if s.offsets[old.start as usize..(old.start + old.len) as usize] == *offsets {
                    return Ok(());
                }
            }
            g.validate_offsets(offsets)?;
            if s.offsets
                .len()
                .checked_add(offsets.len())
                .is_none_or(|n| n > 1_048_576)
            {
                return Err(Error::Rejected("dynamic offset storage budget"));
            }
            let index = if let Some(old) = same {
                old.index
            } else if let Some(&i) = s.group_ids.get(&g.id()) {
                i
            } else {
                let i = s.groups.len() as u32;
                s.groups.push(g.clone());
                s.used_groups.push(false);
                s.group_ids.insert(g.id(), i);
                i
            };
            let state = GroupState {
                index,
                start: s.offsets.len() as u32,
                len: offsets.len() as u32,
            };
            s.offsets.extend_from_slice(offsets);
            if s.bound.len() <= slot as usize {
                s.bound.resize(slot as usize + 1, None);
            }
            s.bound[slot as usize] = Some(state);
            // Offset validation above is sufficient for the same immutable group.
            // Its layout and scope cannot change merely because a dynamic offset did.
            if same.is_none() {
                s.dirty_slots |= 1u32 << slot;
            }
            Ok(())
        })
    }
    fn set_index(
        &mut self,
        b: &Buffer<B>,
        format: types::IndexFormat,
        offset: u64,
        size: Option<u64>,
    ) -> Result<()> {
        self.checked(|s| {
            if !Rc::ptr_eq(&s.owner, &b.0.owner) || !b.0.usage.contains(U::INDEX) {
                return Err(Error::Rejected("index buffer device/usage"));
            }
            let size = size
                .or_else(|| b.size().checked_sub(offset))
                .ok_or(Error::Rejected("index offset"))?;
            let unit = NativeTypes::<B>::index_bytes(format);
            if size == 0
                || offset % unit != 0
                || size % unit != 0
                || offset.checked_add(size).is_none_or(|n| n > b.size())
            {
                return Err(Error::Rejected("index buffer range/alignment"));
            }
            let index = if let Some(&i) = s.index_ids.get(&b.id()) {
                i
            } else {
                let i = s.indices.len() as u32;
                s.indices.push(b.clone());
                s.index_ids.insert(b.id(), i);
                i
            };
            s.index = Some(IndexState {
                index,
                format,
                offset,
                size,
            });
            Ok(())
        })
    }
    fn set_viewport(&mut self, [x, y, w, h, z0, z1]: [f32; 6]) -> Result<()> {
        self.checked(|s| {
            validate_viewport(
                &s.owner,
                Viewport {
                    x,
                    y,
                    width: w,
                    height: h,
                    min_depth: z0,
                    max_depth: z1,
                },
            )?;
            s.viewport = Some(Viewport {
                x,
                y,
                width: w,
                height: h,
                min_depth: z0,
                max_depth: z1,
            });
            Ok(())
        })
    }
    fn set_scissor(&mut self, [x, y, w, h]: [u32; 4]) -> Result<()> {
        self.checked(|s| {
            if x.checked_add(w).is_none_or(|n| n > s.width)
                || y.checked_add(h).is_none_or(|n| n > s.height)
            {
                return Err(Error::Rejected("scissor bounds"));
            }
            s.scissor = Some([x, y, w, h]);
            Ok(())
        })
    }
    fn set_blend(&mut self, c: [f64; 4]) -> Result<()> {
        self.checked(|s| {
            if !c
                .into_iter()
                .all(|v| v.is_finite() && (v as f32).is_finite())
            {
                return Err(Error::Rejected("blend representation"));
            }
            s.blend = Some(c.map(|v| v as f32));
            Ok(())
        })
    }
    fn set_stencil(&mut self, reference: u32) -> Result<()> {
        self.checked(|s| {
            s.stencil = reference;
            Ok(())
        })
    }
    fn draw(&mut self, v: Range<u32>, i: Range<u32>) -> Result<()> {
        self.checked(|s| s.record_draw(v, i, None))
    }
    fn draw_indexed(&mut self, v: Range<u32>, base: i32, i: Range<u32>) -> Result<()> {
        self.checked(|s| s.record_draw(v, i, Some(base)))
    }
}
impl<B: Backend + 'static> Encoder<B> {
    pub fn render_recorded(&mut self, r: &PassRecorder<B>) -> Result<PassStats> {
        self.owner.healthy()?;
        r.healthy()?;
        if !Rc::ptr_eq(&self.owner, &r.owner) {
            return Err(Error::Rejected("recorded pass device"));
        }
        for (b, u) in r.buffers.values() {
            self.transition(b, *u);
        }
        for ((_, mip, layer), (t, u)) in &r.images {
            self.transition_texture(t, *mip, *layer, *u);
        }
        for p in &r.pipelines {
            self.holds.entry(p.0.id).or_insert_with(|| p.0.clone());
        }
        for g in &r.groups {
            self.holds.entry(g.id()).or_insert_with(|| g.0.clone());
        }
        for c in r.colors.iter().flatten() {
            self.holds
                .entry(c.view.id())
                .or_insert_with(|| c.view.retained());
        }
        for v in r.resolves.iter().flatten() {
            self.holds.entry(v.id()).or_insert_with(|| v.retained());
        }
        if let Some(d) = &r.depth {
            self.holds
                .entry(d.view.id())
                .or_insert_with(|| d.view.retained());
        }
        let colors: Vec<_> = r
            .colors
            .iter()
            .enumerate()
            .map(|(i, c)| {
                c.as_ref().map(|c| hal::ColorAttachment {
                    target: hal::Attachment {
                        view: c.view.raw(),
                        usage: T::COLOR_TARGET,
                    },
                    depth_slice: None,
                    resolve_target: r.resolves.get(i).and_then(Option::as_ref).map(|v| {
                        hal::Attachment {
                            view: v.raw(),
                            usage: T::COLOR_TARGET,
                        }
                    }),
                    ops: hal::AttachmentOps::STORE
                        | if c.clear.is_some() {
                            hal::AttachmentOps::LOAD_CLEAR
                        } else {
                            hal::AttachmentOps::LOAD
                        },
                    clear_value: c.clear.unwrap_or(types::Color::TRANSPARENT),
                })
            })
            .collect();
        unsafe {
            let raw = self.raw.as_mut().unwrap();
            raw.begin_render_pass(&hal::RenderPassDescriptor {
                label: Some("native recorded pass"),
                extent: types::Extent3d {
                    width: r.width,
                    height: r.height,
                    depth_or_array_layers: 1,
                },
                sample_count: r.samples,
                color_attachments: &colors,
                depth_stencil_attachment: r.depth.as_ref().map(|d| hal::DepthStencilAttachment {
                    target: hal::Attachment {
                        view: d.view.raw(),
                        usage: T::DEPTH_STENCIL_WRITE,
                    },
                    depth_ops: hal::AttachmentOps::STORE
                        | if d.depth.is_some() {
                            hal::AttachmentOps::LOAD_CLEAR
                        } else {
                            hal::AttachmentOps::LOAD
                        },
                    stencil_ops: if d.view.texture().descriptor().format.has_stencil_aspect() {
                        hal::AttachmentOps::STORE
                            | if d.stencil.is_some() {
                                hal::AttachmentOps::LOAD_CLEAR
                            } else {
                                hal::AttachmentOps::LOAD
                            }
                    } else {
                        hal::AttachmentOps::LOAD_DONT_CARE | hal::AttachmentOps::STORE_DISCARD
                    },
                    clear_value: (d.depth.unwrap_or(0.), d.stencil.unwrap_or(0)),
                }),
                multiview_mask: None,
                timestamp_writes: None,
                occlusion_query_set: None,
            })
            .map_err(|e| self.owner.device_error(e))?;
            let scope = RenderScope {
                raw,
                owner: &self.owner,
            };
            let raw = &mut *scope.raw;
            let mut layout = None;
            for command in &r.commands {
                match *command {
                    Command::Pipeline(i) => {
                        let p = &r.pipelines[i as usize].0;
                        raw.set_render_pipeline(p.raw.as_ref().unwrap());
                        layout = Some(&p.layout);
                    }
                    Command::Group(slot, g) => raw.set_bind_group(
                        layout.unwrap().raw(),
                        slot,
                        r.groups[g.index as usize].raw(),
                        &r.offsets[g.start as usize..(g.start + g.len) as usize],
                    ),
                    Command::Index(i) => raw.set_index_buffer(
                        hal::BufferBinding::new_unchecked(
                            r.indices[i.index as usize].raw(),
                            i.offset,
                            NonZeroU64::new(i.size),
                        ),
                        i.format,
                    ),
                    Command::Viewport(v) => raw.set_viewport(
                        &hal::Rect {
                            x: v.x,
                            y: v.y,
                            w: v.width,
                            h: v.height,
                        },
                        v.min_depth..v.max_depth,
                    ),
                    Command::Scissor([x, y, w, h]) => {
                        raw.set_scissor_rect(&hal::Rect { x, y, w, h })
                    }
                    Command::Blend(c) => raw.set_blend_constants(&c),
                    Command::Stencil(v) => raw.set_stencil_reference(v),
                    Command::Draw {
                        first,
                        count,
                        first_instance,
                        instances,
                        base: Some(base),
                    } => raw.draw_indexed(first, count, base, first_instance, instances),
                    Command::Draw {
                        first,
                        count,
                        first_instance,
                        instances,
                        base: None,
                    } => raw.draw(first, count, first_instance, instances),
                }
            }
        }
        Ok(r.stats)
    }
}
