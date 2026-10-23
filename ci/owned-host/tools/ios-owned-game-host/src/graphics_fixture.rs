//! Simulator fixture-tools only. Does not prepare/start a guest or borrow any
//! renderer/layer. Register this module only for fixture-tools+metal-renderer
//! on plain ARM64 iOS; the SDK test owns an independent process deadline.
use std::panic::{AssertUnwindSafe, catch_unwind};

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct OHGraphicsPresentationProbeV1 {
    pub version: u32,
    pub bytes: u32,
    pub backend: u32,
    pub reserved: u32,
    pub checks: u64,
    pub rendered_cases: u64,
}
const _: () = assert!(std::mem::size_of::<OHGraphicsPresentationProbeV1>() == 32);

/// # Safety
/// `out` must be exclusively writable/aligned for the complete 32-byte result.
/// No output bytes are written on failure. Caller enforces its process deadline.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn OHRunPublicGraphicsProbeV1(
    out: *mut OHGraphicsPresentationProbeV1,
    bytes: usize,
) -> i32 {
    if out.is_null()
        || bytes != std::mem::size_of::<OHGraphicsPresentationProbeV1>()
        || !(out as usize).is_multiple_of(std::mem::align_of::<OHGraphicsPresentationProbeV1>())
    {
        return -1;
    }
    match catch_unwind(AssertUnwindSafe(
        xenos_render::native::run_public_presentation_probe,
    )) {
        Ok(Ok(value)) => {
            let result = OHGraphicsPresentationProbeV1 {
                version: 0x0001_0000,
                bytes: 32,
                backend: value.backend,
                reserved: 0,
                checks: value.checks,
                rendered_cases: value.rendered_cases,
            };
            unsafe {
                out.write(result);
            }
            0
        }
        Ok(Err(category)) => -100 - (category as i32),
        Err(_) => -2,
    }
}
