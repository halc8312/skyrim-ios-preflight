//! Authored constant streams only. The reference is the original write/full-scan algorithm.
use super::*;

fn reference_write(regs: &mut RegisterFile, index: u32, value: u32) {
    let i = index as usize;
    if let Some(r) = regs.values.get_mut(i) {
        *r = value;
        if (FLOAT_CONSTANTS..FETCH_CONSTANTS).contains(&i) {
            let v = (i - FLOAT_CONSTANTS) / 4;
            regs.float_written[v / 64] |= 1 << (v % 64);
        }
        regs.dirty |= match i {
            FLOAT_CONSTANTS..PIXEL_FLOAT_CONSTANTS => dirty::FLOAT_VS,
            PIXEL_FLOAT_CONSTANTS..FETCH_CONSTANTS => dirty::FLOAT_PS,
            FETCH_CONSTANTS..BOOL_LOOP_CONSTANTS => dirty::FETCH,
            BOOL_LOOP_CONSTANTS.. => dirty::BOOL_LOOP,
            _ => 0,
        };
    }
}

// Evaluation-only alternative: preserve exact bits/forced dirty state, but replace
// the equal-value branch with weighted dirty masks and unconditional stores.
fn branchless_write(regs: &mut RegisterFile, index: u32, value: u32) {
    let i = index as usize;
    if let Some(r) = regs.values.get_mut(i) {
        let changed = u64::from(*r != value);
        *r = value;
        if (FLOAT_CONSTANTS..FETCH_CONSTANTS).contains(&i) {
            let v = (i - FLOAT_CONSTANTS) / 4;
            regs.float_written[v / 64] |= changed << (v % 64);
        }
        let bit = match i {
            FLOAT_CONSTANTS..PIXEL_FLOAT_CONSTANTS => dirty::FLOAT_VS,
            PIXEL_FLOAT_CONSTANTS..FETCH_CONSTANTS => dirty::FLOAT_PS,
            FETCH_CONSTANTS..BOOL_LOOP_CONSTANTS => dirty::FETCH,
            BOOL_LOOP_CONSTANTS.. => dirty::BOOL_LOOP,
            _ => 0,
        };
        regs.dirty |= bit * changed as u8;
    }
}

fn reference_delta(regs: &RegisterFile, stage: usize, count: usize, previous: &[u8], values: &mut Vec<u8>) -> [u32; 8] {
    let have = previous.len() / 16;
    let written = regs.float_written(stage);
    let mut mask = [0u32; 8];
    values.clear();
    for i in 0..count {
        let changed = if i >= have {
            true
        } else if written[i / 64] >> (i % 64) & 1 != 0 {
            regs.float_vec4(stage * 256 + i) != previous[i * 16..i * 16 + 16]
        } else {
            false
        };
        if changed {
            mask[i / 32] |= 1 << (i % 32);
            values.extend_from_slice(&regs.float_vec4(stage * 256 + i));
        }
    }
    mask
}

fn apply_patch(previous: &mut Vec<u8>, count: usize, mask: [u32; 8], values: &[u8]) {
    if values.is_empty() {
        return;
    }
    previous.resize(count * 16, 0);
    let mut chunks = values.chunks_exact(16);
    for i in 0..count {
        if mask[i / 32] >> (i % 32) & 1 != 0 {
            previous[i * 16..i * 16 + 16].copy_from_slice(chunks.next().unwrap());
        }
    }
    assert!(chunks.next().is_none());
}

fn clear_after_draw(regs: &mut RegisterFile, stage: usize, count: usize, changed: bool) {
    if count != 0 {
        regs.clear_float_written(stage, if changed { 256 } else { count });
    }
}

#[test]
fn identical_bits_do_not_dirty_but_changed_bits_and_forced_invalidation_do() {
    let mut regs = RegisterFile::default();
    regs.take_dirty();
    for stage in 0..2 {
        regs.clear_float_written(stage, 256);
    }
    for (index, bit) in [
        (FLOAT_CONSTANTS, dirty::FLOAT_VS),
        (PIXEL_FLOAT_CONSTANTS - 1, dirty::FLOAT_VS),
        (PIXEL_FLOAT_CONSTANTS, dirty::FLOAT_PS),
        (FETCH_CONSTANTS - 1, dirty::FLOAT_PS),
        (FETCH_CONSTANTS, dirty::FETCH),
        (FETCH_CONSTANTS + 191, dirty::FETCH),
        (BOOL_LOOP_CONSTANTS, dirty::BOOL_LOOP),
    ] {
        // All values, including NaN payloads and -0, are compared as u32 bits.
        for value in [0x8000_0000, 0x7fc0_0001, 0x7fc0_0002, 0] {
            regs.write(index as u32, value);
            assert_eq!(regs.take_dirty(), bit);
            regs.write(index as u32, value);
            assert_eq!(regs.take_dirty(), 0);
        }
    }
    regs.mark_dirty(dirty::FLOAT_VS | dirty::FLOAT_PS | dirty::FETCH | dirty::BOOL_LOOP);
    regs.write(FLOAT_CONSTANTS as u32, 0);
    regs.write(FETCH_CONSTANTS as u32, 0);
    assert_eq!(regs.take_dirty(), 15, "equal writes must preserve forced dirty state");
    assert_eq!(regs.float_written(0), [u64::MAX; 4]);
    assert_eq!(regs.float_written(1), [u64::MAX; 4]);
    let before = regs.values.clone();
    regs.write(REGISTER_COUNT as u32, 1);
    regs.write(u32::MAX, 2);
    assert_eq!(regs.values, before);
}

