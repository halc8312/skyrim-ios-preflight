//! Native renderer thread. Presents each swap in a window when the host gives it one, and
//! otherwise renders offscreen; either way frames can be written out as PNG files.

use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::time::Duration;

use gpu_webgpu::*;

#[path = "native_frame_stability.rs"]
mod frame_stability;
#[path = "native_performance.rs"]
mod performance;
pub use performance::NativePerformanceSnapshot;

pub use crate::presentation_probe::{PresentationProbe, PresentationProbeError, run_public_presentation_probe};
use crate::renderer::{Renderer, TargetsFactory, wanted_features};
use crate::settings::{PresentAdmission, PresentationSettings, SettingsControl, SettingsSnapshot};
use crate::{PhysicalMemory, RendererHandle};

#[derive(Debug, Clone, Default)]
pub struct NativeOptions {
    /// Write every `dump_every`-th presented frame to `<dir>/frame_NNNNNN.png`.
    pub dump_dir: Option<PathBuf>,
    pub dump_every: u64,
    /// Present in this window.
    pub window: Option<Window>,
}

/// A window to present in: its window-system handles (addresses, so the renderer thread can
/// take them) and its size in pixels, which the host updates when the window is resized.
#[derive(Debug, Clone)]
pub struct Window {
    pub handles: WindowHandles,
    /// Width in the high 32 bits, height in the low ones.
    pub size: Arc<AtomicU64>,
}

#[derive(Debug, Clone, Copy)]
pub enum WindowHandles {
    Xlib {
        display: usize,
        window: u64,
    },
    Xcb {
        connection: usize,
        window: u32,
    },
    Wayland {
        display: usize,
        surface: usize,
    },
    /// Borrowed CAMetalLayer created by the Apple host; no retain/release here.
    MetalLayer(MetalLayerHandle),
}

/// Opaque borrowed Objective-C CAMetalLayer. Construction is unsafe because a
/// numeric address cannot establish the Objective-C type, ownership or lifetime.
#[derive(Debug, Clone, Copy)]
pub struct MetalLayerHandle(std::num::NonZeroUsize);

impl MetalLayerHandle {
    /// Null is rejected. This does not retain the layer or validate its class.
    ///
    /// # Safety
    /// A non-null pointer must be a real CAMetalLayer on macOS/iOS. The host must
    /// own a strong reference until the renderer thread and its WebGPU Surface
    /// have been destroyed, including when its view is detached or backgrounded.
    /// Create/configure UIKit/AppKit ownership on the main thread; synchronize
    /// layer/device/drawable-size changes with rendering. The pointer must not be
    /// moved to another object or released while any copy of this handle is used.
    pub unsafe fn from_raw(layer: *mut std::ffi::c_void) -> Option<Self> {
        std::num::NonZeroUsize::new(layer as usize).map(Self)
    }

    pub fn as_raw(self) -> *mut std::ffi::c_void {
        self.0.get() as *mut std::ffi::c_void
    }
}

impl Window {
    pub fn pack_size(width: u32, height: u32) -> u64 {
        (u64::from(width) << 32) | u64::from(height)
    }

    fn size(&self) -> (u32, u32) {
        let v = self.size.load(Ordering::Relaxed);
        ((v >> 32) as u32, v as u32)
    }
}

/// The window's surface and how it is configured.
struct Presentation {
    causal: Option<Arc<crate::causal::Session>>,
    window: Window,
    surface: Surface,
    format: TextureFormat,
    present_mode: PresentMode,
    configured: (u32, u32),
    max_dimension: u32,
    configures: u64,
}

impl Presentation {
    fn configure(&mut self, device: &Device, settings: PresentationSettings) {
        let _phase = crate::causal::Guard::enter(&self.causal, crate::causal::Stage::Configure);
        self.configures = self.configures.saturating_add(1);
        let (width, height) = self.window.size();
        let size = settings.output_size(width, height, self.max_dimension);
        self.configured = (size.0.max(1), size.1.max(1));
        self.surface.configure(
            device,
            &SurfaceConfiguration {
                format: self.format,
                usage: TextureUsage::RenderAttachment,
                width: self.configured.0,
                height: self.configured.1,
                view_formats: &[],
                alpha_mode: CompositeAlphaMode::Opaque,
                present_mode: self.present_mode,
            },
        );
    }

