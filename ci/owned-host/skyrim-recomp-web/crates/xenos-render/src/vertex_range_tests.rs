use super::*;

#[test]
fn host_entry_admission_requires_the_encoded_draw_contract() {
    let entry = Entry {
        source: EntrySource::Sequential,
        count: 3,
        offset: 4,
        min: 0,
        max: 100,
        first_vertex: 0,
        host_base_vertex: 0,
        first_instance: 0,
        instance_count: 1,
        index_endian: 0,
        index_load: false,
        expanded: false,
    };
    assert_eq!(entry.domain().unwrap().limits(), (4, 6));
    for refused in [
        Entry { source: EntrySource::Converted, ..entry },
        Entry { count: 0, ..entry },
        Entry { first_vertex: 1, ..entry },
        Entry { host_base_vertex: -1, ..entry },
        Entry { first_instance: 1, ..entry },
        Entry { instance_count: 2, ..entry },
        Entry { index_endian: 1, ..entry },
        Entry { index_load: true, ..entry },
        Entry { expanded: true, ..entry },
        Entry { offset: u32::MAX, ..entry },
        Entry { min: 101, ..entry },
    ] {
        assert!(refused.domain().is_none(), "{refused:?}");
    }
    for endian in 0..4 {
        // The complete post-add/post-swap unsigned clamp covers every raw
        // index, even ones changed by GPU work after this host-side decision.
        let direct =
            Entry { source: EntrySource::DirectDma, offset: u32::MAX, min: 2, max: 13, index_endian: endian, ..entry };
        assert_eq!(direct.domain().unwrap().limits(), (2, 13));
    }
    assert!(Entry { source: EntrySource::DirectDma, index_endian: 4, ..entry }.domain().is_none());
    assert!(Entry { source: EntrySource::DirectDma, max: 1 << 23, ..entry }.domain().is_none());
}
#[path = "../../xenos-shader/tests/common/mod.rs"]
mod asm;
use asm::{Alu, Cf, Cond, VFetch, assemble, c, cexec, exece};
use std::cell::RefCell;
use xenos_shader::bindings::BoolLoopConstants;
use xenos_shader::interpreter::{Environment, Interpreter, SharedMemoryWords};
use xenos_shader::ucode::vertex_format as fmt;

struct Memory {
    reads: RefCell<Vec<u32>>,
    allowed: Option<Vec<Range>>,
}
impl SharedMemoryWords for Memory {
    fn word(&self, word: u32) -> u32 {
        if let Some(allowed) = &self.allowed {
            assert!(allowed.iter().any(|r| r.contains_word(word)), "uncovered word {word}");
        }
        self.reads.borrow_mut().push(word);
        // Finite float payloads also make comparisons useful for all float formats.
        0x3E00_0000 | (word.wrapping_mul(17) & 0xFFFF)
    }
}
fn decoded(fetches: &[VFetch]) -> Program {
    Program::decode(ShaderStage::Vertex, &assemble(&[exece(fetches.iter().copied().map(Into::into).collect())]))
        .unwrap()
}
fn constants(address: u32, bytes: u32, endian: u32) -> [u32; 192] {
    let mut result = [0; 192];
    result[0] = address | 3;
    result[1] = ((bytes / 4) << 2) | endian;
    result
}
fn oracle(program: &Program, fc: &[u32; 192], indices: &[u32], ranges: &[Range]) -> usize {
    let full = Memory { reads: RefCell::new(Vec::new()), allowed: None };
    let narrow = Memory { reads: RefCell::new(Vec::new()), allowed: Some(ranges.to_vec()) };
    let floats = [[1000.0; 4]; 256];
    let bools = BoolLoopConstants::default();
    for &index in indices {
        let mut results = Vec::new();
        for memory in [&full, &narrow] {
            let mut machine = Interpreter::new(
                program,
                Environment { float_constants: &floats, bool_loop: &bools, fetch_constants: fc, shared_memory: memory },
            );
            machine.state.registers[0][0] = index as f32;
            machine.run();
            results.push(machine.state.registers.map(|r| r.map(f32::to_bits)));
        }
        assert_eq!(results[0], results[1], "full and restricted memory register results");
    }
    assert_eq!(*full.reads.borrow(), *narrow.reads.borrow());
    let count = full.reads.borrow().len();
    count
}
fn fetch() -> VFetch {
    VFetch { fetch_constant: 0, stride: 6, ..Default::default() }
}
fn swap(value: u32, endian: u32) -> u32 {
    let value =
        if endian == 1 || endian == 2 { ((value >> 8) & 0x00FF00FF) | ((value & 0x00FF00FF) << 8) } else { value };
    if endian == 2 || endian == 3 { value.rotate_left(16) } else { value }
}

