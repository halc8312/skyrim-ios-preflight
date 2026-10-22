//! Experimental unsigned-content byte binding only. Never a loader, signer,
//! signature authenticator, entitlement policy, or permission to execute.
pub mod macho;
mod manifest;
pub use manifest::{
    Manifest, candidate_make_manifest, load_manifest, make_manifest, validate_manifest,
};
use sha2::{Digest, Sha256};
use std::collections::BTreeSet;
pub const MAX_IMAGE: usize = 1 << 30;
pub const MAX_SIGNATURE: usize = 16 << 20;
pub const PAGE: u64 = 16384;
pub const SCHEMA: &str = "strict_unsigned_macho_reconstruction_v1";
pub const CANDIDATE_SCHEMA: &str = "strict_unsigned_macho_reconstruction_lld_padding_candidate_v1";
pub type Check<T> = Result<T, &'static str>;
pub fn need(condition: bool, category: &'static str) -> Check<()> {
    if condition { Ok(()) } else { Err(category) }
}
pub fn sha(data: &[u8]) -> String {
    Sha256::digest(data)
        .iter()
        .map(|byte| format!("{byte:02x}"))
        .collect()
}
fn align(value: u64, unit: u64) -> Check<u64> {
    value
        .checked_add(unit - 1)
        .map(|n| n & !(unit - 1))
        .ok_or("alignment_overflow")
}
fn be32(data: &[u8], at: usize) -> Check<u64> {
    let raw = data
        .get(at..at.checked_add(4).ok_or("signature_bounds")?)
        .ok_or("signature_bounds")?;
    Ok(u32::from_be_bytes(raw.try_into().unwrap()).into())
}