    /// Shows the presenter's current frame, reconfiguring first if the window was resized.
    fn present(
        &mut self,
        device: &Device,
        presenter: &crate::present::Presenter,
        settings: PresentationSettings,
    ) -> bool {
        let (width, height) = self.window.size();
        let size = settings.output_size(width, height, self.max_dimension);
        if size.0 == 0 || size.1 == 0 {
            return false; // Minimized.
        }
        if size != self.configured {
            self.configure(device, settings);
        }
        let acquired = {
            let _phase = crate::causal::Guard::enter(&self.causal, crate::causal::Stage::Acquire);
            self.surface.current_frame()
        };
        let frame = match acquired {
            Ok(frame) => frame,
            Err(Error::SurfaceStatus(status)) => {
                // A temporary timeout does not invalidate the configuration.
                // Reconfiguration itself may wait for GPU work to retire.
                if surface_needs_reconfigure(status) {
                    self.configure(device, settings);
                }
                return false;
            }
            Err(e) => {
                log::warn!("renderer: no window frame: {e}");
                return false;
            }
        };
        let view = frame.texture.create_view(None);
        let mut encoder = device.create_command_encoder("present");
        {
            let _phase = crate::causal::Guard::enter(&self.causal, crate::causal::Stage::PresentEncode);
            presenter.encode(&mut encoder, &view, self.configured.0, self.configured.1);
        }
        let commands=encoder.finish("present");
        #[cfg(not(feature="native-metal"))]
        {
            let _phase = crate::causal::Guard::enter(&self.causal, crate::causal::Stage::PresentSubmit);
            device.queue().submit([commands]);
        }
        let result = {
            let _phase = crate::causal::Guard::enter(&self.causal, crate::causal::Stage::PresentCall);
            #[cfg(feature="native-metal")]
            { frame.present_with(commands) }
            #[cfg(not(feature="native-metal"))]
            { frame.present() }
        };
        match result {
            Ok(()) => true,
            Err(error) => {
                log::warn!("renderer: presenting window frame failed: {error}");
                false
            }
        }
    }
}

fn surface_needs_reconfigure(status: gpu_webgpu::ffi::WGPUSurfaceGetCurrentTextureStatus) -> bool {
    use gpu_webgpu::ffi::WGPUSurfaceGetCurrentTextureStatus as Status;
    matches!(status, Status::Outdated | Status::Lost)
}

#[cfg(test)]
mod surface_recovery_tests {
    use super::*;
    use gpu_webgpu::ffi::WGPUSurfaceGetCurrentTextureStatus as Status;
    #[test]
    fn only_lost_or_outdated_acquisition_forces_reconfiguration() {
        for status in [Status::Outdated, Status::Lost] { assert!(surface_needs_reconfigure(status)); }
        for status in [Status::Timeout, Status::Error, Status::SuccessOptimal, Status::SuccessSuboptimal, Status(99)] {
            assert!(!surface_needs_reconfigure(status));
        }
    }
}

/// Starts the renderer thread. If no WebGPU device can be created the error is logged and
/// the handle never blocks (submitted work is dropped).
pub fn start(
    memory: Arc<dyn PhysicalMemory>,
    options: NativeOptions,
    targets: TargetsFactory,
) -> std::io::Result<RendererHandle> {
    spawn_renderer(memory, options, targets, None, false).map(|(handle, _)| handle)
}

/// Atomic diagnostic counters and a nonblocking coarse performance observation.
/// A decoded swap or a written PNG does not prove
/// that the title is playable, or that its draws produced the intended image.
#[derive(Clone, Default)]
pub struct NativeDiagnostics(Arc<NativeProgress>);

#[derive(Default)]
struct NativeProgress {
    causal: crate::causal::Control,
    performance: performance::PerformanceStore,
    settings: SettingsControl,
    applied_settings: SettingsControl,
    presented_frames: AtomicU64,
    skipped_frames: AtomicU64,
    presentation_size: AtomicU64,
    source_size: AtomicU64,
    initialized: AtomicBool,
    stopped: AtomicBool,
    failed: AtomicBool,
    cancelled: AtomicBool,
    swap_requests: AtomicU64,
    decoded_frames: AtomicU64,
    reused_frames: AtomicU64,
    unrendered_swaps: AtomicU64,
    png_written: AtomicU64,
    png_failures: AtomicU64,
    gpu_errors: AtomicU64,
    first_png_swap: AtomicU64,
}

#[derive(Debug, Clone, Copy, Default)]
pub struct NativeSnapshot {
    pub initialized: bool,
    pub stopped: bool,
    pub failed: bool,
    pub swap_requests: u64,
    pub decoded_frames: u64,
    pub reused_frames: u64,
    pub unrendered_swaps: u64,
    pub png_written: u64,
    pub png_failures: u64,
    pub gpu_errors: u64,
    pub first_png_swap: u64,
}

impl NativeDiagnostics {
    pub fn begin_causal_session(&self, id: u64, origin: std::time::Instant) -> bool {
        self.0.causal.start(id, origin)
    }

    pub fn causal_session(&self) -> Option<Arc<crate::causal::Session>> { self.0.causal.session() }

    /// Latest completed five-second report, with explicit age/staleness. Missing,
    /// contended or poisoned storage returns None; never waits for the renderer.
    pub fn performance_snapshot(&self) -> Option<NativePerformanceSnapshot> {
        self.0.performance.snapshot_at(std::time::Instant::now())
    }

