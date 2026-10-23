//! Authored workloads for optional host elapsed-time diagnostics. No game input.
#![cfg(all(not(target_os = "emscripten"), feature = "cpu-profile"))]

#[path = "../../xenos-shader/tests/common/mod.rs"]
mod asm;
mod common;

use asm::*;
use common::OneTarget;
use std::sync::{
    Arc,
    atomic::{AtomicU64, Ordering},
};
use std::time::{Duration, Instant};
use xenos_render::{
    PhysicalMemory, ShaderStage,
    cpu_profile::{CpuProfileSnapshot, DRAW_STAGE_NAMES},
    executor::Executor,
};
use xenos_state::registers as r;

const SIZE: u32 = 8;
const VERTICES: u32 = 0x10000;
const TEXTURES: u32 = 0x200000;
const RGBA: [u8; 4] = [64, 128, 192, 255];

// Version lookup is intentionally cheap and stable. These counts expose the
// renderer's requested work, not the cost of an actual title's memory adapter.
struct Memory {
    data: Vec<u8>,
    versions: AtomicU64,
    reads: AtomicU64,
    read_bytes: AtomicU64,
}

impl PhysicalMemory for Memory {
    fn read(&self, address: u32, out: &mut [u8]) {
        self.reads.fetch_add(1, Ordering::Relaxed);
        self.read_bytes.fetch_add(out.len() as u64, Ordering::Relaxed);
        out.copy_from_slice(&self.data[address as usize..address as usize + out.len()]);
    }
    fn write(&self, _: u32, _: &[u8]) {
        panic!("authored workload has no resolves")
    }
    fn version(&self, _: u32, _: u32) -> Option<u64> {
        self.versions.fetch_add(1, Ordering::Relaxed);
        Some(1)
    }
}

impl Memory {
    fn take_counts(&self) -> (u64, u64, u64) {
        (
            self.versions.swap(0, Ordering::Relaxed),
            self.reads.swap(0, Ordering::Relaxed),
            self.read_bytes.swap(0, Ordering::Relaxed),
        )
    }
}

#[derive(Clone, Copy)]
struct Workload {
    name: &'static str,
    vertex_bytes: u32,
    textures: u32,
    pipeline_churn: bool,
    flush_every: u32,
}

