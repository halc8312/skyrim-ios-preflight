//! Public authored Executor A/B oracle: full synchronization versus proven ranges.
#![cfg(not(target_os = "emscripten"))]

#[path = "../../xenos-shader/tests/common/mod.rs"]
mod asm;
mod common;

use asm::*;
use common::OneTarget;
use std::sync::{
    Arc, Mutex,
    atomic::{AtomicU64, Ordering},
};
use xenos_render::{
    PhysicalMemory, ShaderStage,
    executor::{Executor, ExecutorStats},
};
use xenos_state::registers as reg;

const SIZE: u32 = 16;
const BASE: u32 = 0x10000;
const INDEX: u32 = 0x1000;
const DECLARED: u32 = 1 << 20;
const PAGE: u32 = 4096;
const LEFT: [[f32; 3]; 3] = [[-1.0, -1.0, 0.5], [0.0, -1.0, 0.5], [-1.0, 0.0, 0.5]];
const RIGHT: [[f32; 3]; 3] = [[1.0, 1.0, 0.5], [0.0, 1.0, 0.5], [1.0, 0.0, 0.5]];

struct Memory {
    bytes: Mutex<Vec<u8>>,
    versions: Vec<AtomicU64>,
    queries: AtomicU64,
    reads: AtomicU64,
}
impl Memory {
    fn new() -> Arc<Self> {
        Arc::new(Self {
            bytes: Mutex::new(vec![0; 4 << 20]),
            versions: (0..1024).map(|_| AtomicU64::new(1)).collect(),
            queries: AtomicU64::new(0),
            reads: AtomicU64::new(0),
        })
    }
    fn put(&self, address: u32, data: &[u8]) {
        assert!(!data.is_empty());
        self.bytes.lock().unwrap()[address as usize..address as usize + data.len()].copy_from_slice(data);
        for p in address / PAGE..=(address + data.len() as u32 - 1) / PAGE {
            self.versions[p as usize].fetch_add(1, Ordering::Relaxed);
        }
    }
    fn word(&self, address: u32, value: u32, endian: u32) {
        self.put(address, &swap(value, endian).to_le_bytes());
    }
    fn vertices(&self, base: u32, first: u32, stride: u32, offset: u32, endian: u32, data: &[[f32; 3]]) {
        for (i, vertex) in data.iter().enumerate() {
            for (j, value) in vertex.iter().enumerate() {
                self.word(base + ((first + i as u32) * stride + offset + j as u32) * 4, value.to_bits(), endian);
            }
        }
    }
    fn observations(&self) -> (u64, u64) {
        (self.queries.load(Ordering::Relaxed), self.reads.load(Ordering::Relaxed))
    }
}
impl PhysicalMemory for Memory {
    fn read(&self, address: u32, output: &mut [u8]) {
        self.reads.fetch_add(1, Ordering::Relaxed);
        output.copy_from_slice(&self.bytes.lock().unwrap()[address as usize..address as usize + output.len()]);
    }
    fn write(&self, _: u32, _: &[u8]) {
        panic!("authored fixture has no guest-memory resolves");
    }
    fn version(&self, address: u32, len: u32) -> Option<u64> {
        assert_eq!((address % PAGE, len), (0, PAGE));
        self.queries.fetch_add(1, Ordering::Relaxed);
        Some(self.versions[(address / PAGE) as usize].load(Ordering::Relaxed))
    }
}
fn swap(value: u32, endian: u32) -> u32 {
    match endian {
        0 => value,
        1 => ((value & 0x00ff_00ff) << 8) | ((value & 0xff00_ff00) >> 8),
        2 => value.swap_bytes(),
        3 => value.rotate_left(16),
        _ => panic!("bad authored endian"),
    }
}
fn fetch(stride: u32, offset: i32) -> VFetch {
    VFetch { dest: 1, dest_swizzle: "xyz1", format: 57, stride, offset, fetch_constant: 0, ..Default::default() }
}
fn shader(instructions: Vec<Ins>, position: u32, color: Src) -> Arc<[u32]> {
    let mut instructions = instructions;
    instructions.push(Alu::vector(2, 62, 15, &[r(position), r(position)]).export().into());
    instructions.push(Alu::vector(2, 0, 15, &[color, color]).export().into());
    assemble(&[exece(instructions)]).into()
}
fn setup(
    device: &gpu_webgpu::Device,
    memory: Arc<Memory>,
    narrow: bool,
    code: Arc<[u32]>,
) -> (Executor, gpu_webgpu::Texture) {
    let target = OneTarget::new(device, SIZE);
    let texture = target.texture.clone();
    let mut executor = Executor::new(device, memory, Box::new(target));
    executor.set_vertex_range_narrowing(narrow);
    executor.write_registers(&[
        (reg::RB_MODECONTROL, 4),
        (reg::RB_SURFACE_INFO, SIZE),
        (reg::RB_COLOR_INFO, 0),
        (reg::RB_COLOR_MASK, 15),
        (reg::RB_BLENDCONTROL0, 0x0001_0001),
        (reg::RB_DEPTHCONTROL, 0),
        (reg::PA_CL_VTE_CNTL, 0x3f | 1 << 10),
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
        (reg::SQ_PROGRAM_CNTL, 3 | 1 << 8 | 1 << 31),
        (reg::SQ_VS_CONST, 255 << 12),
        (reg::SQ_PS_CONST, 256 | 255 << 12),
        (reg::VGT_MIN_VTX_INDX, 0),
        (reg::VGT_MAX_VTX_INDX, 2),
        (reg::SHADER_CONSTANT_BOOL_000_031, 1),
    ]);
    binding(&mut executor, BASE, DECLARED, 2);
    color(&mut executor, [1.0, 0.0, 0.0, 1.0]);
    executor.load_shader(ShaderStage::Vertex, code);
    executor.load_shader(
        ShaderStage::Pixel,
        assemble(&[exece(vec![Alu::vector(2, 0, 15, &[r(0), r(0)]).export().into()])]).into(),
    );
    select_auto(&mut executor, 4, 3, 0);
    (executor, texture)
}
fn binding(executor: &mut Executor, base: u32, bytes: u32, endian: u32) {
    executor.write_registers(&[
        (reg::SHADER_CONSTANT_FETCH_00_0, base | 3),
        (reg::SHADER_CONSTANT_FETCH_00_0 + 1, bytes | endian),
    ]);
}
fn color(executor: &mut Executor, value: [f32; 4]) {
    for (i, v) in value.into_iter().enumerate() {
        executor.write_registers(&[(reg::SHADER_CONSTANT_000_X + i as u32, v.to_bits())]);
    }
}
fn select_auto(executor: &mut Executor, primitive: u32, count: u32, offset: u32) {
    executor.write_registers(&[
        (reg::VGT_DRAW_INITIATOR, primitive | 2 << 6 | count << 16),
        (reg::VGT_INDX_OFFSET, offset),
    ]);
}
struct Frame {
    pixels: Vec<u8>,
    queries: u64,
    reads: u64,
    uploaded: u64,
}
fn frame(
    executor: &mut Executor,
    memory: &Memory,
    device: &gpu_webgpu::Device,
    texture: &gpu_webgpu::Texture,
) -> Frame {
    let (queries, reads) = memory.observations();
    let before = executor.stats;
    executor.draw();
    executor.flush();
    let pixels = OneTarget::read(device, texture, SIZE);
    assert_eq!(executor.stats.draws, before.draws + 1, "{:?}", executor.stats);
    assert_eq!(executor.stats.skipped_draws, 0);
    let errors = device.take_errors();
    assert!(errors.is_empty(), "{errors:?}");
    let after = memory.observations();
    Frame {
        pixels,
        queries: after.0 - queries,
        reads: after.1 - reads,
        uploaded: executor.stats.memory_upload_bytes - before.memory_upload_bytes,
    }
}
fn pixel(frame: &Frame, x: u32, y: u32) -> &[u8] {
    let i = ((y * SIZE + x) * 4) as usize;
    &frame.pixels[i..i + 4]
}
fn shape(stats: ExecutorStats) -> (u64, u64, u64, u64) {
    (stats.draws, stats.skipped_draws, stats.submissions, stats.index_upload_bytes)
}

