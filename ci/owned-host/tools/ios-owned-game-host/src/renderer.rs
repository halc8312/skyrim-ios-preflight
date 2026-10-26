//! One-shot CAMetalLayer attachment. UIKit owns the layer for process lifetime.
//! No game image, command stream or file is exported from this bridge.
pub use crate::renderer_policy::OHRendererStatusV1;
use crate::device_settings::{BootConfiguration, OHBootGraphicsV1, OHGraphicsStatusV1, OHPresentationSettingsV1};
use crate::renderer_policy::{Attempt, size_allowed};
use std::ffi::c_void;
use std::mem::{align_of, size_of};
use std::sync::atomic::{AtomicU32, AtomicU64, Ordering};
use std::sync::{Arc, OnceLock};
use std::time::Duration;
use xenos_render::native::{
    MetalLayerHandle, NativeOptions, StartedRenderer, Window, WindowHandles,
};

#[path = "../../../skyrim-recomp-web/hosts/common/gpu.rs"]
mod common_gpu;

static ATTACHMENT: AtomicU32 = AtomicU32::new(0);
static OWNER: OnceLock<MetalOwner> = OnceLock::new();
static BOOT_SETTINGS: OnceLock<BootConfiguration> = OnceLock::new();
fn boot_settings() -> &'static BootConfiguration { BOOT_SETTINGS.get_or_init(BootConfiguration::default) }

pub(crate) fn begin_causal_session(token: u64, origin: std::time::Instant) -> crate::Check<Arc<xenos_render::causal::Session>> {
    let owner = OWNER.get().ok_or("causal renderer owner absent")?;
    if owner.token != token || ATTACHMENT.load(Ordering::Acquire) != 2 {
        return Err("causal renderer owner invalid");
    }
    if !owner.renderer.diagnostics.begin_causal_session(token, origin) {
        return Err("causal session already active");
    }
    owner.renderer.diagnostics.causal_session().ok_or("causal session unavailable")
}

pub(crate) fn causal_snapshot(token: u64) -> Option<serde_json::Value> {
    let owner = OWNER.get()?;
    if owner.token != token || ATTACHMENT.load(Ordering::Acquire) != 2 { return None; }
    let snapshot = owner.renderer.diagnostics.causal_session()?.snapshot();
    let ring = |(overwritten, unavailable, records): (u64, u64, Vec<Vec<u64>>)| {
        serde_json::json!({"overwritten":overwritten, "unavailable":unavailable, "records":records})
    };
    Some(serde_json::json!({
        "session":snapshot.session, "capture_started_us":snapshot.capture_started_us,
        "observed_us":snapshot.observed_us, "failure_reason":snapshot.failure, "experiment_id":1,
        "presentation_words":owner.renderer.diagnostics.presentation_status().requested.settings.words(),
        "boot_words":[owner.boot.video_width, owner.boot.video_height, owner.boot.internal_scale,
            owner.boot.mirror_mib, owner.boot.mobile_texture_budget],
        "live":snapshot.live, "audio_live":snapshot.audio_live,
        "render":ring(snapshot.render), "audio":ring(snapshot.audio), "frames":ring(snapshot.frames),
        "stage_totals":snapshot.totals, "reasons":snapshot.reasons,
        "backend_detail":{"schema":1, "backend":1, "host_elapsed_totals":snapshot.details,
            "constant_capacity_rejections":snapshot.capacity_rejections,
            "constant_block_bytes":snapshot.constant_block_bytes},
        "thread_work":{"schema":1,"units":"nanoseconds",
            "rows":["main_encoder_finish","constant_encoder_finish","queue_submit"],
            "columns":["calls","wall_ns","successful_cpu_samples","cpu_ns","maximum_cpu_ns"],
            "totals":snapshot.thread_work},
    }))
}
struct MetalOwner {
    token: u64,
    boot: OHBootGraphicsV1,
    size: Arc<AtomicU64>,
    renderer: StartedRenderer,
}

