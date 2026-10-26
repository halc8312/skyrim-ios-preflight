//! Shared CPU contracts. No GPU API, device objects, or private game input.
pub mod sink;
pub mod pipeline;
pub mod constants;
pub mod edram_shaders;
pub mod edram_ownership;
pub mod edram_transfer;
pub mod presentation;
pub mod edram_resolve;
pub mod constant_segments;

pub mod edram_clear;
