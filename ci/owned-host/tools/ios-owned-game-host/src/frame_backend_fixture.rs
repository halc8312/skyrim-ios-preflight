//! Public Xenos shaders + CPU-owned pass plans; never attaches to the game.
use gpu_webgpu::{BackendType, Instance, RequestAdapterOptions};
use std::{
    ffi::c_void,
    panic::{AssertUnwindSafe, catch_unwind},
};
use xenos_render::frame_probe::{self, WebGpuFrames};
use xenos_render::target_frame::{AllocationAudit, AllocationPlan};
unsafe extern "C" {
    fn OHMetalTargetFrameRenderAuditedV1(
        context: *mut c_void,
        packet: *const u8,
        bytes: usize,
        policy: u32,
        output: *mut u8,
        output_bytes: usize,
        audit: *mut AllocationAudit,
        audit_bytes: usize,
    ) -> i32;
    fn OHMetalFrameCreateV1() -> *mut c_void;
    fn OHMetalSampledFrameCreateV1() -> *mut c_void;
    fn OHMetalTargetFrameCreateV1() -> *mut c_void;
    fn OHMetalTargetFrameRenderReusingV1(
        context: *mut c_void,
        packet: *const u8,
        bytes: usize,
        output: *mut u8,
        output_bytes: usize,
    ) -> i32;
    fn OHMetalTargetFrameRenderV1(
        context: *mut c_void,
        packet: *const u8,
        bytes: usize,
        output: *mut u8,
        output_bytes: usize,
    ) -> i32;
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
#[derive(Clone, Copy, PartialEq, Eq)]
enum Mode {
    Buffer,
    Sampled,
    Targets,
    TargetsReusing,
}
impl Mode {
    fn is_targets(self) -> bool {
        matches!(self, Self::Targets | Self::TargetsReusing)
    }
    fn tag(self) -> &'static str {
        match self {
            Self::Buffer => "PUBLIC_FRAME",
            Self::Sampled => "PUBLIC_SAMPLED_FRAME",
            Self::Targets => "PUBLIC_TARGET_FRAME",
            Self::TargetsReusing => "PUBLIC_TARGET_REUSED_FRAME",
        }
    }
}
struct Metal {
    raw: *mut c_void,
    mode: Mode,
}
fn native_render(raw: *mut c_void, mode: Mode, packet: &[u8], output: &mut [u8]) -> i32 {
    unsafe {
        if mode == Mode::TargetsReusing {
            OHMetalTargetFrameRenderReusingV1(
                raw,
                packet.as_ptr(),
                packet.len(),
                output.as_mut_ptr(),
                output.len(),
            )
        } else if mode == Mode::Targets {
            OHMetalTargetFrameRenderV1(
                raw,
                packet.as_ptr(),
                packet.len(),
                output.as_mut_ptr(),
                output.len(),
            )
        } else if mode == Mode::Sampled {
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
        native_render(self.raw, self.mode, packet, output)
    }
    fn render_audited(&self, packet: &[u8], output: &mut [u8], audit: &mut AllocationAudit) -> i32 {
        unsafe {
            OHMetalTargetFrameRenderAuditedV1(
                self.raw,
                packet.as_ptr(),
                packet.len(),
                u32::from(self.mode == Mode::TargetsReusing),
                output.as_mut_ptr(),
                output.len(),
                audit,
                std::mem::size_of::<AllocationAudit>(),
            )
        }
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
    Targets(xenos_render::target_fixtures::Fixture),
}
impl PlannedFixture {
    fn id(&self) -> u32 {
        match self {
            Self::Buffer(f) => f.id,
            Self::Sampled(f) => f.id,
            Self::Targets(f) => f.id,
        }
    }
    fn expected(&self) -> &[u8] {
        match self {
            Self::Buffer(f) => &f.expected,
            Self::Sampled(f) => &f.expected,
            Self::Targets(f) => &f.expected,
        }
    }
    fn tolerance(&self) -> u8 {
        match self {
            Self::Buffer(f) => f.tolerance,
            Self::Sampled(f) => f.tolerance,
            Self::Targets(f) => f.tolerance,
        }
    }
    fn packet(&self) -> Vec<u8> {
        match self {
            Self::Buffer(f) => f.frame.packet(),
            Self::Sampled(f) => f.frame.packet(),
            Self::Targets(f) => f.frame.packet(),
        }
    }
    fn passes(&self) -> usize {
        match self {
            Self::Buffer(f) => f.frame.passes().len(),
            Self::Sampled(f) => f.frame.geometry().passes().len(),
            Self::Targets(f) => f.frame.geometry().passes().len(),
        }
    }
    fn verify(&self, bytes: &[u8]) -> bool {
        bytes.len() == self.expected().len()
            && bytes
                .iter()
                .zip(self.expected())
                .all(|(a, b)| a.abs_diff(*b) <= self.tolerance())
    }
    fn verify_allocation(&self, actual: &AllocationAudit, reuse: bool) -> bool {
        match self {
            Self::Targets(f) => AllocationPlan::new(&f.frame, reuse).matches_audit(actual),
            _ => false,
        }
    }
    fn render(&self, web: &WebGpuFrames, reuse: bool) -> Result<Vec<u8>, String> {
        match self {
            Self::Buffer(f) => web.render(&f.frame),
            Self::Sampled(f) => web.render_sampled(&f.frame),
            Self::Targets(f) if reuse => web.render_targets_reusing(&f.frame),
            Self::Targets(f) => web.render_targets(&f.frame),
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
    if metal.mode.is_targets() {
        let mut audit = AllocationAudit {
            version: 77,
            ..Default::default()
        };
        let original = audit;
        for (policy, audit_bytes) in [
            (u32::from(metal.mode == Mode::TargetsReusing), 199),
            (2, 200),
        ] {
            let status = unsafe {
                OHMetalTargetFrameRenderAuditedV1(
                    metal.raw,
                    packet.as_ptr(),
                    packet.len(),
                    policy,
                    output.as_mut_ptr(),
                    output.len(),
                    &mut audit,
                    audit_bytes,
                )
            };
            if status != -1 || audit != original || output.iter().any(|b| *b != 0x77) {
                return Err("allocation audit guard".into());
            }
        }
        let mut overlap = vec![0u8; packet.len().max(bytes)];
        overlap[..packet.len()].copy_from_slice(packet);
        let before = overlap.clone();
        let status = unsafe {
            let render = if metal.mode == Mode::TargetsReusing {
                OHMetalTargetFrameRenderReusingV1
            } else {
                OHMetalTargetFrameRenderV1
            };
            render(
                metal.raw,
                overlap.as_ptr(),
                packet.len(),
                overlap.as_mut_ptr(),
                bytes,
            )
        };
        if status != -1 || overlap != before {
            return Err("overlap guard".into());
        }
        let mut aligned_overlap = vec![0u64; bytes.max(200).div_ceil(8)];
        let status = unsafe {
            OHMetalTargetFrameRenderAuditedV1(
                metal.raw,
                packet.as_ptr(),
                packet.len(),
                1,
                aligned_overlap.as_mut_ptr().cast::<u8>(),
                bytes,
                aligned_overlap.as_mut_ptr().cast::<AllocationAudit>(),
                200,
            )
        };
        if status != -1 || aligned_overlap.iter().any(|w| *w != 0) {
            return Err("allocation audit overlap guard".into());
        }
    }
    let context = metal.raw as usize;
    let mode = metal.mode;
    let packet = packet.to_vec();
    let result = std::thread::spawn(move || {
        let mut out = vec![0x77; bytes];
        let status = native_render(context as *mut c_void, mode, &packet, &mut out);
        let mut audit = AllocationAudit {
            version: 77,
            ..Default::default()
        };
        let original = audit;
        let audit_guard = !mode.is_targets()
            || unsafe {
                OHMetalTargetFrameRenderAuditedV1(
                    context as *mut c_void,
                    packet.as_ptr(),
                    packet.len(),
                    u32::from(mode == Mode::TargetsReusing),
                    out.as_mut_ptr(),
                    out.len(),
                    &mut audit,
                    200,
                )
            } == -1
                && audit == original;
        (status, out.iter().all(|b| *b == 0x77), audit_guard)
    })
    .join()
    .map_err(|_| "thread guard")?;
    if result != (-1, true, true) {
        return Err("owner guard".into());
    }
    Ok(())
}
fn execute(mode: Mode) -> Result<Report, String> {
    let tag = mode.tag();
    let metal = Metal {
        raw: unsafe {
            match mode {
                Mode::Targets | Mode::TargetsReusing => OHMetalTargetFrameCreateV1(),
                Mode::Sampled => OHMetalSampledFrameCreateV1(),
                Mode::Buffer => OHMetalFrameCreateV1(),
            }
        },
        mode,
    };
    if metal.raw.is_null() {
        return Err("offline Metal shaders".into());
    }
    let fixtures: Vec<PlannedFixture> = match mode {
        Mode::Targets | Mode::TargetsReusing => xenos_render::target_fixtures::fixtures()
            .into_iter()
            .map(PlannedFixture::Targets)
            .collect(),
        Mode::Sampled => xenos_render::sampled_probe::fixtures()
            .into_iter()
            .map(PlannedFixture::Sampled)
            .collect(),
        Mode::Buffer => frame_probe::fixtures()
            .into_iter()
            .map(PlannedFixture::Buffer)
            .collect(),
    };
    let mut failures = Vec::new();
    for fixture in fixtures
        .iter()
        .take(if mode.is_targets() { fixtures.len() } else { 1 })
    {
        if let Err(error) = native_guards(&metal, &fixture.packet(), fixture.expected().len()) {
            eprintln!("{tag}_GUARD_ERROR {error}");
            failures.push(error);
        }
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
        let mut audit = AllocationAudit::default();
        let status = if mode.is_targets() {
            metal.render_audited(&packet, &mut actual, &mut audit)
        } else {
            metal.render(&packet, &mut actual)
        };
        let allocation_ok =
            !mode.is_targets() || f.verify_allocation(&audit, mode == Mode::TargetsReusing);
        let native_ok = status == 0 && allocation_ok && f.verify(&actual);
        if native_ok {
            native_verified += 1;
            eprintln!(
                "{tag}_NATIVE_CASE {}",
                serde_json::json!({"case":f.id(),"ordinal":ordinal,
                "oracle_match":true,"bytes":actual.len(),"game":false,"physical_iphone":false})
            );
            if mode.is_targets() {
                eprintln!(
                    "{tag}_ALLOCATION_CASE {}",
                    serde_json::json!({
                "case":f.id(),"ordinal":ordinal,"policy":audit.policy,"slots":audit.slots,
                "dedicated_bytes":audit.dedicated_bytes,"allocated_bytes":audit.allocated_bytes,
                "native_allocation_matches_rust":true,"game":false,"physical_iphone":false})
                );
            }
        } else {
            let mismatch = actual
                .iter()
                .zip(f.expected())
                .position(|(a, b)| a.abs_diff(*b) > f.tolerance());
            let error = if status == 0 && !allocation_ok {
                format!("Metal allocation evidence case={}", f.id())
            } else if status != 0 {
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
        if mode != Mode::Buffer {
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
        let expected =
            web.as_ref()
                .and_then(|web| match f.render(web, mode == Mode::TargetsReusing) {
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
unsafe fn run_comparison(out: *mut Report, bytes: usize, mode: Mode) -> i32 {
    if out.is_null()
        || bytes != std::mem::size_of::<Report>()
        || !(out as usize).is_multiple_of(std::mem::align_of::<Report>())
    {
        return -1;
    }
    let tag = mode.tag();
    match catch_unwind(AssertUnwindSafe(|| {
        let mut report = execute(mode)?;
        if mode == Mode::Targets {
            let reused = execute(Mode::TargetsReusing)?;
            if report.cases != reused.cases {
                return Err("target policy case mismatch".to_string());
            }
            report.renders += reused.renders;
            report.compared_bytes += reused.compared_bytes;
            report.flags |= 8;
        }
        Ok(report)
    })) {
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
    unsafe { run_comparison(out, bytes, Mode::Buffer) }
}
/// # Safety
/// Same contract, authored sampled-resource fixtures only; no game is attached.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn OHRunPublicSampledFrameComparisonV1(
    out: *mut Report,
    bytes: usize,
) -> i32 {
    unsafe { run_comparison(out, bytes, Mode::Sampled) }
}

/// # Safety
/// Same aligned, unique output and external deadline; public target fixtures only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn OHRunPublicTargetFrameComparisonV1(out: *mut Report, bytes: usize) -> i32 {
    unsafe { run_comparison(out, bytes, Mode::Targets) }
}
