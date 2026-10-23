//! Full real GPU outputs against the unchanged replay and an independent scalar oracle.
use super::*;

fn oracle(records: &[u32], values: &[u8], output: &mut [u8]) {
    let mut current = [[[0u8; 16]; INDICES]; STAGES as usize];
    for record in records[1..].chunks_exact(RECORD_WORDS) {
        let state = &mut current[record[0] as usize];
        let mut source = record[3] as usize * 16;
        for (index, value) in state.iter_mut().enumerate() {
            if record[4 + index / 32] & (1 << (index % 32)) != 0 {
                value.copy_from_slice(&values[source..source + 16]);
                source += 16;
            }
        }
        let destination = record[1] as usize * 16;
        for (index, value) in state.iter().take(record[2] as usize).enumerate() {
            output[destination + index * 16..destination + index * 16 + 16].copy_from_slice(value);
        }
    }
}

fn check_bins(records: &[u32]) {
    let binned = bin_records(records);
    let count = records[0] as usize;
    assert_eq!(&binned[BIN_HEADER_WORDS..BIN_HEADER_WORDS + count * RECORD_WORDS], &records[1..]);
    let mut all = Vec::new();
    for stage in 0..STAGES as usize {
        let header = 1 + stage * 3;
        let first = binned[header] as usize;
        let end = first + binned[header + 1] as usize;
        let indices: Vec<_> = records[1..]
            .chunks_exact(RECORD_WORDS)
            .enumerate()
            .filter(|(_, r)| r[0] as usize == stage)
            .map(|(i, _)| i as u32)
            .collect();
        assert_eq!(&binned[first..end], indices);
        let maximum = indices.iter().map(|i| records[1 + *i as usize * RECORD_WORDS + 2]).max().unwrap_or(0);
        assert_eq!(binned[header + 2], maximum);
        all.extend_from_slice(&binned[first..end]);
    }
    all.sort_unstable();
    assert_eq!(all, (0..count as u32).collect::<Vec<_>>());
}

#[test]
fn bins_preserve_all_records_order_and_exact_maximum_bounds() {
    check_bins(&[0]);
    let mut records = vec![MAX_RECORDS as u32];
    for index in 0..MAX_RECORDS {
        let mut record = [0u32; RECORD_WORDS];
        record[0] = (index % STAGES as usize) as u32;
        record[2] = (index % (INDICES + 1)) as u32;
        record[3] = index as u32;
        records.extend(record);
    }
    check_bins(&records);
    let mut bad = records.clone();
    bad[0] += 1;
    assert!(std::panic::catch_unwind(|| bin_records(&bad)).is_err());
    bad = records.clone();
    bad[1] = STAGES;
    assert!(std::panic::catch_unwind(|| bin_records(&bad)).is_err());
    bad = records.clone();
    bad[3] = INDICES as u32 + 1;
    assert!(std::panic::catch_unwind(|| bin_records(&bad)).is_err());
    bad = records;
    bad.pop();
    assert!(std::panic::catch_unwind(|| bin_records(&bad)).is_err());
}

fn read_blocks(device: &Device, patcher: &mut ConstantPatcher) -> (Vec<u8>, gpu_webgpu::counters::Counters) {
    let before = gpu_webgpu::counters::snapshot();
    let (commands, _) = patcher.encode(device).expect("nonempty replay");
    let encoded = gpu_webgpu::counters::snapshot().since(&before);
    assert_eq!((encoded.compute_passes, encoded.dispatches), (1, 1));
    assert_eq!(encoded.write_buffers, 1 + u64::from(!patcher.values.is_empty()));
    let readback = device.create_buffer(&BufferDescriptor {
        label: "constant replay whole output",
        size: BLOCK_BUFFER_BYTES,
        usage: BufferUsage::MapRead | BufferUsage::CopyDst,
        mapped_at_creation: false,
    });
    let mut copy = device.create_command_encoder("constant replay whole readback");
    copy.copy_buffer_to_buffer(&patcher.buffer, 0, &readback, 0, BLOCK_BUFFER_BYTES);
    device.queue().submit([commands, copy.finish("constant replay whole readback")]);
    device.block_on(readback.map_async(MapMode::Read, 0, None)).expect("whole output map");
    let mut output = vec![0; BLOCK_BUFFER_BYTES as usize];
    readback.read_mapped(0, &mut output).expect("whole output bytes");
    readback.unmap();
    assert!(device.take_errors().is_empty());
    assert!(device.lost_reason().is_none());
    (output, encoded)
}