pub fn is_ready(token: u64) -> bool {
    OWNER.get().is_some_and(|owner| {
        let status = owner.renderer.diagnostics.snapshot();
        owner.token == token
            && ATTACHMENT.load(Ordering::Acquire) == 2
            && status.initialized
            && !status.failed
            && !status.stopped
    })
}

/// Read only the owned renderer's existing coarse report. No GPU work or wait.
/// Renderer and guest elapsed clocks have different initialization epochs.
pub(crate) fn performance_snapshot(token: u64) -> Option<serde_json::Value> {
    let owner = OWNER.get()?;
    if owner.token != token || ATTACHMENT.load(Ordering::Acquire) != 2 {
        return None;
    }
    let s = owner.renderer.diagnostics.performance_snapshot()?;
    macro_rules! numeric {
        ($($field:ident),+ $(,)?) => {{
            let mut values = serde_json::Map::new();
            $(values.insert(stringify!($field).into(), serde_json::Value::from(s.$field));)+
            serde_json::Value::Object(values)
        }};
    }
    Some(numeric!(sample_end_elapsed_ms, window_ms, age_ms, stale,
        presentable_swaps_window, draws_window, draws_total, skipped_draws_total,
        resolves_total, gpu_resolves_total, submissions_total, pipelines_total, upload_waits_total,
        constant_upload_bytes_total, index_upload_bytes_total, memory_upload_bytes_total,
        texture_set_misses_total, texture_set_stale_total, texture_loads_total, texture_reloads_total,
        vertex_range_eligible_bindings_total, vertex_range_narrowed_bindings_total,
        vertex_range_avoided_pages_total, vertex_range_fallback_bindings_total,
        texture_hits_total, texture_fallbacks_total, texture_deferred_total, texture_evictions_total,
        texture_host_bytes, texture_pooled_bytes, mirror_slot_bytes, mirror_blocks,
        mirror_evictions_total, mirror_slot_conflicts_total, texture_sets_window_ms,
        pipelines_window_ms, shaders_window_ms, resolves_window_ms, submits_window_ms,
        renderer_busy_window_ms, swap_intervals_total, swap_gap_max_window_ms,
        swap_gap_max_total_ms, swap_gaps_over_100ms_total, swap_gaps_over_250ms_total,
        swap_gaps_over_1000ms_total, source_samples_window, source_gap_max_window_ms,
        source_gap_max_total_ms, swap_queue_wait_max_window_ms, swap_queue_wait_max_total_ms,
        present_calls_window, present_call_max_window_ms, present_call_max_total_ms,
        present_failures_total, present_gap_max_window_ms, present_gap_max_total_ms,
        gpu_frame_limit_polls_total, gpu_upload_limit_polls_total, gpu_resolve_pending_polls_total,
        pipeline_call_max_total_ms, renderer_iteration_max_total_ms, surface_configures_total))
}

