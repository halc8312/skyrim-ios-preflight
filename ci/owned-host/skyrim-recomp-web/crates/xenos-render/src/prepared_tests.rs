use super::*;
use std::cell::Cell;
use xenos_state::bitfields::{VgtDmaSize, VgtDrawInitiator};
use xenos_state::primitive::{HostIndices, HostTopology, PrimitiveExpansion};
use xenos_state::registers as r;
use xenos_state::xenos::{Endian, MajorMode, SourceSelect, TessellationMode};

struct Unreadable(Cell<usize>);
impl xenos_state::GuestMemory for Unreadable {
    fn read_physical(&self, _: u32, _: &mut [u8]) -> bool {
        self.0.set(self.0.get() + 1);
        false
    }
}

fn desc(primitive: PrimitiveType, source: SourceSelect, format: IndexFormat, count: u32) -> DrawDescription {
    let mut regs = vec![0; 0x5003];
    regs[r::VGT_DRAW_INITIATOR as usize] = VgtDrawInitiator {
        prim_type: primitive,
        source_select: source,
        major_mode: MajorMode::Implicit,
        index_size: format,
        not_eop: false,
        num_indices: count,
    }
    .encode();
    regs[r::VGT_DMA_SIZE as usize] = VgtDmaSize { num_words: count, swap_mode: Endian::Swap8In32 }.encode();
    regs[r::VGT_MAX_VTX_INDX as usize] = 0xff_ffff;
    DrawDescription::from_registers(&regs)
}

#[test]
fn direct_lists_do_not_read_or_copy_guest_indices() {
    for primitive in [PrimitiveType::LineList, PrimitiveType::TriangleList] {
        for format in [IndexFormat::Int16, IndexFormat::Int32] {
            let mut d = desc(primitive, SourceSelect::Dma, format, 9);
            d.index_params.offset = 0xff_fffe;
            d.index_params.min = 3;
            d.index_params.max = 100;
            let memory = Unreadable(Cell::new(0));
            let plan = prepare_geometry(&d, &memory).unwrap();
            assert_eq!(plan.direct_indices, d.index_buffer);
            assert_eq!(plan.draw.indices, HostIndices::None);
            assert_eq!(plan.draw.vertex_count, 9);
            assert!(!plan.draw.index_params_applied);
            assert_eq!(memory.0.get(), 0);
        }
    }
}

#[test]
fn canonical_strip_restart_retains_direct_dma_recipe() {
    for primitive in
        [PrimitiveType::LineStrip, PrimitiveType::LineStrip2D, PrimitiveType::TriangleStrip, PrimitiveType::TriStrip2D]
    {
        for format in [IndexFormat::Int16, IndexFormat::Int32] {
            let mut d = desc(primitive, SourceSelect::Dma, format, 7);
            for reset in [None, Some(if format == IndexFormat::Int16 { 0xffff } else { u32::MAX })] {
                d.reset_index = reset;
                let memory = Unreadable(Cell::new(0));
                let plan = prepare_geometry(&d, &memory).unwrap();
                assert_eq!(plan.direct_indices, d.index_buffer);
                assert!(plan.draw.topology.is_strip());
                assert_eq!(plan.draw.indices, HostIndices::None);
                assert_eq!(plan.draw.vertex_count, 7);
                assert!(!plan.draw.index_params_applied);
                assert_eq!(memory.0.get(), 0);
            }
        }
    }
}

#[test]
fn noncanonical_restart_is_converted_and_owned() {
    let mut d = desc(PrimitiveType::TriangleStrip, SourceSelect::Dma, IndexFormat::Int16, 7);
    d.reset_index = Some(99);
    let mut bytes: Vec<u8> = [1u16, 2, 3, 99, 4, 5, 6].into_iter().flat_map(u16::to_be_bytes).collect();
    let plan = prepare_geometry(&d, &bytes).unwrap();
    assert!(plan.direct_indices.is_none());
    assert_eq!(plan.draw.indices, HostIndices::U16(vec![1, 2, 3, 0xffff, 4, 5, 6]));
    bytes.fill(0);
    assert_eq!(plan.draw.indices, HostIndices::U16(vec![1, 2, 3, 0xffff, 4, 5, 6]));
    assert!(plan.draw.primitive_restart);
}

