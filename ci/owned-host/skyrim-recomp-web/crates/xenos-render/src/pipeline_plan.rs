//! Compatibility path; legacy projection oracle remains in this crate.
pub use xenos_gfx::pipeline::*;

#[cfg(test)]
#[path = "pipeline_plan_tests.rs"]
mod tests;
