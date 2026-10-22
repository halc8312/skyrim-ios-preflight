//! Experimental, one-process owned guest host. Pure default tests do not load
//! an Apple image. A matching hash is content binding, not signature authority,
//! HLE completeness, game compatibility or permission to execute on iOS.
pub type Check<T> = Result<T, &'static str>;
pub mod binding;
pub mod lifecycle;
pub mod mapping;
pub mod paths;
pub mod renderer_policy;
pub mod xex_envelope;
pub use ios_resident_apple_backend::abi;
#[cfg(feature = "fixture-tools")]
pub mod public_fixture;

#[cfg(all(feature = "apple-host", target_os = "ios", target_arch = "aarch64"))]
mod darwin;
#[cfg(all(feature = "apple-host", target_os = "ios", target_arch = "aarch64"))]
mod runtime;
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
