//! Separate public-only Apple resident-adapter experiment. Default tests parse
//! self-authored metadata only; they do not load an Apple image or execute iOS.
pub mod abi;
pub mod macho;
pub mod policy;

pub type Check<T> = Result<T, String>;

#[cfg(all(feature = "apple-runtime", target_os = "ios", target_arch = "aarch64"))]
mod darwin;
#[cfg(all(feature = "apple-runtime", target_os = "ios", target_arch = "aarch64"))]
mod runtime;
#[cfg(all(feature = "apple-runtime", target_os = "ios", target_arch = "aarch64"))]
#[path = "../../src/synthetic.rs"]
pub mod synthetic;

#[cfg(all(
    feature = "apple-runtime",
    not(all(target_os = "ios", target_arch = "aarch64"))
))]
compile_error!(
    "apple-runtime supports plain ARM64 iOS/device or iOS/Simulator only; native pure parser/policy tests use no feature"
);
