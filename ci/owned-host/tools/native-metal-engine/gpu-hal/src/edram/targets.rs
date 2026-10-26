//! Transactional native host-target cache. CPU ownership is published only
//! after its complete command stream is accepted by the one device queue.
//! Dropping or rejecting a frame leaves the previous ownership/resources intact.
//! ConstantFrame also owns GPU stores, immutable texture consumers and clears.
//! The prepared session connects display; the live CPU visibility adapter and
//! raw game command producer remain separate integration gates.
use super::*;
use crate::texture::{TextureDescriptor, TextureView};
use xenos_gfx::edram_ownership::{OwnedRange, OwnershipMap};
use xenos_state::edram::{
    EDRAM_TILE_COUNT, RenderTargetBinding, RenderTargetSetup, RenderTargetSlot,
};
mod clear;
mod journal;
mod resolve;
pub use journal::{GuestByteWrite, GuestWriteJournal};
mod draws;
mod guest_memory;
mod guest_texture;
pub use draws::{
    ConstantDraw, ConstantFrame, ConstantSlot, DrawAdmission, DrawArena, DrawStats, TargetInfo,
};
pub use guest_memory::{GUEST_PAGE_BYTES, GuestMemory, GuestPage, WritebackStats};
pub use guest_texture::{GuestTexture, GuestTextureBinding, TextureAdmission};
pub use resolve::ResolvePixels;

#[derive(Clone)]
struct Target {
    key: RenderTargetKey,
    attachment: TextureView<Platform>,
    read: TextureView<Platform>,
    stencil: Option<TextureView<Platform>>,
    width: u32,
    height: u32,
    format: F,
    samples: u32,
    last_used: u64,
}
#[derive(Clone, Default)]
struct State {
    textures: guest_texture::TextureState,
    targets: HashMap<RenderTargetKey, Target>,
    scratch: HashMap<(u32, u32, u32), TextureView<Platform>>,
    ownership: OwnershipMap,
    epoch: u64,
    // Ownership is never removed in this cache. Every owned source span is
    // redumped before reading it; never-owned shadow tiles remain initialized0.
    resolve_shadow: Option<Buffer<Platform>>,
}
#[derive(Clone, Copy, Debug)]
pub struct TargetOptions {
    pub scale: u32,
    pub max_targets: usize,
    pub stencil_mode: StencilMode,
}
impl Default for TargetOptions {
    fn default() -> Self {
        Self {
            scale: 1,
            max_targets: 128,
            stencil_mode: StencilMode::PortableBits,
        }
    }
}
pub struct TargetCache {
    owner: Rc<Owner<Platform>>,
    state: State,
    pipelines: TransferCache,
    options: TargetOptions,
    last_submission: Serial,
    resolve_kernels: resolve::Pipelines,
    texture_kernels: guest_texture::TextureKernels,
    clear_kernels: clear::Pipelines,
}
/// Each selected binding retains its original guest metadata. Pipeline/shader
/// projection must still apply color exponent/range and float24 semantics.
pub struct SelectedTarget {
    pub binding: RenderTargetBinding,
    pub view: TextureView<Platform>,
    pub format: F,
}
pub struct DrawTargets {
    pub selected: Vec<SelectedTarget>,
    pub width: u32,
    pub height: u32,
    /// Complete guest-owned rows, independent of allocated/grown host height.
    pub owned_height: u32,
    pub samples: u32,
    pub sample_mask: u32,
}
impl DrawTargets {
    pub fn is_empty(&self) -> bool {
        self.selected.is_empty()
    }
}
#[derive(Default, Clone, Copy, Debug, PartialEq, Eq)]
pub struct FrameStats {
    pub transfers: u64,
    pub main_draws: u64,
    pub stencil_draws: u64,
    pub created_targets: u64,
    pub grown_targets: u64,
    pub resolves: u64,
    pub dump_dispatches: u64,
    pub clear_draws: u64,
}
pub struct Frame<'a> {
    cache: &'a mut TargetCache,
    gpu: &'a mut Gpu<Platform>,
    encoder: Option<Encoder<Platform>>,
    state: State,
    failed: bool,
    stats: FrameStats,
    active_colors: [Option<u64>; 4],
    active_depth: Option<u64>,
    active_extent: [u32; 2],
    active_owned_extent: [u32; 2],
    resolve_status: Rc<Cell<resolve::Submission>>,
    resolve_bytes: u64,
    journal_bytes: u64,
}