#[test]
fn sparse_deltas_match_full_scan_across_stages_ranges_reverts_and_partial_writes() {
    let mut candidate = RegisterFile::default();
    let mut reference = RegisterFile::default();
    let mut branchless = RegisterFile::default();
    let mut previous: [Vec<u8>; 2] = Default::default();
    let mut values = Vec::new();
    let mut reference_values = Vec::new();
    let counts = [0, 1, 2, 31, 32, 33, 63, 64, 65, 127, 128, 129, 255, 256];
    let bit_patterns = [0, 0x8000_0000, 0x3f80_0000, 0x7f80_0000, 0x7fc0_0001, 0x7fc0_0002, u32::MAX];
    let mut random = 0x49f3_76d2u32;
    for draw in 0..4096 {
        // Rewrites, both-stage writes, partial vec4 writes, and change-then-revert before a draw.
        for write in 0..(draw % 23) {
            random ^= random << 13;
            random ^= random >> 17;
            random ^= random << 5;
            let index = FLOAT_CONSTANTS + random as usize % 2048;
            let old = candidate.values[index];
            let value = if write % 3 == 0 { old } else { bit_patterns[(random as usize / 2048) % bit_patterns.len()] };
            candidate.write(index as u32, value);
            reference_write(&mut reference, index as u32, value);
            branchless_write(&mut branchless, index as u32, value);
            if write % 7 == 0 {
                candidate.write(index as u32, old);
                reference_write(&mut reference, index as u32, old);
                branchless_write(&mut branchless, index as u32, old);
            }
        }
        if draw % 97 == 0 {
            candidate.mark_dirty(dirty::FLOAT_VS | dirty::FLOAT_PS);
            reference.mark_dirty(dirty::FLOAT_VS | dirty::FLOAT_PS);
            branchless.mark_dirty(dirty::FLOAT_VS | dirty::FLOAT_PS);
        }
        if draw % 131 == 0 {
            previous = Default::default(); // submission discards uploaded blocks
        }
        let stage = draw % 2;
        let count = counts[(draw / 2) % counts.len()];
        let mask = candidate.float_delta(stage, count, &previous[stage], &mut values);
        let expected = reference_delta(&reference, stage, count, &previous[stage], &mut reference_values);
        assert_eq!(mask, expected, "draw {draw}, stage {stage}, count {count}");
        assert_eq!(values, reference_values, "draw {draw}");
        assert_eq!(candidate.values, reference.values);
        assert_eq!(candidate.values, branchless.values);
        assert_eq!(candidate.float_written, branchless.float_written);
        assert_eq!(candidate.dirty, branchless.dirty);
        apply_patch(&mut previous[stage], count, mask, &values);
        if count != 0 {
            let mut expected_block = Vec::new();
            candidate.float_constants_window((stage * 256) as u32, count as u32, &mut expected_block);
            assert_eq!(&previous[stage][..count * 16], expected_block, "reconstructed draw {draw}");
        }
        clear_after_draw(&mut candidate, stage, count, !values.is_empty());
        clear_after_draw(&mut reference, stage, count, !values.is_empty());
        clear_after_draw(&mut branchless, stage, count, !values.is_empty());
    }
}

#[test]
fn missing_suffix_and_word_boundaries_are_included_without_dirty_bits() {
    let mut regs = RegisterFile::default();
    for i in 0..2048 {
        regs.write((FLOAT_CONSTANTS + i) as u32, i as u32 ^ 0xa5a5_5a5a);
    }
    for stage in 0..2 {
        regs.clear_float_written(stage, 256);
        for have in [0, 1, 31, 32, 63, 64, 65, 127, 128, 191, 192, 255, 256] {
            let mut previous = Vec::new();
            regs.float_constants_window((stage * 256) as u32, have as u32, &mut previous);
            for count in [0, 1, 31, 32, 63, 64, 65, 127, 128, 191, 192, 255, 256] {
                let mut values = Vec::new();
                let mut expected = Vec::new();
                assert_eq!(
                    regs.float_delta(stage, count, &previous, &mut values),
                    reference_delta(&regs, stage, count, &previous, &mut expected)
                );
                assert_eq!(values, expected);
            }
        }
    }
}

