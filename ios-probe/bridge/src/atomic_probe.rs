//! Public synthetic cross-language atomic element probe; no guest execution.
use std::mem::{align_of, size_of};
use std::ptr;
use std::sync::atomic::{AtomicI32, AtomicU8, Ordering};
use std::time::{Duration, Instant};

use super::{ProbeRuntimeResult, RUNTIME_PROBE_LOCK, pointer_check};

const LAYOUT_WORDS: usize = 5;
const EA_INDEX: usize = 4080;
const DIRTY_INDEX: usize = 0xFFF10;
const A: i32 = -0x123_4567;
const B: i32 = 0x2345_6789;
const C: i32 = -0x345_6789;
const D: i32 = 0x4567_89AB;

// The callback is a bounded, no-unwind native worker. It retains no pointer and
// accesses both objects ONLY with compatible atomic operations of their full
// width. Neither object may be accessed by any running guest during this probe.
pub type AtomicCall = unsafe extern "C" fn(*mut i32, *mut u8) -> u32;

fn layout() -> [u32; LAYOUT_WORDS] {
    [
        1,
        size_of::<AtomicI32>() as u32,
        align_of::<AtomicI32>() as u32,
        size_of::<AtomicU8>() as u32,
        align_of::<AtomicU8>() as u32,
    ]
}

/// # Safety
/// Aligned `out` has five writable u32 slots. Null/short/unaligned writes none.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn probe_runtime_atomic_layout(out: *mut u32, capacity: usize) -> i32 {
    if let Err(error) = pointer_check(out) {
        return error;
    }
    if capacity < LAYOUT_WORDS {
        return -2;
    }
    unsafe {
        ptr::copy_nonoverlapping(layout().as_ptr(), out, LAYOUT_WORDS);
    }
    LAYOUT_WORDS as i32
}

struct Restore<'a> {
    ea: &'a AtomicI32,
    dirty: &'a AtomicU8,
    ea_old: i32,
    dirty_old: u8,
}
impl Drop for Restore<'_> {
    fn drop(&mut self) {
        self.ea.store(self.ea_old, Ordering::Release);
        self.dirty.store(self.dirty_old, Ordering::Release);
    }
}

fn roundtrip(call: AtomicCall) -> Result<ProbeRuntimeResult, i32> {
    let _lock = RUNTIME_PROBE_LOCK.lock().map_err(|_| -3)?;
    let ea = &runtime_core::memory::skyrt_ea_offset[EA_INDEX];
    let dirty = &runtime_core::dirty::skyrt_written[DIRTY_INDEX];
    let ea_old = ea.load(Ordering::Acquire);
    let dirty_old = dirty.load(Ordering::Acquire);
    let restore = Restore {
        ea,
        dirty,
        ea_old,
        dirty_old,
    };
    let mut result = ProbeRuntimeResult::new();
    result.check(1, layout() == [1, 4, 4, 1, 1]);
    let ea_address = ea.as_ptr() as usize;
    let dirty_address = dirty.as_ptr() as usize;
    result.check(2, ea_address.is_multiple_of(4));
    if result.failed != 0 {
        return Ok(result);
    }

    ea.store(A, Ordering::Release);
    dirty.store(1, Ordering::Release);
    result.check(
        4,
        ea.load(Ordering::Acquire) == A && dirty.load(Ordering::Acquire) == 1,
    );
    std::thread::scope(|scope| {
        let worker =
            scope.spawn(move || unsafe { call(ea_address as *mut i32, dirty_address as *mut u8) });
        // A failed or unexpectedly returning callback cannot leave this side
        // spinning forever. The documented worker also bounds its own waits.
        let wait_for = |phase: u8| {
            let until = Instant::now() + Duration::from_secs(5);
            loop {
                let current = dirty.load(Ordering::Acquire);
                if current == phase {
                    return true;
                }
                if current == 255 || worker.is_finished() || Instant::now() >= until {
                    // The worker may publish the final phase and finish after
                    // our first load. Re-read after observing completion so a
                    // successful phase 4 cannot be overwritten with abort 255.
                    if dirty.load(Ordering::Acquire) == phase {
                        return true;
                    }
                    dirty.store(255, Ordering::Release);
                    return false;
                }
                std::thread::yield_now();
            }
        };
        let phase_two = wait_for(2);
        result.check(8, phase_two);
        result.check(16, phase_two && ea.load(Ordering::Acquire) == B);
        let exchanged = phase_two
            && ea
                .compare_exchange(B, C, Ordering::AcqRel, Ordering::Acquire)
                .is_ok();
        result.check(32, exchanged);
        if exchanged {
            dirty.store(3, Ordering::Release);
        } else {
            dirty.store(255, Ordering::Release);
        }
        let phase_four = exchanged && wait_for(4);
        result.check(64, phase_four && ea.load(Ordering::Acquire) == D);
        let callback_result = worker.join().unwrap_or(0);
        result.callback_result = callback_result;
        result.check(128, callback_result == 0xF);
    });
    drop(restore);
    result.check(
        256,
        ea.load(Ordering::Acquire) == ea_old && dirty.load(Ordering::Acquire) == dirty_old,
    );
    Ok(result)
}

