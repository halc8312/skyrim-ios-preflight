//! Executor end to end on a real WebGPU device: Xenos microcode draws (triangle list and
//! rectangle list, vertices fetched from guest memory) rendered into a test target.
#![cfg(not(target_os = "emscripten"))]

#[path = "../../xenos-shader/tests/common/mod.rs"]
mod asm;
mod common;

use std::sync::{Arc, Mutex};

use asm::*;
use common::{Memory, OneTarget};
use xenos_render::ShaderStage;
use xenos_render::executor::Executor;
use xenos_state::registers as r;

const SIZE: u32 = 64;
const VERTICES: u32 = 0x1_0000;

fn f(v: f32) -> u32 {
    v.to_bits()
}

/// oPos = vfetch(position, xyz1); o0 = c0.
fn vertex_shader() -> Vec<u32> {
    let fetch =
        VFetch { dest: 1, dest_swizzle: "xyz1", format: 57, stride: 3, fetch_constant: 0, ..Default::default() };
    assemble(&[exece(vec![
        fetch.into(),
        Alu::vector(2, 62, 0b1111, &[r(1), r(1)]).export().into(),
        Alu::vector(2, 0, 0b1111, &[c(0), c(0)]).export().into(),
    ])])
}

/// oC0 = interpolator 0.
fn pixel_shader() -> Vec<u32> {
    assemble(&[exece(vec![Alu::vector(2, 0, 0b1111, &[r(0), r(0)]).export().into()])])
}

fn base_registers() -> Vec<(u32, u32)> {
    vec![
        (r::RB_MODECONTROL, 4),
        (r::RB_SURFACE_INFO, SIZE),
        (r::RB_COLOR_INFO, 0),
        (r::RB_COLOR_MASK, 0xF),
        (r::RB_BLENDCONTROL0, 0x0001_0001),
        (r::RB_DEPTHCONTROL, 0),
        (r::PA_CL_VTE_CNTL, 0x3F | 1 << 10),
        (r::PA_CL_VPORT_XSCALE, f(SIZE as f32 / 2.0)),
        (r::PA_CL_VPORT_XOFFSET, f(SIZE as f32 / 2.0)),
        (r::PA_CL_VPORT_YSCALE, f(-(SIZE as f32) / 2.0)),
        (r::PA_CL_VPORT_YOFFSET, f(SIZE as f32 / 2.0)),
        (r::PA_CL_VPORT_ZSCALE, f(1.0)),
        (r::PA_CL_VPORT_ZOFFSET, f(0.0)),
        (r::PA_SC_WINDOW_SCISSOR_TL, 1 << 31),
        (r::PA_SC_WINDOW_SCISSOR_BR, SIZE | SIZE << 16),
        (r::PA_SC_SCREEN_SCISSOR_TL, 0),
        (r::PA_SC_SCREEN_SCISSOR_BR, 8192 | 8192 << 16),
        // 2 VS and PS registers, 1 interpolator, vertex index in r0.
        (r::SQ_PROGRAM_CNTL, 1 | 1 << 8 | 1 << 31),
        (r::SQ_VS_CONST, 255 << 12),
        (r::SQ_PS_CONST, 256 | 255 << 12),
        (r::VGT_MAX_VTX_INDX, 0xFF_FFFF),
        // Vertex fetch constant 0: big-endian (8in32) float3 positions.
        (r::SHADER_CONSTANT_FETCH_00_0, VERTICES | 3),
        (r::SHADER_CONSTANT_FETCH_00_0 + 1, 2 | (9 * 4) << 2),
    ]
}

fn color(rgba: [f32; 4]) -> Vec<(u32, u32)> {
    (0..4).map(|i| (r::SHADER_CONSTANT_000_X + i, f(rgba[i as usize]))).collect()
}

fn put_vertices(memory: &Memory, base: u32, positions: &[[f32; 3]]) {
    let bytes: Vec<u8> = positions.iter().flatten().flat_map(|v| v.to_be_bytes()).collect();
    memory.write_bytes(base, &bytes);
}