#[test]
fn redundant_full_bank_rewrites_reduce_comparison_candidates() {
    let mut reference = RegisterFile::default();
    let mut candidate = RegisterFile::default();
    for regs in [&mut reference, &mut candidate] {
        regs.clear_float_written(0, 256);
    }
    for i in 0..1024 {
        let value = u32::from(i % 80 == 0);
        reference_write(&mut reference, (FLOAT_CONSTANTS + i) as u32, value);
        candidate.write((FLOAT_CONSTANTS + i) as u32, value);
    }
    assert_eq!(reference.float_written(0).iter().map(|v| v.count_ones()).sum::<u32>(), 256);
    assert_eq!(candidate.float_written(0).iter().map(|v| v.count_ones()).sum::<u32>(), 13);
    let previous = vec![0; 4096];
    let mut expected = Vec::new();
    let mut actual = Vec::new();
    assert_eq!(
        reference_delta(&reference, 0, 256, &previous, &mut expected),
        candidate.float_delta(0, 256, &previous, &mut actual)
    );
    assert_eq!(actual, expected);
}

#[derive(Clone, Copy, Debug)]
enum Mode {
    Reference,
    EqualWriteOnly,
    SparseOnly,
    Combined,
    Branchless,
}

fn measured_stream(mode: Mode, count: usize, changed_every: usize, changed_words: usize, draws: usize) -> (u64, u64) {
    use std::hint::black_box;
    let mut regs = RegisterFile::default();
    let mut previous = vec![0; count * 16];
    let mut values = Vec::with_capacity(count * 16);
    let mut input = vec![0u32; count * 4];
    regs.clear_float_written(0, 256);
    let mut patch_bytes = 0u64;
    let mut checksum = 0u64;
    for draw in 0..draws {
        if changed_every != 0 {
            for i in (0..count).step_by(changed_every) {
                for word in 0..changed_words {
                    input[i * 4 + (draw + word) % 4] = ((draw + 1) * 4 + word) as u32;
                }
            }
        }
        for (i, &value) in black_box(&input).iter().enumerate() {
            match mode {
                Mode::Reference | Mode::SparseOnly => reference_write(&mut regs, (FLOAT_CONSTANTS + i) as u32, value),
                Mode::EqualWriteOnly | Mode::Combined => regs.write((FLOAT_CONSTANTS + i) as u32, value),
                Mode::Branchless => branchless_write(&mut regs, (FLOAT_CONSTANTS + i) as u32, value),
            }
        }
        let mask = match mode {
            Mode::Reference | Mode::EqualWriteOnly => {
                reference_delta(&regs, 0, count, black_box(&previous), &mut values)
            }
            Mode::SparseOnly | Mode::Combined | Mode::Branchless => {
                regs.float_delta(0, count, black_box(&previous), &mut values)
            }
        };
        patch_bytes += values.len() as u64;
        checksum = checksum.wrapping_add(mask.iter().map(|&v| v as u64).sum::<u64>());
        checksum = checksum.wrapping_add(values.iter().map(|&v| v as u64).sum::<u64>());
        apply_patch(&mut previous, count, mask, black_box(&values));
        clear_after_draw(&mut regs, 0, count, !values.is_empty());
        black_box((&regs, &previous));
    }
    black_box((checksum, patch_bytes))
}

/// CPU constant preparation only, not GPU/frame/iPhone performance. Run explicitly in release.
#[test]
#[ignore = "explicit public microbenchmark: --release --ignored --nocapture benchmark_constant_deltas"]
fn benchmark_constant_deltas() {
    use std::time::Instant;
    let modes = [Mode::Reference, Mode::EqualWriteOnly, Mode::SparseOnly, Mode::Combined, Mode::Branchless];
    for count in [8, 64, 256] {
        for (label, changed_every, changed_words) in
            [("unchanged", 0, 0), ("sparse_vec4", 20, 1), ("all_vec4", 1, 1), ("all_dwords", 1, 4)]
        {
            let draws = 50000;
            let expected = measured_stream(Mode::Reference, count, changed_every, changed_words, draws);
            let mut samples: [Vec<u128>; 5] = Default::default();
            // Rotate order to avoid always assigning cold/warm or frequency drift to one mode.
            for round in 0..11 {
                for order in 0..modes.len() {
                    let mode = (round + order) % modes.len();
                    let start = Instant::now();
                    let result = measured_stream(modes[mode], count, changed_every, changed_words, draws);
                    let elapsed = start.elapsed().as_nanos();
                    assert_eq!(result, expected, "{count}/{label}/{:?}", modes[mode]);
                    samples[mode].push(elapsed);
                }
            }
            for (mode, times) in modes.iter().zip(samples.iter_mut()) {
                times.sort_unstable();
                println!(
                    "constant_bench count={count} workload={label} mode={mode:?} draws={draws} median_ns={} min_ns={} max_ns={} patch_bytes={} checksum={}",
                    times[5], times[0], times[10], expected.1, expected.0
                );
            }
        }
    }
}
