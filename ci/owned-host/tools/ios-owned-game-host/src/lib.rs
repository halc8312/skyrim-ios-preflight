//! Experimental, one-process owned guest host. Pure default tests do not load
//! an Apple image. A matching hash is content binding, not signature authority,
//! HLE completeness, game compatibility or permission to execute on iOS.
pub type Check<T> = Result<T, &'static str>;
pub mod abi_v2;
pub mod audio_output;
pub mod binding;
pub mod config_overlay;
pub mod causal_diagnostics;
#[cfg(all(feature = "fixture-tools", feature = "metal-renderer", target_os = "ios"))]
mod alpha_coverage_probe;
pub mod device_settings;
#[cfg(all(feature = "fixture-tools", feature = "metal-renderer", target_os = "ios", target_arch = "aarch64"))]
mod graphics_fixture;
#[cfg(all(feature = "fixture-tools", feature = "metal-renderer", target_os = "ios", target_arch = "aarch64"))]
mod native_surface_fixture;
#[cfg(all(feature = "fixture-tools", feature = "metal-renderer", target_os = "ios", target_arch = "aarch64"))]
mod constant_backend_fixture;
pub mod guest_fatal;
pub mod guest_abi;
pub mod lifecycle;
pub mod mapping;
pub mod native_trace;
pub mod paths;
pub mod performance_diagnostics;
pub mod progress_diagnostics;
pub mod renderer_policy;
pub mod save_store;
pub mod xex_envelope;
pub use ios_resident_apple_backend::abi;
#[cfg(feature = "fixture-tools")]
pub mod public_fixture;

#[cfg(all(feature = "apple-host", target_os = "ios", target_arch = "aarch64"))]
mod darwin;
#[cfg(all(feature = "apple-host", target_os = "ios", target_arch = "aarch64"))]
mod runtime;
#[cfg(all(feature = "apple-host", target_os = "ios", target_arch = "aarch64"))]
mod xma_callbacks;
#[cfg(all(feature = "apple-host", target_os = "ios", target_arch = "aarch64"))]
pub use runtime::{mark_renderer_attached, prepared_kernel};
#[cfg(all(feature = "metal-renderer", target_os = "ios", target_arch = "aarch64"))]
pub mod renderer;

#[cfg(all(
    feature = "apple-host",
    not(all(target_os = "ios", target_arch = "aarch64"))
))]
compile_error!(
    "apple-host requires plain ARM64 iOS/device or Simulator; default tests are portable"
);
#[cfg(all(feature = "fixture-tools", feature = "metal-renderer", target_os = "ios", target_arch = "aarch64"))]
mod frame_backend_fixture;
