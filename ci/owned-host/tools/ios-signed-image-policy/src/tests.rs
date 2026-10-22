use super::*;
use std::collections::BTreeSet;
fn u32put(out: &mut [u8], at: usize, v: u32) {
    out[at..at + 4].copy_from_slice(&v.to_le_bytes())
}
fn u64put(out: &mut [u8], at: usize, v: u64) {
    out[at..at + 8].copy_from_slice(&v.to_le_bytes())
}
fn segment(
    name: &[u8],
    vm: u64,
    vmsize: u64,
    off: u64,
    size: u64,
    section: Option<(u64, u32, u64)>,
) -> Vec<u8> {
    let mut out = vec![0; 72 + if section.is_some() { 80 } else { 0 }];
    let length = out.len();
    u32put(&mut out, 0, 0x19);
    u32put(&mut out, 4, length as u32);
    out[8..8 + name.len()].copy_from_slice(name);
    u64put(&mut out, 24, vm);
    u64put(&mut out, 32, vmsize);
    u64put(&mut out, 40, off);
    u64put(&mut out, 48, size);
    u32put(&mut out, 56, 7);
    u32put(&mut out, 60, 5);
    u32put(&mut out, 64, section.is_some() as u32);
    if let Some((address, offset, bytes)) = section {
        out[72..78].copy_from_slice(b"__part");
        out[88..88 + name.len()].copy_from_slice(name);
        u64put(&mut out, 104, address);
        u64put(&mut out, 112, bytes);
        u32put(&mut out, 120, offset);
        u32put(&mut out, 124, 4);
    }
    out
}
fn fixture(platform: u32, vm: u64) -> Vec<u8> {
    let base = 0x100000000;
    let mut identity = vec![0; 64];
    u32put(&mut identity, 0, 0xd);
    u32put(&mut identity, 4, 64);
    u32put(&mut identity, 8, 24);
    let name = b"@rpath/Synthetic.framework/Synthetic\0";
    identity[24..24 + name.len()].copy_from_slice(name);
    let mut build = vec![0; 24];
    u32put(&mut build, 0, 0x32);
    u32put(&mut build, 4, 24);
    u32put(&mut build, 8, platform);
    u32put(&mut build, 12, 0x1a0000);
    let commands = [
        segment(
            b"__TEXT",
            base,
            0x4000,
            0,
            0x400,
            Some((base + 0x300, 0x300, 32)),
        ),
        segment(
            b"__DATA",
            base + 0x4000,
            0x4000,
            0x400,
            0x100,
            Some((base + 0x4000, 0x400, 16)),
        ),
        segment(b"__LINKEDIT", base + 0x8000, vm, 0x500, 0x31, None),
        identity,
        build,
    ];
    let mut data = vec![0; 0x531];
    for (at, v) in [
        (0, 0xfeedfacf),
        (4, 0x100000c),
        (12, 6),
        (16, commands.len() as u32),
        (20, commands.iter().map(Vec::len).sum::<usize>() as u32),
    ] {
        u32put(&mut data, at, v);
    }
    let mut at = 32;
    for cmd in commands {
        data[at..at + cmd.len()].copy_from_slice(&cmd);
        at += cmd.len();
    }
    for (i, v) in data[0x300..0x320].iter_mut().enumerate() {
        *v = i as u8;
    }
    data[0x400..0x410].copy_from_slice(b"PUBLIC_DATA_ONLY");
    data
}
fn blob(cms: bool, reservation: usize) -> Vec<u8> {
    let header = if cms { 28 } else { 20 };
    let length = header + 44 + if cms { 8 } else { 0 };
    let mut out = vec![0; ((length + 15) & !15) + reservation];
    let put = |out: &mut [u8], at: usize, value: u32| {
        out[at..at + 4].copy_from_slice(&value.to_be_bytes())
    };
    put(&mut out, 0, 0xfade0cc0);
    put(&mut out, 4, length as u32);
    put(&mut out, 8, if cms { 2 } else { 1 });
    put(&mut out, 16, header as u32);
    put(&mut out, header, 0xfade0c02);
    put(&mut out, header + 4, 44);
    if cms {
        put(&mut out, 20, 0x10000);
        put(&mut out, 24, (header + 44) as u32);
        put(&mut out, header + 44, 0xfade0b01);
        put(&mut out, header + 48, 8);
    }
    out
}
fn signed(original: &[u8], blob: &[u8]) -> (Vec<u8>, Manifest) {
    let m = make_manifest(original).unwrap();
    signed_from_manifest(original, blob, m)
}
fn signed_from_manifest(original: &[u8], blob: &[u8], m: Manifest) -> (Vec<u8>, Manifest) {
    let offset = align(original.len() as u64, 16).unwrap() as usize;
    let mut data = original.to_vec();
    data.resize(offset, 0);
    data.extend(blob);
    u32put(&mut data, 16, m.ncmds as u32 + 1);
    u32put(&mut data, 20, m.sizeofcmds as u32 + 16);
    let cmd = 32 + m.sizeofcmds as usize;
    for (i, v) in [0x1d, 16, offset as u32, blob.len() as u32]
        .into_iter()
        .enumerate()
    {
        u32put(&mut data, cmd + i * 4, v)
    }
    let size = data.len() as u64 - m.linkedit_fileoff;
    let vm = if size <= m.linkedit_vmsize {
        m.linkedit_vmsize
    } else {
        align(size, PAGE).unwrap()
    };
    u64put(&mut data, m.linkedit_command_offset as usize + 32, vm);
    u64put(&mut data, m.linkedit_command_offset as usize + 48, size);
    (data, m)
}
fn padded_fixture() -> (Vec<u8>, usize, usize) {
    let mut original = fixture(2, 0x4000);
    let data_command = 32 + 152;
    let old_end = 32 + u32::from_le_bytes(original[20..24].try_into().unwrap()) as usize;
    let inserted = data_command + 152;
    let mut bss = vec![0u8; 80];
    bss[..5].copy_from_slice(b"__bss");
    bss[16..22].copy_from_slice(b"__DATA");
    u64put(&mut bss, 32, 0x100004010);
    u64put(&mut bss, 40, 32);
    u32put(&mut bss, 64, 1);
    let tail = original[inserted..old_end].to_vec();
    original[inserted..inserted + 80].copy_from_slice(&bss);
    original[inserted + 80..old_end + 80].copy_from_slice(&tail);
    u32put(&mut original, data_command + 4, 232);
    u32put(&mut original, data_command + 64, 2);
    u32put(&mut original, 20, (old_end - 32 + 80) as u32);
    (original, data_command, inserted)
}
#[test]
fn candidate_padding_roundtrip_has_distinct_schema_and_unchanged_mask() {
    let (original, _, _) = padded_fixture();
    assert!(make_manifest(&original).is_err());
    let m = candidate_make_manifest(&original).unwrap();
    assert_eq!(m.schema, CANDIDATE_SCHEMA);
    let (mut data, m) = signed_from_manifest(&original, &blob(true, 18000), m);
    assert!(candidate_verify_binding(&data, &m, &sha(&original)).is_ok());
    assert!(verify_binding(&data, &m, &sha(&original)).is_err());
    assert!(candidate_verify_binding(&data, &m, &"0".repeat(64)).is_err());
    let mutable: BTreeSet<_> = restoration_ranges(&m)
        .unwrap()
        .into_iter()
        .flatten()
        .collect();
    assert_eq!(mutable.len(), 40);
    for position in 0..original.len() {
        if !mutable.contains(&position) {
            data[position] ^= 1;
            assert!(candidate_verify_binding(&data, &m, &m.unsigned_sha256).is_err());
            data[position] ^= 1;
        }
    }
}
#[test]
fn candidate_alias_header_padding_offset_and_order_are_rejected() {
    let (original, segment, bss) = padded_fixture();
    for address in [0x100004000, 0x100004008] {
        let mut bad = original.clone();
        u64put(&mut bad, bss + 32, address);
        assert!(candidate_make_manifest(&bad).is_err());
    }
    let mut bad = original.clone();
    bad[0x4ff] = 1;
    assert!(candidate_make_manifest(&bad).is_err());
    let mut bad = original.clone();
    u32put(&mut bad, bss + 48, 0x410);
    assert!(candidate_make_manifest(&bad).is_err());
    let mut bad = original.clone();
    let init = segment + 72;
    let first = bad[init..init + 80].to_vec();
    let second = bad[bss..bss + 80].to_vec();
    bad[init..init + 80].copy_from_slice(&second);
    bad[bss..bss + 80].copy_from_slice(&first);
    assert!(candidate_make_manifest(&bad).is_err());
    let mut bad = original.clone();
    let end = 32 + u32::from_le_bytes(bad[20..24].try_into().unwrap()) as u64;
    let text = 32 + 72;
    u64put(&mut bad, text + 32, 0x100000000 + end);
    u32put(&mut bad, text + 48, 0);
    u32put(&mut bad, text + 64, 1);
    assert!(candidate_make_manifest(&bad).is_err());
    let mut bad = original.clone();
    u64put(&mut bad, segment + 24, 0x100000000);
    assert!(candidate_make_manifest(&bad).is_err());
}
#[test]
fn candidate_zerofill_boundary_and_multiple_ranges_are_explicit() {
    let (original, segment, bss) = padded_fixture();
    for (delta, baseline_accepts) in [(0x100u64, true), (0xff, false)] {
        let mut data = original.clone();
        u64put(&mut data, bss + 32, 0x100004000 + delta);
        assert_eq!(make_manifest(&data).is_ok(), baseline_accepts);
        assert!(candidate_make_manifest(&data).is_ok());
    }
    let mut data = original.clone();
    let old_end = 32 + u32::from_le_bytes(data[20..24].try_into().unwrap()) as usize;
    let inserted = bss + 80;
    let tail = data[inserted..old_end].to_vec();
    let mut second = data[bss..bss + 80].to_vec();
    u64put(&mut second, 32, 0x100004040);
    data[inserted..inserted + 80].copy_from_slice(&second);
    data[inserted + 80..old_end + 80].copy_from_slice(&tail);
    u32put(&mut data, segment + 4, 312);
    u32put(&mut data, segment + 64, 3);
    u32put(&mut data, 20, (old_end - 32 + 80) as u32);
    assert!(candidate_make_manifest(&data).is_ok());
    u64put(&mut data, inserted + 32, 0x100004018);
    assert!(candidate_make_manifest(&data).is_err());
}

