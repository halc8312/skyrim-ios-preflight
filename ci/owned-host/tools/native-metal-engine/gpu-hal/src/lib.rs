//! Experimental direct HAL resource/queue backend. No game-specific data.
//!
//! One owner thread and one queue. Validation stays on in release builds.
//! Pending CPU uploads precede explicitly submitted command buffers, regardless
//! of recording order. Handles keep their device and native resources alive.
//! This crate is not yet the live game's complete graphics facade.
pub mod backend;
pub mod bindings;
pub mod compute;
pub mod constants;
pub mod guest_shader;
pub mod edram;
mod fixup;
mod ordered_upload;
pub use ordered_upload::OrderedUpload;
mod reservation;
pub use reservation::UploadReservation;
pub mod pipeline;
pub mod presentation;
pub mod render;
pub mod shader;
pub mod session;
pub mod surface;
mod staging;
pub mod texture;
mod timeline;
pub use staging::StagingStats;

use backend::{Backend, Completion};
use std::{
    cell::{Cell, RefCell},
    collections::BTreeMap,
    num::NonZeroU64,
    ops::Range,
    rc::Rc,
    sync::atomic::{AtomicU64, Ordering},
    time::{Duration, Instant},
};
use timeline::Timeline;
use types::BufferUses as U;
use wgpu_hal::{
    self as hal, Adapter as _, CommandEncoder as _, Device as _, Instance as _, Queue as _,
};
pub use wgpu_types as types;

