//! Select exactly one complete device implementation for a renderer session.
//! The installed/default backend is unchanged until native conformance passes.
#[cfg(feature="native-metal")]
pub use gpu_native_bridge::*;
#[cfg(not(feature="native-metal"))]
include!("legacy.rs");