#[test]
fn formats_endian_rounding_partial_masks_and_page_edges_match_actual_interpreter() {
    let formats = [
        fmt::K_8_8_8_8,
        fmt::K_2_10_10_10,
        fmt::K_10_11_11,
        fmt::K_11_11_10,
        fmt::K_16_16,
        fmt::K_16_16_16_16,
        fmt::K_16_16_FLOAT,
        fmt::K_16_16_16_16_FLOAT,
        fmt::K_32,
        fmt::K_32_32,
        fmt::K_32_32_32_32,
        fmt::K_32_FLOAT,
        fmt::K_32_32_FLOAT,
        fmt::K_32_32_32_FLOAT,
        fmt::K_32_32_32_32_FLOAT,
    ];
    let mut calls = 0;
    for format in formats {
        for endian in 0..4 {
            for round_index in [false, true] {
                for (address, offset, stride, mask) in
                    [(4092, 0, 6, "xyzw"), (4096, 1023, 9, "x__w"), (8192, 1, 0, "_y__")]
                {
                    let p = decoded(&[VFetch { format, offset, stride, dest_swizzle: mask, round_index, ..fetch() }]);
                    let fc = constants(address, 1 << 20, endian);
                    let domain = IndexDomain::nonindexed(19, 7, 9, 23, false).unwrap();
                    let ranges = ShaderProof::analyze(&p).unwrap().ranges(&fc, domain).unwrap();
                    let indices: Vec<_> = (7..26).map(|i| i.clamp(9, 23)).collect();
                    calls += oracle(&p, &fc, &indices, &ranges);
                }
            }
        }
    }
    // The fifteen formats need 59 words across the three destination masks;
    // every case executes for 4 endian modes, 2 rounding modes and 19 vertices.
    assert_eq!(calls, 59 * 4 * 2 * 19);
}

#[test]
fn full_mini_state_survives_r0_overwrite_and_same_constant_can_have_two_strides() {
    let p = decoded(&[
        VFetch { dest: 0, ..fetch() },
        VFetch { mini: true, dest: 2, offset: 4, format: fmt::K_32_32_FLOAT, dest_swizzle: "xy__", ..fetch() },
    ]);
    let fc = constants(4096, 1 << 20, 2);
    let d = IndexDomain::nonindexed(3, 0, 0, 2, false).unwrap();
    let ranges = ShaderProof::analyze(&p).unwrap().ranges(&fc, d).unwrap();
    assert_eq!(ranges, [Range { constant: 0, address: 4096, bytes: 72 }]);
    assert_eq!(oracle(&p, &fc, &[0, 1, 2], &ranges), 18);
    let p = decoded(&[fetch(), VFetch { dest: 3, stride: 20, offset: 2, ..fetch() }]);
    let ranges = ShaderProof::analyze(&p).unwrap().ranges(&fc, d).unwrap();
    assert_eq!(ranges[0].bytes, (2 * 20 + 2 + 4) * 4);
    oracle(&p, &fc, &[0, 1, 2], &ranges);
    assert!(ShaderProof::analyze(&decoded(&[VFetch { dest: 0, ..fetch() }, fetch()])).is_err());
}

#[test]
fn arbitrary_index_arithmetic_is_refused_and_naive_draw_count_is_unsound() {
    let write_index =
        Alu { vector_op: 2, vector_dest: 0, vector_mask: 1, src: [c(0), c(0), c(0)], ..Default::default() };
    let p =
        Program::decode(ShaderStage::Vertex, &assemble(&[exece(vec![write_index.into(), fetch().into()])])).unwrap();
    assert_eq!(ShaderProof::analyze(&p).unwrap_err(), "unproven_index");
    let memory = Memory { reads: RefCell::new(Vec::new()), allowed: None };
    let floats = [[1000.0; 4]; 256];
    let bools = BoolLoopConstants::default();
    let fc = constants(4096, 1 << 20, 0);
    let mut machine = Interpreter::new(
        &p,
        Environment { float_constants: &floats, bool_loop: &bools, fetch_constants: &fc, shared_memory: &memory },
    );
    machine.state.registers[0][0] = 0.0;
    machine.run();
    assert_eq!(memory.reads.borrow()[0], 4096 / 4 + 1000 * 6);
    assert!(!Range { constant: 0, address: 4096, bytes: 72 }.contains_word(memory.reads.borrow()[0]));
}