#[test]
fn sequential_page_crossings_mutations_and_current_constants_match_full_sync() {
    let device = common::device();
    let run = |narrow| {
        let memory = Memory::new();
        let base = BASE + PAGE - 16;
        memory.vertices(base, 0, 5, 2, 2, &LEFT);
        let (mut executor, texture) = setup(&device, memory.clone(), narrow, shader(vec![fetch(5, 2).into()], 1, c(0)));
        binding(&mut executor, base, DECLARED, 2);
        let mut frames = vec![frame(&mut executor, &memory, &device, &texture)];
        frames.push(frame(&mut executor, &memory, &device, &texture));
        // A changed page outside this draw's envelope must be noticed when a later
        // draw changes the fetch constant to use it. The cached proof is retained.
        let later = BASE + 0x80000;
        memory.vertices(later, 0, 5, 2, 2, &RIGHT);
        frames.push(frame(&mut executor, &memory, &device, &texture));
        memory.vertices(base, 0, 5, 2, 2, &RIGHT);
        color(&mut executor, [0.0, 1.0, 0.0, 1.0]);
        frames.push(frame(&mut executor, &memory, &device, &texture));
        binding(&mut executor, later, DECLARED, 2);
        color(&mut executor, [0.0, 0.0, 1.0, 1.0]);
        frames.push(frame(&mut executor, &memory, &device, &texture));
        (frames, executor.stats)
    };
    let (full, full_stats) = run(false);
    let (narrow, narrow_stats) = run(true);
    assert_eq!(shape(full_stats), shape(narrow_stats));
    assert_eq!(full.len(), 5);
    for (a, b) in full.iter().zip(&narrow) {
        assert_eq!(a.pixels, b.pixels);
    }
    assert_eq!(pixel(&narrow[0], 1, 14), [255, 0, 0, 255]);
    assert_eq!(pixel(&narrow[3], 14, 1), [0, 255, 0, 255]);
    assert_eq!(pixel(&narrow[4], 14, 1), [0, 0, 255, 255]);
    assert_eq!((full[1].queries, narrow[1].queries), (257, 2));
    assert_eq!((full[1].reads, narrow[1].reads, full[1].uploaded, narrow[1].uploaded), (0, 0, 0, 0));
    assert!(full[2].uploaded > 0);
    assert_eq!((narrow[2].reads, narrow[2].uploaded), (0, 0));
    assert!(narrow[3].uploaded > 0 && narrow[4].uploaded > 0);
    assert_eq!(
        (
            narrow_stats.vertex_range_eligible_bindings,
            narrow_stats.vertex_range_narrowed_bindings,
            narrow_stats.vertex_range_avoided_pages,
            narrow_stats.vertex_range_fallback_bindings
        ),
        (5, 5, 5 * 255, 0)
    );
    assert_eq!(full_stats.vertex_range_fallback_bindings, 5);
}