/// # Safety
/// See the public C header. This test temporarily changes two real process-wide
/// runtime elements; it must never overlap an actual guest. No Swift frame is
/// called from the worker. Callback must finish, never unwind or retain pointers.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn probe_runtime_atomic_roundtrip(
    call: Option<AtomicCall>,
    out: *mut ProbeRuntimeResult,
    capacity: usize,
) -> i32 {
    if let Err(error) = pointer_check(out) {
        return error;
    }
    if capacity < size_of::<ProbeRuntimeResult>() {
        return -2;
    }
    let Some(call) = call else {
        return -5;
    };
    let Ok(answer) = std::panic::catch_unwind(|| roundtrip(call)) else {
        return -3;
    };
    let result = match answer {
        Ok(result) => result,
        Err(error) => return error,
    };
    let status = if result.failed == 0 { 0 } else { -7 };
    unsafe {
        out.write(result);
    }
    status
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn atomic_layout_reports_full_width_representations() {
        let mut words = [0xA5; 7];
        assert_eq!(
            unsafe { probe_runtime_atomic_layout(words.as_mut_ptr().add(1), 5) },
            5
        );
        assert_eq!(words, [0xA5, 1, 4, 4, 1, 1, 0xA5]);
    }

    #[test]
    fn atomic_layout_invalid_outputs_are_untouched() {
        let mut words = [0xA5; 5];
        assert_eq!(
            unsafe { probe_runtime_atomic_layout(ptr::null_mut(), 5) },
            -1
        );
        assert_eq!(
            unsafe { probe_runtime_atomic_layout(words.as_mut_ptr(), 4) },
            -2
        );
        let unaligned = unsafe { words.as_mut_ptr().cast::<u8>().add(1).cast() };
        assert_eq!(unsafe { probe_runtime_atomic_layout(unaligned, 5) }, -4);
        assert_eq!(words, [0xA5; 5]);
    }

    #[test]
    fn missing_atomic_callback_does_not_write_result() {
        let mut result = ProbeRuntimeResult {
            passed: 0xA5,
            ..Default::default()
        };
        assert_eq!(
            unsafe { probe_runtime_atomic_roundtrip(None, &mut result, 32) },
            -5
        );
        assert_eq!(
            unsafe { probe_runtime_atomic_roundtrip(None, &mut result, 31) },
            -2
        );
        assert_eq!(
            unsafe { probe_runtime_atomic_roundtrip(None, ptr::null_mut(), 32) },
            -1
        );
        assert_eq!(result.passed, 0xA5);
    }

    #[test]
    fn two_thread_atomic_protocol_restores_original_runtime_elements() {
        unsafe extern "C" fn worker(ea: *mut i32, dirty: *mut u8) -> u32 {
            let ea = unsafe { AtomicI32::from_ptr(ea) };
            let dirty = unsafe { AtomicU8::from_ptr(dirty) };
            if dirty.load(Ordering::Acquire) != 1 || ea.load(Ordering::Acquire) != A {
                dirty.store(255, Ordering::Release);
                return 0;
            }
            if ea
                .compare_exchange(A, B, Ordering::AcqRel, Ordering::Acquire)
                .is_err()
            {
                dirty.store(255, Ordering::Release);
                return 1;
            }
            dirty.store(2, Ordering::Release);
            let until = Instant::now() + Duration::from_secs(5);
            loop {
                let phase = dirty.load(Ordering::Acquire);
                if phase == 3 {
                    break;
                }
                if phase == 255 || Instant::now() >= until {
                    dirty.store(255, Ordering::Release);
                    return 3;
                }
                std::thread::yield_now();
            }
            if ea.swap(D, Ordering::AcqRel) != C {
                dirty.store(255, Ordering::Release);
                return 7;
            }
            dirty.store(4, Ordering::Release);
            0xF
        }
        let mut result = ProbeRuntimeResult::default();
        assert_eq!(
            unsafe { probe_runtime_atomic_roundtrip(Some(worker), &mut result, 32) },
            0
        );
        assert_eq!(
            (result.passed, result.failed, result.callback_result),
            (0x1FF, 0, 0xF)
        );
    }

    #[test]
    fn unexpectedly_returning_atomic_worker_cannot_pass() {
        unsafe extern "C" fn no_op(_: *mut i32, _: *mut u8) -> u32 {
            0
        }
        let mut result = ProbeRuntimeResult::default();
        assert_eq!(
            unsafe { probe_runtime_atomic_roundtrip(Some(no_op), &mut result, 32) },
            -7
        );
        assert_eq!(
            (result.passed, result.failed, result.callback_result),
            (0x107, 0xF8, 0)
        );
    }
}
