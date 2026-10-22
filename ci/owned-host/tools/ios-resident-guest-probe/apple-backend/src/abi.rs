//! Mirror of the shared public PrivateGuestABI.h. Apple SDK NativeHost returns
//! all 49 size/alignment/field-offset words; runtime refuses mismatch before
//! passing these structs to C. Native opaque jmp_buf is never defined in Rust.
use std::{
    ffi::c_void,
    mem::{align_of, offset_of, size_of},
};

pub const VERSION: u32 = 0x0001_0000;
pub const CONTRACTS: u32 = 0x1f;
pub const APPLE_JUMP: u32 = 0x414a_0001;
pub const APPLE_UNWIND: u32 = 0x4155_0001;
pub const OK: i32 = 0;
pub const BUSY: i32 = -8;
pub const CONFIG: [u32; 4] = [0x82000000, 0x2000, 0x82001000, 0x10];
pub const FUNCTIONS: [u32; 3] = [0x82001000, 0x82001004, 0x82001008];

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct PGRequirementsV1 {
    pub abi_version: u32,
    pub struct_size: u32,
    pub pointer_bits: u32,
    pub little_endian: u32,
    pub plain_arm64: u32,
    pub reserved: u32,
    pub context_layout: [u64; 20],
    pub jump_buffer_size: u64,
    pub jump_buffer_alignment: u64,
    pub ea_offset_count: u64,
    pub written_count: u64,
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct PGMemoryV1 {
    pub ea_offsets: *const i32,
    pub ea_offset_count: u64,
    pub written: *mut u8,
    pub written_count: u64,
    pub guest_base: *mut u8,
    pub guest_bytes: u64,
}
pub type KernelCall =
    unsafe extern "C-unwind" fn(*mut c_void, u32, u32, *mut c_void, *mut u8) -> i32;
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct PGHostCallbacksV1 {
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
    pub reserved: [u64; 4],
}
pub type RequirementsFn = unsafe extern "C" fn(*mut PGRequirementsV1, u32) -> i32;
pub type BindFn = unsafe extern "C" fn(*const PGHostCallbacksV1, u32) -> i32;
pub type PublishFn = unsafe extern "C" fn(*const PGMemoryV1, u32) -> i32;
pub type InitFn = unsafe extern "C" fn(*mut c_void, u64) -> i32;
pub type CallFn = unsafe extern "C-unwind" fn(u32, *mut c_void, u64) -> i32;
pub type ConfigFn = unsafe extern "C" fn(*mut u64);
pub type FunctionsFn = unsafe extern "C" fn(*mut usize) -> *const u32;

pub fn layout49() -> [u64; 49] {
    macro_rules! layout {($t:ty;$($f:ident),+)=>{[size_of::<$t>() as u64,align_of::<$t>() as u64,$(offset_of!($t,$f) as u64),+]};}
    let a = layout!(PGRequirementsV1;abi_version,struct_size,pointer_bits,little_endian,plain_arm64,reserved,context_layout,jump_buffer_size,jump_buffer_alignment,ea_offset_count,written_count);
    let b =
        layout!(PGMemoryV1;ea_offsets,ea_offset_count,written,written_count,guest_base,guest_bytes);
    let c = layout!(PGHostCallbacksV1;abi_version,struct_size,contracts,jump_abi,context_layout,jump_buffer_size,jump_buffer_alignment,jump_pair_cookie,unwind_abi,unwind_reserved,unwind_pair_cookie,user,memory,timebase,kernel_call,fatal,copy_memory,zero_memory,exp2_float,log2_float,native_setjmp,native_longjmp,native_personality,native_unwind_resume,stack_guard_seed,reserved);
    let mut all = [0; 49];
    all[..13].copy_from_slice(&a);
    all[13..21].copy_from_slice(&b);
    all[21..].copy_from_slice(&c);
    all
}
#[cfg(feature = "apple-runtime")]
pub fn context_layout() -> [u64; 20] {
    use runtime_core::context::offsets as o;
    [
        o::R3,
        o::R0,
        o::R1,
        o::R2,
        o::R4,
        o::R13,
        o::R31,
        o::LR,
        o::CTR,
        o::XER,
        o::RESERVED,
        o::MSR,
        o::CR0,
        o::FPSCR,
        o::F0,
        o::F1,
        o::V0,
        o::V127,
        o::SIZE,
        o::ALIGN,
    ]
    .map(|v| v as u64)
}
const _: () = {
    use std::sync::atomic::{AtomicI32, AtomicU8};
    assert!(size_of::<AtomicI32>() == 4 && align_of::<AtomicI32>() == 4);
    assert!(size_of::<AtomicU8>() == 1 && align_of::<AtomicU8>() == 1);
    assert!(cfg!(target_has_atomic = "32") && cfg!(target_has_atomic = "8"));
};