    /// Atomically publish a complete validated tuple; applied on a later valid
    /// swap. Does not change guest simulation, draws, resolves, or sampler policy.
    pub fn set_presentation_settings(&self, settings: PresentationSettings) -> u32 {
        self.0.settings.set(settings)
    }
    pub fn presentation_status(&self) -> PresentationStatus {
        let size = self.0.presentation_size.load(Ordering::Acquire);
        let source = self.0.source_size.load(Ordering::Acquire);
        // Applied is published only after reading requested. Read it first so
        // the following requested load cannot precede that causal publication.
        let applied = self.0.applied_settings.snapshot();
        let requested = self.0.settings.snapshot();
        PresentationStatus {
            requested,
            applied,
            presented_frames: self.0.presented_frames.load(Ordering::Acquire),
            skipped_frames: self.0.skipped_frames.load(Ordering::Acquire),
            output_width: (size >> 32) as u32,
            output_height: size as u32,
            source_width: (source >> 32) as u32,
            source_height: source as u32,
        }
    }
    pub fn snapshot(&self) -> NativeSnapshot {
        let p = &self.0;
        NativeSnapshot {
            initialized: p.initialized.load(Ordering::Acquire),
            stopped: p.stopped.load(Ordering::Acquire),
            failed: p.failed.load(Ordering::Acquire),
            swap_requests: p.swap_requests.load(Ordering::Acquire),
            decoded_frames: p.decoded_frames.load(Ordering::Acquire),
            reused_frames: p.reused_frames.load(Ordering::Acquire),
            unrendered_swaps: p.unrendered_swaps.load(Ordering::Acquire),
            png_written: p.png_written.load(Ordering::Acquire),
            png_failures: p.png_failures.load(Ordering::Acquire),
            gpu_errors: p.gpu_errors.load(Ordering::Acquire),
            first_png_swap: p.first_png_swap.load(Ordering::Acquire),
        }
    }
}

/// Counts surface present submissions, not display scanout or guest-draw FPS.
/// Counter/extent observations are diagnostic snapshots, not a joint transaction.
#[derive(Debug, Clone, Copy, Default)]
pub struct PresentationStatus {
    pub requested: SettingsSnapshot,
    /// Renderer consumed the tuple at a valid swap. A capped/minimized/lost
    /// surface may not have submitted it yet; actual extent and submission
    /// counters below are separate evidence, never display-scanout proof.
    pub applied: SettingsSnapshot,
    pub presented_frames: u64,
    pub skipped_frames: u64,
    pub output_width: u32,
    pub output_height: u32,
    /// Actual decoded front-buffer descriptor (reused frames retain its size),
    /// before output scaling and unrelated to supersampled EDRAM attachments.
    pub source_width: u32,
    pub source_height: u32,
}

pub struct StartedRenderer {
    pub handle: RendererHandle,
    pub diagnostics: NativeDiagnostics,
    pub adapter: AdapterInfo,
}

impl StartedRenderer {
    /// Requests shutdown and releases command-queue waiters. This cannot
    /// interrupt a synchronous driver call; `diagnostics.stopped` reports when
    /// the renderer thread has actually finished.
    pub fn request_stop(&self) {
        self.diagnostics.0.cancelled.store(true, Ordering::Release);
        self.handle.close();
    }
}

type Ready = std::sync::mpsc::SyncSender<std::result::Result<AdapterInfo, String>>;

/// Waits for adapter/device/renderer construction and its validation scope.
/// A timeout fails the caller; it cannot forcibly interrupt a driver call. The
/// host must enforce an independent process startup deadline. No title commands
/// should be submitted until this returns successfully.
pub fn start_checked(
    memory: Arc<dyn PhysicalMemory>,
    options: NativeOptions,
    targets: TargetsFactory,
    timeout: Duration,
) -> std::io::Result<StartedRenderer> {
    if timeout.is_zero() {
        return Err(std::io::Error::new(std::io::ErrorKind::InvalidInput, "renderer startup timeout is zero"));
    }
    let (tx, rx) = std::sync::mpsc::sync_channel(1);
    let (handle, diagnostics) = spawn_renderer(memory, options, targets, Some(tx), true)?;
    match await_initialized(&rx, timeout) {
        Ok(adapter) => Ok(StartedRenderer { handle, diagnostics, adapter }),
        Err(error) => {
            diagnostics.0.cancelled.store(true, Ordering::Release);
            diagnostics.0.failed.store(true, Ordering::Release);
            handle.close();
            Err(error)
        }
    }
}

fn await_initialized<T>(
    rx: &std::sync::mpsc::Receiver<std::result::Result<T, String>>,
    timeout: Duration,
) -> std::io::Result<T> {
    match rx.recv_timeout(timeout) {
        Ok(Ok(value)) => Ok(value),
        Ok(Err(message)) => Err(std::io::Error::other(message)),
        Err(std::sync::mpsc::RecvTimeoutError::Timeout) => {
            Err(std::io::Error::new(std::io::ErrorKind::TimedOut, "renderer initialization timed out"))
        }
        Err(_) => Err(std::io::Error::other("renderer stopped before initialization completed")),
    }
}

fn spawn_renderer(
    memory: Arc<dyn PhysicalMemory>,
    options: NativeOptions,
    targets: TargetsFactory,
    mut ready: Option<Ready>,
    checked: bool,
) -> std::io::Result<(RendererHandle, NativeDiagnostics)> {
    let handle = RendererHandle::new();
    let renderer = handle.clone();
    let diagnostics = NativeDiagnostics::default();
    let progress = diagnostics.clone();
    std::thread::Builder::new().name("renderer".into()).spawn(move || {
        let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            run(&renderer, memory, &options, targets, &progress, &mut ready, checked)
        }));
        let failure = match result {
            Ok(Ok(())) => None,
            Ok(Err(e)) => Some(e.to_string()),
            Err(payload) => {
                std::mem::forget(payload);
                Some("renderer panicked".to_string())
            }
        };
        if let Some(error) = failure {
            progress.0.failed.store(true, Ordering::Release);
            if let Some(tx) = ready.take() {
                let _ = tx.send(Err(error.clone()));
            }
            log::error!("renderer stopped: {error}");
            renderer.fail(1);
        }
        renderer.close();
        progress.0.stopped.store(true, Ordering::Release);
    })?;
    Ok((handle, diagnostics))
}

