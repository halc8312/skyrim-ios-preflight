//! Test-only public offscreen probe. Root registration is restricted to
//! fixture-tools + metal-renderer + iOS; it never uses a guest or a layer.
use std::panic::{AssertUnwindSafe, catch_unwind};

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct OHAlphaCoverageProbeV1 {
    pub version: u32,
    pub bytes: u32,
    pub backend: u32,
    pub reserved: u32,
    pub checks: u64,
    pub rendered_cases: u64,
}
const _: () = assert!(std::mem::size_of::<OHAlphaCoverageProbeV1>() == 32);

/// # Safety
/// `out` must exclusively own a complete writable/aligned 32-byte result.
/// Error paths leave it untouched. Caller retains its process deadline.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn OHFixtureAlphaCoverageProbeV1(
    out: *mut OHAlphaCoverageProbeV1,
    bytes: usize,
) -> i32 {
    if out.is_null()
        || bytes != std::mem::size_of::<OHAlphaCoverageProbeV1>()
        || !(out as usize).is_multiple_of(std::mem::align_of::<OHAlphaCoverageProbeV1>())
    {
        return -1;
    }
    match catch_unwind(AssertUnwindSafe(
        xenos_render::debug_alpha_coverage::run_public_alpha_coverage_probe,
    )) {
        Ok(Ok(value)) => {
            let result = OHAlphaCoverageProbeV1 {
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
