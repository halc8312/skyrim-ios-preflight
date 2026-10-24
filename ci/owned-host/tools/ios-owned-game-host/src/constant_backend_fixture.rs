//! Public-only vertical slice. Never attaches to or switches the game's renderer.
use std::ffi::c_void;
use std::panic::{AssertUnwindSafe, catch_unwind};
use xenos_render::backend_probe::{self, Backend, Batch, Times, WebGpuBackend};
use gpu_webgpu::{Instance, RequestAdapterOptions, DeviceDescriptor, BackendType};

unsafe extern "C" {
    fn OHMetalConstantCreateV1() -> *mut c_void;
    fn OHMetalConstantDestroyV1(context: *mut c_void) -> i32;
    fn OHMetalConstantLimitsV1(out: *mut u64, count: usize) -> i32;
    fn OHMetalConstantPrepareV1(context: *mut c_void, bytes: usize) -> i32;
    fn OHMetalConstantRunV1(context: *mut c_void, words: *const u32, word_count: usize,
        values: *const u8, value_bytes: usize, output: *mut u8, output_bytes: usize, times: *mut Times) -> i32;
}
// The raw pointer's !Send/!Sync is intentional: context creation/use/destruction stay on this thread.
struct DirectMetal(*mut c_void);
impl Drop for DirectMetal { fn drop(&mut self) {
    if !self.0.is_null() { let status = unsafe { OHMetalConstantDestroyV1(self.0) };
        if status != 0 { eprintln!("PUBLIC_METAL_CONSTANT_DESTROY {status}"); } }
} }
impl Backend for DirectMetal {
    fn prepare(&mut self, batch: &Batch) -> Result<(), String> {
        if unsafe { OHMetalConstantPrepareV1(self.0, batch.expected.len()) } != 0 { return Err("Metal reset failed".into()); }
        Ok(())
    }
    fn reset(&mut self, batch: &Batch) -> Result<(), String> { self.prepare(batch) }
    fn run(&mut self, batch: &Batch) -> Result<(Vec<u8>, Times), String> {
        let mut output = vec![0u8; batch.expected.len()]; let mut times = Times::default();
        let result = unsafe { OHMetalConstantRunV1(self.0, batch.words.as_ptr(), batch.words.len(),
            batch.values.as_ptr(), batch.values.len(), output.as_mut_ptr(), output.len(), &mut times) };
        if result != 0 { return Err(format!("Metal execute failed {result}")); }
        Ok((output, times))
    }
}

#[repr(C)]
#[derive(Default)]
pub struct Comparison { version:u32, bytes:u32, backend_a:u32, backend_b:u32,
    cases:u64, samples:u64, compared_bytes:u64, flags:u64, reserved:u64 }
const _: () = assert!(std::mem::size_of::<Comparison>() == 56);

fn native_guards(metal: &mut DirectMetal) -> Result<(), String> {
    if unsafe { OHMetalConstantPrepareV1(metal.0, 0) } != -1
        || unsafe { OHMetalConstantPrepareV1(metal.0, (backend_probe::limits()[6]+1) as usize) } != -1
        || unsafe { OHMetalConstantPrepareV1(metal.0, 16) } != 0 { return Err("native prepare guards".into()); }
    let mut records = [0u32;16];
    for stage in 0..5 { records[1+stage*3] = 16; }
    let mut output = [0x11u8;32];
    let sentinel = Times { total_ns:77, ..Default::default() }; let mut times = sentinel;
    let status = unsafe { OHMetalConstantRunV1(metal.0, records.as_ptr(), 16, std::ptr::null(), 0,
        output.as_mut_ptr(), 32, &mut times) };
    if status != -1 || output != [0x11;32] || times != sentinel { return Err("native mismatch guard".into()); }
    let unaligned = unsafe { records.as_ptr().cast::<u8>().add(1).cast::<u32>() };
    let status = unsafe { OHMetalConstantRunV1(metal.0, unaligned, 16, std::ptr::null(), 0,
        output.as_mut_ptr(), 16, &mut times) };
    if status != -1 || output != [0x11;32] || times != sentinel { return Err("native alignment guard".into()); }
    let pointer = metal.0 as usize;
    // Intentional foreign-thread refusal test: owner remains alive, untouched and joins the reader.
    let wrong_thread = std::thread::spawn(move || unsafe { OHMetalConstantPrepareV1(pointer as *mut c_void, 16) })
        .join().map_err(|_| "foreign-thread guard panicked")?;
    if wrong_thread != -1 { return Err("native owner-thread guard".into()); }
    let status = unsafe { OHMetalConstantRunV1(metal.0, records.as_ptr(), 16, std::ptr::null(), 0,
        output.as_mut_ptr(), 16, &mut times) };
    if status != 0 || output[..16] != [0xa5;16] { return Err("native empty-input or null-values guard".into()); }
    Ok(())
}