fn populate(old: &mut ConstantPatcher, new: &mut ConstantPatcher, round: usize) {
    let mut previous: [Vec<u8>; STAGES as usize] = Default::default();
    let mut seed = 0x1234_5678u32 ^ round as u32;
    let mut random = || {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        seed
    };
    for index in 0..600 {
        let stage = [3, 0, 4, 1, 2, 1, 3][index % 7];
        let count = [256, 0, 1, 7, 48, 10, 255, 129, 2, 64][(index + round) % 10];
        let mut data = previous[stage].clone();
        data.resize(count * 16, 0);
        if count != 0 {
            for _ in 0..4 {
                let offset = (random() as usize % (count * 4)) * 4;
                let word = [0x7fc0_0001, 0xff80_0000, 0x8000_0000, 0x0000_0001, random()][index % 5];
                data[offset..offset + 4].copy_from_slice(&word.to_le_bytes());
            }
        }
        assert!(old.has_room(&[4096]) && new.has_room(&[4096]));
        if index % 3 == 0 {
            let mut mask = [0u32; 8];
            let mut values = Vec::new();
            for (i, value) in data.chunks_exact(16).enumerate() {
                if previous[stage].get(i * 16..i * 16 + 16) != Some(value) {
                    mask[i / 32] |= 1 << (i % 32);
                    values.extend_from_slice(value);
                }
            }
            assert_eq!(
                old.push_masked(stage as u32, count, mask, &values, 4096),
                new.push_masked(stage as u32, count, mask, &values, 4096)
            );
        } else {
            assert_eq!(
                old.push(stage as u32, Some(&previous[stage]), &data, 4096),
                new.push(stage as u32, Some(&previous[stage]), &data, 4096)
            );
        }
        previous[stage] = data;
    }
    // A value outside a short block is still state carried into a later long block.
    let mut high_mask = [0u32; 8];
    high_mask[7] = 1 << 31;
    let high = [0xa5u8; 16];
    assert_eq!(old.push_masked(4, 1, high_mask, &high, 4096), new.push_masked(4, 1, high_mask, &high, 4096));
    assert_eq!(old.push_masked(4, 256, [0; 8], &[], 4096), new.push_masked(4, 256, [0; 8], &[], 4096));
    assert_eq!(old.records, new.records);
    assert_eq!(old.values, new.values);
    assert_eq!(old.cursor, new.cursor);
}

#[test]
fn binned_replay_matches_full_baseline_and_scalar_oracle_across_rewinds() {
    let instance = Instance::new().expect("instance");
    let adapter = instance.block_on(instance.request_adapter(&RequestAdapterOptions::default())).expect("adapter");
    let device = instance.block_on(adapter.request_device(&DeviceDescriptor::default())).expect("device");
    device.push_error_scope(ErrorFilter::Validation);
    let mut old = ConstantPatcher::with_binning(&device, 256, false);
    let mut new = ConstantPatcher::with_binning(&device, 256, true);
    assert!(device.block_on(device.pop_error_scope()).expect("validation scope").is_none());
    let mut expected = vec![0; BLOCK_BUFFER_BYTES as usize];
    assert!(old.encode(&device).is_none() && new.encode(&device).is_none());
    for round in 0..3 {
        populate(&mut old, &mut new, round);
        check_bins(&new.records);
        oracle(&old.records, &old.values, &mut expected);
        let (old_output, old_calls) = read_blocks(&device, &mut old);
        let (new_output, new_calls) = read_blocks(&device, &mut new);
        assert_eq!(old_output, expected, "unchanged baseline vs scalar round {round}");
        assert_eq!(new_output, expected, "candidate vs scalar round {round}");
        assert_eq!(old_calls.write_buffers, new_calls.write_buffers);
        old.rewind();
        new.rewind();
        assert_eq!(old.records, vec![0]);
        assert_eq!(new.records, vec![0]);
        assert_eq!(old.cursor, 0);
        assert_eq!(new.cursor, 0);
        assert!(old.values.is_empty() && new.values.is_empty());
        assert!(old.encode(&device).is_none() && new.encode(&device).is_none());
    }
}

