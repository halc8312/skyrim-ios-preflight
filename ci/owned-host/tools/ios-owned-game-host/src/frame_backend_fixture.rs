//! Public Xenos shaders + CPU-owned pass plans; never attaches to the game.
use gpu_webgpu::{BackendType, Instance, RequestAdapterOptions};
use std::{
    ffi::c_void,
    panic::{AssertUnwindSafe, catch_unwind},
};
use xenos_render::frame_probe::{self, WebGpuFrames};
unsafe extern "C" {
    fn OHMetalFrameCreateV1() -> *mut c_void;
    fn OHMetalSampledFrameCreateV1() -> *mut c_void;
    fn OHMetalSampledFrameRenderV1(
        context: *mut c_void,
        packet: *const u8,
        bytes: usize,
        output: *mut u8,
        output_bytes: usize,
    ) -> i32;
    fn OHMetalFrameDestroyV1(context: *mut c_void) -> i32;
    fn OHMetalFrameRenderV1(
        context: *mut c_void,
        packet: *const u8,
        bytes: usize,
        output: *mut u8,
        output_bytes: usize,
    ) -> i32;
}
struct Metal {
    raw: *mut c_void,
    sampled: bool,
}
fn native_render(raw: *mut c_void, sampled: bool, packet: &[u8], output: &mut [u8]) -> i32 {
    unsafe {
        if sampled {
            OHMetalSampledFrameRenderV1(
                raw,
                packet.as_ptr(),
                packet.len(),
                output.as_mut_ptr(),
                output.len(),
            )
        } else {
            OHMetalFrameRenderV1(
                raw,
                packet.as_ptr(),
                packet.len(),
                output.as_mut_ptr(),
                output.len(),
            )
        }
    }
}
impl Metal {
    fn render(&self, packet: &[u8], output: &mut [u8]) -> i32 {
        native_render(self.raw, self.sampled, packet, output)
    }
}
impl Drop for Metal {
    fn drop(&mut self) {
        if !self.raw.is_null() {
            let r = unsafe { OHMetalFrameDestroyV1(self.raw) };
            if r != 0 {
                eprintln!("PUBLIC_FRAME_DESTROY {r}");
            }
        }
    }
}
enum PlannedFixture {
    Buffer(frame_probe::Fixture),
    Sampled(xenos_render::sampled_probe::Fixture),
}
impl PlannedFixture {
    fn id(&self) -> u32 {
        match self {
            Self::Buffer(f) => f.id,
            Self::Sampled(f) => f.id,
        }
    }
    fn expected(&self) -> &[u8] {
        match self {
            Self::Buffer(f) => &f.expected,
            Self::Sampled(f) => &f.expected,
        }
    }
    fn tolerance(&self) -> u8 {
        match self {
            Self::Buffer(f) => f.tolerance,
            Self::Sampled(f) => f.tolerance,
        }
    }
    fn packet(&self) -> Vec<u8> {
        match self {
            Self::Buffer(f) => f.frame.packet(),
            Self::Sampled(f) => f.frame.packet(),
        }
    }
    fn passes(&self) -> usize {
        match self {
            Self::Buffer(f) => f.frame.passes().len(),
            Self::Sampled(f) => f.frame.geometry().passes().len(),
        }
    }
    fn verify(&self, bytes: &[u8]) -> bool {
        bytes.len() == self.expected().len()
            && bytes
                .iter()
                .zip(self.expected())
                .all(|(a, b)| a.abs_diff(*b) <= self.tolerance())
    }
    fn render(&self, web: &WebGpuFrames) -> Result<Vec<u8>, String> {
        match self {
            Self::Buffer(f) => web.render(&f.frame),
            Self::Sampled(f) => web.render_sampled(&f.frame),
        }
    }
}
#[repr(C)]
#[derive(Default)]
pub struct Report {
    version: u32,
    bytes: u32,
    backend_a: u32,
    backend_b: u32,
    cases: u64,
    renders: u64,
    compared_bytes: u64,
    flags: u64,
}
const _: () = assert!(std::mem::size_of::<Report>() == 48);
fn native_guards(metal: &Metal, packet: &[u8], bytes: usize) -> Result<(), String> {
    let mut output = vec![0x77; bytes];
    if metal.render(packet, &mut output[..bytes - 1]) != -1 || output.iter().any(|b| *b != 0x77) {
        return Err("size guard".into());
    }
    let mut bad = packet.to_vec();
    bad[60] = 1;
    if metal.render(&bad, &mut output) != -1 || output.iter().any(|b| *b != 0x77) {
        return Err("packet guard".into());
    }
    let context = metal.raw as usize;
    let sampled = metal.sampled;
    let packet = packet.to_vec();
    let result = std::thread::spawn(move || {
        let mut out = vec![0x77; bytes];
        let status = native_render(context as *mut c_void, sampled, &packet, &mut out);
        (status, out.iter().all(|b| *b == 0x77))
    })
    .join()
    .map_err(|_| "thread guard")?;
    if result != (-1, true) {
        return Err("owner guard".into());
    }
    Ok(())
}
fn execute(sampled: bool) -> Result<Report, String> {
    let tag = if sampled {
        "PUBLIC_SAMPLED_FRAME"
    } else {
        "PUBLIC_FRAME"
    };
    let metal = Metal {
        raw: unsafe {
            if sampled {
                OHMetalSampledFrameCreateV1()
            } else {
                OHMetalFrameCreateV1()
            }
        },
        sampled,
    };
    if metal.raw.is_null() {
        return Err("offline Metal shaders".into());
    }
    let fixtures: Vec<PlannedFixture> = if sampled {
        xenos_render::sampled_probe::fixtures()
            .into_iter()
            .map(PlannedFixture::Sampled)
            .collect()
    } else {
        frame_probe::fixtures()
            .into_iter()
            .map(PlannedFixture::Buffer)
            .collect()
    };
    let mut failures = Vec::new();
    if let Err(error) = native_guards(&metal, &fixtures[0].packet(), fixtures[0].expected().len()) {
        eprintln!("{tag}_GUARD_ERROR {error}");
        failures.push(error);
    }
    let mut native_verified = 0;
    let mut webgpu_verified = 0;
    let mut native_outputs = Vec::new();
    let mut report = Report {
        version: 0x10000,
        bytes: 48,
        backend_a: 1,
        backend_b: 2,
        cases: fixtures.len() as u64,
        // Bits: 1 immutable versions, 2 offline authored Xenos shaders,
        // 4 every pixel matches both an independent oracle and the peer backend.
        flags: 7,
        ..Default::default()
    };
    for (ordinal, f) in fixtures.iter().chain(fixtures.iter().rev()).enumerate() {
        let packet = f.packet();
        let mut actual = vec![0x77; f.expected().len()];
        let status = metal.render(&packet, &mut actual);
        let native_ok = status == 0 && f.verify(&actual);
        if native_ok {
            native_verified += 1;
            eprintln!(
                "{tag}_NATIVE_CASE {}",
                serde_json::json!({"case":f.id(),"ordinal":ordinal,
                "oracle_match":true,"bytes":actual.len(),"game":false,"physical_iphone":false})
            );
        } else {
            let mismatch = actual
                .iter()
                .zip(f.expected())
                .position(|(a, b)| a.abs_diff(*b) > f.tolerance());
            let error = if status != 0 {
                format!("Metal render status={status} case={}", f.id())
            } else {
                format!(
                    "Metal pixels case={} first_byte={mismatch:?} values={:?}",
                    f.id(),
                    mismatch.map(|i| (actual[i], f.expected()[i]))
                )
            };
            eprintln!("{tag}_NATIVE_ERROR {error}");
            failures.push(error);
        }
        native_outputs.push(native_ok.then_some(actual));
    }
    // Finish and log all native evidence before entering a backend that could
    // terminate the process on an uncaught compiler/driver error.
    eprintln!(
        "{tag}_NATIVE_COMPLETE {}",
        serde_json::json!({"oracle_renders":native_verified,
        "expected_renders":fixtures.len()*2,"game":false,"physical_iphone":false,"performance_measured":false})
    );
    let web = (|| -> Result<WebGpuFrames, String> {
        let instance = Instance::new().map_err(|e| format!("instance: {e:?}"))?;
        let adapter = instance
            .block_on(instance.request_adapter(&RequestAdapterOptions::default()))
            .map_err(|e| format!("adapter: {e:?}"))?;
        if adapter.info().backend != BackendType::Metal {
            return Err("Metal adapter required".into());
        }
        let descriptor = frame_probe::device_descriptor(&adapter)?;
        eprintln!(
            "{tag}_DEVICE_LIMITS {}",
            serde_json::json!({"adapter_storage_buffers":adapter.limits().maxStorageBuffersPerShaderStage,
            "requested_storage_buffers":frame_probe::REQUIRED_STORAGE_BUFFERS,"game":false,"physical_iphone":false})
        );
        let device = instance
            .block_on(adapter.request_device(&descriptor))
            .map_err(|e| format!("device: {e:?}"))?;
        if sampled {
            WebGpuFrames::new_sampled(device)
        } else {
            WebGpuFrames::new(device)
        }
    })();
    let web = match web {
        Ok(web) => Some(web),
        Err(error) => {
            eprintln!("{tag}_WGPU_ERROR initialization: {error}");
            failures.push(error);
            None
        }
    };
    for (ordinal, f) in fixtures.iter().chain(fixtures.iter().rev()).enumerate() {
        let expected = web.as_ref().and_then(|web| match f.render(web) {
            Ok(bytes) if f.verify(&bytes) => {
                webgpu_verified += 1;
                Some(bytes)
            }
            result => {
                let error = format!(
                    "WebGPU case={} ordinal={ordinal}: {}",
                    f.id(),
                    result
                        .err()
                        .unwrap_or_else(|| "independent pixel oracle mismatch".into())
                );
                eprintln!("{tag}_WGPU_ERROR {error}");
                failures.push(error);
                None
            }
        });
        let Some(expected) = expected else {
            continue;
        };
        let Some(actual) = &native_outputs[ordinal] else {
            continue;
        };
        if actual
            .iter()
            .zip(&expected)
            .any(|(a, b)| a.abs_diff(*b) > f.tolerance())
        {
            failures.push(format!("backend pixel disagreement case {}", f.id()));
            continue;
        }
        report.renders += 2;
        report.compared_bytes += (actual.len() + expected.len()) as u64;
        eprintln!(
            "{tag}_BACKEND_CASE {}",
            serde_json::json!({"case":f.id(),"ordinal":ordinal,"bytes_per_backend":actual.len(),
            "maximum_unorm_error":f.tolerance(),"oracle_and_pair_match":true,"offline_xenos_shaders":true,
            "immutable_buffer_versions":true,"passes":f.passes(),"game":false,"physical_iphone":false,"performance_measured":false})
        );
    }
    eprintln!(
        "{tag}_PARTIAL_RESULT {}",
        serde_json::json!({"native_oracle_renders":native_verified,
        "webgpu_oracle_renders":webgpu_verified,"full_pairs":report.renders/2,"failures":failures.len(),
        "game":false,"physical_iphone":false,"performance_measured":false})
    );
    if let Some(error) = failures.first() {
        return Err(format!("{} failures; first: {error}", failures.len()));
    }
    if report.renders != fixtures.len() as u64 * 4
        || native_verified != fixtures.len() * 2
        || webgpu_verified != fixtures.len() * 2
    {
        return Err("incomplete planned comparison".into());
    }
    Ok(report)
}
/// # Safety
/// Aligned, unique output; external process deadline. Failure leaves bytes intact.
unsafe fn run_comparison(out: *mut Report, bytes: usize, sampled: bool) -> i32 {
    if out.is_null()
        || bytes != std::mem::size_of::<Report>()
        || !(out as usize).is_multiple_of(std::mem::align_of::<Report>())
    {
        return -1;
    }
    let tag = if sampled {
        "PUBLIC_SAMPLED_FRAME"
    } else {
        "PUBLIC_FRAME"
    };
    match catch_unwind(AssertUnwindSafe(|| execute(sampled))) {
        Ok(Ok(report)) => {
            unsafe { out.write(report) };
            0
        }
        Ok(Err(error)) => {
            eprintln!("{tag}_BACKEND_ERROR {error}");
            -2
        }
        Err(_) => -3,
    }
}

/// # Safety
/// Aligned unique caller-owned output, external process deadline.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn OHRunPublicFrameComparisonV1(out: *mut Report, bytes: usize) -> i32 {
    unsafe { run_comparison(out, bytes, false) }
}
/// # Safety
/// Same contract, authored sampled-resource fixtures only; no game is attached.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn OHRunPublicSampledFrameComparisonV1(
    out: *mut Report,
    bytes: usize,
) -> i32 {
    unsafe { run_comparison(out, bytes, true) }
}
