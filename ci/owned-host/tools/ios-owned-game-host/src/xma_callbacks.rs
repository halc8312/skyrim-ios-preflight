//! Synchronous V2 host callbacks for the retained candidate Kernel.
//!
//! These functions use the supplied binding user, never Kernel::global(). The
//! user must originate from retained_kernel_user and remain trusted throughout
//! the binding. Null/alignment checks cannot establish an arbitrary pointer's
//! provenance or lifetime. The caller also owns valid, non-aliasing read output.
//!
//! A captured operand goes directly to the existing candidate MMIO operation.
//! No register polling, decoder substitution or new shutdown policy is added.

use kernel_hle::Kernel;
use std::{
    ffi::c_void,
    mem::align_of,
    panic::{AssertUnwindSafe, catch_unwind},
};

pub const UNHANDLED: i32 = 0;
pub const HANDLED: i32 = 1;
pub const INVALID_ARGUMENT: i32 = -1;
pub const DEVICE_ERROR: i32 = -2;
pub const PANIC: i32 = -3;

/// A borrowed process-retained handle; ownership is never transferred to C.
pub fn retained_kernel_user(kernel: &'static Kernel) -> *mut c_void {
    (kernel as *const Kernel).cast_mut().cast()
}

fn is_mmio_word(address: u32) -> bool {
    (0x7fea_0000..0x7feb_0000).contains(&address) && address & 3 == 0
}

fn valid_user_shape(user: *mut c_void) -> bool {
    !user.is_null() && (user as usize) % align_of::<Kernel>() == 0
}

/// Contain unwind-mode Rust panics before returning through a plain C ABI.
/// This cannot recover from panic=abort, invalid pointers or process faults.
fn contain_unwind<T>(operation: impl FnOnce() -> T) -> Result<T, i32> {
    match catch_unwind(AssertUnwindSafe(operation)) {
        Ok(value) => Ok(value),
        Err(payload) => {
            // A user-defined panic payload can itself panic on drop. Retain it
            // on this terminal error path so a second unwind cannot escape C.
            std::mem::forget(payload);
            Err(PANIC)
        }
    }
}

/// Return one device-logical LE word; the C++ bridge owns PPC byte swapping.
///
/// # Safety
/// After shape validation, user must identify the retained Kernel supplied by
/// validated binding. out_word must identify writable u32 storage, independent
/// of the Kernel and any concurrently accessed storage, for this call. The
/// output is untouched on fallback, validation error or caught panic.
pub unsafe extern "C" fn xma_read_u32(user: *mut c_void, address: u32, out_word: *mut u32) -> i32 {
    if !valid_user_shape(user)
        || !is_mmio_word(address)
        || out_word.is_null()
        || (out_word as usize) % align_of::<u32>() != 0
    {
        return INVALID_ARGUMENT;
    }
    // SAFETY: The process-retained binding lifetime/provenance is the caller's
    // contract; the checks above only reject observable malformed arguments.
    let kernel: &'static Kernel = unsafe { &*user.cast::<Kernel>() };
    match contain_unwind(|| kernel.xma_mmio_read(address)) {
        Ok(Some(word)) => {
            // SAFETY: Valid writable, non-aliasing output is the C contract.
            unsafe { out_word.write(word) };
            HANDLED
        }
        Ok(None) => UNHANDLED,
        Err(status) => status,
    }
}

/// Forward exactly one captured device-logical LE store operand.
///
/// # Safety
/// After shape validation, user must identify the process-retained Kernel
/// supplied by validated binding. Callback completion does not mean queued
/// decode completion; the unchanged candidate defines that separate contract.
pub unsafe extern "C" fn xma_write_u32(user: *mut c_void, address: u32, word: u32) -> i32 {
    if !valid_user_shape(user) || !is_mmio_word(address) {
        return INVALID_ARGUMENT;
    }
    // SAFETY: See the retained binding contract above; no global is consulted.
    let kernel: &'static Kernel = unsafe { &*user.cast::<Kernel>() };
    match contain_unwind(|| kernel.xma_mmio_write(address, word)) {
        Ok(Ok(true)) => HANDLED,
        Ok(Ok(false)) => UNHANDLED,
        Ok(Err(_)) => DEVICE_ERROR,
        Err(status) => status,
    }
}

// The fields remain plain C function pointers, not C-unwind or coerced bools.
const _: crate::abi_v2::XmaReadU32 = xma_read_u32;
const _: crate::abi_v2::XmaWriteU32 = xma_write_u32;