#[test]
fn control_flow_relative_predicated_and_ambiguous_mini_inputs_fall_back() {
    for cf in [
        cexec(Cond::Bool(0, true), true, vec![fetch().into()]),
        Cf::LoopStart { loop_id: 0, skip: 1, repeat: false },
        Cf::Jmp { target: 0, cond: Cond::Always },
        Cf::Call { target: 0, cond: Cond::Always },
        Cf::Ret,
    ] {
        let p = Program::decode(ShaderStage::Vertex, &assemble(&[cf, exece(vec![fetch().into()])])).unwrap();
        assert!(ShaderProof::analyze(&p).is_err());
    }
    for f in [
        VFetch { predicated: Some(true), ..fetch() },
        VFetch { mini: true, ..fetch() },
        VFetch { src: 1, ..fetch() },
        VFetch { src_component: 1, ..fetch() },
        VFetch { offset: -1, ..fetch() },
    ] {
        assert!(ShaderProof::analyze(&decoded(&[f])).is_err());
    }
    let mut p = decoded(&[fetch()]);
    if let Instruction::VertexFetch(f) = &mut p.cf[0].instructions[0].instruction {
        f.index_operand.addressing = AddressingMode::LoopRelative;
    }
    assert!(ShaderProof::analyze(&p).is_err());
}

#[test]
fn direct_clamp_bounds_arbitrary_endian_indices_and_later_gpu_values() {
    let p = decoded(&[VFetch { stride: 7, offset: 2, round_index: true, ..fetch() }]);
    let fc = constants(16384, 1 << 20, 3);
    let domain = IndexDomain::direct_clamped(1000, 1010, false).unwrap();
    let ranges = ShaderProof::analyze(&p).unwrap().ranges(&fc, domain).unwrap();
    let mut state = 0x9876_5432u32;
    for endian in 0..4 {
        let mut indices = Vec::new();
        for _ in 0..257 {
            state = state.wrapping_mul(1664525).wrapping_add(1013904223);
            // This matches the current WGSL entry's swap/add/unsigned clamp.
            indices.push(swap(state, endian).wrapping_add(0x00FF_FF00).clamp(1000, 1010));
        }
        indices.extend(1000..=1010); // also include interior, not just saturated random inputs.
        oracle(&p, &fc, &indices, &ranges);
    }
    // No narrow result may be based on a mutable CPU-only min/max scan.
    assert!(IndexDomain::direct_clamped(0, 0xFF_FFFF, false).is_err());
}

#[test]
fn overflow_precision_expansion_and_constant_aliases_refuse() {
    assert!(IndexDomain::nonindexed(3, u32::MAX, 0, 2, false).is_err());
    assert!(IndexDomain::nonindexed(3, 0, 0, 2, true).is_err());
    assert!(IndexDomain::nonindexed(0, 0, 0, 2, false).is_err());
    assert!(IndexDomain::direct_clamped(5, 4, false).is_err());
    assert!(IndexDomain::direct_clamped(0, 1 << 23, false).is_err());
    assert!(IndexDomain::direct_clamped(0, 2, true).is_err());
    let proof = ShaderProof::analyze(&decoded(&[fetch()])).unwrap();
    let d = IndexDomain::nonindexed(3, 0, 0, 2, false).unwrap();
    assert!(proof.ranges(&constants(PHYSICAL_BYTES, 4096, 0), d).is_err());
    assert!(proof.ranges(&constants(PHYSICAL_BYTES - 4, 4096, 0), d).is_err());
    let high = ShaderProof::analyze(&decoded(&[VFetch { stride: 255, offset: 4_194_303, ..fetch() }])).unwrap();
    assert!(
        high.ranges(
            &constants(PHYSICAL_BYTES - 4096, 4096, 0),
            IndexDomain::direct_clamped(MAX_INDEX, MAX_INDEX, false).unwrap()
        )
        .is_err()
    );
    let p = decoded(&[VFetch { stride: 1, round_index: true, ..fetch() }]);
    let fc = constants(4096, 40 << 20, 0);
    let d = IndexDomain::direct_clamped(MAX_INDEX - 2, MAX_INDEX, false).unwrap();
    let ranges = ShaderProof::analyze(&p).unwrap().ranges(&fc, d).unwrap();
    assert_eq!(ranges[0].bytes, 24);
    assert_eq!(oracle(&p, &fc, &[MAX_INDEX - 2, MAX_INDEX - 1, MAX_INDEX], &ranges), 12);
}

