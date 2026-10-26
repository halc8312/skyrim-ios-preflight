//! Diagnostic-only current-thread CPU clock. Never substitute elapsed wall time
//! when the OS cannot supply CPU time. No GPU time is measured here.
use std::marker::PhantomData;
use std::rc::Rc;

pub(crate) struct Start {
    nanos: u64,
    // A sample may only be completed on the thread on which it was started.
    _thread: PhantomData<Rc<()>>,
}
impl Start {
    pub fn now() -> Option<Self> {
        Some(Self { nanos: now_ns()?, _thread: PhantomData })
    }
    pub fn elapsed_ns(&self) -> Option<u64> { now_ns()?.checked_sub(self.nanos) }
}

#[cfg(any(target_os = "linux", target_os = "android", target_vendor = "apple"))]
fn now_ns() -> Option<u64> {
    let mut time = libc::timespec { tv_sec: 0, tv_nsec: 0 };
    // SAFETY: a valid writable timespec, using the calling thread's CPU clock.
    if unsafe { libc::clock_gettime(libc::CLOCK_THREAD_CPUTIME_ID, &mut time) } != 0 { return None; }
    let seconds = u64::try_from(time.tv_sec).ok()?;
    let nanos = u64::try_from(time.tv_nsec).ok()?;
    if nanos >= 1_000_000_000 { return None; }
    seconds.checked_mul(1_000_000_000)?.checked_add(nanos)
}

#[cfg(target_os = "windows")]
fn now_ns() -> Option<u64> {
    #[repr(C)]
    #[derive(Default)]
    struct FileTime { low: u32, high: u32 }
    #[link(name = "kernel32")]
    unsafe extern "system" {
        fn GetCurrentThread() -> *mut std::ffi::c_void;
        fn GetThreadTimes(thread: *mut std::ffi::c_void, created: *mut FileTime,
            exited: *mut FileTime, kernel: *mut FileTime, user: *mut FileTime) -> i32;
    }
    let (mut created, mut exited, mut kernel, mut user) =
        (FileTime::default(), FileTime::default(), FileTime::default(), FileTime::default());
    // SAFETY: valid writable structures and the documented current-thread pseudo-handle.
    if unsafe { GetThreadTimes(GetCurrentThread(), &mut created, &mut exited, &mut kernel, &mut user) } == 0 { return None; }
    let ticks = |t: FileTime| (u64::from(t.high) << 32) | u64::from(t.low);
    ticks(kernel).checked_add(ticks(user))?.checked_mul(100)
}

#[cfg(not(any(target_os = "linux", target_os = "android", target_vendor = "apple", target_os = "windows")))]
fn now_ns() -> Option<u64> { None }

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn sampled_work_uses_a_monotonic_cpu_clock_without_assuming_scheduler_timing() {
        let Some(start) = Start::now() else { return };
        let mut value = 1u64;
        for i in 0..100_000 { value = std::hint::black_box(value.wrapping_mul(17).wrapping_add(i)); }
        std::hint::black_box(value);
        let elapsed = start.elapsed_ns().expect("clock remains available");
        assert!(start.elapsed_ns().unwrap() >= elapsed);
    }
}