fn attach(token: u64, layer: *mut c_void, width: u32, height: u32) -> Result<(), i32> {
    // Startup waits for the renderer, whose iOS layer configuration dispatches
    // synchronously to main. Refuse main-thread entry before claiming attachment
    // state; callers must use a worker while the UIKit main queue stays available.
    #[cfg(target_os = "ios")]
    if unsafe { libc::pthread_main_np() } != 0 {
        return Err(-9);
    }
    if layer.is_null() || !size_allowed(width, height) {
        return Err(-1);
    }
    let kernel = crate::runtime::prepared_kernel(token).map_err(|_| -2)?;
    let attempt = Attempt::begin(&ATTACHMENT)?;
    let boot = boot_settings().freeze().map_err(|_| -10)?;
    for setting in [0x13, 0x14, 0x15] {
        if !kernel.xconfig.write(kernel_hle::xconfig::category::USER, setting, &boot.packed_video_size().to_be_bytes()) {
            return Err(-10);
        }
    }
    xenos_render::set_resolution_scale(boot.internal_scale).map_err(|_| -10)?;
    if boot.mobile_texture_budget != 0 { xenos_render::handheld_budgets(); }
    if boot.mirror_mib != 0 { xenos_render::set_mirror_limit(boot.mirror_mib); }
    // The typed UIKit caller must retain this exact layer, including failed
    // initialization and detached views, until process exit. It must never
    // mutate its device/drawableSize after this call. Resize uses only `size`.
    let handle = unsafe { MetalLayerHandle::from_raw(layer) }.ok_or(-1)?;
    let size = Arc::new(AtomicU64::new(Window::pack_size(width, height)));
    xenos_render::set_d3d_mode(false);
    // Match the upstream handheld fence policy before any guest commands run.
    // This orders CPU-visible GPU effects after renderer completion; it is
    // not evidence that the separate native iOS loading crash is resolved.
    kernel_hle::gpu::set_sync_default(true);
    let renderer = xenos_render::native::start_checked(
        Arc::new(common_gpu::GuestPhysical(kernel)),
        NativeOptions {
            dump_dir: None,
            dump_every: 0,
            window: Some(Window {
                handles: WindowHandles::MetalLayer(handle),
                size: size.clone(),
            }),
        },
        common_gpu::render_targets(),
        Duration::from_secs(15),
    )
    .map_err(|_| -4)?;
    let status = renderer.diagnostics.snapshot();
    if !status.initialized || status.failed || status.stopped {
        renderer.request_stop();
        return Err(-5);
    }
    if !kernel
        .gpu
        .set_backend(Arc::new(common_gpu::Renderer(renderer.handle.clone())))
    {
        renderer.request_stop();
        return Err(-6);
    }
    // Retain the owner before publishing the ready state, even on a later
    // phase failure. No second renderer or runtime may replace this object.
    OWNER
        .set(MetalOwner {
            token,
            boot,
            size,
            renderer,
        })
        .map_err(|owner| {
            owner.renderer.request_stop();
            -3
        })?;
    if crate::runtime::mark_renderer_attached(token).is_err() {
        OWNER.get().unwrap().renderer.request_stop();
        return Err(-7);
    }
    attempt.commit();
    Ok(())
}

/// # Safety
/// `layer` is a real main-thread-created CAMetalLayer retained by UIKit's
/// process-global owner until process exit, never rebound or released. The
/// caller does not mutate its device/drawableSize concurrently with rendering.
/// Call on a worker with the main queue available; main-thread calls return -9
/// before changing the one-shot attachment state.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn OHAttachMetalLayerV1(
    token: u64,
    layer: *mut c_void,
    width: u32,
    height: u32,
) -> i32 {
    match std::panic::catch_unwind(|| attach(token, layer, width, height)) {
        Ok(Ok(())) => 0,
        Ok(Err(code)) => code,
        Err(_) => -8,
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn OHResizeMetalLayerV1(token: u64, width: u32, height: u32) -> i32 {
    if !size_allowed(width, height) {
        return -1;
    }
    let Some(owner) = OWNER.get().filter(|owner| owner.token == token) else {
        return -2;
    };
    if !is_ready(token) {
        return -3;
    }
    owner
        .size
        .store(Window::pack_size(width, height), Ordering::Release);
    0
}

/// # Safety
/// `out` is an exclusive, aligned, valid writable 56-byte result buffer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn OHReadRendererStatusV1(
    token: u64,
    out: *mut OHRendererStatusV1,
    bytes: usize,
) -> i32 {
    if out.is_null()
        || out as usize % align_of::<OHRendererStatusV1>() != 0
        || bytes != size_of::<OHRendererStatusV1>()
    {
        return -1;
    }
    let Some(owner) = OWNER.get().filter(|owner| owner.token == token) else {
        return -2;
    };
    let s = owner.renderer.diagnostics.snapshot();
    let result = OHRendererStatusV1 {
        abi_version: 0x0001_0000,
        struct_bytes: size_of::<OHRendererStatusV1>() as u32,
        attachment: ATTACHMENT.load(Ordering::Acquire),
        initialized: s.initialized.into(),
        stopped: s.stopped.into(),
        failed: s.failed.into(),
        swap_requests: s.swap_requests,
        decoded_frames: s.decoded_frames,
        gpu_errors: s.gpu_errors,
        unrendered_swaps: s.unrendered_swaps,
    };
    unsafe { out.write(result) };
    0
}

/// # Safety
/// `settings` is an immutable aligned32-byte buffer, valid for this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn OHConfigureBootGraphicsV1(token: u64, settings: *const OHBootGraphicsV1, bytes: usize) -> i32 {
    if settings.is_null() || settings as usize % align_of::<OHBootGraphicsV1>() != 0
        || bytes != size_of::<OHBootGraphicsV1>() { return -1; }
    if crate::runtime::renderer_configuration_allowed(token).is_err() { return -2; }
    match boot_settings().configure(unsafe { settings.read() }) { Ok(()) => 0, Err(_) => -3 }
}

