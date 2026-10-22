//! Explicit test executable for the single pinned self-authored LLD fixture.
//! Reads metadata only, builds an in-memory unauthenticated envelope, never runs
//! the input or writes reconstructed bytes. Not a general framework test tool.
use ios_signed_image_policy::{
    Manifest, candidate_make_manifest, candidate_verify_binding, make_manifest, restoration_ranges,
    sha, verify_binding,
};
use std::{collections::BTreeSet, io::Read};
fn put32(bytes: &mut [u8], at: usize, value: u32) {
    bytes[at..at + 4].copy_from_slice(&value.to_le_bytes())
}
fn put64(bytes: &mut [u8], at: usize, value: u64) {
    bytes[at..at + 8].copy_from_slice(&value.to_le_bytes())
}
fn envelope(original: &[u8], m: &Manifest) -> Vec<u8> {
    let mut blob = vec![0u8; 80];
    for (at, value) in [
        (0, 0xfade0cc0u32),
        (4, 80),
        (8, 2),
        (12, 0),
        (16, 28),
        (20, 0x10000),
        (24, 72),
        (28, 0xfade0c02),
        (32, 44),
        (72, 0xfade0b01),
        (76, 8),
    ] {
        blob[at..at + 4].copy_from_slice(&value.to_be_bytes());
    }
    blob.resize(18080, 0);
    let offset = (original.len() + 15) & !15;
    let mut data = original.to_vec();
    data.resize(offset, 0);
    data.extend(blob);
    put32(&mut data, 16, m.ncmds as u32 + 1);
    put32(&mut data, 20, m.sizeofcmds as u32 + 16);
    let command = 32 + m.sizeofcmds as usize;
    let size = data.len() - offset;
    for (index, value) in [0x1du32, 16, offset as u32, size as u32]
        .into_iter()
        .enumerate()
    {
        put32(&mut data, command + index * 4, value);
    }
    let size = data.len() as u64 - m.linkedit_fileoff;
    let vm = if size <= m.linkedit_vmsize {
        m.linkedit_vmsize
    } else {
        (size + 16383) & !16383
    };
    put64(&mut data, m.linkedit_command_offset as usize + 32, vm);
    put64(&mut data, m.linkedit_command_offset as usize + 48, size);
    data
}
fn main() {
    let args: Vec<_> = std::env::args_os().collect();
    assert_eq!(
        args.len(),
        2,
        "explicit pinned public fixture path required"
    );
    let mut original = Vec::new();
    std::fs::File::open(&args[1])
        .unwrap()
        .take(49433)
        .read_to_end(&mut original)
        .unwrap();
    let expected = "166c05ad0bbc6d0aa3f317043bc04029a572cba999db7a822e16be2caaeb6483";
    assert_eq!(original.len(), 49432);
    assert_eq!(
        sha(&original),
        expected,
        "not the pinned self-authored LLD fixture"
    );
    assert!(make_manifest(&original).is_err());
    let manifest = candidate_make_manifest(&original).unwrap();
    let mut data = envelope(&original, &manifest);
    candidate_verify_binding(&data, &manifest, expected).unwrap();
    assert!(verify_binding(&data, &manifest, expected).is_err());
    assert!(candidate_verify_binding(&data, &manifest, &"0".repeat(64)).is_err());
    let mutable: BTreeSet<_> = restoration_ranges(&manifest)
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
                candidate_verify_binding(&data, &manifest, expected).is_err(),
                "unbound public position {index}"
            );
            data[index] ^= 1;
            checked += 1;
        }
    }
    println!(
        "{}",
        serde_json::json!({"public_unsigned_bytes":original.len(),"public_sha256":expected,"candidate_reconstruction_passed":true,"baseline_still_refuses":true,"outside40_mutations_rejected":checked,"restoration_positions":40,"os_signature_authenticated":false,"altserver_tested":false,"private_framework_tested":false,"loader_integration_allowed":false,"input_executed":false})
    );
}