#[test]
fn independent_fetch_constants_are_recomputed_after_register_changes() {
    let p = decoded(&[
        fetch(),
        VFetch {
            fetch_constant: 95,
            dest: 2,
            stride: 3,
            offset: 1,
            format: fmt::K_16_16_16_16,
            dest_swizzle: "x__w",
            ..fetch()
        },
    ]);
    let proof = ShaderProof::analyze(&p).unwrap();
    let domain = IndexDomain::nonindexed(3, 0, 0, 2, false).unwrap();
    let mut fc = constants(4096, 1 << 20, 0);
    fc[190] = 12284 | 3;
    fc[191] = 8192 | 2;
    let ranges = proof.ranges(&fc, domain).unwrap();
    assert_eq!(
        ranges,
        [Range { constant: 0, address: 4096, bytes: 64 }, Range { constant: 95, address: 12288, bytes: 32 }]
    );
    assert_eq!(oracle(&p, &fc, &[0, 1, 2], &ranges), 18);
    fc[0] = 8192 | 3;
    fc[191] = 8 | 3;
    let changed = proof.ranges(&fc, domain).unwrap();
    assert_eq!(
        changed,
        [Range { constant: 0, address: 8192, bytes: 64 }, Range { constant: 95, address: 12288, bytes: 4 }]
    );
    assert_eq!(oracle(&p, &fc, &[0, 1, 2], &changed), 13);
}

#[test]
fn end_clipping_constant_only_fetch_and_zero_read_formats_match_oracle() {
    let p = decoded(&[fetch()]);
    let proof = ShaderProof::analyze(&p).unwrap();
    let fc = constants(4096, 20, 0);
    let d = IndexDomain::nonindexed(9, 0, 0, 8, false).unwrap();
    let ranges = proof.ranges(&fc, d).unwrap();
    assert_eq!(ranges[0].bytes, 20);
    oracle(&p, &fc, &(0..9).collect::<Vec<_>>(), &ranges);
    let p = decoded(&[VFetch { stride: 0, src: 57, offset: 8, ..fetch() }]);
    let fc = constants(4096, 1 << 20, 0);
    let ranges = ShaderProof::analyze(&p).unwrap().ranges(&fc, d).unwrap();
    assert_eq!(ranges[0].address, 4096 + 32);
    assert_eq!(ranges[0].bytes, 16);
    oracle(&p, &fc, &[0, 2, 8], &ranges);
    let p = decoded(&[VFetch { format: 0, ..fetch() }]);
    let ranges = ShaderProof::analyze(&p).unwrap().ranges(&fc, d).unwrap();
    assert!(ranges.is_empty());
    assert_eq!(oracle(&p, &fc, &[0, 1, 2], &ranges), 0);
}

#[test]
fn exact_page_count_estimates_include_crossings_and_full_range_worst_case() {
    let p = decoded(&[
        fetch(),
        VFetch { mini: true, offset: 4, format: fmt::K_32_32_FLOAT, dest_swizzle: "xy__", ..fetch() },
    ]);
    let proof = ShaderProof::analyze(&p).unwrap();
    let d = IndexDomain::nonindexed(3, 0, 0, 2, false).unwrap();
    let r = proof.ranges(&constants(4096, 1 << 20, 0), d).unwrap();
    assert_eq!(r[0].bytes, 72);
    assert_eq!(r[0].pages(), 1);
    assert_eq!(512 * 256, 131072);
    assert_eq!(512 * r[0].pages(), 512);
    let crossing = proof.ranges(&constants(4092, 1 << 20, 0), d).unwrap();
    assert_eq!(crossing[0].pages(), 2);
    let all =
        proof.ranges(&constants(4096, 1 << 20, 0), IndexDomain::direct_clamped(0, 100_000, false).unwrap()).unwrap();
    assert_eq!(all[0].pages(), 256);
}

