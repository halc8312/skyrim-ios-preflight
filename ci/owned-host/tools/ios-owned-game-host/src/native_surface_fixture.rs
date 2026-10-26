//! Authored SDK-only presentation/queue/lifecycle oracle; no guest inputs.
use crate::graphics_fixture::OHGraphicsPresentationProbeV1;
use std::ffi::c_void;

#[unsafe(no_mangle)]
pub extern "C" fn OHNativeRendererSelectedV1() -> u32 { cfg!(feature = "native-metal") as u32 }

/// # Safety
/// The caller retains an attached CAMetalLayer until this synchronous call
/// completes. The output must be uniquely writable for the exact public ABI.
/// The SDK test supplies an independent process deadline.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn OHFixtureNativeSurfaceProbeV1(layer: *mut c_void, out: *mut OHGraphicsPresentationProbeV1, bytes: usize) -> i32 {
    if layer.is_null() || out.is_null() || bytes != 32 || (out as usize) % 8 != 0 { return -1; }
    #[cfg(feature = "native-metal")]
    {
        // A dedicated owner thread also guarantees the optional staging TLS
        // is retired with this probe, never inherited by another renderer.
        let layer = layer as usize;
        match std::thread::spawn(move || run(layer)).join() {
            Ok(Ok(value)) => { unsafe { out.write(value); } 0 },
            Ok(Err(error)) => { eprintln!("PUBLIC_NATIVE_SURFACE_REFUSED {error}"); -3 },
            Err(_) => -2,
        }
    }
    #[cfg(not(feature = "native-metal"))]
    { -4 }
}

#[cfg(feature = "native-metal")]
fn run(layer: usize) -> gpu_webgpu::Result<OHGraphicsPresentationProbeV1> {
    use gpu_webgpu::*;
    let instance = Instance::new()?;
    let surface = unsafe { instance.create_surface(SurfaceTarget::Metal { layer: layer as *mut c_void })? };
    let adapter = instance.block_on(instance.request_adapter(&RequestAdapterOptions { compatible_surface: Some(&surface), ..Default::default() }))?;
    if adapter.info().backend != BackendType::Metal { return Err(Error::Invalid("actual Metal required".into())); }
    let device = instance.block_on(adapter.request_device(&DeviceDescriptor::default()))?;
    let configure = |width| surface.configure(&device, &SurfaceConfiguration {
        format: TextureFormat::BGRA8Unorm, usage: TextureUsage::RenderAttachment,
        width, height: width, view_formats: &[], alpha_mode: CompositeAlphaMode::Opaque, present_mode: PresentMode::Fifo,
    });
    configure(64);
    staging::enable(&device, 2, 64);
    let source = device.create_buffer(&BufferDescriptor { label: "surface prefix oracle", size: 8,
        usage: BufferUsage::CopySrc | BufferUsage::CopyDst, mapped_at_creation: false });
    let readback = device.create_buffer(&BufferDescriptor { label: "surface prefix readback", size: 8,
        usage: BufferUsage::MapRead | BufferUsage::CopyDst, mapped_at_creation: false });
    let mut checks = 1u64;
    for case in 0..3u8 {
        let frame = surface.current_frame()?;
        let view = frame.texture.create_view(None);
        let mut encoder = device.create_command_encoder("surface lifecycle oracle");
        encoder.begin_render_pass("public green frame", &[Some(RenderPassColorAttachment::clear(&view,
            Color { r: 0.05, g: 0.9, b: 0.25, a: 1.0 }))], None).end();
        encoder.copy_buffer_to_buffer(&source, 0, &readback, 0, 8);
        // The upload is intentionally later than recording and must become
        // visible to this same present submission, including discarded frames.
        let expected = [case + 41;8];
        device.queue().write_buffer(&source, 0, &expected);
        if case == 0 { configure(128); }
        if case == 1 { surface.unconfigure(); }
        let result = frame.present_with(encoder.finish("surface lifecycle oracle"));
        if case < 2 {
            if !matches!(result, Err(Error::SurfaceStatus(s)) if s == ffi::WGPUSurfaceGetCurrentTextureStatus::Outdated) {
                return Err(Error::Invalid("stale surface was not reported as outdated".into()));
            }
        } else { result?; }
        device.block_on(device.queue().on_submitted_work_done())?;
        device.block_on(readback.map_async(MapMode::Read, 0, None))?;
        let mut actual = [0;8]; readback.read_mapped(0, &mut actual)?; readback.unmap();
        if actual != expected || device.lost_reason().is_some() || device.error_count() != 0 {
            return Err(Error::Invalid("surface upload/order/lifetime mismatch".into()));
        }
        checks |= 1 << (case + 1);
        if case == 1 { configure(64); }
    }
    Ok(OHGraphicsPresentationProbeV1 { version: 0x0001_0000, bytes: 32, backend: BackendType::Metal.0,
        reserved: 0, checks, rendered_cases: 3 })
}
