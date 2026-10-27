//! Native end to end: a swap of a tiled front buffer in "guest memory" is rendered headlessly
//! and dumped; the PNG must match the CPU decode of the same front buffer.
#![cfg(not(target_os = "emscripten"))]

mod common;

// The iOS SDK's authored workloads must also pass on the selected backend
// before an app build. No game input, Apple execution or timing claim here.
#[cfg(all(feature = "public-probes", feature = "metal-shaders"))]
mod public_probe_regressions {
    use gpu_webgpu::*;
    use xenos_render::{backend_probe, frame_probe, sampled_probe, target_fixtures};
    use backend_probe::Backend;
    fn device(frame_limits: bool) -> Device {
        let instance = Instance::new().unwrap();
        let adapter = instance.block_on(instance.request_adapter(&RequestAdapterOptions::default())).unwrap();
        let descriptor = if frame_limits { frame_probe::device_descriptor(&adapter).unwrap() }
            else { DeviceDescriptor::default() };
        instance.block_on(adapter.request_device(&descriptor)).unwrap()
    }
    fn pixels(actual: &[u8], expected: &[u8], tolerance: u8, case: u32) {
        assert_eq!(actual.len(), expected.len(), "case {case}");
        let mismatch = actual.iter().zip(expected).position(|(a, b)| a.abs_diff(*b) > tolerance);
        assert_eq!(mismatch, None, "case {case} mismatch {:?}", mismatch.map(|i| (i, actual[i], expected[i])));
    }
    #[test]
    fn presentation_filters_and_constants_match_independent_oracles() {
        let report = xenos_render::native::run_public_presentation_probe().unwrap();
        assert_eq!((report.checks, report.rendered_cases), (255, 6));
        let mut backend = backend_probe::WebGpuBackend::new(device(false));
        let batches = backend_probe::batches();
        assert_eq!(batches.len(), 6);
        for batch in batches.iter().chain(batches.iter().rev()) {
            backend.prepare(batch).unwrap();
            let (actual, _) = backend.run(batch).unwrap();
            assert_eq!(actual, batch.expected, "constant records {}", batch.records);
        }
    }
    #[test]
    fn authored_buffer_and_sampled_frames_match_independent_pixels() {
        let host = frame_probe::WebGpuFrames::new(device(true)).unwrap();
        let fixtures = frame_probe::fixtures();
        assert_eq!(fixtures.len(), 9);
        for f in fixtures.iter().chain(fixtures.iter().rev()) {
            pixels(&host.render(&f.frame).unwrap(), &f.expected, f.tolerance, f.id);
        }
        let host = frame_probe::WebGpuFrames::new_sampled(device(true)).unwrap();
        let fixtures = sampled_probe::fixtures();
        assert_eq!(fixtures.len(), 15);
        for f in fixtures.iter().chain(fixtures.iter().rev()) {
            pixels(&host.render_sampled(&f.frame).unwrap(), &f.expected, f.tolerance, f.id);
        }
    }
    #[test]
    fn authored_target_frames_preserve_depth_versions_and_both_allocation_policies() {
        let fixtures = target_fixtures::fixtures();
        assert_eq!(fixtures.len(), 16);
        for reuse in [false, true] {
            let host = frame_probe::WebGpuFrames::new_sampled(device(true)).unwrap();
            for f in fixtures.iter().chain(fixtures.iter().rev()) {
                let rendered = if reuse { host.render_targets_reusing(&f.frame) } else { host.render_targets(&f.frame) };
                pixels(&rendered.unwrap_or_else(|e| panic!("target {} reuse={reuse}: {e}", f.id)), &f.expected, f.tolerance, f.id);
            }
        }
    }
}

use std::path::PathBuf;
use std::sync::Arc;

use xenos_render::native::{self, NativeOptions};
use xenos_render::{Command, PhysicalMemory};
use xenos_texture::address::tiled_2d;
use xenos_texture::{Endian, TextureFetchConstant, TextureFormat, frontbuffer_to_rgba8};

struct Memory(Vec<u8>);

impl PhysicalMemory for Memory {
    fn read(&self, addr: u32, out: &mut [u8]) {
        for (i, b) in out.iter_mut().enumerate() {
            *b = self.0.get(addr as usize + i).copied().unwrap_or(0);
        }
    }

    fn write(&self, _addr: u32, _data: &[u8]) {
        unreachable!("presenting never writes guest memory")
    }
}

impl xenos_texture::GuestMemory for Memory {
    fn read(&self, addr: u32, out: &mut [u8]) {
        PhysicalMemory::read(self, addr, out);
    }
}

const BASE: u32 = 0x10_0000;
const W: u32 = 128;
const H: u32 = 64;

fn quadrant_color(x: u32, y: u32) -> [u8; 4] {
    match (x < W / 2, y < H / 2) {
        (true, true) => [0xFF, 0x20, 0x10, 0xFF],
        (false, true) => [0x10, 0xE0, 0x30, 0xFF],
        (true, false) => [0x20, 0x30, 0xD0, 0xFF],
        (false, false) => [0xF0, 0xF0, 0x40, 0xFF],
    }
}

#[test]
fn swap_of_a_tiled_front_buffer_is_presented() {
    let mut mem = vec![0u8; (BASE + W * H * 4) as usize];
    for y in 0..H {
        for x in 0..W {
            let offset = tiled_2d(x as i32, y as i32, W, 2) as u32;
            let at = (BASE + offset) as usize;
            mem[at..at + 4].copy_from_slice(&quadrant_color(x, y));
        }
    }
    let mut fetch = TextureFetchConstant::new_2d(TextureFormat::k_8_8_8_8, W, H);
    fetch.tiled = true;
    fetch.endianness = Endian::None;
    fetch.base_page = BASE >> 12;
    let memory = Arc::new(Memory(mem));
    let expected = frontbuffer_to_rgba8(&fetch, memory.as_ref()).expect("CPU decode");

    let dir = PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join(format!("present-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    let handle = native::start(
        memory,
        NativeOptions { dump_dir: Some(dir.clone()), dump_every: 1, window: None },
        Box::new(|device| Box::new(common::OneTarget::new(device, 16))),
    )
    .expect("renderer");
    handle.submit(Command::Swap { fetch: fetch.encode(), width: W, height: H });
    handle.wait_idle().expect("native renderer completed presentation");

    let png = dir.join("frame_000001.png");
    let decoder = png::Decoder::new(std::fs::File::open(&png).expect("frame dumped (is a WebGPU adapter available?)"));
    let mut reader = decoder.read_info().unwrap();
    let mut rgba = vec![0u8; reader.output_buffer_size()];
    let info = reader.next_frame(&mut rgba).unwrap();
    assert_eq!((info.width, info.height), (W, H));
    for (x, y) in [(5, 5), (100, 10), (10, 50), (120, 60), (63, 31), (64, 32)] {
        let i = ((y * W + x) * 4) as usize;
        assert_eq!(&rgba[i..i + 4], &expected.data[i..i + 4], "pixel ({x},{y})");
    }
}