struct Offscreen {
    width: u32,
    height: u32,
    texture: Texture,
    view: TextureView,
    readback: Buffer,
    row: u32,
}

impl Offscreen {
    fn new(device: &Device, width: u32, height: u32) -> Self {
        let texture = device.create_texture(&TextureDescriptor {
            label: "present target",
            size: extent(width, height, 1),
            format: TextureFormat::RGBA8Unorm,
            usage: TextureUsage::RenderAttachment | TextureUsage::CopySrc,
            ..Default::default()
        });
        let view = texture.create_view(None);
        let row = (width * 4).next_multiple_of(256);
        let readback = device.create_buffer(&BufferDescriptor {
            label: "present readback",
            size: u64::from(row * height),
            usage: BufferUsage::MapRead | BufferUsage::CopyDst,
            mapped_at_creation: false,
        });
        Offscreen { width, height, texture, view, readback, row }
    }
}

fn run(
    handle: &RendererHandle,
    memory: Arc<dyn PhysicalMemory>,
    options: &NativeOptions,
    targets: TargetsFactory,
    diagnostics: &NativeDiagnostics,
    ready: &mut Option<Ready>,
    checked: bool,
) -> Result<()> {
    let instance = Instance::new()?;
    let surface = match &options.window {
        // SAFETY: host window handles remain valid for the surface's lifetime.
        // MetalLayerHandle's unsafe constructor additionally requires the host
        // to retain/synchronize the CAMetalLayer until renderer destruction.
        Some(window) => Some(unsafe {
            instance.create_surface(match window.handles {
                WindowHandles::Xlib { display, window } => SurfaceTarget::Xlib { display: display as *mut _, window },
                WindowHandles::Xcb { connection, window } => {
                    SurfaceTarget::Xcb { connection: connection as *mut _, window }
                }
                WindowHandles::Wayland { display, surface } => {
                    SurfaceTarget::Wayland { display: display as *mut _, surface: surface as *mut _ }
                }
                WindowHandles::MetalLayer(layer) => SurfaceTarget::Metal { layer: layer.as_raw() },
            })?
        }),
        None => None,
    };
    let adapter = instance.block_on(instance.request_adapter(&RequestAdapterOptions {
        power_preference: PowerPreference::HighPerformance,
        compatible_surface: surface.as_ref(),
        ..Default::default()
    }))?;
    let info = adapter.info();
    log::info!("renderer: {:?} {} ({})", info.backend, info.description, implementation());
    // Xenos programs need more than WebGPU's default limits (e.g. 16 interpolators plus
    // front_facing), so take everything the adapter offers.
    let features = wanted_features(&adapter.features());
    let device = instance.block_on(adapter.request_device(&DeviceDescriptor {
        label: "xenos",
        required_features: &features,
        required_limits: Some(adapter.limits()),
    }))?;
    // RGBA8 (what the PNG dumps read back) when the window takes it.
    let mut presentation = match (surface, &options.window) {
        (Some(surface), Some(window)) => {
            let caps = surface.capabilities(&adapter)?;
            let format = [TextureFormat::RGBA8Unorm, TextureFormat::BGRA8Unorm]
                .into_iter()
                .find(|f| caps.formats.contains(f))
                .or(caps.formats.first().copied())
                .ok_or(Error::Surface("the window offers no formats".into()))?;
            // Mailbox never blocks the renderer on the display (FIFO would, and stalls for good
            // on Wayland while the window is hidden).
            let present_mode = if caps.present_modes.contains(&PresentMode::Mailbox) {
                PresentMode::Mailbox
            } else {
                PresentMode::Fifo
            };
            log::info!("renderer: presenting in a window as {format:?}, {present_mode:?}");
            let mut p = Presentation {
                causal: None,
                window: window.clone(),
                surface,
                format,
                present_mode,
                configured: (0, 0),
                max_dimension: device.limits().maxTextureDimension2D,
                configures: 0,
            };
            p.configure(&device, PresentationSettings::default());
            Some(p)
        }
        _ => None,
    };
    let format = presentation.as_ref().map_or(TextureFormat::RGBA8Unorm, |p| p.format);
    if checked {
        device.push_error_scope(ErrorFilter::Validation);
    }
    let mut renderer = Renderer::new(&device, memory, format, targets);
    let mut offscreen: Option<Offscreen> = None;
    if options.dump_dir.is_some() && format != TextureFormat::RGBA8Unorm {
        log::warn!("renderer: the window is {format:?}; frames are not dumped");
    }
    if let Some(dir) = &options.dump_dir {
        std::fs::create_dir_all(dir).map_err(|e| Error::Invalid(format!("{}: {e}", dir.display())))?;
    }
    if checked {
        if let Some(error) = device.block_on(device.pop_error_scope())? {
            diagnostics.0.gpu_errors.fetch_add(1, Ordering::Release);
            return Err(error.into());
        }
        device.poll();
        instance.process_events();
        if let Some(reason) = device.lost_reason() {
            return Err(Error::RequestDevice(format!("device lost during initialization: {reason}")));
        }
        let errors = device.take_errors();
        diagnostics.0.gpu_errors.fetch_add(errors.len() as u64, Ordering::Release);
        if let Some(error) = errors.into_iter().next() {
            return Err(error.into());
        }
    }
    if diagnostics.0.cancelled.load(Ordering::Acquire) {
        return Err(Error::Invalid("renderer initialization was cancelled".into()));
    }
    diagnostics.0.initialized.store(true, Ordering::Release);
    if let Some(tx) = ready.take() {
        tx.send(Ok(info)).map_err(|_| Error::Invalid("renderer startup receiver disappeared".into()))?;
    }
    let every = options.dump_every.max(1);
    let mut presents = Vec::new();
    let mut busy = Duration::ZERO;
    let presentation_clock = std::time::Instant::now();
    let mut report_at = presentation_clock;
    let mut report_calls = gpu_webgpu::counters::snapshot();
    let mut report_times = renderer.executor.times();
    let mut report_draws = renderer.executor.stats.draws;
    let mut last_decoded_frame = 0;
    let mut presentation_admission = PresentAdmission::default();
    let mut frame_stability = frame_stability::FrameStability::default();
    let mut source_cursor = 0;
    let mut iteration_max_ms = 0u64;

    loop {
        if diagnostics.0.cancelled.load(Ordering::Acquire) {
            return Ok(());
        }
        if renderer.executor.causal.is_none() {
            renderer.executor.causal = diagnostics.causal_session();
        }
        {
            let _phase = crate::causal::Guard::enter(&renderer.executor.causal, crate::causal::Stage::Poll);
            instance.process_events();
        }
        let started = std::time::Instant::now();
        let before = handle.completed();
        renderer.pump(handle, Some(Duration::from_millis(5)), None, |r, size| {
            let swap = diagnostics.0.swap_requests.fetch_add(1, Ordering::Release) + 1;
            let Some((w, h)) = size else {
                diagnostics.0.unrendered_swaps.fetch_add(1, Ordering::Release);
                frame_stability.swap(presentation_clock.elapsed());
                return;
            };
            let mut swap_finished = None;
            diagnostics.0.source_size.store(Window::pack_size(w, h), Ordering::Release);
            if r.presenter.frames != last_decoded_frame {
                diagnostics.0.decoded_frames.fetch_add(1, Ordering::Release);
                last_decoded_frame = r.presenter.frames;
            } else {
                // Presenter::swap keeps the preceding frame when a new front
                // buffer cannot be decoded. Do not count that as a new frame.
                diagnostics.0.reused_frames.fetch_add(1, Ordering::Release);
            }
            if let Some(p) = presentation.as_mut() {
                let requested = diagnostics.0.settings.snapshot();
                r.presenter.set_output_options(&r.device, requested.settings, format);
                diagnostics.0.applied_settings.applied(requested);
                if presentation_admission.admit(presentation_clock.elapsed(), requested.settings.frame_limit()) {
                    if p.causal.is_none() { p.causal = r.executor.causal.clone(); }
                    let present_started = std::time::Instant::now();
                    let success = p.present(&r.device, &r.presenter, requested.settings);
                    let present_finished = std::time::Instant::now();
                    frame_stability.present(
                        present_started.duration_since(presentation_clock),
                        present_finished.duration_since(presentation_clock),
                        success,
                    );
                    swap_finished = Some(present_finished);
                    if success {
                        diagnostics.0.presented_frames.fetch_add(1, Ordering::Release);
                        diagnostics
                            .0
                            .presentation_size
                            .store(Window::pack_size(p.configured.0, p.configured.1), Ordering::Release);
                    }
                } else {
                    diagnostics.0.skipped_frames.fetch_add(1, Ordering::Release);
                }
            } else {
                let requested = diagnostics.0.settings.snapshot();
                r.presenter.set_output_options(&r.device, requested.settings, format);
                diagnostics.0.applied_settings.applied(requested);
            }
            let swap_finished = swap_finished.unwrap_or_else(std::time::Instant::now);
            frame_stability.swap(swap_finished.duration_since(presentation_clock));
            presents.push(swap_finished);
            let Some(dir) = &options.dump_dir else { return };
            if format != TextureFormat::RGBA8Unorm {
                return;
            }
            let number = if checked { swap } else { r.presenter.frames };
            if !number.is_multiple_of(every) {
                return;
            }
            let target = match offscreen.take() {
                Some(o) if o.width == w && o.height == h => o,
                _ => Offscreen::new(&r.device, w, h),
            };
            let path = dir.join(format!("frame_{number:06}.png"));
            match dump(&r.device, &r.presenter, &target, &path, checked) {
                Ok(()) => {
                    diagnostics.0.png_written.fetch_add(1, Ordering::Release);
                    let _ =
                        diagnostics.0.first_png_swap.compare_exchange(0, swap, Ordering::Release, Ordering::Relaxed);
                    log::info!("renderer: wrote {}", path.display());
                }
                Err(e) => {
                    diagnostics.0.png_failures.fetch_add(1, Ordering::Release);
                    diagnostics.0.failed.store(true, Ordering::Release);
                    log::error!("renderer: {}: {e}", path.display());
                }
            }
            offscreen = Some(target);
        });
        // Reuse the enqueue/dequeue timestamps already collected by Renderer;
        // keep these distinct from callback and native-present completion gaps.
        for (submitted, queue_wait) in renderer.swap_timings.iter().skip(source_cursor) {
            if let Some(enqueued) = submitted.checked_duration_since(presentation_clock) {
                frame_stability.source(enqueued, *queue_wait);
            }
        }
        source_cursor = renderer.swap_timings.len();
        let errors = device.take_errors();
        diagnostics.0.gpu_errors.fetch_add(errors.len() as u64, Ordering::Release);
        let had_errors = !errors.is_empty();
        if had_errors || device.lost_reason().is_some() {
            diagnostics.0.failed.store(true, Ordering::Release);
        }
        for e in errors {
            log::error!("renderer: GPU error: {e:?}");
        }
        if checked
            && (had_errors || device.lost_reason().is_some() || diagnostics.0.png_failures.load(Ordering::Acquire) != 0)
        {
            return Err(Error::Invalid("checked renderer encountered a GPU, device-loss or PNG error".into()));
        }
        if let Some(reason) = renderer.executor.failure {
            if let Some(session) = &renderer.executor.causal { session.fail(reason); }
            return Err(Error::Invalid(format!("renderer memory contract failed ({reason})")));
        }
        // Time spent executing commands (not waiting for the title to submit some).
        let iteration_elapsed = started.elapsed();
        iteration_max_ms = iteration_max_ms.max(iteration_elapsed.as_millis().try_into().unwrap_or(u64::MAX));
        if handle.completed() != before || renderer.backlog_len() > 0 {
            busy += iteration_elapsed;
        }
        let window = report_at.elapsed();
        if window >= Duration::from_secs(5) {
            let frames = presents.len() as u64;
            let fps = frames as f64 / window.as_secs_f64();
            let draws = renderer.executor.stats.draws;
            // Log arguments are lazy: consume each window even when INFO is disabled.
            let pacing = crate::renderer::finish_pacing_window(
                &mut presents,
                &mut renderer.swap_timings,
                log::log_enabled!(log::Level::Info),
            );
            source_cursor = 0;
            let frame_window = frame_stability.finish_window();
            if let Some(pacing) = pacing {
                log::info!(
                    "perf: {fps:.1} guest swaps/s, {} draws/frame; renderer busy {:.0}%; {}",
                    (draws - report_draws) / frames.max(1),
                    100.0 * busy.as_secs_f64() / window.as_secs_f64(),
                    pacing
                );
            }
            let calls = gpu_webgpu::counters::snapshot();
            log::info!("perf: per frame: {}", crate::renderer::calls_per_frame(&calls.since(&report_calls), frames));
            let times = renderer.executor.times();
            let observed_at = report_at + window;
            diagnostics.0.performance.try_publish(observed_at, || {
                NativePerformanceSnapshot::capture(
                    performance::Window {
                        end_elapsed: observed_at.duration_since(presentation_clock),
                        span: window,
                        busy,
                        draws_before: report_draws,
                        presentable_swaps: frames,
                        frame_stability: frame_window,
                        iteration_max_ms,
                        surface_configures: presentation.as_ref().map_or(0, |p| p.configures),
                    },
                    renderer.executor.stats,
                    renderer.executor.texture_stats(),
                    renderer.executor.mirror_stats(),
                    times,
                    report_times,
                )
            });
            let ms = |now: Duration, then: Duration| (now - then).as_secs_f64() * 1000.0;
            log::info!(
                "perf: renderer time: {:.0} ms binding new texture sets, {:.0} ms creating pipelines, {:.0} ms on \
                 shaders, {:.0} ms recording resolves, {:.0} ms submitting",
                ms(times.texture_sets, report_times.texture_sets),
                ms(times.pipelines, report_times.pipelines),
                ms(times.shaders, report_times.shaders),
                ms(times.resolves, report_times.resolves),
                ms(times.submits, report_times.submits),
            );
            report_calls = calls;
            report_times = times;
            report_draws = draws;
            busy = Duration::ZERO;
            report_at = std::time::Instant::now();
        }
    }
}