impl Gpu<Platform> {
    pub fn edram_target_cache(&self, options: TargetOptions) -> Result<TargetCache> {
        self.owner.healthy()?;
        if !(1..=3).contains(&options.scale) || !(1..=256).contains(&options.max_targets) {
            return Err(Error::Rejected("EDRAM target cache options"));
        }
        Ok(TargetCache {
            owner: self.owner.clone(),
            state: State::default(),
            pipelines: self.edram_transfer_cache(64)?,
            options,
            last_submission: 0,
            resolve_kernels: Default::default(),
            texture_kernels: Default::default(),
            clear_kernels: Default::default(),
        })
    }
}
impl TargetCache {
    pub fn begin<'a>(&'a mut self, gpu: &'a mut Gpu<Platform>) -> Result<Frame<'a>> {
        gpu.owner.healthy()?;
        if !Rc::ptr_eq(&self.owner, &gpu.owner) {
            return Err(Error::Rejected("EDRAM target cache device"));
        }
        let mut state = self.state.clone();
        state.epoch = state
            .epoch
            .checked_add(1)
            .ok_or(Error::Rejected("EDRAM frame epoch exhaustion"))?;
        let encoder = Some(gpu.encoder()?);
        Ok(Frame {
            cache: self,
            gpu,
            encoder,
            state,
            failed: false,
            stats: Default::default(),
            active_colors: [None; 4],
            active_depth: None,
            active_extent: [0; 2],
            active_owned_extent: [0; 2],
            resolve_status: Rc::new(Cell::new(resolve::Submission::Staged)),
            resolve_bytes: 0,
            journal_bytes: 0,
        })
    }
    pub fn ownership(&self) -> &[OwnedRange] {
        self.state.ownership.ranges()
    }
    pub fn target_count(&self) -> usize {
        self.state.targets.len()
    }
    pub fn last_submission(&self) -> Serial {
        self.last_submission
    }
    /// Read-only snapshot. Clients must not mutate a target outside a cache frame.
    pub fn view(&self, key: RenderTargetKey) -> Option<TextureView<Platform>> {
        self.state.targets.get(&key).map(|t| t.attachment.clone())
    }
    /// Only unowned targets can be discarded; queued uses retain native handles.
    pub fn trim_unowned(&mut self) {
        let owners = self.state.ownership.ranges();
        self.state
            .targets
            .retain(|k, _| owners.iter().any(|r| r.owner == *k));
        self.state.scratch.clear();
    }
}
struct Work {
    source: Target,
    destination: Target,
    plan: Transfer,
    pipeline: TransferPipeline,
    scratch: Option<TextureView<Platform>>,
}
impl Frame<'_> {
    pub fn gpu(&self) -> &Gpu<Platform> {
        self.gpu
    }
    pub fn stats(&self) -> FrameStats {
        self.stats
    }
    fn healthy(&self) -> Result<()> {
        if self.failed {
            Err(Error::Rejected("EDRAM frame was rejected"))
        } else {
            self.gpu.owner.healthy()
        }
    }
    pub fn targets(&mut self, setup: &RenderTargetSetup, min_height: u32) -> Result<DrawTargets> {
        self.healthy()?;
        let result = self.targets_inner(setup, min_height);
        if result.is_err() {
            self.failed = true;
        } else if let Ok(targets) = &result {
            self.active_colors = [None; 4];
            self.active_depth = None;
            self.active_extent = [targets.width, targets.height];
            self.active_owned_extent = [targets.width, targets.owned_height];
            for target in &targets.selected {
                match target.binding.slot {
                    RenderTargetSlot::Color(slot) => {
                        self.active_colors[slot as usize] = Some(target.view.id())
                    }
                    RenderTargetSlot::Depth => self.active_depth = Some(target.view.id()),
                }
            }
        }
        result
    }
    pub fn render_full(
        &mut self,
        colors: &[Option<ColorAttachment<'_, Platform>>],
        depth: Option<DepthAttachment<'_, Platform>>,
        resolves: &[Option<&TextureView<Platform>>],
        draws: &[Draw<'_, Platform>],
    ) -> Result<crate::render::PassStats> {
        self.healthy()?;
        if colors.len() > 4
            || colors.iter().enumerate().any(|(slot, color)| {
                color
                    .as_ref()
                    .is_some_and(|c| Some(c.view.id()) != self.active_colors[slot])
            })
            || depth
                .as_ref()
                .is_some_and(|d| Some(d.view.id()) != self.active_depth)
            || resolves.iter().any(Option::is_some)
        {
            self.failed = true;
            return Err(Error::Rejected(
                "EDRAM draw does not own the selected attachments",
            ));
        }
        // LoadOp clears affect the whole grown allocation regardless of scissor.
        // Guest EDRAM clears require their own resolve contract, not host clears.
        if colors.iter().flatten().any(|c| c.clear.is_some())
            || depth
                .as_ref()
                .is_some_and(|d| d.clear_depth.is_some() || d.clear_stencil.is_some())
            || draws.iter().any(|d| {
                let [x, y, w, h] =
                    d.scissor
                        .unwrap_or([0, 0, self.active_extent[0], self.active_extent[1]]);
                x.checked_add(w)
                    .is_none_or(|n| n > self.active_owned_extent[0])
                    || y.checked_add(h)
                        .is_none_or(|n| n > self.active_owned_extent[1])
            })
        {
            self.failed = true;
            return Err(Error::Rejected(
                "EDRAM draw exceeds owned extent or uses host clear",
            ));
        }
        let result = self
            .encoder
            .as_mut()
            .unwrap()
            .render_full(colors, depth, resolves, draws);
        if result.is_err() {
            self.failed = true;
        }
        result
    }
    pub fn render_recorded(
        &mut self,
        recorded: &crate::render::PassRecorder<Platform>,
    ) -> Result<crate::render::PassStats> {
        self.healthy()?;
        if !recorded.targets_are_selected(&self.active_colors, self.active_depth)
            || !recorded.fits_owned_extent(self.active_owned_extent)
        {
            self.failed = true;
            return Err(Error::Rejected("EDRAM recorded draw has stale attachments"));
        }
        let result = self.encoder.as_mut().unwrap().render_recorded(recorded);
        if result.is_err() {
            self.failed = true;
        }
        result
    }
    /// Never publishes speculative ownership before the backend accepts submit.
    /// Unknown/failed submission already poisons the GPU and quarantines resources.
    pub fn submit(mut self) -> Result<(Serial, FrameStats)> {
        self.healthy()?;
        let commands = self.encoder.take().unwrap().finish()?;
        let serial = self.gpu.submit(vec![commands])?;
        self.resolve_status
            .set(resolve::Submission::Submitted(serial));
        self.cache.state = std::mem::take(&mut self.state);
        self.cache.last_submission = serial;
        Ok((serial, self.stats))
    }
    fn host_format(&self, key: RenderTargetKey) -> Result<F> {
        Transfer::new(key, key, [1, 1], [0, 0], None, self.cache.options.scale)
            .map_err(|_| Error::Rejected("EDRAM target key"))?;
        if key.is_depth {
            // Actual exhaustive host-texture tests find millions of payloads
            // lost by D24 storage, for BOTH normalized and float guest depth.
            // Explicit D32S8 is lossless for all2^24 values with the shared codec.
            if !self
                .gpu
                .features()
                .contains(types::Features::DEPTH32FLOAT_STENCIL8)
            {
                return Err(Error::Rejected(
                    "exact EDRAM depth requires Depth32FloatStencil8",
                ));
            }
            Ok(F::Depth32FloatStencil8)
        } else {
            color_format(key).ok_or(Error::Rejected("EDRAM target color format"))
        }
    }
    fn create_target(
        &mut self,
        key: RenderTargetKey,
        width: u32,
        height: u32,
        format: F,
    ) -> Result<Target> {
        let samples = host_samples(key);
        let texture = self.gpu.create_texture_recorded(
            self.encoder.as_mut().unwrap(),
            "native EDRAM target",
            TextureDescriptor {
                size: types::Extent3d {
                    width,
                    height,
                    depth_or_array_layers: 1,
                },
                mip_levels: 1,
                sample_count: samples,
                dimension: types::TextureDimension::D2,
                format,
                usage: types::TextureUses::RESOURCE
                    | if key.is_depth {
                        types::TextureUses::DEPTH_STENCIL_WRITE
                    } else {
                        types::TextureUses::COLOR_TARGET
                    },
            },
        )?;
        let attachment = self.gpu.texture_view(&texture, V::D2, 0..1, 0..1)?;
        let (read, stencil) = if key.is_depth {
            (
                self.gpu.texture_view_aspect(
                    &texture,
                    V::D2,
                    0..1,
                    0..1,
                    types::TextureAspect::DepthOnly,
                )?,
                Some(self.gpu.texture_view_aspect(
                    &texture,
                    V::D2,
                    0..1,
                    0..1,
                    types::TextureAspect::StencilOnly,
                )?),
            )
        } else {
            (attachment.clone(), None)
        };
        Ok(Target {
            key,
            attachment,
            read,
            stencil,
            width,
            height,
            format,
            samples,
            last_used: self.state.epoch,
        })
    }
    fn work(&mut self, source: Target, destination: Target, range: [u32; 2]) -> Result<Work> {
        let scale = self.cache.options.scale;
        let plan = Transfer::new(
            source.key,
            destination.key,
            [source.width, source.height],
            range,
            None,
            scale,
        )
        .map_err(|_| Error::Rejected("EDRAM owned transfer plan"))?;
        let kind = if source.key.is_depth {
            kernels::SrcKind::Depth {
                multisampled: source.samples > 1,
            }
        } else {
            kernels::SrcKind::Color {
                multisampled: source.samples > 1,
            }
        };
        let pipeline = self.cache.pipelines.get(
            self.gpu,
            kind,
            destination.format,
            destination.samples,
            scale,
            self.cache.options.stencil_mode,
        )?;
        let scratch = if destination.key.is_depth {
            let key = (destination.width, destination.height, destination.samples);
            if !self.state.scratch.contains_key(&key) {
                // Scratch contents never survive a transfer batch, so unused dimensions
                // may be forgotten. Actual encoded resources keep their own references.
                if self.state.scratch.len() >= 16 {
                    self.state.scratch.clear();
                }
                let image = self.gpu.create_texture_recorded(
                    self.encoder.as_mut().unwrap(),
                    "native EDRAM stencil scratch",
                    TextureDescriptor {
                        size: types::Extent3d {
                            width: key.0,
                            height: key.1,
                            depth_or_array_layers: 1,
                        },
                        mip_levels: 1,
                        sample_count: key.2,
                        dimension: types::TextureDimension::D2,
                        format: F::R32Uint,
                        usage: types::TextureUses::RESOURCE | types::TextureUses::COLOR_TARGET,
                    },
                )?;
                let view = self.gpu.texture_view(&image, V::D2, 0..1, 0..1)?;
                self.state.scratch.insert(key, view);
            }
            Some(self.state.scratch[&key].clone())
        } else {
            None
        };
        Ok(Work {
            source,
            destination,
            plan,
            pipeline,
            scratch,
        })
    }
    fn encode_works(&mut self, works: &[Work]) -> Result<()> {
        if !works.is_empty() {
            let plans: Vec<_> = works.iter().map(|w| w.plan.clone()).collect();
            let params = self.gpu.edram_transfer_params(&plans)?;
            for (index, work) in works.iter().enumerate() {
                let stats = work.pipeline.encode_prepared(
                    self.gpu,
                    self.encoder.as_mut().unwrap(),
                    &params,
                    index,
                    Images {
                        source: &work.source.read,
                        source_stencil: work.source.stencil.as_ref(),
                        destination: &work.destination.attachment,
                        stencil_scratch: work.scratch.as_ref(),
                    },
                )?;
                self.stats.transfers += 1;
                self.stats.main_draws += stats.main_draws;
                self.stats.stencil_draws += stats.stencil_draws;
            }
        }
        Ok(())
    }
    fn targets_inner(&mut self, setup: &RenderTargetSetup, min_height: u32) -> Result<DrawTargets> {
        let scale = self.cache.options.scale;
        let msaa = setup.msaa_samples;
        if msaa.count().is_none() {
            return Err(Error::Rejected("EDRAM target MSAA"));
        }
        let mut selected = Vec::new();
        let mut bases = Vec::new();
        // Match the established legacy/Xenia rule: first color wins equal bases;
        // depth is considered last and is dropped when it aliases a color base.
        for (slot, binding) in setup
            .color
            .iter()
            .enumerate()
            .filter_map(|(i, b)| b.as_ref().map(|b| (RenderTargetSlot::Color(i as u32), b)))
            .chain(setup.depth.iter().map(|b| (RenderTargetSlot::Depth, b)))
        {
            if binding.key.msaa_samples != msaa
                || binding.key.pitch_tiles_at_32bpp != setup.pitch_tiles_at_32bpp
                || binding.key.is_depth != (slot == RenderTargetSlot::Depth)
            {
                return Err(Error::Rejected("EDRAM inconsistent target setup"));
            }
            self.host_format(binding.key)?;
            if !bases.contains(&binding.key.base_tiles) {
                bases.push(binding.key.base_tiles);
                selected.push(RenderTargetBinding { slot, ..*binding });
            }
        }
        if selected.is_empty() {
            return Ok(DrawTargets {
                selected: Vec::new(),
                width: 0,
                height: 0,
                owned_height: 0,
                samples: 1,
                sample_mask: !0,
            });
        }
        let max_guest = (self.gpu.owner.limits.max_texture_dimension_2d / scale).min(8192);
        let tile_h = 16 >> msaa.samples_y_log2();
        let pitch = setup.pitch_tiles_at_32bpp;
        let max_height =
            (EDRAM_TILE_COUNT.div_ceil(pitch) * tile_h).min(max_guest / tile_h * tile_h);
        if max_height == 0 {
            return Err(Error::Rejected("EDRAM target height limit"));
        }
        let rows = min_height.clamp(1, max_height).div_ceil(tile_h);
        let mut common = rows * tile_h;
        let width = selected[0].key.width_pixels();
        if width == 0 || width > max_guest {
            return Err(Error::Rejected("EDRAM target width limit"));
        }
        for b in &selected {
            if let Some(old) = self.state.targets.get(&b.key) {
                common = common.max(old.height / scale);
            }
        }
        let height = common.next_multiple_of(tile_h * 8).min(max_height) * scale;
        let width = width * scale;
        let required = selected
            .iter()
            .filter(|b| !self.state.targets.contains_key(&b.key))
            .count();
        while self.state.targets.len() + required > self.cache.options.max_targets {
            let remove = self
                .state
                .targets
                .iter()
                .filter(|(key, _)| {
                    !selected.iter().any(|b| b.key == **key)
                        && !self
                            .state
                            .ownership
                            .ranges()
                            .iter()
                            .any(|r| r.owner == **key)
                })
                .min_by_key(|(_, t)| t.last_used)
                .map(|(key, _)| *key);
            let Some(key) = remove else {
                return Err(Error::Rejected(
                    "EDRAM target cache cannot evict owned data",
                ));
            };
            self.state.targets.remove(&key);
        }
        let mut works = Vec::new();
        for b in &selected {
            if self
                .state
                .targets
                .get(&b.key)
                .is_some_and(|old| old.height >= height)
            {
                self.state.targets.get_mut(&b.key).unwrap().last_used = self.state.epoch;
                continue;
            }
            let format = self.host_format(b.key)?;
            let target = self.create_target(b.key, width, height, format)?;
            self.stats.created_targets += 1;
            if let Some(old) = self.state.targets.insert(b.key, target.clone()) {
                self.stats.grown_targets += 1;
                let owned: Vec<_> = self
                    .state
                    .ownership
                    .ranges()
                    .iter()
                    .filter(|r| r.owner == b.key)
                    .copied()
                    .collect();
                for range in owned {
                    works.push(self.work(
                        old.clone(),
                        target.clone(),
                        [range.start, range.end - range.start],
                    )?);
                }
            }
        }
        let mut order: Vec<_> = selected.iter().map(|b| b.key).collect();
        order.sort_by_key(|k| k.base_tiles);
        let mut previous = Vec::new();
        let mut owned_height = height;
        for (n, key) in order.iter().enumerate() {
            let next = if n + 1 < order.len() {
                order[n + 1].base_tiles
            } else {
                order[0].base_tiles + EDRAM_TILE_COUNT
            };
            let length = (rows * key.pitch_tiles())
                .min(next - key.base_tiles)
                .min(EDRAM_TILE_COUNT);
            owned_height = owned_height.min(length / key.pitch_tiles() * tile_h * scale);
            for range in self.state.ownership.claim(key.base_tiles, length, *key) {
                previous.push((*key, range));
            }
        }
        for (destination, range) in previous {
            let source = self
                .state
                .targets
                .get(&range.owner)
                .ok_or(Error::Rejected("EDRAM owner resource missing"))?
                .clone();
            let dest = self.state.targets[&destination].clone();
            works.push(self.work(source, dest, [range.start, range.end - range.start])?);
        }
        self.encode_works(&works)?;
        Ok(DrawTargets {
            selected: selected
                .into_iter()
                .map(|binding| {
                    let target = &self.state.targets[&binding.key];
                    SelectedTarget {
                        binding,
                        view: target.attachment.clone(),
                        format: target.format,
                    }
                })
                .collect(),
            width,
            height,
            owned_height,
            samples: host_samples(order[0]),
            sample_mask: if msaa == MsaaSamples::X2 { 9 } else { !0 },
        })
    }
}

impl Drop for Frame<'_> {
    fn drop(&mut self) {
        if self.resolve_status.get() == resolve::Submission::Staged {
            self.resolve_status.set(resolve::Submission::Abandoned);
        }
    }
}
