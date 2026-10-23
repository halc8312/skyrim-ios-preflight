//! Authored offscreen cache conformance. No guest memory or external images.
use super::*;
use std::time::{Duration, Instant};

fn device() -> Device {
    #[cfg(target_os = "linux")]
    unsafe {
        unsafe extern "C" { fn prctl(option: i32, ...) -> i32; }
        assert_eq!(prctl(4, 0 as std::ffi::c_long, 0 as std::ffi::c_long,
                        0 as std::ffi::c_long, 0 as std::ffi::c_long), 0);
    }
    let instance = Instance::new().unwrap();
    let adapter = instance.block_on(instance.request_adapter(&RequestAdapterOptions::default())).unwrap();
    eprintln!("public present-cache backend={}", adapter.info().backend.0);
    instance.block_on(adapter.request_device(&DeviceDescriptor::default())).unwrap()
}

fn source(device: &Device, pixels: &[u8; 16]) -> Texture {
    let texture = device.create_texture(&TextureDescriptor {
        label: "authored cache source", size: extent(2, 2, 1), format: TextureFormat::RGBA8Unorm,
        usage: TextureUsage::TextureBinding | TextureUsage::CopyDst, ..Default::default()
    });
    device.queue().write_texture(&TexelCopyTextureInfo::whole(&texture), pixels, 8, None, extent(2, 2, 1));
    texture
}

fn front(view: &TextureView, size: (u32, u32), swizzle: [u32; 4]) -> FrontBuffer {
    FrontBuffer { view: view.clone(), filterable: true, width: size.0, height: size.1, swizzle }
}

fn image(device: &Device, presenter: &Presenter) -> Vec<u8> {
    let texture = device.create_texture(&TextureDescriptor {
        label: "authored cache output", size: extent(8, 8, 1), format: TextureFormat::RGBA8Unorm,
        usage: TextureUsage::RenderAttachment | TextureUsage::CopySrc, ..Default::default()
    });
    let view = texture.create_view(None);
    let read = device.create_buffer(&BufferDescriptor {
        label: "authored cache readback", size: 256 * 8,
        usage: BufferUsage::CopyDst | BufferUsage::MapRead, mapped_at_creation: false,
    });
    let mut encoder = device.create_command_encoder("authored cache conformance");
    presenter.encode(&mut encoder, &view, 8, 8);
    encoder.copy_texture_to_buffer(&TexelCopyTextureInfo::whole(&texture),
        &TexelCopyBufferInfo { buffer: &read, offset: 0, bytes_per_row: 256, rows_per_image: None }, extent(8, 8, 1));
    device.queue().submit([encoder.finish("authored cache conformance")]);
    device.block_on(read.map_async(MapMode::Read, 0, None)).unwrap();
    let mut data = vec![0; 256 * 8];
    read.read_mapped(0, &mut data).unwrap();
    read.unmap();
    data.chunks(256).flat_map(|row| row[..8 * 4].iter().copied()).collect()
}

// Only the preserved original-source scratch sets this test-only oracle to 0.
fn expect_reuse() -> bool { std::env::var("PUBLIC_PRESENT_CACHE_EXPECT_REUSE").as_deref() != Ok("0") }