fn benchmark_input(old: &mut ConstantPatcher, new: &mut ConstantPatcher, kind: usize) {
    if kind == 1 {
        populate(old, new, 0);
        return;
    }
    let mut previous: [Vec<u8>; STAGES as usize] = Default::default();
    for (stage, count) in [256, 256, 48, 7, 10].into_iter().enumerate() {
        let data = vec![stage as u8 + 1; count * 16];
        assert_eq!(old.push(stage as u32, None, &data, 4096), new.push(stage as u32, None, &data, 4096));
        previous[stage] = data;
    }
    if kind == 2 {
        // Authored small system blocks with less frequent fetch changes. This is
        // a workload, not a measurement of a title's actual constant distribution.
        for index in 0u32..1024 {
            let stage = if index % 12 == 0 { 2 } else { 3 };
            let data = &mut previous[stage];
            let before = data.clone();
            let at = index as usize % (data.len() / 4) * 4;
            data[at..at + 4].copy_from_slice(&index.to_le_bytes());
            assert!(old.has_room(&[4096]) && new.has_room(&[4096]));
            assert_eq!(
                old.push(stage as u32, Some(&before), data, 4096),
                new.push(stage as u32, Some(&before), data, 4096)
            );
        }
    }
}

fn replay_wall_ns(device: &Device, patcher: &mut ConstantPatcher, iterations: usize) -> u64 {
    let before = gpu_webgpu::counters::snapshot();
    let started = std::time::Instant::now();
    for _ in 0..iterations {
        let (commands, _) = patcher.encode(device).unwrap();
        device.queue().submit([commands]);
        device.block_on(device.queue().on_submitted_work_done()).unwrap();
    }
    let elapsed = started.elapsed().as_nanos() as u64 / iterations as u64;
    let calls = gpu_webgpu::counters::snapshot().since(&before);
    assert_eq!(
        (calls.compute_passes, calls.dispatches, calls.submits),
        (iterations as u64, iterations as u64, iterations as u64)
    );
    assert_eq!(calls.write_buffers, iterations as u64 * 2);
    assert!(device.take_errors().is_empty() && device.lost_reason().is_none());
    elapsed
}

#[test]
#[ignore = "full encode/upload/submit/completion timing; requires an explicitly quiet host"]
fn benchmark_full_replay_with_upload_and_completion() {
    assert_eq!(std::env::var("PUBLIC_CONSTANT_PATCH_BENCH").as_deref(), Ok("1"));
    assert!(!cfg!(debug_assertions), "use the release profile");
    let instance = Instance::new().expect("instance");
    let adapter = instance.block_on(instance.request_adapter(&RequestAdapterOptions::default())).expect("adapter");
    let device = instance.block_on(adapter.request_device(&DeviceDescriptor::default())).expect("device");
    for (kind, name) in
        ["small_five_blocks", "interleaved_variable_blocks", "small_system_and_fetch_blocks"].into_iter().enumerate()
    {
        let mut old = ConstantPatcher::with_binning(&device, 256, false);
        let mut new = ConstantPatcher::with_binning(&device, 256, true);
        benchmark_input(&mut old, &mut new, kind);
        let mut expected = vec![0; BLOCK_BUFFER_BYTES as usize];
        oracle(&old.records, &old.values, &mut expected);
        assert_eq!(read_blocks(&device, &mut old).0, expected);
        assert_eq!(read_blocks(&device, &mut new).0, expected);
        // Warm both complete paths, including CPU packing and the actual GPU dispatch.
        replay_wall_ns(&device, &mut old, 3);
        replay_wall_ns(&device, &mut new, 3);
        let (mut baseline, mut candidate) = (Vec::new(), Vec::new());
        for sample in 0..7 {
            let (a, b) = if sample % 2 == 0 {
                (replay_wall_ns(&device, &mut old, 8), replay_wall_ns(&device, &mut new, 8))
            } else {
                let b = replay_wall_ns(&device, &mut new, 8);
                (replay_wall_ns(&device, &mut old, 8), b)
            };
            baseline.push(a);
            candidate.push(b);
        }
        assert_eq!(read_blocks(&device, &mut old).0, expected);
        assert_eq!(read_blocks(&device, &mut new).0, expected);
        println!(
            "PUBLIC_CONSTANT_PATCH_BENCH name={name} records={} values_bytes={} extra_record_bytes={} baseline_wall_ns={baseline:?} candidate_wall_ns={candidate:?} full_bytes_equal=true cpu_thread_time=false gpu_execution_time=false game_verified=false iphone_verified=false",
            old.records[0],
            old.values.len(),
            (BIN_HEADER_WORDS - 1 + old.records[0] as usize) * 4
        );
    }
}
