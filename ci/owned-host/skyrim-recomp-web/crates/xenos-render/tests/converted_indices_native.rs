//! Authored converted-index draws compared with explicit triangle lists on a real device.
#![cfg(not(target_os = "emscripten"))]

#[path = "../../xenos-shader/tests/common/mod.rs"]
mod asm;
mod common;

use asm::*;
use common::OneTarget;
use std::alloc::{GlobalAlloc, Layout, System};
use std::cell::Cell;
use std::sync::Arc;
use std::time::Instant;
use xenos_render::{PhysicalMemory, ShaderStage, executor::Executor};
use xenos_state::registers as reg;

const SIZE: u32 = 16;
const VERTICES: u32 = 0x10000;
const INDICES: u32 = 0x1000;

// Count only allocations on the calling test thread during Executor::draw. Device setup,
// readback and driver worker threads are outside the measurement.
thread_local! {
    static ALLOCATIONS: Cell<Option<(u64, u64)>> = const { Cell::new(None) };
}
struct CountedAllocator;
#[global_allocator]
static ALLOCATOR: CountedAllocator = CountedAllocator;
fn allocated(bytes: usize) {
    let _ = ALLOCATIONS.try_with(|count| {
        if let Some((calls, total)) = count.get() {
            count.set(Some((calls + 1, total + bytes as u64)));
        }
    });
}
unsafe impl GlobalAlloc for CountedAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        allocated(layout.size());
        unsafe { System.alloc(layout) }
    }
    unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
        allocated(layout.size());
        unsafe { System.alloc_zeroed(layout) }
    }
    unsafe fn realloc(&self, pointer: *mut u8, layout: Layout, size: usize) -> *mut u8 {
        allocated(size);
        unsafe { System.realloc(pointer, layout, size) }
    }
    unsafe fn dealloc(&self, pointer: *mut u8, layout: Layout) {
        unsafe { System.dealloc(pointer, layout) }
    }
}

struct ImmutableMemory(Vec<u8>);
impl PhysicalMemory for ImmutableMemory {
    fn read(&self, addr: u32, out: &mut [u8]) {
        out.copy_from_slice(&self.0[addr as usize..addr as usize + out.len()]);
    }
    fn write(&self, _: u32, _: &[u8]) {
        panic!("fixture has no resolves");
    }
    fn version(&self, _: u32, _: u32) -> Option<u64> {
        Some(1)
    }
}

fn put_vertices(memory: &mut [u8], first: u32, vertices: &[[f32; 3]]) {
    for (index, value) in vertices.iter().flatten().enumerate() {
        let offset = VERTICES as usize + first as usize * 12 + index * 4;
        memory[offset..offset + 4].copy_from_slice(&value.to_be_bytes());
    }
}

fn put_indices(memory: &mut [u8], address: u32, indices: &[u32]) {
    for (index, value) in indices.iter().enumerate() {
        let offset = address as usize + index * 4;
        memory[offset..offset + 4].copy_from_slice(&value.to_be_bytes());
    }
}

fn setup(device: &gpu_webgpu::Device, memory: Vec<u8>, vertex_count: u32) -> (Executor, gpu_webgpu::Texture) {
    let target = OneTarget::new(device, SIZE);
    let texture = target.texture.clone();
    let mut executor = Executor::new(device, Arc::new(ImmutableMemory(memory)), Box::new(target));
    executor.write_registers(&[
        (reg::RB_MODECONTROL, 4),
        (reg::RB_SURFACE_INFO, SIZE),
        (reg::RB_COLOR_INFO, 0),
        (reg::RB_COLOR_MASK, 15),
        (reg::RB_BLENDCONTROL0, 0x0001_0001),
        (reg::RB_DEPTHCONTROL, 0),
        (reg::PA_CL_VTE_CNTL, 0x3F | 1 << 10),
        (reg::PA_CL_VPORT_XSCALE, 8.0f32.to_bits()),
        (reg::PA_CL_VPORT_XOFFSET, 8.0f32.to_bits()),
        (reg::PA_CL_VPORT_YSCALE, (-8.0f32).to_bits()),
        (reg::PA_CL_VPORT_YOFFSET, 8.0f32.to_bits()),
        (reg::PA_CL_VPORT_ZSCALE, 1.0f32.to_bits()),
        (reg::PA_CL_VPORT_ZOFFSET, 0),
        (reg::PA_SC_WINDOW_SCISSOR_TL, 1 << 31),
        (reg::PA_SC_WINDOW_SCISSOR_BR, SIZE | SIZE << 16),
        (reg::PA_SC_SCREEN_SCISSOR_TL, 0),
        (reg::PA_SC_SCREEN_SCISSOR_BR, SIZE | SIZE << 16),
        (reg::SQ_PROGRAM_CNTL, 1 | 1 << 8 | 1 << 31),
        (reg::SQ_VS_CONST, 255 << 12),
        (reg::SQ_PS_CONST, 256 | 255 << 12),
        (reg::VGT_MAX_VTX_INDX, 0xFF_FFFF),
        (reg::SHADER_CONSTANT_FETCH_00_0, VERTICES | 3),
        (reg::SHADER_CONSTANT_FETCH_00_0 + 1, 2 | (vertex_count * 3) << 2),
    ]);
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
    executor.load_shader(
        ShaderStage::Pixel,
        assemble(&[exece(vec![Alu::vector(2, 0, 15, &[r(0), r(0)]).export().into()])]).into(),
    );
    (executor, texture)
}

