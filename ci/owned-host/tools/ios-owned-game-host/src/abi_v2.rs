//! Explicit mandatory-MMIO host revision. The shared V1 ABI remains unchanged.
//! V2 requirements and memory retain their V1 layouts; only the host callback
//! table grows. The Apple SDK's independent 51-word query must match before use.

pub use crate::abi::{KernelCall, PGMemoryV1, PGRequirementsV1 as PGRequirementsV2};
use std::{
    ffi::c_void,
    mem::{align_of, offset_of, size_of},
};

pub const VERSION: u32 = 0x0002_0000;
pub const CONTRACTS: u32 = 0x3f;
pub type XmaReadU32 = unsafe extern "C" fn(*mut c_void, u32, *mut u32) -> i32;
pub type XmaWriteU32 = unsafe extern "C" fn(*mut c_void, u32, u32) -> i32;

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct PGHostCallbacksV2 {
    pub abi_version: u32,
    pub struct_size: u32,
    pub contracts: u32,
    pub jump_abi: u32,
    pub context_layout: [u64; 20],
    pub jump_buffer_size: u64,
    pub jump_buffer_alignment: u64,
    pub jump_pair_cookie: u64,
    pub unwind_abi: u32,
    pub unwind_reserved: u32,
    pub unwind_pair_cookie: u64,
    pub user: *mut c_void,
    pub memory: PGMemoryV1,
    pub timebase: Option<unsafe extern "C" fn(*mut c_void) -> u64>,
    pub kernel_call: Option<KernelCall>,
    pub fatal: Option<unsafe extern "C" fn(*mut c_void, u32, u64)>,
    pub copy_memory: Option<unsafe extern "C" fn(*mut c_void, *const c_void, usize) -> *mut c_void>,
    pub zero_memory: Option<unsafe extern "C" fn(*mut c_void, usize)>,
    pub exp2_float: Option<unsafe extern "C" fn(f32) -> f32>,
    pub log2_float: Option<unsafe extern "C" fn(f32) -> f32>,
    pub native_setjmp: Option<unsafe extern "C" fn(*mut c_void) -> i32>,
    pub native_longjmp: Option<unsafe extern "C" fn(*mut c_void, i32)>,
    pub native_personality:
        Option<unsafe extern "C" fn(i32, i32, u64, *mut c_void, *mut c_void) -> i32>,
    pub native_unwind_resume: Option<unsafe extern "C" fn(*mut c_void)>,
    pub stack_guard_seed: usize,
    pub xma_read_u32: Option<XmaReadU32>,
    pub xma_write_u32: Option<XmaWriteU32>,
    pub reserved: [u64; 4],
}

pub type BindFn = unsafe extern "C" fn(*const PGHostCallbacksV2, u32) -> i32;

pub fn layout51() -> [u64; 51] {
    macro_rules! layout {
        ($t:ty; $($f:ident),+) => {
            [size_of::<$t>() as u64, align_of::<$t>() as u64, $(offset_of!($t, $f) as u64),+]
        };
    }
    let requirements = layout!(PGRequirementsV2;
        abi_version, struct_size, pointer_bits, little_endian, plain_arm64,
        reserved, context_layout, jump_buffer_size, jump_buffer_alignment,
        ea_offset_count, written_count);
    let memory = layout!(PGMemoryV1;
        ea_offsets, ea_offset_count, written, written_count, guest_base, guest_bytes);
    let host = layout!(PGHostCallbacksV2;
        abi_version, struct_size, contracts, jump_abi, context_layout,
        jump_buffer_size, jump_buffer_alignment, jump_pair_cookie, unwind_abi,
        unwind_reserved, unwind_pair_cookie, user, memory, timebase, kernel_call,
        fatal, copy_memory, zero_memory, exp2_float, log2_float, native_setjmp,
        native_longjmp, native_personality, native_unwind_resume, stack_guard_seed,
        xma_read_u32, xma_write_u32, reserved);
    let mut all = [0; 51];
    all[..13].copy_from_slice(&requirements);
    all[13..21].copy_from_slice(&memory);
    all[21..].copy_from_slice(&host);
    all
}

#[cfg(target_pointer_width = "64")]
const _: () = {
    assert!(size_of::<PGRequirementsV2>() == 216);
    assert!(size_of::<PGMemoryV1>() == 48);
    assert!(size_of::<PGHostCallbacksV2>() == 416);
    assert!(offset_of!(PGHostCallbacksV2, xma_read_u32) == 368);
    assert!(offset_of!(PGHostCallbacksV2, xma_write_u32) == 376);
    assert!(offset_of!(PGHostCallbacksV2, reserved) == 384);
};

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    #[cfg(target_pointer_width = "64")]
    fn v2_layout_keeps_v1_unchanged_and_matches_all_51_words() {
        assert_eq!(size_of::<crate::abi::PGHostCallbacksV1>(), 400);
        assert_eq!(offset_of!(crate::abi::PGHostCallbacksV1, reserved), 368);
        assert_eq!(crate::abi::layout49().len(), 49);
        assert_eq!(
            layout51(),
            [
                216, 8, 0, 4, 8, 12, 16, 20, 24, 184, 192, 200, 208, 48, 8, 0, 8, 16, 24, 32, 40,
                416, 8, 0, 4, 8, 12, 16, 176, 184, 192, 200, 204, 208, 216, 224, 272, 280, 288,
                296, 304, 312, 320, 328, 336, 344, 352, 360, 368, 376, 384,
            ]
        );
    }
}