#[test]
fn direct_dma_u16_u32_endian_and_complete_clamps_match_full_sync() {
    let device = common::device();
    for wide in [false, true] {
        for endian in 0..4 {
            let run = |narrow| {
                let memory = Memory::new();
                memory.vertices(BASE, 10, 3, 0, 2, &LEFT);
                // Values below/inside/above the clamp, not a host-side min/max scan.
                let logical = [0u32, 10, if wide { 0xffff_fffe } else { 65534 }];
                let normalized = if wide { endian } else { [0, 1, 1, 0][endian as usize] };
                for (i, value) in logical.into_iter().enumerate() {
                    if wide {
                        memory.word(INDEX + i as u32 * 4, value, normalized);
                    } else {
                        memory.put(
                            INDEX + i as u32 * 2,
                            &(if normalized == 0 { value as u16 } else { (value as u16).swap_bytes() }).to_le_bytes(),
                        );
                    }
                }
                let (mut executor, texture) =
                    setup(&device, memory.clone(), narrow, shader(vec![fetch(3, 0).into()], 1, c(0)));
                executor.write_registers(&[
                    (reg::VGT_MIN_VTX_INDX, 10),
                    (reg::VGT_MAX_VTX_INDX, 12),
                    (reg::VGT_INDX_OFFSET, 1),
                    (reg::VGT_DMA_BASE, INDEX),
                    (reg::VGT_DMA_SIZE, 3 | endian << 30),
                    (reg::VGT_DRAW_INITIATOR, 4 | u32::from(wide) << 11 | 3 << 16),
                ]);
                let first = frame(&mut executor, &memory, &device, &texture);
                let warm = frame(&mut executor, &memory, &device, &texture);
                (first, warm, executor.stats)
            };
            let (a, aw, sa) = run(false);
            let (b, bw, sb) = run(true);
            assert_eq!(a.pixels, b.pixels, "{wide}/{endian}");
            assert_eq!(aw.pixels, bw.pixels);
            assert_eq!(pixel(&b, 1, 14), [255, 0, 0, 255]);
            assert_eq!(shape(sa), shape(sb));
            assert_eq!(sa.index_upload_bytes, 0, "real direct-index path");
            assert_eq!(sb.vertex_range_narrowed_bindings, 2);
            assert_eq!((aw.queries, bw.queries), (258, 3)); // both existing index syncs retained
            assert_eq!((aw.reads, bw.reads), (0, 0));
        }
    }
}