fn execute() -> Result<Comparison, String> {
    let mut limits = [0u64; 7];
    if unsafe { OHMetalConstantLimitsV1(limits.as_mut_ptr(), limits.len()) } != 0
        || limits != backend_probe::limits() { return Err("native constant ABI limits mismatch".into()); }
    let instance = Instance::new().map_err(|e| format!("instance {e:?}"))?;
    let adapter = instance.block_on(instance.request_adapter(&RequestAdapterOptions::default()))
        .map_err(|e| format!("adapter {e:?}"))?;
    // Refuse another backend: software Vulkan is useful locally but is not the Apple comparison.
    if adapter.info().backend != BackendType::Metal { return Err("actual Metal required".into()); }
    let device = instance.block_on(adapter.request_device(&DeviceDescriptor::default())).map_err(|e| format!("device {e:?}"))?;
    let mut web = WebGpuBackend::new(device);
    let mut metal = DirectMetal(unsafe { OHMetalConstantCreateV1() });
    if metal.0.is_null() { return Err("native Metal pipeline unavailable".into()); }
    native_guards(&mut metal)?;
    let report = backend_probe::compare(&mut web, &mut metal)?;
    let mut result = Comparison { version:0x10000, bytes:56, backend_a:1, backend_b:2,
        cases:report.cases as u64, samples:report.samples.len() as u64, flags:7, ..Default::default() };
    for sample in report.samples {
        let (records, backend, t) = (sample.records, sample.backend, sample.times);
        result.compared_bytes += sample.bytes as u64;
        // Complete numeric sample set, including slow results; no chosen-winner reporting.
        eprintln!("PUBLIC_CONSTANT_BACKEND_SAMPLE {}", serde_json::json!({
            "records":records,"backend":backend+1,"upload_ns":t.upload_ns,"encode_ns":t.encode_ns,
            "finish_ns":t.finish_ns,"submit_ns":t.submit_ns,"completion_ns":t.completion_ns,
            "readback_ns":t.readback_ns,"total_ns":t.total_ns,"bytes":sample.bytes,
            "sample_index":sample.ordinal,"start_ns":sample.start_ns,"phase_semantics":backend+1,
            "upload_strategy":backend+1,"reset_and_readback_poison_before_each_sample":true,
            "equal_storage_mode_proven":false,
            "exact":true,"host_elapsed":true,"gpu_execution_time_measured":false,"game":false}));
    }
    Ok(result)
}
/// # Safety
/// Caller supplies a unique, complete, aligned output and an external process deadline.
/// Refusal/failure leaves its bytes unchanged. No game state is accessed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn OHRunPublicConstantComparisonV1(out: *mut Comparison, bytes: usize) -> i32 {
    if out.is_null() || bytes != std::mem::size_of::<Comparison>()
        || !(out as usize).is_multiple_of(std::mem::align_of::<Comparison>()) { return -1; }
    match catch_unwind(AssertUnwindSafe(execute)) {
        Ok(Ok(result)) => { unsafe { out.write(result); } 0 },
        Ok(Err(reason)) => { eprintln!("PUBLIC_CONSTANT_BACKEND_ERROR {reason}"); -2 },
        Err(_) => -3,
    }
}
