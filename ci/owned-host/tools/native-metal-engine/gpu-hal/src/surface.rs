//! Display lifetime control shared by the UI hand-off and the native owner.
//! A queued presentation carries an epoch; resize/background invalidates it.
use crate::*;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SurfaceEpoch {
    identity: u64,
    generation: u64,
    size: [u32; 2],
}
pub struct SurfaceState {
    identity: u64,
    generation: u64,
    size: [u32; 2],
    active: bool,
}
impl SurfaceState {
    pub fn new() -> Result<Self> {
        Ok(Self {
            identity: id()?,
            generation: 0,
            size: [0; 2],
            active: false,
        })
    }
    pub fn configure(&mut self, size: [u32; 2], active: bool) -> Result<()> {
        if size.iter().any(|&s| s > 8192) {
            return Err(Error::Rejected("native surface dimensions"));
        }
        if self.size != size || self.active != active {
            let generation = self
                .generation
                .checked_add(1)
                .ok_or(Error::Rejected("native surface generation exhausted"))?;
            self.generation = generation;
            self.size = size;
            self.active = active;
        }
        Ok(())
    }
    pub fn epoch(&self) -> Option<SurfaceEpoch> {
        (self.active && self.size.iter().all(|&s| s != 0)).then_some(SurfaceEpoch {
            identity: self.identity,
            generation: self.generation,
            size: self.size,
        })
    }
    pub fn accepts(&self, epoch: SurfaceEpoch) -> bool {
        self.epoch() == Some(epoch)
    }
}
#[cfg(target_vendor = "apple")]
mod metal;
#[cfg(target_vendor = "apple")]
pub use metal::{MetalSurface, MetalFrame};

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn display_epoch_invalidates_queued_work_on_resize_background_and_recreate() {
        let mut s = SurfaceState::new().unwrap();
        assert_eq!(s.epoch(), None);
        s.configure([1920, 1080], true).unwrap();
        let a = s.epoch().unwrap();
        let mut other = SurfaceState::new().unwrap();
        other.configure([1920, 1080], true).unwrap();
        assert!(!other.accepts(a));
        s.configure([1920, 1080], true).unwrap();
        assert!(s.accepts(a));
        s.configure([1080, 1920], true).unwrap();
        let b = s.epoch().unwrap();
        assert!(!s.accepts(a));
        s.configure([1080, 1920], false).unwrap();
        assert_eq!(s.epoch(), None);
        assert!(!s.accepts(b));
        s.configure([1080, 1920], true).unwrap();
        let c = s.epoch().unwrap();
        assert_ne!(b, c);
        assert!(s.configure([9000, 1], true).is_err());
        assert!(s.accepts(c));
        s.configure([0, 0], true).unwrap();
        assert_eq!(s.epoch(), None);
        s.configure([1080, 1920], true).unwrap();
        assert_ne!(s.epoch().unwrap(), c);
    }
    #[test]
    fn exhausted_display_generation_keeps_last_state() {
        let mut s = SurfaceState {
            identity: id().unwrap(),
            generation: u64::MAX,
            size: [1, 1],
            active: true,
        };
        let epoch = s.epoch().unwrap();
        assert!(s.configure([2, 1], true).is_err());
        assert!(s.accepts(epoch));
    }
}
