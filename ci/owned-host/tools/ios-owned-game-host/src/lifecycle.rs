//! Numeric process state, independent of a guest pointer or UI lifetime.
use crate::Check;
use std::sync::atomic::{AtomicU32, Ordering};
pub const VERSION: u32 = 0x10000;
pub const FRESH: u32 = 0;
pub const PREPARING: u32 = 1;
pub const PREPARED: u32 = 2;
pub const RENDERER_ATTACHED: u32 = 3;
pub const RUNNING: u32 = 4;
pub const EXITED: u32 = 5;
pub const FAILED: u32 = 6;
pub const MAX_DIAGNOSTIC_SECONDS: u32 = 600;
pub struct Lifecycle(AtomicU32);
impl Default for Lifecycle {
    fn default() -> Self {
        Self(AtomicU32::new(FRESH))
    }
}
impl Lifecycle {
    pub const fn new() -> Self {
        Self(AtomicU32::new(FRESH))
    }
    pub fn state(&self) -> u32 {
        self.0.load(Ordering::Acquire)
    }
    pub fn begin_prepare(&self) -> Check<()> {
        self.transition(FRESH, PREPARING)
    }
    pub fn prepared(&self) -> Check<()> {
        self.transition(PREPARING, PREPARED)
    }
    pub fn renderer_attached(&self) -> Check<()> {
        self.transition(PREPARED, RENDERER_ATTACHED)
    }
    pub fn start(&self, seconds: u32) -> Check<()> {
        validate_duration(seconds)?;
        self.transition(RENDERER_ATTACHED, RUNNING)
    }
    pub fn exited(&self) -> Check<()> {
        self.transition(RUNNING, EXITED)
    }
    pub fn fail(&self) {
        self.0.store(FAILED, Ordering::Release)
    }
    fn transition(&self, old: u32, new: u32) -> Check<()> {
        self.0
            .compare_exchange(old, new, Ordering::AcqRel, Ordering::Acquire)
            .map(|_| ())
            .map_err(|_| "process host state refuses this transition")
    }
}
pub fn validate_duration(seconds: u32) -> Check<()> {
    if (1..=MAX_DIAGNOSTIC_SECONDS).contains(&seconds) {
        Ok(())
    } else {
        Err("diagnostic guest duration must be within 1..600 seconds")
    }
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct Status {
    pub version: u32,
    pub bytes: u32,
    pub state: u32,
    pub policy: u32,
    pub failure: u32,
    pub missing_module: u32,
    pub missing_ordinal: u32,
    pub exit_code: u32,
    pub function_imports: u64,
    pub registered_functions: u64,
    pub variable_imports: u64,
    pub supported_variables: u64,
    pub elapsed_ms: u64,
    pub function_count: u64,
    /// All remain zero: content reconstruction is not any of these assertions.
    pub ready_to_play: u32,
    pub signature_authenticated: u32,
    pub iphone_game_verified: u32,
    pub reserved: u32,
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn ordering_and_one_shot() {
        assert_eq!(std::mem::size_of::<Status>(), 96);
        assert_eq!(std::mem::align_of::<Status>(), 8);
        let s = Lifecycle::default();
        assert!(s.start(5).is_err());
        s.begin_prepare().unwrap();
        assert!(s.begin_prepare().is_err());
        assert!(s.renderer_attached().is_err());
        s.prepared().unwrap();
        assert!(s.start(5).is_err());
        s.renderer_attached().unwrap();
        assert!(s.start(0).is_err());
        assert_eq!(s.state(), RENDERER_ATTACHED);
        s.start(600).unwrap();
        assert!(s.start(5).is_err());
        s.exited().unwrap();
        assert!(s.begin_prepare().is_err());
    }
    #[test]
    fn failed_is_terminal_and_duration_is_bounded() {
        for n in [0, 601, u32::MAX] {
            assert!(validate_duration(n).is_err());
        }
        for n in [1, 5, 120, 600] {
            validate_duration(n).unwrap();
        }
        let s = Lifecycle::default();
        s.begin_prepare().unwrap();
        s.fail();
        assert!(s.prepared().is_err());
        assert!(s.begin_prepare().is_err());
    }
}