fn setup(device: &gpu_webgpu::Device, workload: Workload) -> (Executor, Arc<Memory>, gpu_webgpu::Texture) {
    let mut data = vec![0; 4 << 20];
    let vertices: [[f32; 3]; 6] =
        [[-1.0, -1.0, 0.5], [1.0, -1.0, 0.5], [-1.0, 1.0, 0.5], [-1.0, 1.0, 0.5], [1.0, -1.0, 0.5], [1.0, 1.0, 0.5]];
    for (index, value) in vertices.iter().flatten().enumerate() {
        let at = VERTICES as usize + index * 4;
        data[at..at + 4].copy_from_slice(&value.to_be_bytes());
    }
    let mut fetches = Vec::new();
    for slot in 1..=workload.textures {
        let address = TEXTURES + slot * 0x10000;
        let mut fetch = xenos_texture::TextureFetchConstant::new_2d(xenos_texture::TextureFormat::k_8_8_8_8, 32, 32);
        fetch.base_page = address >> 12;
        fetch.tiled = false;
        for texel in data[address as usize..address as usize + 0x10000].chunks_exact_mut(4) {
            texel.copy_from_slice(&RGBA);
        }
        fetches.extend(
            fetch
                .encode()
                .into_iter()
                .enumerate()
                .map(|(i, word)| (r::SHADER_CONSTANT_FETCH_00_0 + slot * 6 + i as u32, word)),
        );
    }
    let memory =
        Arc::new(Memory { data, versions: AtomicU64::new(0), reads: AtomicU64::new(0), read_bytes: AtomicU64::new(0) });
    let target = OneTarget::new(device, SIZE);
    let texture = target.texture.clone();
    let mut executor = Executor::new(device, memory.clone(), Box::new(target));
    executor.write_registers(&[
        (r::RB_MODECONTROL, 4),
        (r::RB_SURFACE_INFO, SIZE),
        (r::RB_COLOR_INFO, 0),
        (r::RB_COLOR_MASK, 15),
        (r::RB_BLENDCONTROL0, 0x0001_0001),
        (r::RB_DEPTHCONTROL, 0),
        (r::PA_CL_VTE_CNTL, 0x3F | 1 << 10),
        (r::PA_CL_VPORT_XSCALE, 4.0f32.to_bits()),
        (r::PA_CL_VPORT_XOFFSET, 4.0f32.to_bits()),
        (r::PA_CL_VPORT_YSCALE, (-4.0f32).to_bits()),
        (r::PA_CL_VPORT_YOFFSET, 4.0f32.to_bits()),
        (r::PA_CL_VPORT_ZSCALE, 1.0f32.to_bits()),
        (r::PA_CL_VPORT_ZOFFSET, 0),
        (r::PA_SC_WINDOW_SCISSOR_TL, 1 << 31),
        (r::PA_SC_WINDOW_SCISSOR_BR, SIZE | SIZE << 16),
        (r::PA_SC_SCREEN_SCISSOR_TL, 0),
        (r::PA_SC_SCREEN_SCISSOR_BR, SIZE | SIZE << 16),
        (r::SQ_PROGRAM_CNTL, 1 | 1 << 8 | 1 << 31),
        (r::SQ_VS_CONST, 255 << 12),
        (r::SQ_PS_CONST, 256 | 255 << 12),
        (r::VGT_MAX_VTX_INDX, 0xFF_FFFF),
        (r::SHADER_CONSTANT_FETCH_00_0, VERTICES | 3),
        (r::SHADER_CONSTANT_FETCH_00_0 + 1, 2 | workload.vertex_bytes),
        (r::VGT_DRAW_INITIATOR, 4 | 2 << 6 | 6 << 16),
    ]);
    executor.write_registers(&fetches);
    let color = if workload.textures == 0 { RGBA.map(|v| f32::from(v) / 255.0) } else { [0.5, 0.5, 0.0, 1.0] };
    for (i, value) in color.into_iter().enumerate() {
        executor.write_registers(&[(r::SHADER_CONSTANT_000_X + i as u32, value.to_bits())]);
    }
    let fetch =
        VFetch { dest: 1, dest_swizzle: "xyz1", format: 57, stride: 3, fetch_constant: 0, ..Default::default() };
    executor.load_shader(
        ShaderStage::Vertex,
        assemble(&[exece(vec![
            fetch.into(),
            Alu::vector(2, 62, 15, &[r(1), r(1)]).export().into(),
            Alu::vector(2, 0, 15, &[c(0), c(0)]).export().into(),
        ])])
        .into(),
    );
    let mut pixel: Vec<Ins> = (1..=workload.textures)
        .map(|slot| {
            TFetch {
                dest: 1,
                src: 0,
                fetch_constant: slot,
                mag: 0,
                min: 0,
                mip: 2,
                computed_lod: false,
                ..Default::default()
            }
            .into()
        })
        .collect();
    let result = if workload.textures == 0 { 0 } else { 1 };
    pixel.push(Alu::vector(2, 0, 15, &[r(result), r(result)]).export().into());
    // EXEC slots hold at most six instructions; keep each authored fetch group bounded.
    let chunks: Vec<_> = pixel
        .chunks(6)
        .enumerate()
        .map(|(i, chunk)| if (i + 1) * 6 >= pixel.len() { exece(chunk.to_vec()) } else { exec(chunk.to_vec()) })
        .collect();
    executor.load_shader(ShaderStage::Pixel, assemble(&chunks).into());
    (executor, memory, texture)
}

fn wait_ready(executor: &mut Executor) {
    let deadline = Instant::now() + Duration::from_secs(5);
    while executor.busy() {
        assert!(Instant::now() < deadline, "authored renderer backpressure exceeded5s");
        std::thread::yield_now();
    }
}

fn frame(executor: &mut Executor, workload: Workload, draws: u32) -> u128 {
    let started = Instant::now();
    for index in 0..draws {
        wait_ready(executor);
        if workload.pipeline_churn {
            executor.write_registers(&[(r::RB_COLOR_MASK, if index + 1 == draws { 15 } else { index % 15 + 1 })]);
        }
        executor.draw();
        if workload.flush_every != 0 && (index + 1) % workload.flush_every == 0 {
            executor.flush();
        }
        wait_ready(executor);
    }
    let draw_loop_ns = started.elapsed().as_nanos();
    executor.end_frame();
    draw_loop_ns
}

fn verify_pixels(device: &gpu_webgpu::Device, texture: &gpu_webgpu::Texture) -> Vec<u8> {
    let pixels = OneTarget::read(device, texture, SIZE);
    let at = ((SIZE / 2 * SIZE + SIZE / 2) * 4) as usize;
    assert_eq!(&pixels[at..at + 4], &RGBA);
    assert!(device.take_errors().is_empty());
    pixels
}

