//! One-shot CAMetalLayer attachment. UIKit owns the layer for process lifetime.
//! No game image, command stream or file is exported from this bridge.
pub use crate::renderer_policy::OHRendererStatusV1;
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
struct MetalOwner {
    token: u64,
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

fn attach(token: u64, layer: *mut c_void, width: u32, height: u32) -> Result<(), i32> {
    if layer.is_null() || !size_allowed(width, height) {
        return Err(-1);
    }
    let kernel = crate::runtime::prepared_kernel(token).map_err(|_| -2)?;
    let attempt = Attempt::begin(&ATTACHMENT)?;
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