pub type Serial = u64;
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Error {
    Rejected(&'static str),
    Backend(String),
    Failed { serial: Serial, detail: String },
    Timeout(Serial),
}
type Result<T> = std::result::Result<T, Error>;
fn backend_error(error: impl std::fmt::Debug) -> Error {
    Error::Backend(format!("{error:?}"))
}
static NEXT_ID: AtomicU64 = AtomicU64::new(1);
fn id() -> Result<u64> {
    NEXT_ID
        .fetch_update(Ordering::Relaxed, Ordering::Relaxed, |n| n.checked_add(1))
        .map_err(|_| Error::Rejected("resource identity exhaustion"))
}

/// Development bounds. Validation layers are an explicit test option, while
/// the backend's range, alias and lifetime checks are always enabled.
#[derive(Clone, Debug)]
pub struct Config {
    pub validation: bool,
    /// Live renderer may request the adapter limits (e.g. all 16 varyings).
    pub adapter_limits: bool,
    /// Negotiated only when advertised. Unsupported requested bits stay disabled;
    /// resource creation always checks the ACTUAL enabled feature set.
    pub optional_texture_features: types::Features,
    pub max_in_flight: usize,
    pub backpressure_timeout: Duration,
    pub drop_timeout: Duration,
    pub max_resident_buffer_bytes: u64,
    pub max_resident_texture_bytes: u64,
    pub max_pending_upload_bytes: u64,
    pub max_staging_buffer_bytes: u64,
    /// Opt-in until workload comparisons justify a default. Reuse only after retirement.
    pub retained_staging_bytes: u64,
    pub retained_staging_buffers: usize,
    pub max_buffer_objects: u64,
    pub max_texture_objects: u64,
    pub max_native_encoders: usize,
    pub retained_encoders: usize,
}
impl Default for Config {
    fn default() -> Self {
        Self {
            validation: false,
            adapter_limits: false,
            optional_texture_features: types::Features::DEPTH32FLOAT_STENCIL8,
            max_in_flight: 8,
            backpressure_timeout: Duration::from_secs(5),
            drop_timeout: Duration::from_secs(2),
            max_resident_buffer_bytes: 512 << 20,
            max_resident_texture_bytes: 512 << 20,
            max_pending_upload_bytes: 32 << 20,
            max_staging_buffer_bytes: 256 << 20,
            retained_staging_bytes: 0,
            retained_staging_buffers: 256,
            max_buffer_objects: 16384,
            max_texture_objects: 8192,
            max_native_encoders: 64,
            retained_encoders: 16,
        }
    }
}

#[cfg(feature = "fault-injection")]
#[derive(Default)]
struct Faults {
    fence_cap: Cell<Option<Serial>>,
    completion_failure: Cell<Option<Serial>>,
    submit_failure: Cell<bool>,
    submit_panic: Cell<bool>,
    end_failure: Cell<bool>,
    fence_error: Cell<bool>,
    poison_textures: Cell<bool>,
    poison_resolve: Cell<bool>,
    skip_last_texture_zero: Cell<bool>,
}

struct Owner<B: Backend> {
    // Rust drops fields in declaration order: children before device/instance.
    encoders: RefCell<Vec<B::CommandEncoder>>,
    group_layouts: RefCell<Vec<std::rc::Weak<bindings::GroupLayoutInner<B>>>>,
    queue: B::Queue,
    device: B::Device,
    _adapter: B::Adapter,
    _instance: B::Instance,
    timeline: Rc<RefCell<Timeline>>,
    config: Config,
    limits: types::Limits,
    features: types::Features,
    live_buffers: Cell<u64>,
    live_buffer_bytes: Cell<u64>,
    texture_bytes: Cell<u64>,
    texture_objects: Cell<u64>,
    staging_bytes: Cell<u64>,
    native_encoders: Cell<usize>,
    encoders_created: Cell<u64>,
    encoders_reused: Cell<u64>,
    downlevel: types::DownlevelFlags,
    #[cfg(feature = "fault-injection")]
    faults: Faults,
}
impl<B: Backend> Owner<B> {
    fn healthy(&self) -> Result<()> {
        self.timeline.borrow().healthy()
    }
    fn fail_next(&self, detail: String) -> Error {
        let mut t = self.timeline.borrow_mut();
        let serial = t.issued().saturating_add(1);
        t.fail(serial, detail);
        t.healthy().unwrap_err()
    }
    fn device_error(&self, error: hal::DeviceError) -> Error {
        let mut t = self.timeline.borrow_mut();
        let serial = if matches!(error, hal::DeviceError::Lost | hal::DeviceError::Unexpected) {
            t.verified().saturating_add(1)
        } else {
            t.issued().saturating_add(1)
        };
        t.fail(serial, format!("native device: {error:?}"));
        t.healthy().unwrap_err()
    }
    fn shader_error(&self, error: hal::ShaderError) -> Error {
        match error {
            hal::ShaderError::Device(e) => self.device_error(e),
            other => backend_error(other),
        }
    }
    fn pipeline_error(&self, error: hal::PipelineError) -> Error {
        match error {
            hal::PipelineError::Device(e) => self.device_error(e),
            other => backend_error(other),
        }
    }
    fn recycle(&self, encoder: B::CommandEncoder) {
        if self.healthy().is_ok() && self.encoders.borrow().len() < self.config.retained_encoders {
            self.encoders.borrow_mut().push(encoder);
        } else {
            drop(encoder);
            self.native_encoders.set(self.native_encoders.get() - 1);
        }
    }
}

struct BufferInner<B: Backend> {
    owner: Rc<Owner<B>>,
    raw: Option<B::Buffer>,
    id: u64,
    size: u64,
    usage: U,
    state: Cell<U>,
    staging: bool,
}
impl<B: Backend> Drop for BufferInner<B> {
    fn drop(&mut self) {
        // Last strong reference disappears only after any encoder/submission
        // referencing this buffer has been discarded or verified terminal.
        unsafe { self.owner.device.destroy_buffer(self.raw.take().unwrap()) };
        self.owner
            .live_buffers
            .set(self.owner.live_buffers.get() - 1);
        let bytes = if self.staging {
            &self.owner.staging_bytes
        } else {
            &self.owner.live_buffer_bytes
        };
        bytes.set(bytes.get() - self.size);
    }
}
pub struct Buffer<B: Backend>(Rc<BufferInner<B>>);
impl<B: Backend> Clone for Buffer<B> {
    fn clone(&self) -> Self {
        Self(self.0.clone())
    }
}
impl<B: Backend> Buffer<B> {
    pub fn id(&self) -> u64 {
        self.0.id
    }
    pub fn size(&self) -> u64 {
        self.0.size
    }
    fn raw(&self) -> &B::Buffer {
        self.0.raw.as_ref().unwrap()
    }
    fn range(&self, offset: u64, size: u64) -> Result<Range<u64>> {
        let end = offset
            .checked_add(size)
            .ok_or(Error::Rejected("buffer range overflow"))?;
        if end > self.size() || !offset.is_multiple_of(4) || !size.is_multiple_of(4) {
            return Err(Error::Rejected("buffer range or alignment"));
        }
        Ok(offset..end)
    }
}

struct Use<B: Backend> {
    buffer: Buffer<B>,
    first: U,
    last: U,
}

pub struct Encoder<B: Backend> {
    id: u64,
    raw: Option<B::CommandEncoder>,
    owner: Rc<Owner<B>>,
    recording: bool,
    uses: BTreeMap<u64, Use<B>>,
    texture_uses: BTreeMap<(u64, u32, u32), texture::TextureUse<B>>,
    holds: BTreeMap<u64, Rc<dyn std::any::Any>>,
    ordered_upload_bytes: u64,
    ordered_staging_bytes: u64,
    ordered_staging: Vec<Buffer<B>>,
    reservations: Option<Rc<reservation::Book<B>>>,
}
impl<B: Backend> Encoder<B> {
    fn new(owner: Rc<Owner<B>>) -> Result<Self> {
        owner.healthy()?;
        let encoder_id = id()?;
        let cached = owner.encoders.borrow_mut().pop();
        let mut raw = if let Some(raw) = cached {
            owner.encoders_reused.set(owner.encoders_reused.get() + 1);
            raw
        } else {
            if owner.native_encoders.get() >= owner.config.max_native_encoders {
                return Err(Error::Rejected("native encoder budget"));
            }
            let raw = unsafe {
                owner
                    .device
                    .create_command_encoder(&hal::CommandEncoderDescriptor {
                        label: Some("native gpu commands"),
                        queue: &owner.queue,
                    })
            }
            .map_err(|e| owner.device_error(e))?;
            owner.native_encoders.set(owner.native_encoders.get() + 1);
            owner.encoders_created.set(owner.encoders_created.get() + 1);
            raw
        };
        if let Err(e) = unsafe { raw.begin_encoding(Some("native gpu commands")) } {
            drop(raw);
            owner.native_encoders.set(owner.native_encoders.get() - 1);
            return Err(owner.device_error(e));
        }
        Ok(Self {
            id: encoder_id,
            owner,
            raw: Some(raw),
            recording: true,
            uses: BTreeMap::new(),
            texture_uses: BTreeMap::new(),
            holds: BTreeMap::new(),
            ordered_upload_bytes: 0,
            ordered_staging_bytes: 0,
            ordered_staging: Vec::new(),
            reservations: None,
        })
    }
    fn check(&self, buffer: &Buffer<B>, usage: U) -> Result<()> {
        self.owner.healthy()?;
        if !Rc::ptr_eq(&self.owner, &buffer.0.owner) {
            return Err(Error::Rejected("different device"));
        }
        if !buffer.0.usage.contains(usage) {
            return Err(Error::Rejected("buffer usage"));
        }
        Ok(())
    }
    fn transition(&mut self, buffer: &Buffer<B>, usage: U) {
        if let Some(state) = self.uses.get_mut(&buffer.id()) {
            if state.last != usage || !U::INCLUSIVE.contains(usage) {
                unsafe {
                    self.raw
                        .as_mut()
                        .unwrap()
                        .transition_buffers(std::iter::once(hal::BufferBarrier {
                            buffer: buffer.raw(),
                            usage: hal::StateTransition {
                                from: state.last,
                                to: usage,
                            },
                        }))
                };
            }
            state.last = usage;
        } else {
            // The true initial state depends on SUBMISSION order. A fixup is
            // emitted immediately before this command buffer at submit time.
            self.uses.insert(
                buffer.id(),
                Use {
                    buffer: buffer.clone(),
                    first: usage,
                    last: usage,
                },
            );
        }
    }
    pub fn clear_buffer(&mut self, buffer: &Buffer<B>, range: Range<u64>) -> Result<()> {
        self.check(buffer, U::COPY_DST)?;
        let size = range
            .end
            .checked_sub(range.start)
            .ok_or(Error::Rejected("inverted range"))?;
        let range = buffer.range(range.start, size)?;
        if size == 0 {
            return Ok(());
        }
        self.transition(buffer, U::COPY_DST);
        unsafe { self.raw.as_mut().unwrap().clear_buffer(buffer.raw(), range) };
        Ok(())
    }
    pub fn copy_buffer(
        &mut self,
        src: &Buffer<B>,
        src_offset: u64,
        dst: &Buffer<B>,
        dst_offset: u64,
        size: u64,
    ) -> Result<()> {
        self.check(src, U::COPY_SRC)?;
        self.check(dst, U::COPY_DST)?;
        src.range(src_offset, size)?;
        dst.range(dst_offset, size)?;
        if src.id() == dst.id() {
            return Err(Error::Rejected("same-buffer copy is unsupported"));
        }
        let Some(size) = NonZeroU64::new(size) else {
            return Ok(());
        };
        self.transition(src, U::COPY_SRC);
        self.transition(dst, U::COPY_DST);
        unsafe {
            self.raw.as_mut().unwrap().copy_buffer_to_buffer(
                src.raw(),
                dst.raw(),
                std::iter::once(hal::BufferCopy {
                    src_offset,
                    dst_offset,
                    size,
                }),
            )
        };
        Ok(())
    }
    pub fn finish(mut self) -> Result<Commands<B>> {
        self.owner.healthy()?;
        if self.reservations.as_ref().is_some_and(|b| !b.is_empty()) {
            return Err(Error::Rejected("unconsumed upload reservation"));
        }
        let watch = B::watch(self.raw.as_ref().unwrap());
        // HAL specifies end_encoding leaves the encoder CLOSED even on error.
        self.recording = false;
        let commands = unsafe { self.raw.as_mut().unwrap().end_encoding() }
            .map_err(|e| self.owner.device_error(e))?;
        #[cfg(feature = "fault-injection")]
        if self.owner.faults.end_failure.replace(false) {
            unsafe {
                self.raw
                    .as_mut()
                    .unwrap()
                    .reset_all(std::iter::once(commands));
            }
            return Err(self.owner.fail_next("injected end_encoding failure".into()));
        }
        Ok(Commands {
            owner: self.owner.clone(),
            encoder: self.raw.take(),
            arena: None,
            commands: Some(commands),
            watch,
            uses: std::mem::take(&mut self.uses),
            texture_uses: std::mem::take(&mut self.texture_uses),
            holds: std::mem::take(&mut self.holds),
            ordered_staging: std::mem::take(&mut self.ordered_staging),
            submitted: false,
            terminal: false,
        })
    }
}
impl<B: Backend> Drop for Encoder<B> {
    fn drop(&mut self) {
        if let Some(book) = &self.reservations {
            book.close();
        }
        if let Some(mut raw) = self.raw.take() {
            if self.recording {
                unsafe { raw.discard_encoding() };
            }
            unsafe {
                raw.reset_all(std::iter::empty());
            }
            self.owner.recycle(raw);
        }
    }
}

/// Consumed by submit, so a command buffer can never be submitted twice.
pub struct Commands<B: Backend> {
    owner: Rc<Owner<B>>,
    encoder: Option<B::CommandEncoder>,
    arena: Option<Rc<fixup::Arena<B>>>,
    commands: Option<B::CommandBuffer>,
    watch: B::Watch,
    uses: BTreeMap<u64, Use<B>>,
    texture_uses: BTreeMap<(u64, u32, u32), texture::TextureUse<B>>,
    holds: BTreeMap<u64, Rc<dyn std::any::Any>>,
    ordered_staging: Vec<Buffer<B>>,
    submitted: bool,
    terminal: bool,
}
impl<B: Backend> Drop for Commands<B> {
    fn drop(&mut self) {
        // Normally Gpu only drops submitted commands after terminal evidence.
        // Unknown device state is quarantined rather than risking GPU use-after-free.
        if self.submitted && !self.terminal {
            std::mem::forget(self.encoder.take());
            std::mem::forget(self.arena.take());
            std::mem::forget(self.commands.take());
            std::mem::forget(std::mem::take(&mut self.uses));
            std::mem::forget(std::mem::take(&mut self.texture_uses));
            std::mem::forget(std::mem::take(&mut self.holds));
            std::mem::forget(std::mem::take(&mut self.ordered_staging));
            std::mem::forget(self.owner.clone());
            return;
        }
        if let Some(arena) = self.arena.take() {
            if let Some(command) = self.commands.take() {
                arena.returned.borrow_mut().push(command);
            }
        }
        if let Some(mut encoder) = self.encoder.take() {
            unsafe { encoder.reset_all(self.commands.take().into_iter()) };
            self.owner.recycle(encoder);
        }
    }
}
/// The app uses panic=unwind and catches renderer failures. Once submission
/// preparation consumes pending work, a caught panic must not leave this owner
/// reusable with unpublished/partially prepared state. Any uncertain flight
/// stays owned and is quarantined by the existing GPU destruction path.
struct SubmissionUnwind<B: Backend> {
    owner: Rc<Owner<B>>,
}
impl<B: Backend> Drop for SubmissionUnwind<B> {
    fn drop(&mut self) {
        if std::thread::panicking() {
            let _ = self
                .owner
                .fail_next("panic during submission preparation or queue call".into());
        }
    }
}

struct InFlight<B: Backend> {
    serial: Serial,
    commands: Vec<Commands<B>>,
    staging: Vec<Buffer<B>>,
    observed: bool,
    retire: bool,
}

pub struct Gpu<B: Backend = backend::Platform> {
    owner: Rc<Owner<B>>,
    fence: Option<B::Fence>,
    pending: Option<Encoder<B>>,
    pending_upload_bytes: u64,
    zero_buffer: Option<Buffer<B>>,
    staging_free: Vec<Buffer<B>>,
    pending_staging: Vec<Buffer<B>>,
    staging_cached_bytes: u64,
    staging_created: u64,
    staging_reused: u64,
    staging_waits: u64,
    flights: Vec<InFlight<B>>,
    timeline: Rc<RefCell<Timeline>>,
    pub adapter_name: String,
    pub adapter_info: types::AdapterInfo,
}
impl<B: Backend> Gpu<B> {
    pub fn open() -> Result<Self> {
        Self::open_with_config(Config::default())
    }
    /// Enabled device limits, not the adapter's potentially larger limits.
    pub fn limits(&self) -> &types::Limits { &self.owner.limits }
    pub fn features(&self) -> types::Features {
        self.owner.features
    }
    /// Remaining space in the bounded queue-prefix upload batch. A facade may
    /// submit that prefix and continue, preserving write order without a wait.
    pub fn pending_upload_remaining(&self) -> u64 {
        self.owner.config.max_pending_upload_bytes.saturating_sub(self.pending_upload_bytes)
    }
    pub fn open_with_config(config: Config) -> Result<Self> {
        let implemented = types::Features::TEXTURE_COMPRESSION_BC
            | types::Features::TEXTURE_COMPRESSION_BC_SLICED_3D
            | types::Features::FLOAT32_FILTERABLE
            | types::Features::TEXTURE_FORMAT_16BIT_NORM
            | types::Features::DEPTH32FLOAT_STENCIL8;
        if !implemented.contains(config.optional_texture_features) {
            return Err(Error::Rejected("optional feature contract not implemented"));
        }
        if config.max_in_flight == 0
            || config.max_native_encoders == 0
            || config.retained_encoders > config.max_native_encoders
            || config.backpressure_timeout.is_zero()
            || config.retained_staging_bytes > config.max_staging_buffer_bytes
        {
            return Err(Error::Rejected("native backend configuration"));
        }
        let instance = unsafe {
            B::Instance::init(&hal::InstanceDescriptor {
                name: "Skyrim native GPU development",
                flags: if config.validation {
                    types::InstanceFlags::VALIDATION | types::InstanceFlags::DEBUG
                } else {
                    types::InstanceFlags::empty()
                },
                memory_budget_thresholds: Default::default(),
                backend_options: Default::default(),
                telemetry: None,
                display: None,
            })
        }
        .map_err(backend_error)?;
        let exposed = unsafe { instance.enumerate_adapters(None) }
            .into_iter()
            .next()
            .ok_or(Error::Rejected("no native backend adapter"))?;
        let limits = if config.adapter_limits { exposed.capabilities.limits.clone() } else { types::Limits::default() };
        if !limits.check_limits(&exposed.capabilities.limits) {
            return Err(Error::Rejected("adapter limits"));
        }
        let mut features = config.optional_texture_features & exposed.features;
        if !features.contains(types::Features::TEXTURE_COMPRESSION_BC) {
            features.remove(types::Features::TEXTURE_COMPRESSION_BC_SLICED_3D);
        }
        let opened = unsafe {
            exposed
                .adapter
                .open(features, &limits, &types::MemoryHints::MemoryUsage)
        }
        .map_err(backend_error)?;
        let fence = unsafe { opened.device.create_fence() }.map_err(backend_error)?;
        let timeline = Rc::new(RefCell::new(Timeline::default()));
        Ok(Self {
            owner: Rc::new(Owner {
                encoders: RefCell::new(Vec::new()),
                group_layouts: RefCell::new(Vec::new()),
                device: opened.device,
                queue: opened.queue,
                _adapter: exposed.adapter,
                _instance: instance,
                limits,
                features,
                live_buffers: Cell::new(0),
                timeline: timeline.clone(),
                config,
                native_encoders: Cell::new(0),
                encoders_created: Cell::new(0),
                encoders_reused: Cell::new(0),
                #[cfg(feature = "fault-injection")]
                faults: Faults::default(),
                live_buffer_bytes: Cell::new(0),
                texture_bytes: Cell::new(0),
                texture_objects: Cell::new(0),
                staging_bytes: Cell::new(0),
                downlevel: exposed.capabilities.downlevel.flags,
            }),
            fence: Some(fence),
            pending: None,
            pending_upload_bytes: 0,
            zero_buffer: None,
            staging_free: Vec::new(),
            pending_staging: Vec::new(),
            staging_cached_bytes: 0,
            staging_created: 0,
            staging_reused: 0,
            staging_waits: 0,
            flights: Vec::new(),
            timeline,
            adapter_name: exposed.info.name.clone(),
            adapter_info: exposed.info,
        })
    }
    fn raw_buffer(&self, label: &str, size: u64, usage: U, staging: bool) -> Result<Buffer<B>> {
        self.owner.healthy()?;
        if size == 0 || size > self.owner.limits.max_buffer_size || !size.is_multiple_of(4) {
            return Err(Error::Rejected("buffer size"));
        }
        // Conservative development budget, not a claim about device memory limits.
        let current = if staging {
            &self.owner.staging_bytes
        } else {
            &self.owner.live_buffer_bytes
        };
        let limit = if staging {
            self.owner.config.max_staging_buffer_bytes
        } else {
            self.owner.config.max_resident_buffer_bytes
        };
        let resident = current
            .get()
            .checked_add(size)
            .filter(|&bytes| bytes <= limit)
            .ok_or(Error::Rejected(if staging {
                "staging buffer budget"
            } else {
                "resident buffer budget"
            }))?;
        if self.owner.live_buffers.get() >= self.owner.config.max_buffer_objects {
            return Err(Error::Rejected("buffer object budget"));
        }
        let id = id()?;
        let raw = unsafe {
            self.owner.device.create_buffer(&hal::BufferDescriptor {
                label: Some(label),
                size,
                usage,
                memory_flags: hal::MemoryFlags::empty(),
            })
        }
        .map_err(|e| self.owner.device_error(e))?;
        self.owner
            .live_buffers
            .set(self.owner.live_buffers.get() + 1);
        current.set(resident);
        Ok(Buffer(Rc::new(BufferInner {
            owner: self.owner.clone(),
            raw: Some(raw),
            id,
            size,
            usage,
            state: Cell::new(U::empty()),
            staging,
        })))
    }
    fn pending(&mut self) -> Result<&mut Encoder<B>> {
        if self.pending.is_none() {
            self.pending = Some(self.acquire_encoder()?);
        }
        Ok(self.pending.as_mut().unwrap())
    }
    fn validate_buffer_usage(usage: U) -> Result<()> {
        let allowed = U::MAP_READ
            | U::COPY_SRC
            | U::COPY_DST
            | U::STORAGE_READ_ONLY
            | U::STORAGE_READ_WRITE
            | U::UNIFORM
            | U::INDEX;
        if usage.is_empty() || !allowed.contains(usage) {
            return Err(Error::Rejected("buffer usage not implemented"));
        }
        if usage.contains(U::MAP_READ) && !(U::MAP_READ | U::COPY_DST).contains(usage) {
            return Err(Error::Rejected("MAP_READ requires readback-only usage"));
        }
        Ok(())
    }
    pub fn create_buffer(&mut self, label: &str, size: u64, usage: U) -> Result<Buffer<B>> {
        Self::validate_buffer_usage(usage)?;
        let mut buffer = self.raw_buffer(label, size, usage | U::COPY_DST, false)?;
        Rc::get_mut(&mut buffer.0).unwrap().usage = usage;
        // Initialization may use the internally added COPY_DST flag, but it
        // must not grant the caller write permissions absent from its descriptor.
        let pending = self.pending()?;
        pending.transition(&buffer, U::COPY_DST);
        unsafe {
            pending
                .raw
                .as_mut()
                .unwrap()
                .clear_buffer(buffer.raw(), 0..size)
        };
        Ok(buffer)
    }
    /// Exact-size, fully initialized upload. Every byte is copied by the pending
    /// queue before submitted commands may consume it, so a preceding GPU clear
    /// adds no initialization coverage. Partial uploads still use create_buffer.
    /// COPY_DST is explicit; this never grants an undeclared usage to a caller.
    pub fn create_buffer_init(&mut self, label: &str, data: &[u8], usage: U) -> Result<Buffer<B>> {
        Self::validate_buffer_usage(usage)?;
        if !usage.contains(U::COPY_DST) || data.is_empty() || data.len() % 4 != 0 {
            return Err(Error::Rejected(
                "initialized buffer requires complete aligned COPY_DST data",
            ));
        }
        let buffer = self.raw_buffer(label, data.len() as u64, usage, false)?;
        self.write_buffer(&buffer, 0, data)?;
        Ok(buffer)
    }
    fn acquire_encoder(&mut self) -> Result<Encoder<B>> {
        let deadline = Instant::now()
            .checked_add(self.owner.config.backpressure_timeout)
            .ok_or(Error::Rejected("deadline overflow"))?;
        loop {
            match Encoder::new(self.owner.clone()) {
                Err(Error::Rejected("native encoder budget")) if !self.flights.is_empty() => {
                    self.poll()?;
                    if Instant::now() >= deadline {
                        return Err(Error::Timeout(
                            self.flights
                                .first()
                                .map_or(self.verified_serial(), |f| f.serial),
                        ));
                    }
                    std::thread::sleep(Duration::from_millis(1));
                }
                result => return result,
            }
        }
    }
    pub fn encoder(&mut self) -> Result<Encoder<B>> {
        self.acquire_encoder()
    }
    pub fn write_buffer(
        &mut self,
        destination: &Buffer<B>,
        offset: u64,
        data: &[u8],
    ) -> Result<()> {
        self.owner.healthy()?;
        if !Rc::ptr_eq(&self.owner, &destination.0.owner)
            || !destination.0.usage.contains(U::COPY_DST)
        {
            return Err(Error::Rejected("write device or usage"));
        }
        destination.range(offset, data.len() as u64)?;
        if data.is_empty() {
            return Ok(());
        }
        let pending_bytes = self
            .pending_upload_bytes
            .checked_add(data.len() as u64)
            .filter(|&bytes| bytes <= self.owner.config.max_pending_upload_bytes)
            .ok_or(Error::Rejected("pending upload budget; submit first"))?;
        // The lease is inaccessible to the caller. Reuse is allowed only after
        // ALL referencing command buffers and the signaling fence have retired.
        let staging = self.take_upload(data.len() as u64)?;
        let range = 0..staging.size();
        let mapped = unsafe { self.owner.device.map_buffer(staging.raw(), range.clone()) }
            .map_err(|e| self.owner.device_error(e))?;
        unsafe {
            std::ptr::copy_nonoverlapping(data.as_ptr(), mapped.ptr.as_ptr(), data.len());
            if !mapped.is_coherent {
                self.owner
                    .device
                    .flush_mapped_ranges(staging.raw(), std::iter::once(range));
            }
            self.owner.device.unmap_buffer(staging.raw());
        }
        staging.0.state.set(U::MAP_WRITE);
        self.pending()?
            .copy_buffer(&staging, 0, destination, offset, data.len() as u64)?;
        self.pending_staging.push(staging);
        self.pending_upload_bytes = pending_bytes;
        Ok(())
    }
    pub fn submit(&mut self, commands: Vec<Commands<B>>) -> Result<Serial> {
        self.owner.healthy()?;
        if commands.iter().any(|c| !Rc::ptr_eq(&self.owner, &c.owner)) {
            return Err(Error::Rejected("command buffer from another device"));
        }
        // Capacity means RETIRED, not merely successful. Never consume pending
        // uploads, or silently discard the caller's work on a retryable timeout.
        let deadline = Instant::now()
            .checked_add(self.owner.config.backpressure_timeout)
            .ok_or(Error::Rejected("deadline overflow"))?;
        while self.flights.len() >= self.owner.config.max_in_flight {
            self.poll()?;
            if self.flights.len() < self.owner.config.max_in_flight {
                break;
            }
            if Instant::now() >= deadline {
                return Err(self
                    .owner
                    .fail_next("backpressure retirement deadline".into()));
            }
            std::thread::sleep(Duration::from_millis(1));
        }
        let needs_fixups = B::FIXUP_BARRIERS
            && (self.pending.is_some()
                || commands
                    .iter()
                    .any(|c| !c.uses.is_empty() || !c.texture_uses.is_empty()));
        // Reserve one shared fixup allocation before consuming uploads. It can
        // produce arbitrarily many bounded CBs without one pool per barrier phase.
        let fixups = if needs_fixups {
            loop {
                match fixup::Fixups::new(self.owner.clone()) {
                    Ok(f) => break Some(f),
                    Err(Error::Rejected("native encoder budget")) if !self.flights.is_empty() => {
                        self.poll()?;
                        if Instant::now() >= deadline {
                            return Err(self.owner.fail_next("encoder retirement deadline".into()));
                        }
                        std::thread::sleep(Duration::from_millis(1));
                    }
                    Err(e) => return Err(e),
                }
            }
        } else {
            None
        };
        let _unwind = SubmissionUnwind {
            owner: self.owner.clone(),
        };
        let result = self.submit_inner(commands, fixups);
        // From this point pending uploads may have been consumed. EVERY error
        // is fail-stop, not a recoverable rejection with silently lost writes.
        match result {
            Ok(serial) => Ok(serial),
            Err(e) => Err(self.owner.fail_next(format!("submit preparation: {e:?}"))),
        }
    }
    fn submit_inner(
        &mut self,
        commands: Vec<Commands<B>>,
        fixups: Option<fixup::Fixups<B>>,
    ) -> Result<Serial> {
        let mut ordered = Vec::new();
        if let Some(pending) = self.pending.take() {
            ordered.push(pending.finish()?);
        }
        self.pending_upload_bytes = 0;
        ordered.extend(commands);
        if ordered.is_empty() {
            ordered.push(self.encoder()?.finish()?);
        }
        let mut submit = Vec::new();
        // Plan transitions transactionally. Recording order cannot decide a
        // command buffer's initial state, and a rejected plan publishes none.
        let mut final_states: BTreeMap<u64, (Buffer<B>, U)> = BTreeMap::new();
        let mut final_textures: BTreeMap<
            (u64, u32, u32),
            (texture::Texture<B>, types::TextureUses),
        > = BTreeMap::new();
        for command in ordered {
            if let Some(fixups) = &fixups {
                let mut buffers = BTreeMap::new();
                let mut textures = BTreeMap::new();
                for (&id, usage) in &command.uses {
                    let from = final_states
                        .get(&id)
                        .map_or(usage.buffer.0.state.get(), |(_, s)| *s);
                    if from != usage.first || !U::INCLUSIVE.contains(from) {
                        buffers.insert(
                            id,
                            Use {
                                buffer: usage.buffer.clone(),
                                first: from,
                                last: usage.first,
                            },
                        );
                    }
                }
                for (&key @ (_, mip, layer), usage) in &command.texture_uses {
                    let from = final_textures
                        .get(&key)
                        .map_or(usage.texture.state(mip, layer), |(_, s)| *s);
                    if from != usage.first || !types::TextureUses::INCLUSIVE.contains(from) {
                        textures.insert(
                            key,
                            texture::TextureUse {
                                texture: usage.texture.clone(),
                                first: from,
                                last: usage.first,
                            },
                        );
                    }
                }
                if !buffers.is_empty() || !textures.is_empty() {
                    submit.push(fixups.encode(buffers, textures)?);
                }
            }
            for (&id, usage) in &command.uses {
                final_states.insert(id, (usage.buffer.clone(), usage.last));
            }
            for (&key, usage) in &command.texture_uses {
                final_textures.insert(key, (usage.texture.clone(), usage.last));
            }
            submit.push(command);
        }
        let serial = self.timeline.borrow_mut().issue()?;
        let mut staging = std::mem::take(&mut self.pending_staging);
        for command in &mut submit {
            staging.append(&mut command.ordered_staging);
            command.submitted = true;
        }
        // Register ownership BEFORE the unsafe submit. Even unwinding or an
        // ambiguous partial submission must leave the fence/resources retained.
        self.flights.push(InFlight {
            serial,
            commands: submit,
            staging,
            observed: false,
            retire: false,
        });
        #[cfg(feature = "fault-injection")]
        if self.owner.faults.submit_panic.replace(false) {
            panic!("injected unwind after flight registration");
        }
        let raw: Vec<_> = self
            .flights
            .last()
            .unwrap()
            .commands
            .iter()
            .map(|c| c.commands.as_ref().unwrap())
            .collect();
        // SAFETY: single queue, owned encoder allocations and all referenced
        // resources are retained until terminal; fence outlives submissions.
        #[cfg(feature = "fault-injection")]
        if self.owner.faults.submit_failure.replace(false) {
            self.timeline.borrow_mut().fail(
                serial,
                "injected submit failure; ownership quarantined".into(),
            );
            return self.owner.healthy().map(|_| serial);
        }
        let result = unsafe {
            self.owner
                .queue
                .submit(&raw, &[], (self.fence.as_mut().unwrap(), serial))
        };
        if let Err(error) = result {
            self.timeline
                .borrow_mut()
                .fail(serial, format!("submit {error:?}"));
            return self.owner.healthy().map(|_| serial);
        }
        for (_, (buffer, state)) in final_states {
            buffer.0.state.set(state);
        }
        for ((_, mip, layer), (texture, state)) in final_textures {
            texture.publish(mip, layer, state);
        }
        Ok(serial)
    }
    pub fn poll(&mut self) -> Result<Serial> {
        let native_fence = unsafe {
            self.owner
                .device
                .get_fence_value(self.fence.as_ref().unwrap())
        };
        #[cfg(feature = "fault-injection")]
        let native_fence = if self.owner.faults.fence_error.get() {
            Err(hal::DeviceError::Lost)
        } else {
            native_fence
        };
        let fence_value = match native_fence {
            Ok(value) => value,
            Err(error) => {
                let serial = self
                    .flights
                    .first()
                    .map_or(self.timeline.borrow().issued().saturating_add(1), |f| {
                        f.serial
                    });
                self.timeline
                    .borrow_mut()
                    .fail(serial, format!("fence {error:?}"));
                0
            }
        };
        #[cfg(feature = "fault-injection")]
        let fence_value = self
            .owner
            .faults
            .fence_cap
            .get()
            .map_or(fence_value, |cap| fence_value.min(cap));
        for flight in &mut self.flights {
            if flight.observed {
                flight.retire = fence_value >= flight.serial;
                continue;
            }
            let mut terminal = true;
            let mut error = None;
            for command in &mut flight.commands {
                match B::completion(&command.watch, fence_value >= flight.serial) {
                    Completion::Pending => terminal = false,
                    Completion::Success => command.terminal = true,
                    Completion::Failure(message) => {
                        command.terminal = true;
                        error = Some(message);
                    }
                }
            }
            #[cfg(feature = "fault-injection")]
            if terminal && self.owner.faults.completion_failure.get() == Some(flight.serial) {
                error = Some("injected terminal failure".into());
            }
            if let Some(message) = &error {
                self.timeline
                    .borrow_mut()
                    .fail(flight.serial, message.clone());
            }
            if terminal {
                flight.observed = true;
                // A later failure must not erase an earlier genuinely successful
                // prefix. Classify this submission by its own command buffers.
                let result = error.map_or(Completion::Success, Completion::Failure);
                self.timeline.borrow_mut().observe(flight.serial, result);
                // Keep the HAL fence alive until its signaling callback is also
                // observed. Terminal Metal status alone doesn't prove that.
                flight.retire = fence_value >= flight.serial;
            }
        }
        let mut index = 0;
        while index < self.flights.len() {
            if !self.flights[index].retire {
                index += 1;
                continue;
            }
            let mut flight = self.flights.remove(index);
            // Drop/reset all native commands BEFORE making upload memory writable.
            flight.commands.clear();
            for buffer in flight.staging.drain(..) {
                self.recycle_upload(buffer);
            }
        }
        self.owner.healthy()?;
        Ok(self.timeline.borrow().verified())
    }
    pub fn wait(&mut self, serial: Serial, timeout: Duration) -> Result<()> {
        if self.timeline.borrow().status(serial)? {
            return Ok(());
        }
        let deadline = Instant::now()
            .checked_add(timeout)
            .ok_or(Error::Rejected("deadline overflow"))?;
        loop {
            let _ = self.poll();
            if self.timeline.borrow().status(serial)? {
                return Ok(());
            }
            if Instant::now() >= deadline {
                return Err(Error::Timeout(serial));
            }
            std::thread::sleep(Duration::from_millis(1));
        }
    }
    pub fn read_buffer(
        &mut self,
        buffer: &Buffer<B>,
        range: Range<u64>,
        timeout: Duration,
    ) -> Result<Vec<u8>> {
        self.owner.healthy()?;
        let size = range
            .end
            .checked_sub(range.start)
            .ok_or(Error::Rejected("inverted range"))?;
        buffer.range(range.start, size)?;
        let mut encoder = self.encoder()?;
        encoder.check(buffer, U::MAP_READ)?;
        if size == 0 {
            return Ok(Vec::new());
        }
        // Explicit host-read transition follows all pending uploads and GPU work.
        encoder.transition(buffer, U::MAP_READ);
        let serial = self.submit(vec![encoder.finish()?])?;
        self.wait(serial, timeout)?;
        let mapped = unsafe { self.owner.device.map_buffer(buffer.raw(), 0..buffer.size()) }
            .map_err(|e| self.owner.device_error(e))?;
        let mut result = vec![0; size as usize];
        unsafe {
            if !mapped.is_coherent {
                self.owner
                    .device
                    .invalidate_mapped_ranges(buffer.raw(), std::iter::once(0..buffer.size()));
            }
            std::ptr::copy_nonoverlapping(
                mapped.ptr.as_ptr().add(range.start as usize),
                result.as_mut_ptr(),
                result.len(),
            );
            self.owner.device.unmap_buffer(buffer.raw());
        }
        Ok(result)
    }
    pub fn verified_serial(&self) -> Serial {
        self.timeline.borrow().verified()
    }
    pub fn live_buffer_count(&self) -> u64 {
        self.owner.live_buffers.get()
    }
    pub fn encoder_stats(&self) -> (u64, u64, usize) {
        (
            self.owner.encoders_created.get(),
            self.owner.encoders_reused.get(),
            self.owner.encoders.borrow().len(),
        )
    }
    pub fn native_counters(&self) -> types::HalCounters {
        self.owner.device.get_internal_counters()
    }
    pub fn trim_encoder_pool(&self) {
        let count = self.owner.encoders.borrow().len();
        self.owner.encoders.borrow_mut().clear();
        self.owner
            .native_encoders
            .set(self.owner.native_encoders.get() - count);
    }
}

// The feature is off in ordinary builds. Faults may only delay completion or
// change success to failure; they cannot manufacture successful GPU execution.
#[cfg(feature = "fault-injection")]
impl<B: Backend> Gpu<B> {
    pub fn delay_fence_observation(&self, cap: Option<Serial>) {
        self.owner.faults.fence_cap.set(cap);
    }
    pub fn fail_completion(&self, serial: Serial) {
        self.owner.faults.completion_failure.set(Some(serial));
    }
    /// Test-only panic after ownership registration, before calling the queue.
    pub fn unwind_next_submit(&self) {
        self.owner.faults.submit_panic.set(true);
    }
    pub fn fail_next_submit(&self) {
        self.owner.faults.submit_failure.set(true);
    }
    pub fn fail_next_end_encoding(&self) {
        self.owner.faults.end_failure.set(true);
    }
    pub fn fail_fence_observation(&self, enabled: bool) {
        self.owner.faults.fence_error.set(enabled);
    }
    pub fn in_flight_count(&self) -> usize {
        self.flights.len()
    }
    pub fn test_resolve_initialization(&self, poison: bool) {
        self.owner.faults.poison_resolve.set(poison);
    }
    #[cfg(feature = "fault-injection")]
    pub fn test_texture_initialization(&self, poison: bool, skip_last: bool) {
        self.owner.faults.poison_textures.set(poison);
        self.owner.faults.skip_last_texture_zero.set(skip_last);
    }

    /// Records a deliberately missing synchronization barrier and DISCARDS it.
    /// No invalid command buffer is ever submitted to the driver. The test
    /// logger must observe the expected validation diagnostic during recording.
    #[cfg(not(target_vendor = "apple"))]
    pub fn synchronization_validation_canary(&mut self) -> Result<()> {
        let buffer = self.create_buffer("discarded validation canary", 64, U::COPY_DST)?;
        let mut encoder = self.encoder()?;
        encoder.clear_buffer(&buffer, 0..64)?;
        unsafe {
            encoder
                .raw
                .as_mut()
                .unwrap()
                .clear_buffer(buffer.raw(), 0..64);
        }
        drop(encoder);
        Ok(())
    }
}
impl<B: Backend> Drop for Gpu<B> {
    fn drop(&mut self) {
        self.pending.take();
        let deadline = Instant::now() + self.owner.config.drop_timeout;
        while !self.flights.is_empty() && Instant::now() < deadline {
            let _ = self.poll();
            if !self.flights.is_empty() {
                std::thread::sleep(Duration::from_millis(1));
            }
        }
        if self.flights.is_empty() {
            if let Some(fence) = self.fence.take() {
                unsafe { self.owner.device.destroy_fence(fence) };
            }
        } else {
            // Unknown GPU state cannot justify freeing live GPU references.
            // Fail-stop quarantine; normal completion always reclaims them.
            log::warn!(
                "native GPU quarantines {} unretired submissions",
                self.flights.len()
            );
            std::mem::forget(std::mem::take(&mut self.flights));
            std::mem::forget(self.fence.take());
            std::mem::forget(self.owner.clone());
        }
    }
}

// One shared public protocol; no duplicate native interpretation of draw packets.
pub use xenos_gfx::sink;