fn select(executor: &mut Executor, primitive: u32, address: u32, count: u32, color: [f32; 4]) {
    executor.write_registers(&[
        (reg::VGT_DMA_BASE, address),
        (reg::VGT_DMA_SIZE, count | 2 << 30),
        (reg::VGT_DRAW_INITIATOR, primitive | 1 << 11 | count << 16),
    ]);
    for (index, value) in color.into_iter().enumerate() {
        executor.write_registers(&[(reg::SHADER_CONSTANT_000_X + index as u32, value.to_bits())]);
    }
}

#[test]
fn converted_u16_u32_indices_preserve_pixels_and_ring_rewind() {
    let device = common::device();
    let render = |converted: bool| {
        let mut memory = vec![0; 0x100000];
        put_vertices(&mut memory, 0, &[[-1.0, -1.0, 0.5], [0.0, -1.0, 0.5], [-1.0, 0.0, 0.5]]);
        put_vertices(&mut memory, 65536, &[[0.0, 0.0, 0.5], [1.0, 0.0, 0.5], [1.0, 1.0, 0.5], [0.0, 1.0, 0.5]]);
        put_vertices(&mut memory, 8, &[[0.0, -1.0, 0.5], [1.0, -1.0, 0.5], [1.0, 0.0, 0.5], [0.0, 0.0, 0.5]]);
        let fans = [vec![0, 1, 2], vec![65536, 65537, 65538, 65539], vec![8, 9, 10, 11]];
        let lists = [vec![0, 1, 2], vec![65536, 65537, 65538, 65536, 65538, 65539], vec![8, 9, 10, 8, 10, 11]];
        let indices = if converted { &fans } else { &lists };
        for (index, values) in indices.iter().enumerate() {
            put_indices(&mut memory, INDICES + index as u32 * 0x100, values);
        }
        let (mut executor, texture) = setup(&device, memory, 65540);
        for (index, values) in indices.iter().enumerate() {
            let mut color = [0.0, 0.0, 0.0, 1.0];
            color[index] = 1.0;
            select(
                &mut executor,
                if converted { 5 } else { 4 },
                INDICES + index as u32 * 0x100,
                values.len() as u32,
                color,
            );
            executor.draw();
            if index == 1 {
                executor.flush();
            }
        }
        executor.flush();
        assert_eq!((executor.stats.draws, executor.stats.skipped_draws, executor.stats.submissions), (3, 0, 2));
        assert_eq!(executor.stats.index_upload_bytes, if converted { 44 } else { 0 });
        let pixels = OneTarget::read(&device, &texture, SIZE);
        assert!(device.take_errors().is_empty());
        pixels
    };
    let converted = render(true);
    assert_eq!(converted, render(false), "entire target equals explicitly triangulated guest indices");
    for (x, y, rgba) in
        [(1, 14, [255, 0, 0, 255]), (14, 1, [0, 255, 0, 255]), (14, 14, [0, 0, 255, 255]), (1, 1, [0, 0, 0, 0])]
    {
        let start = ((y * SIZE + x) * 4) as usize;
        assert_eq!(&converted[start..start + 4], &rgba);
    }
}

#[test]
#[ignore = "explicit release allocation/timing comparison; no device FPS claim"]
fn converted_index_staging_measurement() {
    let device = common::device();
    for count in [34u32, 130, 1026] {
        let mut memory = vec![0; 0x20000];
        let vertices: Vec<_> = (0..count)
            .map(|index| match index % 3 {
                0 => [-1.0, -1.0, 0.5],
                1 => [1.0, -1.0, 0.5],
                _ => [-1.0, 1.0, 0.5],
            })
            .collect();
        put_vertices(&mut memory, 0, &vertices);
        put_indices(&mut memory, INDICES, &(0..count).collect::<Vec<_>>());
        let (mut executor, texture) = setup(&device, memory, count);
        select(&mut executor, 5, INDICES, count, [1.0, 0.0, 0.0, 1.0]);
        // Limit the synthetic raster work; the measured CPU path still converts every index.
        executor.write_registers(&[(reg::PA_SC_WINDOW_SCISSOR_BR, 1 | 1 << 16)]);
        executor.draw();
        executor.flush();
        let _ = OneTarget::read(&device, &texture, SIZE);
        for sample in 0..7 {
            let before_draws = executor.stats.draws;
            let before_bytes = executor.stats.index_upload_bytes;
            ALLOCATIONS.with(|value| value.set(Some((0, 0))));
            let started = Instant::now();
            for _ in 0..128 {
                executor.draw();
            }
            let elapsed = started.elapsed().as_nanos();
            let (allocations, bytes) = ALLOCATIONS.with(|value| value.replace(None).unwrap());
            executor.flush();
            let _ = OneTarget::read(&device, &texture, SIZE);
            assert_eq!(executor.stats.draws - before_draws, 128);
            assert_eq!(executor.stats.index_upload_bytes - before_bytes, u64::from((count - 2) * 3 * 2) * 128);
            assert_eq!(executor.stats.skipped_draws, 0);
            assert!(device.take_errors().is_empty());
            println!(
                "INDEX_STAGING count={count} sample={sample} draws=128 allocations={allocations} allocated_bytes={bytes} record_ns={elapsed}"
            );
        }
    }
}
