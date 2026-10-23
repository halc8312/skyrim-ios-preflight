//! Hand-authored, offscreen presentation conformance. No guest, borrowed layer,
//! global renderer, paths or external image data. Native calls can block in a
//! driver: SDK callers must retain their independent process watchdog.
use crate::present::{FrontBuffer, Presenter};
use crate::settings::PresentationSettings;
use gpu_webgpu::*;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PresentationProbe {
    pub backend: u32,
    pub checks: u64,
    pub rendered_cases: u64,
}
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum PresentationProbeError {
    Instance = 1,
    Adapter = 2,
    Device = 3,
    Readback = 4,
    Pixels = 5,
    Validation = 6,
    DeviceLost = 7,
    CorePolicy = 8,
}

fn render(
    presenter: &mut Presenter,
    device: &Device,
    target: &Texture,
    view: &TextureView,
    words: [u32; 4],
) -> std::result::Result<Vec<u8>, PresentationProbeError> {
    presenter.set_output_options(
        device,
        PresentationSettings::from_words(words).map_err(|_| PresentationProbeError::Pixels)?,
        TextureFormat::RGBA8Unorm,
    );
    let read = device.create_buffer(&BufferDescriptor {
        label: "public filter readback",
        size: 256 * 4,
        usage: BufferUsage::MapRead | BufferUsage::CopyDst,
        mapped_at_creation: false,
    });
    let mut encoder = device.create_command_encoder("public filter render");
    presenter.encode(&mut encoder, view, 12, 4);
    encoder.copy_texture_to_buffer(
        &TexelCopyTextureInfo::whole(target),
        &TexelCopyBufferInfo { buffer: &read, offset: 0, bytes_per_row: 256, rows_per_image: None },
        extent(12, 4, 1),
    );
    device.queue().submit([encoder.finish("public filter render")]);
    device.block_on(read.map_async(MapMode::Read, 0, None)).map_err(|_| PresentationProbeError::Readback)?;
    let mut padded = vec![0; 1024];
    read.read_mapped(0, &mut padded).map_err(|_| PresentationProbeError::Readback)?;
    read.unmap();
    Ok(padded.chunks(256).flat_map(|row| row[..48].iter().copied()).collect())
}

/// Execute six tiny GPU renders and check all authored nearest pixels, filter
/// differences, sharpening, opaque output, exact default restoration, and GPU
/// validation/device state. Results contain no pixels or implementation strings.
pub fn run_public_presentation_probe() -> std::result::Result<PresentationProbe, PresentationProbeError> {
    #[cfg(target_os = "linux")]
    unsafe {
        unsafe extern "C" {
            fn prctl(option: i32, ...) -> i32;
        }
        if prctl(4, 0 as std::ffi::c_long, 0 as std::ffi::c_long, 0 as std::ffi::c_long, 0 as std::ffi::c_long) != 0
            || prctl(3, 0 as std::ffi::c_long, 0 as std::ffi::c_long, 0 as std::ffi::c_long, 0 as std::ffi::c_long) != 0
        {
            return Err(PresentationProbeError::CorePolicy);
        }
    }
    let instance = Instance::new().map_err(|_| PresentationProbeError::Instance)?;
    let adapter = instance
        .block_on(instance.request_adapter(&RequestAdapterOptions::default()))
        .map_err(|_| PresentationProbeError::Adapter)?;
    let backend = adapter.info().backend.0;
    let device = instance
        .block_on(adapter.request_device(&DeviceDescriptor::default()))
        .map_err(|_| PresentationProbeError::Device)?;
    device.push_error_scope(ErrorFilter::Validation);
    let source = device.create_texture(&TextureDescriptor {
        label: "authored three texels",
        size: extent(3, 1, 1),
        format: TextureFormat::RGBA8Unorm,
        usage: TextureUsage::TextureBinding | TextureUsage::CopyDst,
        ..Default::default()
    });
    device.queue().write_texture(
        &TexelCopyTextureInfo::whole(&source),
        &[64, 64, 64, 255, 192, 192, 192, 255, 64, 64, 64, 255],
        12,
        None,
        extent(3, 1, 1),
    );
    let target = device.create_texture(&TextureDescriptor {
        label: "authored presentation output",
        size: extent(12, 4, 1),
        format: TextureFormat::RGBA8Unorm,
        usage: TextureUsage::RenderAttachment | TextureUsage::CopySrc,
        ..Default::default()
    });
    let view = target.create_view(None);
    let mut presenter = Presenter::new(&device, TextureFormat::RGBA8Unorm);
    presenter.swap(
        &device,
        Some(FrontBuffer {
            view: source.create_view(None),
            filterable: true,
            width: 3,
            height: 1,
            swizzle: [0, 1, 2, 3],
        }),
    );
    let original = render(&mut presenter, &device, &target, &view, [0, 100, 0, 0])?;
    let bicubic = render(&mut presenter, &device, &target, &view, [0, 100, 3, 0])?;
    let bilinear = render(&mut presenter, &device, &target, &view, [0, 100, 1, 0])?;
    let nearest = render(&mut presenter, &device, &target, &view, [0, 100, 2, 0])?;
    let sharpened = render(&mut presenter, &device, &target, &view, [0, 100, 1, 100])?;
    let restored = render(&mut presenter, &device, &target, &view, [0, 100, 0, 0])?;
    let mut checks = 0u64;
    for (bit, ok) in [
        (1, original == bicubic),
        (
            2,
            nearest.len() == 12 * 4 * 4
                && nearest.chunks_exact(4).enumerate().all(|(pixel, rgba)| {
                    let level = if (4..8).contains(&(pixel % 12)) { 192 } else { 64 };
                    rgba == [level, level, level, 255]
                }),
        ),
        (4, bilinear != bicubic),
        (8, sharpened != bilinear),
        (16, restored == original),
        (32, sharpened.chunks_exact(4).all(|p| p[3] == 255)),
    ] {
        if !ok {
            return Err(PresentationProbeError::Pixels);
        }
        checks |= bit;
    }
    if device.block_on(device.pop_error_scope()).map_err(|_| PresentationProbeError::Validation)?.is_some()
        || !device.take_errors().is_empty()
    {
        return Err(PresentationProbeError::Validation);
    }
    checks |= 64;
    if device.lost_reason().is_some() {
        return Err(PresentationProbeError::DeviceLost);
    }
    checks |= 128;
    Ok(PresentationProbe { backend, checks, rendered_cases: 6 })
}