/// Recognizes only a bounded SuperBlob envelope. Child bodies, hashes, CMS,
/// certificates, entitlements and OS acceptance are deliberately not verified.
pub fn signature_shape(blob: &[u8]) -> Check<()> {
    need(
        (16..=MAX_SIGNATURE).contains(&blob.len()) && blob.len() % 16 == 0,
        "signature_blob_size",
    )?;
    let magic = be32(blob, 0)?;
    let length = be32(blob, 4)?;
    let count = be32(blob, 8)?;
    let rounded = align(length, 16)?;
    let padded = blob.len() as u64 == rounded + 18000;
    need(
        magic == 0xfade0cc0
            && (1..=32).contains(&count)
            && 12 + 8 * count <= length
            && length <= blob.len() as u64
            && (rounded == blob.len() as u64 || padded),
        "signature_superblob",
    )?;
    let length = length as usize;
    need(
        blob[length..].iter().all(|v| *v == 0),
        "signature_superblob",
    )?;
    let mut slots = BTreeSet::new();
    let mut intervals = Vec::new();
    let mut empty_cms = false;
    for number in 0..count as usize {
        let slot = be32(blob, 12 + 8 * number)?;
        let offset = be32(blob, 16 + 8 * number)? as usize;
        let expected = match slot {
            0 | 0x1000..=0x1004 => Some(0xfade0c02),
            2 => Some(0xfade0c01),
            5 => Some(0xfade7171),
            7 => Some(0xfade7172),
            0x10000 => Some(0xfade0b01),
            _ => None,
        };
        need(
            expected.is_some()
                && slots.insert(slot)
                && offset >= 12 + 8 * count as usize
                && offset <= length - 8,
            "signature_index",
        )?;
        let child_magic = be32(blob, offset)?;
        let child_length = be32(blob, offset + 4)? as usize;
        need(
            Some(child_magic) == expected && child_length >= 8 && child_length <= length - offset,
            "signature_child",
        )?;
        if slot == 0x10000 {
            empty_cms = child_length == 8;
        }
        intervals.push((offset, offset + child_length));
    }
    need(slots.contains(&0), "signature_primary_code_directory")?;
    need(!padded || empty_cms, "signature_adhoc_reservation")?;
    intervals.sort_unstable();
    let mut cursor = 12 + 8 * count as usize;
    for (start, end) in intervals {
        need(start >= cursor, "signature_overlap_or_gap")?;
        need(
            blob[cursor..start].iter().all(|v| *v == 0),
            "signature_overlap_or_gap",
        )?;
        cursor = end;
    }
    need(
        blob[cursor..length].iter().all(|v| *v == 0),
        "signature_trailing_data",
    )
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Report {
    pub original_byte_count: u64,
    pub reconstructed_sha256: String,
    pub signature_bytes: u64,
}
impl Report {
    pub fn to_json(&self) -> serde_json::Value {
        serde_json::json!({"unsigned_full_sha256_matches":true,"original_byte_count":self.original_byte_count,"reconstructed_sha256":self.reconstructed_sha256,"signature_bytes":self.signature_bytes,"os_signature_authenticated":false,"attestation":false,"apple_signer_tested":false,"loader_integration_allowed":false})
    }
}

/// Exactly forty original positions may be restored: two u32 header fields,
/// the appended 16-byte command slot, and two u64 LINKEDIT size fields. Every
/// other original byte is covered by the separately trusted full SHA-256.
pub fn restoration_ranges(manifest: &Manifest) -> Check<[std::ops::Range<usize>; 4]> {
    let length = usize::try_from(manifest.unsigned_length).map_err(|_| "manifest_limits")?;
    let command = usize::try_from(
        manifest
            .sizeofcmds
            .checked_add(32)
            .ok_or("manifest_layout")?,
    )
    .map_err(|_| "manifest_layout")?;
    let link = usize::try_from(manifest.linkedit_command_offset).map_err(|_| "manifest_layout")?;
    let ranges = [
        16..24,
        command..command.checked_add(16).ok_or("manifest_layout")?,
        link.checked_add(32).ok_or("manifest_layout")?
            ..link.checked_add(40).ok_or("manifest_layout")?,
        link.checked_add(48).ok_or("manifest_layout")?
            ..link.checked_add(56).ok_or("manifest_layout")?,
    ];
    need(
        ranges.iter().all(|range| range.end <= length),
        "manifest_layout",
    )?;
    for (i, a) in ranges.iter().enumerate() {
        for b in &ranges[i + 1..] {
            need(
                a.end <= b.start || b.end <= a.start,
                "overlapping_restoration_fields",
            )?;
        }
    }
    Ok(ranges)
}
pub fn verify_binding(
    signed: &[u8],
    manifest: &Manifest,
    expected_unsigned_sha256: &str,
) -> Check<Report> {
    verify_binding_for(signed, manifest, expected_unsigned_sha256, false)
}
/// Explicit structural candidate only. Its distinct schema cannot be passed to
/// the default verifier. No runtime/loader policy is changed by this function.
pub fn candidate_verify_binding(
    signed: &[u8],
    manifest: &Manifest,
    expected_unsigned_sha256: &str,
) -> Check<Report> {
    verify_binding_for(signed, manifest, expected_unsigned_sha256, true)
}
fn verify_binding_for(
    signed: &[u8],
    manifest: &Manifest,
    expected_unsigned_sha256: &str,
    candidate: bool,
) -> Check<Report> {
    manifest::validate_manifest_for(manifest, expected_unsigned_sha256, candidate)?;
    let image = if candidate {
        macho::parse_lld_candidate(signed, true)?
    } else {
        macho::parse(signed, true)?
    };
    let signature = image.signature.as_ref().ok_or("signature_presence")?;
    let link = &image.linkedit;
    need(
        image.ncmds == manifest.ncmds + 1
            && image.sizeofcmds == manifest.sizeofcmds + 16
            && signature.command_offset == 32 + manifest.sizeofcmds
            && image.first_data == manifest.first_data,
        "appended_signature_command_only",
    )?;
    need(
        link.command_offset == manifest.linkedit_command_offset
            && link.fileoff == manifest.linkedit_fileoff,
        "linkedit_identity",
    )?;
    let original_length = manifest.unsigned_length;
    need(
        signature.dataoff == align(original_length, 16)?
            && signature.datasize > 0
            && signature.datasize <= MAX_SIGNATURE as u64
            && signature.dataoff.checked_add(signature.datasize) == Some(signed.len() as u64)
            && original_length <= signature.dataoff,
        "signature_append_only",
    )?;
    need(
        signed
            .get(original_length as usize..signature.dataoff as usize)
            .ok_or("signature_append_only")?
            .iter()
            .all(|v| *v == 0),
        "signature_append_only",
    )?;
    let expected_vm = if link.filesize <= manifest.linkedit_vmsize {
        manifest.linkedit_vmsize
    } else {
        align(link.filesize, PAGE)?
    };
    need(
        link.filesize == signed.len() as u64 - link.fileoff && link.vmsize == expected_vm,
        "linkedit_size_policy",
    )?;
    signature_shape(
        signed
            .get(signature.dataoff as usize..)
            .ok_or("signature_append_only")?,
    )?;
    let ranges = restoration_ranges(manifest)?;
    let mut restored = signed
        .get(..original_length as usize)
        .ok_or("signature_append_only")?
        .to_vec();
    restored[16..20].copy_from_slice(&(manifest.ncmds as u32).to_le_bytes());
    restored[20..24].copy_from_slice(&(manifest.sizeofcmds as u32).to_le_bytes());
    restored[ranges[1].clone()].fill(0);
    restored[ranges[2].clone()].copy_from_slice(&manifest.linkedit_vmsize.to_le_bytes());
    restored[ranges[3].clone()].copy_from_slice(&manifest.linkedit_filesize.to_le_bytes());
    need(
        sha(&restored) == expected_unsigned_sha256,
        "unsigned_full_sha256_mismatch",
    )?;
    need(
        manifest::make_manifest_for(&restored, candidate)? == *manifest,
        "restored_manifest_mismatch",
    )?;
    Ok(Report {
        original_byte_count: original_length,
        reconstructed_sha256: expected_unsigned_sha256.into(),
        signature_bytes: signature.datasize,
    })
}

#[cfg(test)]
mod tests;