/// Renders the current front buffer into `target`, reads it back and writes a PNG.
fn dump(
    device: &Device,
    presenter: &crate::present::Presenter,
    target: &Offscreen,
    path: &Path,
    exclusive: bool,
) -> Result<()> {
    let mut encoder = device.create_command_encoder("dump");
    presenter.encode(&mut encoder, &target.view, target.width, target.height);
    encoder.copy_texture_to_buffer(
        &TexelCopyTextureInfo::whole(&target.texture),
        &TexelCopyBufferInfo { buffer: &target.readback, offset: 0, bytes_per_row: target.row, rows_per_image: None },
        extent(target.width, target.height, 1),
    );
    device.queue().submit([encoder.finish("dump")]);
    device.block_on(target.readback.map_async(MapMode::Read, 0, None))?;
    let mut padded = vec![0u8; (target.row * target.height) as usize];
    target.readback.read_mapped(0, &mut padded)?;
    target.readback.unmap();
    let rgba: Vec<u8> =
        padded.chunks(target.row as usize).flat_map(|r| &r[..target.width as usize * 4]).copied().collect();
    write_png_mode(path, target.width, target.height, &rgba, exclusive).map_err(|e| Error::Invalid(e.to_string()))
}

fn write_png(path: &Path, width: u32, height: u32, rgba: &[u8]) -> std::io::Result<()> {
    write_png_mode(path, width, height, rgba, false)
}