trait WriteBytes {
    fn write_bytes(&self, addr: u32, data: &[u8]);
}

impl WriteBytes for Memory {
    fn write_bytes(&self, addr: u32, data: &[u8]) {
        xenos_render::PhysicalMemory::write(self, addr, data);
    }
}

fn pixel(data: &[u8], x: u32, y: u32) -> [u8; 4] {
    let i = ((y * SIZE + x) * 4) as usize;
    data[i..i + 4].try_into().unwrap()
}

#[test]
fn triangle_and_rectangle_lists_render() {
    let device = common::device();
    let memory = Arc::new(Memory(Mutex::new(vec![0; 0x2_0000])));
    let target = OneTarget::new(&device, SIZE);
    let texture = target.texture.clone();
    let mut executor = Executor::new(&device, memory.clone(), Box::new(target));
    executor.write_registers(&base_registers());
    executor.load_shader(ShaderStage::Vertex, vertex_shader().into());
    executor.load_shader(ShaderStage::Pixel, pixel_shader().into());

    // Lower-left half triangle in red.
    put_vertices(&memory, VERTICES, &[[-1.0, -1.0, 0.5], [1.0, -1.0, 0.5], [-1.0, 1.0, 0.5]]);
    executor.write_registers(&color([1.0, 0.0, 0.0, 1.0]));
    executor.write_registers(&[(r::VGT_DRAW_INITIATOR, 4 | 2 << 6 | 3 << 16)]);
    executor.draw();

    // Upper-right quadrant as a rectangle list (the fourth corner is derived) in green, with the
    // vertices rewritten in place: the executor must submit the first draw before uploading them.
    put_vertices(&memory, VERTICES, &[[0.0, 1.0, 0.5], [1.0, 1.0, 0.5], [0.0, 0.0, 0.5]]);
    executor.write_registers(&color([0.0, 1.0, 0.0, 1.0]));
    executor.write_registers(&[(r::VGT_DRAW_INITIATOR, 8 | 2 << 6 | 3 << 16)]);
    executor.draw();
    executor.flush();

    assert_eq!(executor.stats.draws, 2, "{:?}", executor.stats);
    assert!(executor.stats.submissions >= 2, "conflicting vertex upload must split the batch");
    let errors = device.take_errors();
    assert!(errors.is_empty(), "{errors:?}");
    let data = OneTarget::read(&device, &texture, SIZE);
    assert_eq!(pixel(&data, 8, 56), [255, 0, 0, 255], "inside the triangle");
    assert_eq!(pixel(&data, 40, 40), [0, 0, 0, 0], "outside both");
    assert_eq!(pixel(&data, 56, 8), [0, 255, 0, 255], "derived rectangle corner");
    assert_eq!(pixel(&data, 34, 30), [0, 255, 0, 255], "rectangle near its diagonal");
    assert_eq!(pixel(&data, 24, 36), [255, 0, 0, 255], "triangle, near its hypotenuse");
}

