//! Predictions versus the real push path across reuse, A-B-A-C and rewinds.
use super::*;

#[path = "../../xenos-shader/tests/common/mod.rs"]
mod asm;
#[path = "../tests/common/mod.rs"]
mod draw_support;

/// Force the actual late-flush branch, then overwrite its vertex page in the
/// following draw before submission. The reference pixels test both consumed
/// clears and the captured memory snapshot. Dropping hazards is a negative control.
#[test]
fn late_constant_flush_keeps_clears_offsets_and_vertex_snapshot() {
    use asm::*;
    use std::sync::Mutex;
    use xenos_state::registers as r;
    const SIZE: u32 = 16;
    const VERTICES: u32 = 0x10000;
    let device = draw_support::device();
    let run = |drop_hazards: bool| {
        let memory = Arc::new(draw_support::Memory(Mutex::new(vec![0; 0x20000])));
        let positions: [[f32; 3]; 6] = [
            [-1.0, -1.0, 0.5], [1.0, -1.0, 0.5], [-1.0, 1.0, 0.5],
            [1.0, -1.0, 0.5], [1.0, 1.0, 0.5], [-1.0, 1.0, 0.5],
        ];
        let vertices: Vec<_> = positions.iter().flatten().flat_map(|v| v.to_be_bytes()).collect();
        memory.write(VERTICES, &vertices);
        let mut target = draw_support::OneTarget::new(&device, SIZE);
        target.initial_clear = Some(Color { r: 0.0, g: 0.0, b: 0.0, a: 1.0 });
        let texture = target.texture.clone();
        let mut executor = Executor::new(&device, memory.clone(), Box::new(target));
        executor.causal = Some(Arc::new(crate::causal::Session::new(1, std::time::Instant::now())));
        let fetch = VFetch { dest: 1, dest_swizzle: "xyz1", format: 57, stride: 3, fetch_constant: 0, ..Default::default() };
        executor.load_shader(Stage::Vertex, assemble(&[exece(vec![
            fetch.into(), Alu::vector(2, 62, 15, &[r(1), r(1)]).export().into(),
            Alu::vector(2, 0, 15, &[c(0), c(0)]).export().into(),
        ])]).into());
        executor.load_shader(Stage::Pixel, assemble(&[exece(vec![Alu::vector(2, 0, 15, &[r(0), r(0)]).export().into()])]).into());
        executor.write_registers(&[
            (r::RB_MODECONTROL, 4), (r::RB_SURFACE_INFO, SIZE), (r::RB_COLOR_INFO, 0),
            (r::RB_COLOR_MASK, 15), (r::RB_BLENDCONTROL0, 0x00010001), (r::RB_DEPTHCONTROL, 0),
            (r::PA_CL_VTE_CNTL, 0x3f | 1 << 10),
            (r::PA_CL_VPORT_XSCALE, 8f32.to_bits()), (r::PA_CL_VPORT_XOFFSET, 8f32.to_bits()),
            (r::PA_CL_VPORT_YSCALE, (-8f32).to_bits()), (r::PA_CL_VPORT_YOFFSET, 8f32.to_bits()),
            (r::PA_CL_VPORT_ZSCALE, 1f32.to_bits()), (r::PA_CL_VPORT_ZOFFSET, 0),
            (r::PA_SC_WINDOW_SCISSOR_TL, 1 << 31), (r::PA_SC_WINDOW_SCISSOR_BR, SIZE | SIZE << 16),
            (r::PA_SC_SCREEN_SCISSOR_TL, 0), (r::PA_SC_SCREEN_SCISSOR_BR, 8192 | 8192 << 16),
            (r::SQ_PROGRAM_CNTL, 1 | 1 << 8 | 1 << 31), (r::SQ_VS_CONST, 255 << 12),
            (r::SQ_PS_CONST, 256 | 255 << 12), (r::VGT_MAX_VTX_INDX, 0xffffff),
            (r::SHADER_CONSTANT_FETCH_00_0, VERTICES | 3),
            (r::SHADER_CONSTANT_FETCH_00_0 + 1, 2 | (vertices.len() as u32) << 2),
            (r::VGT_DRAW_INITIATOR, 4 | 2 << 6 | 6 << 16),
        ]);
        let color = |red: f32, green: f32, blue: f32| [red, green, blue, 1.0].into_iter().enumerate()
            .map(|(i, value)| (r::SHADER_CONSTANT_000_X + i as u32, value.to_bits())).collect::<Vec<_>>();
        executor.write_registers(&color(1.0, 0.0, 0.0));
        executor.draw();
        executor.floats.exhaust_window_for_test();
        executor.write_registers(&color(0.0, 1.0, 0.0));
        executor.write_registers(&[(r::PA_SC_WINDOW_SCISSOR_TL, (1 << 31) | SIZE / 2)]);
        executor.draw();
        assert_eq!(executor.causal.as_ref().unwrap().snapshot().reasons[5], 1, "late capacity branch was not exercised");
        assert!(executor.failure.is_none());
        if drop_hazards { executor.shared.end_batch(); }
        let outside: Vec<_> = (0..6).flat_map(|_| [2f32, 2f32, 0.5].into_iter().flat_map(f32::to_be_bytes)).collect();
        memory.write(VERTICES, &outside);
        executor.write_registers(&color(0.0, 0.0, 1.0));
        executor.draw();
        executor.end_frame();
        assert!(executor.failure.is_none());
        assert_eq!((executor.stats.draws, executor.stats.skipped_draws), (3, 0));
        device.block_on(device.queue().on_submitted_work_done()).unwrap();
        let bytes = draw_support::OneTarget::read(&device, &texture, SIZE);
        assert!(device.take_errors().is_empty());
        let pixel = |x: usize| <[u8; 4]>::try_from(&bytes[(8 * SIZE as usize + x) * 4..][..4]).unwrap();
        (pixel(3), pixel(12))
    };
    assert_eq!(run(false), ([255, 0, 0, 255], [0, 255, 0, 255]));
    assert_ne!(run(true).1, [0, 255, 0, 255], "oracle failed to expose lost batch protection");
}