/// # Safety
/// `settings` is an immutable aligned32-byte buffer, valid for this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn OHSetPresentationSettingsV1(token: u64, settings: *const OHPresentationSettingsV1, bytes: usize) -> i32 {
    if settings.is_null() || settings as usize % align_of::<OHPresentationSettingsV1>() != 0
        || bytes != size_of::<OHPresentationSettingsV1>() { return -1; }
    let Ok(words) = (unsafe { settings.read() }).words() else { return -1; };
    let Ok(value) = xenos_render::settings::PresentationSettings::from_words(words) else { return -1; };
    let Some(owner) = OWNER.get().filter(|owner| owner.token == token && is_ready(token)) else { return -2; };
    owner.renderer.diagnostics.set_presentation_settings(value);
    0
}

/// # Safety
/// `out` is an exclusive aligned120-byte writable buffer, valid for this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn OHReadGraphicsStatusV1(token: u64, out: *mut OHGraphicsStatusV1, bytes: usize) -> i32 {
    if out.is_null() || out as usize % align_of::<OHGraphicsStatusV1>() != 0
        || bytes != size_of::<OHGraphicsStatusV1>() { return -1; }
    let Some(owner) = OWNER.get().filter(|owner| owner.token == token) else { return -2; };
    let Ok((boot, frozen)) = boot_settings().snapshot() else { return -3; };
    let presentation = owner.renderer.diagnostics.presentation_status();
    let native = owner.renderer.diagnostics.snapshot();
    let requested = presentation.requested.settings.words();
    let applied = presentation.applied.settings.words();
    let value = OHGraphicsStatusV1 {
        version: crate::device_settings::VERSION, struct_bytes: size_of::<OHGraphicsStatusV1>() as u32,
        video_width: boot.video_width, video_height: boot.video_height, internal_scale: boot.internal_scale,
        mirror_mib: boot.mirror_mib, mobile_texture_budget: boot.mobile_texture_budget, boot_frozen: frozen.into(),
        requested_frame_limit: requested[0], requested_output_scale_percent: requested[1], requested_filter: requested[2], requested_sharpness: requested[3],
        applied_frame_limit: applied[0], applied_output_scale_percent: applied[1], applied_filter: applied[2], applied_sharpness: applied[3],
        requested_generation: presentation.requested.generation, applied_generation: presentation.applied.generation,
        output_width: presentation.output_width, output_height: presentation.output_height,
        source_width: presentation.source_width, source_height: presentation.source_height,
        presented_frames: presentation.presented_frames, skipped_frames: presentation.skipped_frames,
        decoded_frames: native.decoded_frames, reused_frames: native.reused_frames,
    };
    unsafe { out.write(value) };
    0
}