/// A full register-rewrite stream must produce exactly the same pixels and GPU constant
/// uploads as the canonical stream that sends only changed bits. Both use authored shaders.
#[test]
fn repeated_constant_writes_preserve_pixels_and_uploads() {
    let device = common::device();
    let render = |rewrite_all: bool| {
        let calls_before = gpu_webgpu::counters::snapshot();
        let memory = Arc::new(Memory(Mutex::new(vec![0; 0x2_0000])));
        put_vertices(&memory, VERTICES, &[[-1.0, 1.0, 0.5], [1.0, 1.0, 0.5], [-1.0, -1.0, 0.5]]);
        let target = OneTarget::new(&device, SIZE);
        let texture = target.texture.clone();
        let mut executor = Executor::new(&device, memory, Box::new(target));
        executor.write_registers(&base_registers());
        executor.write_registers(&[(r::VGT_DRAW_INITIATOR, 8 | 2 << 6 | 3 << 16)]);
        let mut prior = [0u32; 2048];
        for tile in 0..16usize {
            let index = [0, 63, 255, 31][tile % 4];
            let fetch = VFetch {
                dest: 1,
                dest_swizzle: "xyz1",
                format: 57,
                stride: 3,
                fetch_constant: 0,
                ..Default::default()
            };
            executor.load_shader(
                ShaderStage::Vertex,
                assemble(&[exece(vec![
                    fetch.into(),
                    Alu::vector(2, 62, 0b1111, &[r(1), r(1)]).export().into(),
                    Alu::vector(2, 0, 0b1111, &[c(index), c(index)]).export().into(),
                ])])
                .into(),
            );
            executor.load_shader(
                ShaderStage::Pixel,
                assemble(&[exece(vec![Alu::vector(0, 0, 0b1111, &[r(0), c(index)]).export().into()])]).into(),
            );
            let mut bank = [0u32; 2048];
            let vs = index as usize * 4;
            let ps = (256 + index as usize) * 4;
            bank[vs] = f((tile & 1) as f32);
            bank[vs + 2] = f(((tile >> 2) & 1) as f32);
            bank[vs + 3] = f(1.0);
            bank[ps + 1] = f(((tile >> 1) & 1) as f32);
            // Distinct NaN payloads and -0 in unread constants exercise exact-bit updates.
            bank[4] = 0x7fc0_0000 | tile as u32;
            bank[5] = 0x8000_0000;
            let writes: Vec<_> = bank
                .iter()
                .enumerate()
                .filter_map(|(i, &value)| {
                    (rewrite_all || prior[i] != value).then_some((r::SHADER_CONSTANT_000_X + i as u32, value))
                })
                .collect();
            executor.write_registers(&writes);
            if rewrite_all {
                executor.write_registers(&writes);
                // Change a single component and put it back before the draw.
                let addr = r::SHADER_CONSTANT_000_X + ps as u32;
                executor.write_registers(&[(addr, f(0.5)), (addr, bank[ps])]);
            }
            prior = bank;
            let x = (tile as u32 % 4) * 16;
            let y = (tile as u32 / 4) * 16;
            executor.write_registers(&[
                (r::PA_SC_WINDOW_SCISSOR_TL, (1 << 31) | x | y << 16),
                (r::PA_SC_WINDOW_SCISSOR_BR, (x + 16) | (y + 16) << 16),
            ]);
            executor.draw();
            if tile == 7 {
                executor.flush(); // uploaded-block invalidation between two equal-sized batches
            }
        }
        executor.flush();
        assert_eq!(executor.stats.draws, 16, "{:?}", executor.stats);
        let data = OneTarget::read(&device, &texture, SIZE);
        let errors = device.take_errors();
        assert!(errors.is_empty(), "{errors:?}");
        // Informational process-global counters: run this case alone (or --test-threads=1)
        // when comparing them. Correctness assertions below are executor-local.
        let calls = gpu_webgpu::counters::snapshot().since(&calls_before);
        println!("constant_pixel rewrite_all={rewrite_all} gpu_calls={calls:?}");
        (data, executor.stats.float_upload_bytes, executor.stats.float_vec4s_changed, executor.stats.submissions)
    };
    let reference = render(false);
    let repeated = render(true);
    assert_eq!(repeated, reference, "all pixels, patch bytes/counts and submission count");
    for y in 0..SIZE {
        for x in 0..SIZE {
            let tile = y / 16 * 4 + x / 16;
            assert_eq!(
                pixel(&repeated.0, x, y),
                [((tile & 1) * 255) as u8, (((tile >> 1) & 1) * 255) as u8, (((tile >> 2) & 1) * 255) as u8, 255],
                "pixel {x},{y}"
            );
        }
    }
}

const INDICES: u32 = 0x1_4000;