fn write_png_mode(path: &Path, width: u32, height: u32, rgba: &[u8], exclusive: bool) -> std::io::Result<()> {
    use std::io::Write;
    let working = if exclusive { path.with_extension("png.part") } else { path.to_path_buf() };
    let file = if exclusive {
        std::fs::OpenOptions::new().write(true).create_new(true).open(&working)?
    } else {
        std::fs::File::create(path)?
    };
    let mut file = std::io::BufWriter::new(file);
    let mut enc = png::Encoder::new(&mut file, width, height);
    enc.set_color(png::ColorType::Rgba);
    enc.set_depth(png::BitDepth::Eight);
    let mut w = enc.write_header().map_err(std::io::Error::other)?;
    w.write_image_data(rgba).map_err(std::io::Error::other)?;
    w.finish().map_err(std::io::Error::other)?;
    file.flush()?;
    file.get_ref().sync_all()?;
    drop(file);
    if exclusive {
        // Publish a complete PNG exclusively. A killed capture leaves only a
        // .part file, which must not count as a completed frame.
        std::fs::hard_link(&working, path)?;
        std::fs::remove_file(&working)?;
    }
    Ok(())
}

#[cfg(test)]
mod checked_start_tests {
    use super::*;

    #[test]
    fn presentation_status_distinguishes_requested_applied_and_actual_extents() {
        let diagnostics = NativeDiagnostics::default();
        let initial = diagnostics.presentation_status();
        assert_eq!(initial.requested, SettingsSnapshot::default());
        assert_eq!(initial.applied, SettingsSnapshot::default());
        assert_eq!(
            (initial.source_width, initial.source_height, initial.output_width, initial.output_height),
            (0, 0, 0, 0)
        );
        let settings = PresentationSettings::from_words([30, 50, 1, 25]).unwrap();
        assert_eq!(diagnostics.set_presentation_settings(settings), 1);
        assert_eq!(diagnostics.presentation_status().applied.generation, 0, "request is not proof of application");
        let requested = diagnostics.0.settings.snapshot();
        diagnostics.0.applied_settings.applied(requested);
        diagnostics.0.source_size.store(Window::pack_size(1280, 720), Ordering::Release);
        diagnostics.0.presentation_size.store(Window::pack_size(640, 360), Ordering::Release);
        let status = diagnostics.presentation_status();
        assert_eq!(status.applied, status.requested);
        assert_eq!((status.source_width, status.source_height), (1280, 720));
        assert_eq!((status.output_width, status.output_height), (640, 360));
        assert_eq!((status.presented_frames, status.skipped_frames), (0, 0));
    }