#[test]
fn twenty_four_bit_guest_restart_is_not_a_native_u32_restart() {
    let mut d = desc(PrimitiveType::TriangleStrip, SourceSelect::Dma, IndexFormat::Int32, 7);
    d.reset_index = Some(0x00ff_ffff);
    let bytes: Vec<u8> = [1u32, 2, 3, 0x00ff_ffff, 4, 5, 6].into_iter().flat_map(u32::to_be_bytes).collect();
    let plan = prepare_geometry(&d, &bytes).unwrap();
    assert!(plan.direct_indices.is_none());
    assert_eq!(plan.draw.indices, HostIndices::U16(vec![1, 2, 3, 0xffff, 4, 5, 6]));
    assert!(plan.draw.primitive_restart);
}

#[test]
fn fan_endian_conversion_and_post_index_transform_remain_distinct() {
    let mut d = desc(PrimitiveType::TriangleFan, SourceSelect::Dma, IndexFormat::Int16, 3);
    d.index_params.offset = 9;
    let bytes: Vec<u8> = [7u16, 8, 9].into_iter().flat_map(u16::to_be_bytes).collect();
    let plan = prepare_geometry(&d, &bytes).unwrap();
    assert!(plan.direct_indices.is_none());
    assert_eq!(plan.draw.topology, HostTopology::TriangleList);
    assert_eq!(plan.draw.indices, HostIndices::U16(vec![8, 9, 7]));
    assert!(!plan.draw.index_params_applied);
}

#[test]
fn rectangle_expansion_is_part_of_cpu_plan() {
    let d = desc(PrimitiveType::RectangleList, SourceSelect::AutoIndex, IndexFormat::Int16, 3);
    let memory = Unreadable(Cell::new(0));
    let plan = prepare_geometry(&d, &memory).unwrap();
    assert!(plan.direct_indices.is_none());
    assert_eq!(plan.draw.expansion, PrimitiveExpansion::RectangleList);
    assert_eq!(plan.draw.topology, HostTopology::TriangleStrip);
    assert_eq!(plan.draw.vertex_count, 4);
    assert_eq!(memory.0.get(), 0);
}

#[test]
fn direct_recipe_uses_capacity_clamped_count_and_original_endian() {
    let mut d = desc(PrimitiveType::TriangleList, SourceSelect::Dma, IndexFormat::Int16, 9);
    d.index_buffer.as_mut().unwrap().count = 3;
    let memory = Unreadable(Cell::new(0));
    let plan = prepare_geometry(&d, &memory).unwrap();
    assert_eq!(plan.draw.vertex_count, 3);
    let ib = plan.direct_indices.unwrap();
    assert_eq!(ib.endian, Endian::Swap8In16);
    assert_eq!(ib.raw_endian, Endian::Swap8In32);
    assert_eq!(memory.0.get(), 0);
}

#[test]
fn unsupported_tessellation_is_explicit() {
    let mut d = desc(PrimitiveType::TriangleList, SourceSelect::Dma, IndexFormat::Int16, 3);
    d.tessellation = Some(TessellationMode::Adaptive);
    assert!(matches!(
        prepare_geometry(&d, &Unreadable(Cell::new(0))),
        Err(xenos_state::primitive::PrimitiveError::TessellationUnsupported(_))
    ));
}

#[test]
fn draw_state_preserves_float_bits_and_independent_values() {
    let v = [0.0, -0.0, 64.0, 32.0, f32::from_bits(0x7fc00123), 1.0];
    let blend = [-0.0, 1.0, f64::from_bits(0x7ff8000000000123), 0.5];
    let state = DrawState::new(v, [1, 2, 3, 4], blend, 123);
    assert_eq!(state.viewport().map(f32::to_bits), v.map(f32::to_bits));
    assert_eq!(state.blend().map(f64::to_bits), blend.map(f64::to_bits));
    assert_eq!(state.scissor, [1, 2, 3, 4]);
    assert_eq!(state.stencil_reference, 123);
    assert_eq!(state.reserved, 0);
    let mut next = state;
    next.viewport_bits[1] = 0.0f32.to_bits();
    assert_ne!(state, next);
    assert_eq!(std::mem::size_of::<DrawState>(), 80);
}