/// DMA-indexed draws read the guest index buffer straight from GPU memory: a big-endian 16-bit
/// strip restarting at 0xFFFF, then a big-endian 32-bit triangle list; nothing goes through the
/// index ring.
#[test]
fn guest_index_buffers_draw_directly() {
    let device = common::device();
    let memory = Arc::new(Memory(Mutex::new(vec![0; 0x2_0000])));
    let target = OneTarget::new(&device, SIZE);
    let texture = target.texture.clone();
    let mut executor = Executor::new(&device, memory.clone(), Box::new(target));
    executor.write_registers(&base_registers());
    executor.load_shader(ShaderStage::Vertex, vertex_shader().into());
    executor.load_shader(ShaderStage::Pixel, pixel_shader().into());

    // Vertices 0..2: lower-left corner triangle; 3..5: upper-right corner triangle. Without the
    // restart the strip would also fill the area between them.
    put_vertices(
        &memory,
        VERTICES,
        &[[-1.0, -1.0, 0.5], [-0.5, -1.0, 0.5], [-1.0, -0.5, 0.5], [1.0, 1.0, 0.5], [0.5, 1.0, 0.5], [1.0, 0.5, 0.5]],
    );
    let strip: Vec<u8> = [0u16, 1, 2, 0xFFFF, 3, 4, 5].iter().flat_map(|i| i.to_be_bytes()).collect();
    memory.write_bytes(INDICES, &strip);
    executor.write_registers(&color([1.0, 0.0, 0.0, 1.0]));
    executor.write_registers(&[
        (r::PA_SU_SC_MODE_CNTL, 1 << 21),
        (r::VGT_MULTI_PRIM_IB_RESET_INDX, 0xFFFF),
        (r::VGT_DMA_BASE, INDICES),
        // 7 words, 8in16.
        (r::VGT_DMA_SIZE, 7 | 1 << 30),
        // Triangle strip, DMA, 16-bit, 7 indices.
        (r::VGT_DRAW_INITIATOR, 6 | 7 << 16),
    ]);
    executor.draw();

    // A 32-bit list drawing the lower-right corner from vertices rewritten at 6..8.
    put_vertices(&memory, VERTICES + 6 * 12, &[[1.0, -1.0, 0.5], [1.0, -0.5, 0.5], [0.5, -1.0, 0.5]]);
    let list: Vec<u8> = [6u32, 7, 8].iter().flat_map(|i| i.to_be_bytes()).collect();
    memory.write_bytes(INDICES + 0x100, &list);
    executor.write_registers(&color([0.0, 0.0, 1.0, 1.0]));
    executor.write_registers(&[
        (r::PA_SU_SC_MODE_CNTL, 0),
        (r::VGT_DMA_BASE, INDICES + 0x100),
        // 3 words, 8in32.
        (r::VGT_DMA_SIZE, 3 | 2 << 30),
        // Triangle list, DMA, 32-bit, 3 indices.
        (r::VGT_DRAW_INITIATOR, 4 | 1 << 11 | 3 << 16),
    ]);
    executor.draw();
    executor.flush();

    assert_eq!(executor.stats.draws, 2, "{:?}", executor.stats);
    assert_eq!(executor.stats.index_upload_bytes, 0, "indices are read from shared memory");
    let errors = device.take_errors();
    assert!(errors.is_empty(), "{errors:?}");
    let data = OneTarget::read(&device, &texture, SIZE);
    assert_eq!(pixel(&data, 2, 61), [255, 0, 0, 255], "first strip triangle");
    assert_eq!(pixel(&data, 61, 2), [255, 0, 0, 255], "strip triangle after the restart");
    assert_eq!(pixel(&data, 32, 32), [0, 0, 0, 0], "between them: the strip restarted");
    assert_eq!(pixel(&data, 61, 61), [0, 0, 255, 255], "32-bit list triangle");
}

const RESOLVE_VERTICES: u32 = 0x1_8000;
const DEST: u32 = 0x4_0000;