struct EmptyMemory;
impl PhysicalMemory for EmptyMemory {
    fn read(&self, _: u32, output: &mut [u8]) { output.fill(0); }
    fn write(&self, _: u32, _: &[u8]) {}
}

#[test]
fn predictions_match_actual_blocks_values_and_offsets_without_consuming_dirty_bits() {
    let instance = Instance::new().unwrap();
    let adapter = instance.block_on(instance.request_adapter(&RequestAdapterOptions::default())).unwrap();
    let device = instance.block_on(adapter.request_device(&DeviceDescriptor::default())).unwrap();
    let alignment = u64::from(device.limits().minUniformBufferOffsetAlignment.max(device.limits().minStorageBufferOffsetAlignment)).max(16);
    let mut executor = Executor::new(&device, Arc::new(EmptyMemory), Box::new(crate::edram::EdramTargets::new(&device)));
    let mut system = SystemConstants::default();
    for (step, (value, reads)) in [
        (1u32, [1, 0]), (2, [3, 2]), (1, [1, 2]), (3, [3, 2]),
        (3, [3, 2]), (3, [0, 0]), (1, [3, 3]), (1, [3, 3]),
    ].into_iter().enumerate() {
        if step == 6 { executor.flush(); }
        executor.regs.write(xenos_state::registers::SHADER_CONSTANT_000_X, value);
        executor.regs.write(xenos_state::registers::SHADER_CONSTANT_000_X + 256 * 4, value + 17);
        system.vertex_base_index = value as i32;
        let before = executor.floats.allocation_state();
        let (demands, count) = executor.constant_demands(&system, reads);
        assert_eq!(executor.floats.allocation_state(), before, "estimation committed data");
        let mut cursor = before.0;
        let mut values = before.2;
        for demand in &demands[..count] {
            cursor = cursor.next_multiple_of(alignment) + demand.bytes.max(16);
            values += demand.changed_vec4s;
        }
        assert!(executor.floats.has_room_for(&demands[..count]));
        executor.push_constants(&system, reads);
        assert_eq!(executor.floats.allocation_state(), (cursor, before.1 + count, values), "step {step}");
        assert_eq!(executor.constant_demands(&system, reads).1, 0, "identical constants must reuse blocks");
    }
    executor.flush();
    device.block_on(device.queue().on_submitted_work_done()).unwrap();
    assert!(device.take_errors().is_empty());
}