#[test]
fn profiling_preserves_pixels_and_handles_skipped_draws() {
    let device = common::device();
    let workload = Workload { name: "control", vertex_bytes: 4096, textures: 0, pipeline_churn: false, flush_every: 0 };
    let render = |enabled| {
        let (mut executor, _, texture) = setup(&device, workload);
        assert_eq!(executor.take_cpu_profile(), CpuProfileSnapshot::default());
        executor.set_cpu_profiling(enabled);
        frame(&mut executor, workload, 1);
        executor.write_registers(&[(r::VGT_DRAW_INITIATOR, 4 | 2 << 6)]); // zero vertices
        executor.draw();
        let profile = executor.take_cpu_profile();
        assert_eq!(executor.take_cpu_profile(), CpuProfileSnapshot { enabled, ..Default::default() });
        if enabled {
            assert_eq!((profile.draw_attempts, profile.draws_recorded, profile.draws_skipped), (2, 1, 1));
            assert_eq!(profile.draw_stages[0].calls, 2);
            assert!(profile.draw_stages[1..].iter().all(|stage| stage.calls == 1));
            assert_eq!(profile.shared_sync.calls, 1);
            assert_eq!(profile.shared_sync_requested_bytes, 4096);
            assert_eq!(profile.pipeline_create.calls, 1);
            assert_eq!(profile.pipeline_validation_wait.calls, 1);
            assert_eq!(profile.submit.calls, 1);
            assert_eq!(profile.busy.calls, 2);
            assert!(profile.draw_stages.iter().map(|stage| stage.elapsed_ns).sum::<u64>() <= profile.draw_elapsed_ns);
        } else {
            assert_eq!(profile, CpuProfileSnapshot::default());
        }
        (
            verify_pixels(&device, &texture),
            (
                executor.stats.draws,
                executor.stats.skipped_draws,
                executor.stats.constant_upload_bytes,
                executor.stats.memory_upload_bytes,
                executor.stats.submissions,
            ),
        )
    };
    assert_eq!(render(false), render(true));
}

#[test]
#[ignore = "explicit public release workload measurements; not device FPS or CPU thread time"]
fn authored_frame_cpu_breakdown() {
    let device = common::device();
    for workload in [
        Workload { name: "cached_small", vertex_bytes: 4096, textures: 0, pipeline_churn: false, flush_every: 0 },
        Workload { name: "wide_vertex", vertex_bytes: 1 << 20, textures: 0, pipeline_churn: false, flush_every: 0 },
        Workload { name: "eight_textures", vertex_bytes: 4096, textures: 8, pipeline_churn: false, flush_every: 0 },
        Workload { name: "pipeline_keys", vertex_bytes: 4096, textures: 0, pipeline_churn: true, flush_every: 0 },
        Workload { name: "submit_batches", vertex_bytes: 4096, textures: 0, pipeline_churn: false, flush_every: 8 },
    ] {
        let (mut executor, memory, texture) = setup(&device, workload);
        // Keep this profiler-overhead workload's original full-range baseline.
        executor.set_vertex_range_narrowing(false);
        executor.set_cpu_profiling(true);
        frame(&mut executor, workload, 256);
        let cold = executor.take_cpu_profile();
        let reference = verify_pixels(&device, &texture);
        println!(
            "CPU_PROFILE_COLD name={} create_calls={} create_ns={} validation_wait_ns={} submit_ns={}",
            workload.name,
            cold.pipeline_create.calls,
            cold.pipeline_create.elapsed_ns,
            cold.pipeline_validation_wait.elapsed_ns,
            cold.submit.elapsed_ns
        );
        for pair in 0..7 {
            for enabled in if pair % 2 == 0 { [false, true] } else { [true, false] } {
                executor.set_cpu_profiling(enabled);
                memory.take_counts();
                let before = executor.stats;
                let elapsed = frame(&mut executor, workload, 512);
                let profile = executor.take_cpu_profile();
                let (versions, reads, read_bytes) = memory.take_counts();
                assert_eq!(verify_pixels(&device, &texture), reference);
                assert_eq!(executor.stats.draws - before.draws, 512);
                assert_eq!(executor.stats.skipped_draws, 0);
                assert_eq!(executor.stats.pipelines - before.pipelines, 0);
                if enabled {
                    assert_eq!(profile.draws_recorded, 512);
                    assert!(profile.draw_stages.iter().all(|stage| stage.calls == 512));
                    assert_eq!(profile.shared_sync.calls, 512);
                    assert_eq!(profile.shared_sync_requested_bytes, u64::from(workload.vertex_bytes) * 512);
                    assert_eq!(profile.pipeline_create.calls, 0);
                } else {
                    assert_eq!(profile, CpuProfileSnapshot::default());
                }
                println!(
                    "CPU_PROFILE_FRAME name={} pair={pair} enabled={} draws=512 loop_ns={elapsed} version_calls={versions} read_calls={reads} read_bytes={read_bytes} shared_ns={} busy_ns={} busy_calls={} busy_true={} queue_ns={} queue_scanned={} queue_max={} submit_ns={} texture_misses={}",
                    workload.name,
                    u8::from(enabled),
                    profile.shared_sync.elapsed_ns,
                    profile.busy.elapsed_ns,
                    profile.busy.calls,
                    profile.busy_true,
                    profile.upload_queue_walk.elapsed_ns,
                    profile.upload_queue_entries_scanned,
                    profile.upload_queue_max,
                    profile.submit.elapsed_ns,
                    executor.stats.texture_set_misses - before.texture_set_misses
                );
                if enabled {
                    for (stage, time) in DRAW_STAGE_NAMES.iter().zip(profile.draw_stages) {
                        println!(
                            "CPU_PROFILE_STAGE name={} pair={pair} stage={stage} calls={} ns={}",
                            workload.name, time.calls, time.elapsed_ns
                        );
                    }
                }
            }
        }
    }
}
