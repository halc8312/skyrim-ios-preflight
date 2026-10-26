//! Typed recording boundary. The live default still owns only WebGPU objects.
//! A future native session must supply all resources from its own backend.
pub mod sink;
pub(crate) mod webgpu;
