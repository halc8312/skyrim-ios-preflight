//! Hand-authored public input plus an independent scalar expected output.
pub fn batch(count: usize, seed:u32) -> (Vec<u8>, Vec<u8>, Vec<u8>) {
    let mut records = vec![0u32;16+count*12]; records[0]=count as u32;
    let mut values = Vec::new(); let mut expected = Vec::new();
    let mut state = [[[0u8;16];256];5];
    let mut stage_indices: [Vec<u32>;5] = std::array::from_fn(|_|Vec::new());
    let mut maxima = [0;5];
    for index in 0..count {
        let stage = index%5;
        let length = if index<5 { 256 } else { [1,31,33,256,0,127,255][index%7] };
        let offset = expected.len().next_multiple_of(256);
        expected.resize(offset+(length*16).max(16),0xa5);
        let record = &mut records[16+index*12..][..12];
        record[..4].copy_from_slice(&[stage as u32,(offset/16) as u32,length as u32,(values.len()/16) as u32]);
        for lane in 0..256 {
            // High bits may update carried state outside a short block.
            if index<5 || (lane+index)%19==0 || lane==255 {
                record[4+lane/32] |= 1<<(lane%32);
                let bytes: Vec<_> = [(index as u32)^seed, lane as u32,0x7fc00001u32,0x80000000u32]
                    .iter().flat_map(|n|n.to_le_bytes()).collect();
                state[stage][lane].copy_from_slice(&bytes); values.extend(bytes);
            }
        }
        for lane in 0..length { expected[offset+lane*16..offset+(lane+1)*16].copy_from_slice(&state[stage][lane]); }
        stage_indices[stage].push(index as u32); maxima[stage]=maxima[stage].max(length as u32);
    }
    expected.resize(expected.len().next_multiple_of(256)+64,0xa5);
    for stage in 0..5 {
        records[1+stage*3]=records.len() as u32;
        records[2+stage*3]=stage_indices[stage].len() as u32;
        records[3+stage*3]=maxima[stage]; records.extend_from_slice(&stage_indices[stage]);
    }
    (records.iter().flat_map(|n|n.to_le_bytes()).collect(),values,expected)
}