#[test]
fn cached_plan_preserves_order_and_bypasses_small_dense_or_unproven_ranges() {
    use super::CachedPlan;
    let p = decoded(&[VFetch { fetch_constant: 95, ..fetch() }, VFetch { dest: 2, ..fetch() }]);
    let plan = CachedPlan::analyze(&p).unwrap();
    let mut fc = constants(8192, 1 << 20, 0);
    fc[190] = 4096 | 3;
    fc[191] = 4096;
    let d = IndexDomain::nonindexed(3, 0, 0, 2, false).unwrap();
    let mut output = [Range { constant: 0, address: 0, bytes: 0 }; 2];
    let mut count = 0;
    let work = plan.for_each_range(&fc, Some(d), |r| {
        output[count] = r;
        count += 1;
    });
    assert_eq!(count, 2);
    assert_eq!(
        output,
        [Range { constant: 95, address: 4096, bytes: 4096 }, Range { constant: 0, address: 8192, bytes: 64 }]
    );
    assert_eq!(work, Work { bindings: 2, eligible: 1, narrowed: 1, avoided_pages: 255, evaluated_terms: 1 });
    // A missing entry proof must not even inspect fetch terms, and every
    // original constant/range remains in the original metadata order.
    count = 0;
    let work = plan.for_each_range(&fc, None, |r| {
        output[count] = r;
        count += 1;
    });
    assert_eq!(work.evaluated_terms, 0);
    assert_eq!(work.narrowed, 0);
    assert_eq!(output[1], Range { constant: 0, address: 8192, bytes: 1 << 20 });
    let dense = IndexDomain::direct_clamped(0, 100_000, false).unwrap();
    count = 0;
    let work = plan.for_each_range(&fc, Some(dense), |r| {
        output[count] = r;
        count += 1;
    });
    assert_eq!(work.evaluated_terms, 1);
    assert_eq!(work.narrowed, 0);
    assert_eq!(output[1], Range { constant: 0, address: 8192, bytes: 1 << 20 });
    fc[0] = PHYSICAL_BYTES | 3;
    count = 0;
    let work = plan.for_each_range(&fc, Some(d), |r| {
        output[count] = r;
        count += 1;
    });
    assert_eq!(work.evaluated_terms, 0);
    assert_eq!(work.narrowed, 0);
    assert_eq!(output[1].address, PHYSICAL_BYTES);
}

#[test]
fn cached_stride_terms_and_current_constants_cover_the_actual_interpreter() {
    use super::CachedPlan;
    let fetches = [
        VFetch { stride: 20, offset: 2, ..fetch() },
        VFetch { dest: 2, ..fetch() },
        VFetch { mini: true, dest: 3, offset: 4, format: fmt::K_32_32_FLOAT, dest_swizzle: "xy__", ..fetch() },
    ];
    let shader = xenos_shader::Shader::new(
        ShaderStage::Vertex,
        &assemble(&[exece(fetches.into_iter().map(Into::into).collect())]),
    )
    .unwrap();
    let p = shader.program();
    let plan = CachedPlan::from_shader(&shader).unwrap();
    let reference = ShaderProof::analyze(p).unwrap();
    let d = IndexDomain::nonindexed(3, 0, 0, 2, false).unwrap();
    for (address, size) in [(4096, 1 << 20), (4092, 1 << 20), (8192, 24), (4096, 0)] {
        for endian in 0..4 {
            let fc = constants(address, size, endian);
            let mut output = [Range { constant: 0, address: 0, bytes: 0 }; 1];
            let mut count = 0;
            let work = plan.for_each_range(&fc, Some(d), |r| {
                output[count] = r;
                count += 1;
            });
            if size >= 16 * PAGE_BYTES {
                assert_eq!(work.narrowed, 1);
                assert_eq!(work.evaluated_terms, 2, "full+mini equal stride merged only at cache construction");
                assert_eq!(&output[..count], reference.ranges(&fc, d).unwrap());
            } else {
                assert_eq!(work.evaluated_terms, 0);
                assert_eq!(work.narrowed, 0);
            }
            oracle(&p, &fc, &[0, 1, 2], &output[..count]);
        }
    }
}