    #[test]
    fn initialization_requires_an_explicit_success_message() {
        let (tx, rx) = std::sync::mpsc::sync_channel(1);
        tx.send(Ok(7u32)).unwrap();
        assert_eq!(await_initialized(&rx, Duration::from_millis(50)).unwrap(), 7);
        let (tx, rx) = std::sync::mpsc::sync_channel(1);
        tx.send(Err::<u32, _>("synthetic initialization failure".into())).unwrap();
        assert!(await_initialized(&rx, Duration::from_millis(50)).is_err());
    }

    #[test]
    fn missing_and_disconnected_startup_are_failures() {
        let (tx, rx) = std::sync::mpsc::sync_channel::<std::result::Result<u32, String>>(1);
        assert_eq!(await_initialized(&rx, Duration::from_millis(1)).unwrap_err().kind(), std::io::ErrorKind::TimedOut);
        drop(tx);
        assert!(await_initialized(&rx, Duration::from_millis(50)).is_err());
    }

    #[test]
    fn completed_png_is_decodable_and_a_second_capture_cannot_overwrite_it() {
        let unique = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos();
        let directory = std::env::temp_dir().join(format!("xenos-public-png-{}-{unique}", std::process::id()));
        std::fs::create_dir(&directory).unwrap();
        let path = directory.join("frame_000001.png");
        let partial = path.with_extension("png.part");
        write_png_mode(&path, 1, 1, &[255, 0, 0, 255], true).unwrap();
        assert!(!partial.exists());
        let original = std::fs::read(&path).unwrap();
        let mut reader = png::Decoder::new(std::io::Cursor::new(&original)).read_info().unwrap();
        let mut rgba = vec![0; reader.output_buffer_size()];
        let info = reader.next_frame(&mut rgba).unwrap();
        assert_eq!((info.width, info.height), (1, 1));
        assert_eq!(&rgba[..info.buffer_size()], &[255, 0, 0, 255]);
        assert!(write_png_mode(&path, 1, 1, &[0, 0, 255, 255], true).is_err());
        assert_eq!(std::fs::read(&path).unwrap(), original);
        // Only files created by this test are removed, with no recursive delete.
        std::fs::remove_file(&partial).unwrap();
        std::fs::remove_file(&path).unwrap();
        std::fs::remove_dir(&directory).unwrap();
    }

