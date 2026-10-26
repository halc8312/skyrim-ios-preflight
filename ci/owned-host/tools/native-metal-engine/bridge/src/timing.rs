//! GPU timestamps are not exposed until the native query implementation exists.
//! Callers receive an explicit unsupported capability, never invented timings.
use crate::Device;
#[derive(Clone, Copy, Debug, Default)]
pub struct LabelTime {
    pub nanos: u64,
    pub passes: u64,
}
pub fn enable(_device: &Device) -> bool {
    false
}
pub fn enabled() -> bool {
    false
}
pub fn collect() {}
pub fn take() -> (Vec<(String, LabelTime)>, u64) {
    (Vec::new(), 0)
}
