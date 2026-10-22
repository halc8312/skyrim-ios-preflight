//! Portable attachment ownership, bounds and C result layout.
use std::sync::atomic::{AtomicU32, Ordering};

pub(crate) struct Attempt<'a> {
    state: &'a AtomicU32,
    committed: bool,
}
impl<'a> Attempt<'a> {
    pub(crate) fn begin(state: &'a AtomicU32) -> Result<Self, i32> {
        state
            .compare_exchange(0, 1, Ordering::AcqRel, Ordering::Acquire)
            .map_err(|_| -3)?;
        Ok(Self {
            state,
            committed: false,
        })
    }
    pub(crate) fn commit(mut self) {
        self.state.store(2, Ordering::Release);
        self.committed = true;
    }
}
impl Drop for Attempt<'_> {
    fn drop(&mut self) {
        if !self.committed {
            self.state.store(3, Ordering::Release);
        }
    }
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct OHRendererStatusV1 {
    pub abi_version: u32,
    pub struct_bytes: u32,
    pub attachment: u32,
    pub initialized: u32,
    pub stopped: u32,
    pub failed: u32,
    pub swap_requests: u64,
    pub decoded_frames: u64,
    pub gpu_errors: u64,
    pub unrendered_swaps: u64,
}

pub(crate) fn size_allowed(width: u32, height: u32) -> bool {
    (1..=4096).contains(&width)
        && (1..=4096).contains(&height)
        && u64::from(width) * u64::from(height) <= 16_777_216
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::mem::{align_of, size_of};
    #[test]
    fn layer_dimensions_and_c_result_layout_are_bounded() {
        for (w, h) in [
            (0, 720),
            (1280, 0),
            (4097, 1),
            (1, 4097),
            (u32::MAX, u32::MAX),
        ] {
            assert!(!size_allowed(w, h));
        }
        assert!(size_allowed(1280, 720));
        assert!(size_allowed(4096, 4096));
        assert_eq!(size_of::<OHRendererStatusV1>(), 56);
        assert_eq!(align_of::<OHRendererStatusV1>(), 8);
    }
    #[test]
    fn a_duplicate_caller_cannot_replace_or_fail_an_inflight_attachment() {
        let state = AtomicU32::new(0);
        let owner = Attempt::begin(&state).unwrap();
        assert!(Attempt::begin(&state).is_err());
        assert_eq!(state.load(Ordering::Acquire), 1);
        owner.commit();
        assert!(Attempt::begin(&state).is_err());
        assert_eq!(state.load(Ordering::Acquire), 2);
        let failed = AtomicU32::new(0);
        {
            let _owner = Attempt::begin(&failed).unwrap();
        }
        assert_eq!(failed.load(Ordering::Acquire), 3);
        assert!(Attempt::begin(&failed).is_err());
    }
}