#[test]
fn mini_fetch_inheritance_and_all_vertex_endians_match_full_sync() {
    let device = common::device();
    for endian in 0..4 {
        let run = |narrow| {
            let memory = Memory::new();
            memory.vertices(BASE, 0, 6, 0, endian, &LEFT);
            for i in 0..3 {
                memory.word(BASE + (i * 6 + 4) * 4, 0xff00_ff00, endian);
            }
            let full = VFetch { dest: 0, ..fetch(6, 0) }; // overwrites entry r0.x
            let mini = VFetch {
                dest: 2,
                dest_swizzle: "xyzw",
                format: 6,
                mini: true,
                stride: 6,
                offset: 4,
                fetch_constant: 0,
                ..Default::default()
            };
            let (mut executor, texture) =
                setup(&device, memory.clone(), narrow, shader(vec![full.into(), mini.into()], 0, r(2)));
            binding(&mut executor, BASE, DECLARED, endian);
            let image = frame(&mut executor, &memory, &device, &texture);
            (image, executor.stats)
        };
        let (a, sa) = run(false);
        let (b, sb) = run(true);
        assert_eq!(a.pixels, b.pixels);
        assert_eq!(pixel(&b, 1, 14), [0, 255, 0, 255]);
        assert_eq!(shape(sa), shape(sb));
        assert_eq!(sb.vertex_range_narrowed_bindings, 1);
    }
}