    #[cfg(target_os = "linux")]
    #[test]
    #[ignore = "requires the pinned local Vulkan library and a selected public software ICD"]
    fn checked_renderer_initializes_and_stops_without_game_data() {
        struct EmptyMemory;
        impl PhysicalMemory for EmptyMemory {
            fn read(&self, _: u32, out: &mut [u8]) {
                out.fill(0);
            }
            fn write(&self, _: u32, _: &[u8]) {}
        }
        let started = start_checked(
            Arc::new(EmptyMemory),
            NativeOptions::default(),
            Box::new(|device| Box::new(crate::edram::EdramTargets::new(device))),
            Duration::from_secs(15),
        )
        .unwrap_or_else(|e| panic!("public checked renderer startup failed: {e}"));
        assert_eq!(started.adapter.backend, gpu_webgpu::ffi::WGPUBackendType::Vulkan);
        let before = started.diagnostics.snapshot();
        assert!(before.initialized && !before.failed);
        assert_eq!((before.swap_requests, before.decoded_frames, before.png_written), (0, 0, 0));
        // Exercise the real five-second publication branch with INFO logging
        // disabled and no guest commands, GPU query, or detailed profiler.
        let report_deadline = std::time::Instant::now() + Duration::from_secs(7);
        let performance = loop {
            if let Some(sample) = started.diagnostics.performance_snapshot() {
                break sample;
            }
            assert!(std::time::Instant::now() < report_deadline, "coarse report was not published");
            std::thread::sleep(Duration::from_millis(10));
        };
        assert!(performance.window_ms >= 5000 && performance.sample_end_elapsed_ms >= performance.window_ms);
        assert_eq!(
            (
                performance.draws_window,
                performance.draws_total,
                performance.skipped_draws_total,
                performance.submissions_total,
                performance.renderer_busy_window_ms
            ),
            (0, 0, 0, 0, 0)
        );
        started.request_stop();
        let deadline = std::time::Instant::now() + Duration::from_secs(5);
        while !started.diagnostics.snapshot().stopped && std::time::Instant::now() < deadline {
            std::thread::sleep(Duration::from_millis(5));
        }
        let after = started.diagnostics.snapshot();
        assert!(after.stopped && !after.failed);
    }
}

const VIEW_DUMP: &str = r#"
@group(0) @binding(0) var src: texture_2d<f32>;

@vertex
fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4<f32> {
    let p = vec2<f32>(f32((i << 1u) & 2u), f32(i & 2u));
    return vec4<f32>(p * 2.0 - 1.0, 0.0, 1.0);
}

@fragment
fn fs(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {
    let c = textureLoad(src, vec2<i32>(pos.xy), 0);
    return vec4<f32>(clamp(c.rgb, vec3<f32>(0.0), vec3<f32>(1.0)), 1.0);
}
"#;

/// Debug aid (`--render-debug trace-frame`): writes the first `width` x `height` texels of a
/// single-sampled color view as an opaque PNG (channels clamped to 0..1). Blocks until done.
pub(crate) fn dump_view(device: &Device, view: &TextureView, width: u32, height: u32, path: &Path) -> Result<()> {
    let module = device.create_shader_module_wgsl("view dump", VIEW_DUMP);
    let layout = device.create_bind_group_layout(
        "view dump",
        &[BindGroupLayoutEntry {
            binding: 0,
            visibility: ShaderStage::Fragment,
            ty: BindingType::Texture {
                sample_type: TextureSampleType::UnfilterableFloat,
                view_dimension: TextureViewDimension::D2,
                multisampled: false,
            },
        }],
    );
    let pipeline_layout = device.create_pipeline_layout("view dump", &[&layout]);
    let pipeline = device.create_render_pipeline(&RenderPipelineDescriptor {
        label: "view dump",
        layout: Some(&pipeline_layout),
        vertex: ProgrammableStage::new(&module, "vs"),
        vertex_buffers: &[],
        primitive: PrimitiveState::default(),
        depth_stencil: None,
        multisample: MultisampleState::default(),
        fragment: Some(FragmentState {
            stage: ProgrammableStage::new(&module, "fs"),
            targets: &[ColorTargetState::new(TextureFormat::RGBA8Unorm)],
        }),
    });
    let group = device.create_bind_group(
        "view dump",
        &layout,
        &[BindGroupEntry { binding: 0, resource: BindingResource::TextureView(view) }],
    );
    let target = Offscreen::new(device, width, height);
    let mut encoder = device.create_command_encoder("view dump");
    let mut pass = encoder.begin_render_pass(
        "view dump",
        &[Some(RenderPassColorAttachment::clear(&target.view, Color { r: 0.0, g: 0.0, b: 0.0, a: 1.0 }))],
        None,
    );
    pass.set_pipeline(&pipeline);
    pass.set_bind_group(0, &group, &[]);
    pass.draw(3, 1, 0, 0);
    pass.end();
    encoder.copy_texture_to_buffer(
        &TexelCopyTextureInfo::whole(&target.texture),
        &TexelCopyBufferInfo { buffer: &target.readback, offset: 0, bytes_per_row: target.row, rows_per_image: None },
        extent(width, height, 1),
    );
    device.queue().submit([encoder.finish("view dump")]);
    device.block_on(target.readback.map_async(MapMode::Read, 0, None))?;
    let mut padded = vec![0u8; (target.row * height) as usize];
    target.readback.read_mapped(0, &mut padded)?;
    target.readback.unmap();
    let rgba: Vec<u8> = padded.chunks(target.row as usize).flat_map(|r| &r[..width as usize * 4]).copied().collect();
    write_png(path, width, height, &rgba).map_err(|e| Error::Invalid(e.to_string()))
}