#[test]
fn device_simulator_and_vm_growth_roundtrip() {
    for platform in [2, 7] {
        for vm in [512, 0x4000] {
            let original = fixture(platform, vm);
            let (data, m) = signed(&original, &blob(true, 18000));
            let before = data.clone();
            let report = verify_binding(&data, &m, &sha(&original)).unwrap();
            assert_eq!(report.original_byte_count, original.len() as u64);
            assert_eq!(report.to_json()["os_signature_authenticated"], false);
            assert_eq!(data, before);
        }
    }
}
#[test]
fn every_original_byte_outside_exact_forty_positions_remains_bound() {
    let original = fixture(2, 0x4000);
    let (mut data, m) = signed(&original, &blob(false, 0));
    let mutable: BTreeSet<_> = restoration_ranges(&m)
        .unwrap()
        .into_iter()
        .flatten()
        .collect();
    assert_eq!(mutable.len(), 40);
    let mut checked = 0;
    for index in 0..original.len() {
        if !mutable.contains(&index) {
            data[index] ^= 1;
            assert!(
                verify_binding(&data, &m, &m.unsigned_sha256).is_err(),
                "unbound position {index}"
            );
            data[index] ^= 1;
            checked += 1;
        }
    }
    assert_eq!(checked, original.len() - 40);
}
#[test]
fn exact_empty_cms_reservation_only() {
    assert!(signature_shape(&blob(true, 18000)).is_ok());
    assert!(signature_shape(&blob(true, 17984)).is_err());
    assert!(signature_shape(&blob(true, 18016)).is_err());
    assert!(signature_shape(&blob(false, 18000)).is_err());
    let mut bad = blob(true, 18000);
    *bad.last_mut().unwrap() = 1;
    assert!(signature_shape(&bad).is_err());
    let mut bad = blob(true, 18000);
    bad[27] = 73;
    assert!(signature_shape(&bad).is_err());
}
#[test]
fn signature_bodies_are_not_authenticated() {
    let original = fixture(2, 0x4000);
    let (mut data, m) = signed(&original, &blob(false, 0));
    *data.last_mut().unwrap() ^= 1;
    let report = verify_binding(&data, &m, &m.unsigned_sha256).unwrap();
    assert_eq!(report.to_json()["os_signature_authenticated"], false);
    assert_eq!(report.to_json()["loader_integration_allowed"], false);
}
#[test]
fn malformed_schema_duplicate_keys_and_untrusted_hash_refuse() {
    let original = fixture(2, 0x4000);
    let (data, m) = signed(&original, &blob(false, 0));
    assert!(verify_binding(&data, &m, &"0".repeat(64)).is_err());
    let raw = m.to_json().to_string();
    let duplicate = raw.replace(
        "\"unsigned_length\":",
        "\"unsigned_length\":0,\"unsigned_length\":",
    );
    assert!(load_manifest(duplicate.as_bytes()).is_err());
    for bad in [
        raw.replace(
            &format!("\"unsigned_length\":{}", m.unsigned_length),
            "\"unsigned_length\":true",
        ),
        raw.replace("\"vm_alignment\":16384", "\"vm_alignment\":1.5"),
        raw.replace("\"schema\":", "\"unexpected\":0,\"schema\":"),
    ] {
        assert!(load_manifest(bad.as_bytes()).is_err());
    }
    assert!(load_manifest(b"{\"x\":NaN}").is_err());
    assert!(load_manifest(&vec![b' '; 16385]).is_err());
    assert!(load_manifest(("[".repeat(1000) + &"]".repeat(1000)).as_bytes()).is_err());
    assert_eq!(load_manifest(raw.as_bytes()).unwrap(), m);
}
#[test]
fn truncations_architecture_and_range_overflow_refuse_without_panic() {
    let original = fixture(2, 0x4000);
    let (data, m) = signed(&original, &blob(false, 0));
    for end in 0..data.len() {
        assert!(
            std::panic::catch_unwind(|| verify_binding(&data[..end], &m, &m.unsigned_sha256))
                .unwrap()
                .is_err()
        );
    }
    assert!(make_manifest(&data).is_err());
    let mut bad = original.clone();
    u32put(&mut bad, 8, 2);
    assert!(make_manifest(&bad).is_err());
    let mut m = m;
    m.sizeofcmds = u64::MAX;
    assert!(restoration_ranges(&m).is_err());
    assert!(validate_manifest(&m, &m.unsigned_sha256).is_err());
}