#[test]
fn declared_end_clips_partial_words_and_proven_zero_reads() {
    let device = common::device();
    for outside in [false, true] {
        let run = |narrow| {
            let memory = Memory::new();
            let positions = 0x200000;
            memory.vertices(positions, 0, 3, 0, 2, &LEFT);
            memory.word(BASE + DECLARED - 4, 1.0f32.to_bits(), 2);
            // Only x is inside the declared buffer, or all three requested
            // words are outside it. The WGSL must independently zero each word.
            let color_fetch = VFetch {
                dest: 2,
                dest_swizzle: "xyz1",
                format: 38,
                stride: 0,
                offset: (DECLARED / 4 - u32::from(!outside)) as i32,
                fetch_constant: 0,
                ..Default::default()
            };
            let position_fetch = VFetch { fetch_constant: 1, ..fetch(3, 0) };
            let (mut executor, texture) = setup(
                &device,
                memory.clone(),
                narrow,
                shader(vec![color_fetch.into(), position_fetch.into()], 1, r(2)),
            );
            executor.write_registers(&[
                (reg::SHADER_CONSTANT_FETCH_00_0 + 2, positions | 3),
                (reg::SHADER_CONSTANT_FETCH_00_0 + 3, DECLARED | 2),
            ]);
            let image = frame(&mut executor, &memory, &device, &texture);
            (image, executor.stats)
        };
        let (a, sa) = run(false);
        let (b, sb) = run(true);
        assert_eq!(a.pixels, b.pixels);
        assert_eq!(pixel(&b, 1, 14), [if outside { 0 } else { 255 }, 0, 0, 255]);
        assert_eq!(shape(sa), shape(sb));
        assert_eq!(sb.vertex_range_narrowed_bindings, 2);
        assert_eq!(sb.vertex_range_avoided_pages, if outside { 511 } else { 510 });
        assert_eq!(a.queries, 512);
        assert_eq!(b.queries, if outside { 1 } else { 2 });
    }
}

#[test]
fn unknown_shaders_converted_expanded_small_and_dense_ranges_keep_full_sync() {
    let device = common::device();
    for case in 0..6 {
        let run = |narrow| {
            let memory = Memory::new();
            let first = if case == 0 { 8192 } else { 0 };
            memory.vertices(BASE, first, 3, 0, 2, &LEFT);
            let code = match case {
                0 => shader(vec![Alu::vector(0, 0, 1, &[r(0), c(1)]).into(), fetch(3, 0).into()], 1, c(0)),
                1 => assemble(&[cexec(
                    Cond::Bool(0, true),
                    true,
                    vec![
                        fetch(3, 0).into(),
                        Alu::vector(2, 62, 15, &[r(1), r(1)]).export().into(),
                        Alu::vector(2, 0, 15, &[c(0), c(0)]).export().into(),
                    ],
                )])
                .into(),
                _ => shader(vec![fetch(3, 0).into()], 1, c(0)),
            };
            let (mut executor, texture) = setup(&device, memory.clone(), narrow, code);
            executor.write_registers(&[(reg::SHADER_CONSTANT_000_X + 4, (first as f32).to_bits())]);
            match case {
                2 => select_auto(&mut executor, 5, 3, 0), // fan -> converted indices
                3 => select_auto(&mut executor, 8, 3, 0), // rectangle -> shader expansion
                4 => binding(&mut executor, BASE, 4096, 2),
                5 => {
                    for (i, v) in [0u32, 1, 2].into_iter().enumerate() {
                        memory.word(INDEX + i as u32 * 4, v, 2);
                    }
                    executor.write_registers(&[
                        (reg::VGT_MAX_VTX_INDX, 100_000),
                        (reg::VGT_DMA_BASE, INDEX),
                        (reg::VGT_DMA_SIZE, 3 | 2 << 30),
                        (reg::VGT_DRAW_INITIATOR, 4 | 1 << 11 | 3 << 16),
                    ]);
                }
                _ => (),
            }
            let image = frame(&mut executor, &memory, &device, &texture);
            (image, executor.stats)
        };
        let (a, sa) = run(false);
        let (b, sb) = run(true);
        assert_eq!(a.pixels, b.pixels, "fallback {case}");
        assert!(b.pixels.chunks_exact(4).any(|p| p == [255, 0, 0, 255]), "visible fallback {case}");
        assert_eq!(shape(sa), shape(sb));
        assert_eq!((a.queries, a.reads, a.uploaded), (b.queries, b.reads, b.uploaded), "full work {case}");
        assert_eq!(
            (sb.vertex_range_narrowed_bindings, sb.vertex_range_avoided_pages, sb.vertex_range_fallback_bindings),
            (0, 0, 1)
        );
    }
}