/// Draw → resolve → swap through the renderer thread with the real EDRAM emulation: the resolve
/// is stored in GPU memory (guest memory stays untouched) and the presented frame shows it.
#[test]
fn draw_resolve_and_present_through_gpu_memory() {
    use xenos_render::Command;
    use xenos_render::native::{self, NativeOptions};

    let memory = Arc::new(Memory(Mutex::new(vec![0; 0x8_0000])));
    put_vertices(&memory, VERTICES, &[[-1.0, -1.0, 0.5], [1.0, -1.0, 0.5], [-1.0, 1.0, 0.5]]);
    let corners: Vec<u8> =
        [0.0f32, 0.0, SIZE as f32, 0.0, SIZE as f32, SIZE as f32].iter().flat_map(|v| v.to_be_bytes()).collect();
    memory.write_bytes(RESOLVE_VERTICES, &corners);

    let dir = std::path::PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join(format!("resolve-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    let handle = native::start(
        memory.clone(),
        NativeOptions { dump_dir: Some(dir.clone()), dump_every: 1, window: None },
        Box::new(|device| Box::new(xenos_render::edram::EdramTargets::new(device))),
    )
    .expect("renderer");
    let mut registers = base_registers();
    registers.extend(color([1.0, 0.0, 0.0, 1.0]));
    handle.submit(Command::Registers(registers));
    handle.submit(Command::Shader { stage: ShaderStage::Vertex, code: vertex_shader().into() });
    handle.submit(Command::Shader { stage: ShaderStage::Pixel, code: pixel_shader().into() });
    handle.submit(Command::Registers(vec![(r::VGT_DRAW_INITIATOR, 4 | 2 << 6 | 3 << 16)]));
    handle.submit(Command::Draw);
    // Resolve the 64x64 surface to DEST as tiled k_8_8_8_8 (rectangle from vertex fetch 0).
    handle.submit(Command::Registers(vec![
        (r::RB_MODECONTROL, 6),
        (r::RB_COPY_CONTROL, 1 << 20),
        (r::RB_COPY_DEST_BASE, DEST),
        (r::RB_COPY_DEST_PITCH, SIZE | SIZE << 16),
        (r::RB_COPY_DEST_INFO, 6 << 7),
        (r::SHADER_CONSTANT_FETCH_00_0, RESOLVE_VERTICES | 3),
        (r::SHADER_CONSTANT_FETCH_00_0 + 1, 2 | 6 << 2),
        (r::VGT_DRAW_INITIATOR, 8 | 2 << 6 | 3 << 16),
    ]));
    handle.submit(Command::Draw);
    let mut front = xenos_texture::TextureFetchConstant::new_2d(xenos_texture::TextureFormat::k_8_8_8_8, SIZE, SIZE);
    front.tiled = true;
    front.base_page = DEST >> 12;
    handle.submit(Command::Swap { fetch: front.encode(), width: SIZE, height: SIZE });
    handle.wait_idle();

    let untouched =
        memory.0.lock().unwrap()[DEST as usize..DEST as usize + (SIZE * SIZE * 4) as usize].iter().all(|&b| b == 0);
    assert!(untouched, "the resolve went through guest memory");

    let png = dir.join("frame_000001.png");
    let decoder = png::Decoder::new(std::fs::File::open(&png).expect("frame dumped"));
    let mut reader = decoder.read_info().unwrap();
    let mut rgba = vec![0u8; reader.output_buffer_size()];
    let info = reader.next_frame(&mut rgba).unwrap();
    assert_eq!((info.width, info.height), (SIZE, SIZE));
    let px = |x: u32, y: u32| -> [u8; 4] { rgba[((y * SIZE + x) * 4) as usize..][..4].try_into().unwrap() };
    assert_eq!(px(8, 56), [255, 0, 0, 255], "inside the triangle");
    assert_eq!(px(56, 8), [0, 0, 0, 255], "outside the triangle");
}