#[test]
fn one_current_view_preserves_pixels_lifetime_and_every_swap() {
    let device = device();
    device.push_error_scope(ErrorFilter::Validation);
    let a = source(&device, &[16, 64, 128, 255, 240, 32, 96, 255, 32, 224, 64, 255, 128, 96, 16, 255]);
    let b = source(&device, &[255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255]);
    let av = a.create_view(None);
    let bv = b.create_view(None);
    let new_av = a.create_view(None);
    assert_ne!(av, new_av, "distinct views of one texture are distinct bindings");
    let mut presenter = Presenter::new(&device, TextureFormat::RGBA8Unorm);
    let cases = [
        (&av, (2, 2), [0, 1, 2, 3], [0, 100, 0, 0], false),
        (&av, (2, 2), [0, 1, 2, 3], [0, 100, 0, 0], true),
        (&av, (2, 2), [2, 0, 1, 3], [0, 100, 0, 0], true),
        (&av, (4, 2), [2, 0, 1, 3], [0, 100, 0, 0], true),
        (&av, (2, 2), [0, 1, 2, 3], [0, 100, 2, 0], true),
        (&av, (2, 2), [0, 1, 2, 3], [0, 100, 1, 100], true),
        (&bv, (2, 2), [0, 1, 2, 3], [0, 100, 0, 0], false),
        (&av, (2, 2), [0, 1, 2, 3], [0, 100, 0, 0], false),
        (&new_av, (2, 2), [0, 1, 2, 3], [0, 100, 3, 0], false),
    ];
    let mut before_image = None;
    for (index, (view, size, swizzle, words, reused)) in cases.into_iter().enumerate() {
        let options = PresentationSettings::from_words(words).unwrap();
        presenter.set_output_options(&device, options, TextureFormat::RGBA8Unorm);
        let old_group = presenter.shown.as_ref().map(|shown| shown.group.clone());
        let before = gpu_webgpu::counters::snapshot();
        assert_eq!(presenter.swap(&device, Some(front(view, size, swizzle))), Some(size));
        let calls = gpu_webgpu::counters::snapshot().since(&before);
        if let Some(old) = old_group {
            assert_eq!(presenter.shown.as_ref().unwrap().group == old, reused && expect_reuse(), "case {index}");
        }
        assert_eq!(presenter.frames, index as u64 + 1);
        let actual = image(&device, &presenter);
        let mut cold = Presenter::new(&device, TextureFormat::RGBA8Unorm);
        cold.set_output_options(&device, options, TextureFormat::RGBA8Unorm);
        cold.swap(&device, Some(front(view, size, swizzle)));
        assert_eq!(actual, image(&device, &cold), "every output byte, case {index}");
        if index == 2 { assert_ne!(actual, before_image.clone().unwrap(), "swizzle must still update"); }
        before_image = Some(actual);
        // Process-global diagnostics are informational here; the standalone
        // ignored benchmark below asserts exact isolated operation counts.
        eprintln!("cache case={index} gpu_calls={calls:?}");
    }
    let kept = image(&device, &presenter);
    let group = presenter.shown.as_ref().unwrap().group.clone();
    let frames = presenter.frames;
    assert_eq!(presenter.swap(&device, None), Some((2, 2)));
    let mut rejected = front(&bv, (4, 2), [2, 1, 0, 3]);
    rejected.filterable = false;
    assert_eq!(presenter.swap(&device, Some(rejected)), None);
    assert_eq!(presenter.frames, frames);
    assert_eq!(presenter.shown.as_ref().unwrap().group, group);
    assert_eq!(image(&device, &presenter), kept);
    // A retained view observes new contents, not a pixel snapshot.
    device.queue().write_texture(&TexelCopyTextureInfo::whole(&a), &[0; 16], 8, None, extent(2, 2, 1));
    presenter.swap(&device, Some(front(&new_av, (2, 2), [0, 1, 2, 3])));
    let updated = image(&device, &presenter);
    assert_ne!(updated, kept);
    drop((a, b, av, bv, new_av));
    assert_eq!(image(&device, &presenter), updated, "caller references can expire");
    // No previous view is retained by the one-entry cache after replacement.
    let c = source(&device, &[255; 16]);
    let cv = c.create_view(None);
    presenter.swap(&device, Some(front(&cv, (2, 2), [0, 1, 2, 3])));
    drop((c, cv));
    assert!(image(&device, &presenter).chunks_exact(4).all(|pixel| pixel == [255; 4]));
    assert!(device.block_on(device.pop_error_scope()).unwrap().is_none());
    assert!(device.take_errors().is_empty());
    assert!(device.lost_reason().is_none());
}

#[test]
#[ignore = "manual isolated release timing, not a pass/fail speed threshold"]
fn present_bind_group_creation_benchmark() {
    const N: usize = 4096;
    const SAMPLES: usize = 5;
    let device = device();
    device.push_error_scope(ErrorFilter::Validation);
    let texture = source(&device, &[255; 16]);
    let a = texture.create_view(None);
    let b = texture.create_view(None);
    for mode in ["repeat", "alternate", "new"] {
        let mut times = Vec::new();
        for sample in 0..SAMPLES {
            let mut presenter = Presenter::new(&device, TextureFormat::RGBA8Unorm);
            let start_calls = gpu_webgpu::counters::snapshot();
            let start = Instant::now();
            for frame in 0..N {
                let view = match mode {
                    "repeat" => a.clone(),
                    "alternate" => if frame % 2 == 0 { a.clone() } else { b.clone() },
                    _ => texture.create_view(None),
                };
                presenter.swap(&device, Some(FrontBuffer {
                    view, filterable: true, width: 2, height: 2, swizzle: [0, 1, 2, 3],
                }));
            }
            let elapsed = start.elapsed();
            let calls = gpu_webgpu::counters::snapshot().since(&start_calls);
            assert_eq!(presenter.frames, N as u64);
            assert_eq!(calls.bind_groups, if mode == "repeat" && expect_reuse() { 1 } else { N as u64 });
            assert_eq!((calls.write_buffers, calls.write_buffer_bytes), (N as u64, N as u64 * 16));
            assert_eq!(calls.views, if mode == "new" { N as u64 } else { 0 });
            // Drain outside the timed region; no queued work carries into the next sample.
            assert!(image(&device, &presenter).chunks_exact(4).all(|pixel| pixel == [255; 4]));
            assert!(elapsed < Duration::from_secs(30));
            times.push(elapsed.as_nanos());
            println!("PRESENT_CACHE_SAMPLE mode={mode} sample={sample} swaps={N} ns={} bind_groups={} writes={} bytes={} views={}",
                     elapsed.as_nanos(), calls.bind_groups, calls.write_buffers, calls.write_buffer_bytes, calls.views);
        }
        times.sort_unstable();
        println!("PRESENT_CACHE_MEDIAN mode={mode} swaps={N} samples={SAMPLES} ns={}", times[SAMPLES / 2]);
    }
    assert!(device.block_on(device.pop_error_scope()).unwrap().is_none());
    assert!(device.take_errors().is_empty());
    assert!(device.lost_reason().is_none());
}
