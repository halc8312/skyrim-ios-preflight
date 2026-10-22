//! Structural metadata parity with the public Python signing-binding parser.
//! This is intentionally not the stricter Apple runtime loader policy. In
//! particular, signed signature append/EOF/size validation belongs to the outer
//! verifier, before it slices signature bytes. Nothing here dereferences a
//! metadata pointer or authenticates a signature.
use crate::{Check, MAX_IMAGE, need};
use std::collections::BTreeSet;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LinkEdit {
    pub command_offset: u64,
    pub vmaddr: u64,
    pub vmsize: u64,
    pub fileoff: u64,
    pub filesize: u64,
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Signature {
    pub command_offset: u64,
    pub dataoff: u64,
    pub datasize: u64,
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Image {
    pub ncmds: u64,
    pub sizeofcmds: u64,
    pub first_data: u64,
    pub linkedit: LinkEdit,
    pub signature: Option<Signature>,
}
struct Segment {
    name: Vec<u8>,
    offset: u64,
    vmaddr: u64,
    vmsize: u64,
    fileoff: u64,
    filesize: u64,
    nsects: u64,
}
fn bounded(data: &[u8], offset: u64, size: u64, category: &'static str) -> Check<()> {
    need(
        offset <= data.len() as u64 && size <= data.len() as u64 - offset,
        category,
    )
}
fn bytes(data: &[u8], offset: u64, size: u64) -> Check<&[u8]> {
    bounded(data, offset, size, "load_command_bounds")?;
    Ok(&data[offset as usize..(offset + size) as usize])
}
fn word(data: &[u8], offset: u64) -> Check<u64> {
    Ok(u32::from_le_bytes(bytes(data, offset, 4)?.try_into().unwrap()).into())
}
fn wide(data: &[u8], offset: u64) -> Check<u64> {
    Ok(u64::from_le_bytes(
        bytes(data, offset, 8)?.try_into().unwrap(),
    ))
}
fn fixed_name(raw: &[u8]) -> Check<Vec<u8>> {
    let end = raw
        .iter()
        .position(|&value| value == 0)
        .unwrap_or(raw.len());
    need(
        end != 0 && raw[end..].iter().all(|&value| value == 0),
        "unsupported_segment_name",
    )?;
    Ok(raw[..end].to_vec())
}

/// Acceptance and exposed metadata follow signing_binding.py::parse. The
/// returned signed signature extent is still untrusted until outer verification.
pub fn parse(data: &[u8], signed: bool) -> Check<Image> {
    parse_impl(data, signed, false)
}
/// Explicit experiment only; default parsing remains byte-for-byte policy
/// compatible with the Python baseline. No loader uses this entry point.
pub fn parse_lld_candidate(data: &[u8], signed: bool) -> Check<Image> {
    parse_impl(data, signed, true)
}
fn parse_impl(data: &[u8], signed: bool, lld_candidate: bool) -> Check<Image> {
    need((32..=MAX_IMAGE).contains(&data.len()), "image_size")?;
    need(
        word(data, 0)? == 0xfeed_facf
            && word(data, 4)? == 0x0100_000c
            && word(data, 8)? == 0
            && word(data, 12)? == 6
            && word(data, 28)? == 0,
        "unsupported_macho_header",
    )?;
    let ncmds = word(data, 16)?;
    let sizeofcmds = word(data, 20)?;
    need(
        (1..=512).contains(&ncmds) && (8..=65536).contains(&sizeofcmds),
        "load_command_limits",
    )?;
    bounded(data, 32, sizeofcmds, "load_command_bounds")?;
    let end = 32 + sizeofcmds; // bounded by the preceding 64 KiB command limit
    let mut position = 32;
    let mut singleton = BTreeSet::new();
    let mut segments = Vec::<Segment>::new();
    let mut first_data = None::<u64>;
    let mut link_data = Vec::<(u64, u64)>::new();
    let mut symbol_count = None;
    let mut dynamic_symbols = None;
    let mut encryption = None;
    let mut signature = None;
    for index in 0..ncmds {
        need(position + 8 <= end, "load_command_bounds")?;
        let command = word(data, position)?;
        let size = word(data, position + 4)?;
        need(
            size >= 8 && size % 8 == 0 && size <= end - position,
            "load_command_size",
        )?;
        match command {
            0x19 => {
                need(size >= 72, "segment_size")?;
                let name = fixed_name(bytes(data, position + 8, 16)?)?;
                let vmaddr = wide(data, position + 24)?;
                let vmsize = wide(data, position + 32)?;
                let fileoff = wide(data, position + 40)?;
                let filesize = wide(data, position + 48)?;
                let nsects = word(data, position + 64)?;
                // nsects is u32-derived, so multiplication fits u64.
                need(
                    size == 72 + nsects * 80 && vmsize >= filesize,
                    "segment_size",
                )?;
                bounded(data, fileoff, filesize, "segment_file_bounds")?;
                // Python permits the legal one-past endpoint 2^64.
                let vm_end = u128::from(vmaddr) + u128::from(vmsize);
                need(vm_end <= 1u128 << 64, "segment_vm_bounds")?;
                need(
                    !segments.iter().any(|old| old.name == name),
                    "duplicate_segment",
                )?;
                let mut section_ranges = Vec::<(u128, u128)>::new();
                let mut initialized_end = u128::from(vmaddr);
                let mut zero_addresses = Vec::<u128>::new();
                let mut seen_zero = false;
                for number in 0..nsects {
                    let section = position + 72 + number * 80;
                    need(
                        fixed_name(bytes(data, section + 16, 16)?)? == name,
                        "section_segment_name",
                    )?;
                    let address = wide(data, section + 32)?;
                    let section_size = wide(data, section + 40)?;
                    let section_offset = word(data, section + 48)?;
                    let exponent = word(data, section + 52)?;
                    let relocation_offset = word(data, section + 56)?;
                    let relocation_count = word(data, section + 60)?;
                    let flags = word(data, section + 64)?;
                    link_data.push((relocation_offset, relocation_count * 8));
                    need(
                        exponent <= 31
                            && vmaddr <= address
                            && u128::from(address) + u128::from(section_size) <= vm_end,
                        "section_vm_bounds",
                    )?;
                    if section_size != 0 && matches!(flags & 0xff, 1 | 0xc | 0x12) {
                        if lld_candidate {
                            need(section_offset == 0, "candidate_zerofill_offset")?;
                            seen_zero = true;
                            zero_addresses.push(u128::from(address));
                        } else {
                            need(
                                section_offset == 0
                                    && u128::from(address)
                                        >= u128::from(vmaddr) + u128::from(filesize),
                                "zerofill_must_be_unmapped_segment_tail",
                            )?;
                        }
                    } else if section_size != 0 {
                        if lld_candidate {
                            need(!seen_zero, "candidate_zerofill_not_last")?;
                            initialized_end =
                                initialized_end.max(u128::from(address) + u128::from(section_size));
                        }
                        need(
                            fileoff <= section_offset
                                && u128::from(section_offset) + u128::from(section_size)
                                    <= u128::from(fileoff) + u128::from(filesize),
                            "section_file_bounds",
                        )?;
                        need(
                            address - vmaddr == section_offset - fileoff,
                            "section_file_vm_mapping",
                        )?;
                        if name == b"__TEXT" {
                            first_data = Some(
                                first_data.map_or(section_offset, |old| old.min(section_offset)),
                            );
                        }
                    }
                    if lld_candidate && section_size != 0 {
                        section_ranges.push((
                            u128::from(address),
                            u128::from(address) + u128::from(section_size),
                        ));
                    }
                }
                if lld_candidate {
                    section_ranges.sort_unstable();
                    need(
                        section_ranges.windows(2).all(|pair| pair[0].1 <= pair[1].0),
                        "candidate_section_alias",
                    )?;
                    need(
                        zero_addresses
                            .iter()
                            .all(|address| *address >= initialized_end),
                        "candidate_zerofill_before_initialized_end",
                    )?;
                    let padding = zero_addresses
                        .iter()
                        .any(|address| *address < u128::from(vmaddr) + u128::from(filesize));
                    if padding {
                        need(
                            name == b"__DATA" && fileoff >= end,
                            "candidate_padding_must_be_nonheader_data",
                        )?;
                        // A padding overlap proves initialized_end <= file end.
                        // Scan the entire remaining file tail once per segment,
                        // not once per zero-fill record. No bytes are rewritten.
                        let delta = u64::try_from(initialized_end - u128::from(vmaddr))
                            .map_err(|_| "candidate_padding_bounds")?;
                        need(delta < filesize, "candidate_padding_bounds")?;
                        need(
                            bytes(data, fileoff + delta, filesize - delta)?
                                .iter()
                                .all(|value| *value == 0),
                            "candidate_nonzero_padding",
                        )?;
                    }
                }
                segments.push(Segment {
                    name,
                    offset: position,
                    vmaddr,
                    vmsize,
                    fileoff,
                    filesize,
                    nsects,
                });
            }
            0x1d => {
                need(
                    signed && signature.is_none() && index == ncmds - 1 && size == 16,
                    "signature_command_shape",
                )?;
                signature = Some(Signature {
                    command_offset: position,
                    dataoff: word(data, position + 8)?,
                    datasize: word(data, position + 12)?,
                });
            }
            0x32 => {
                need(
                    !singleton.contains(&command) && size >= 24,
                    "build_version_shape",
                )?;
                singleton.insert(command);
                let platform = word(data, position + 8)?;
                let ntools = word(data, position + 20)?;
                need(
                    matches!(platform, 2 | 7) && size == 24 + ntools * 8,
                    "unsupported_platform",
                )?;
            }
            0xc | 0xd => {
                need(size >= 32, "dylib_command_shape")?;
                let name_offset = word(data, position + 8)?;
                need(
                    (24..size).contains(&name_offset)
                        && bytes(data, position + name_offset, size - name_offset)?.contains(&0),
                    "dylib_name_bounds",
                )?;
                if command == 0xd {
                    need(singleton.insert(command), "duplicate_load_command")?;
                }
            }
            _ => {
                let expected = match command {
                    0x2 => 24,
                    0xb => 80,
                    0x1b => 24,
                    0x2a => 16,
                    0x2c => 24,
                    0x8000_0022 => 48,
                    0x26 | 0x29 | 0x2e | 0x8000_0033 | 0x8000_0034 => 16,
                    _ => return Err("unrecognized_load_command"),
                };
                need(
                    size == expected && !singleton.contains(&command),
                    "fixed_load_command_shape",
                )?;
                singleton.insert(command);
                match command {
                    0x26 | 0x29 | 0x2e | 0x8000_0033 | 0x8000_0034 => {
                        link_data.push((word(data, position + 8)?, word(data, position + 12)?));
                    }
                    0x2 => {
                        let symbols = word(data, position + 8)?;
                        let count = word(data, position + 12)?;
                        let strings = word(data, position + 16)?;
                        let string_bytes = word(data, position + 20)?;
                        symbol_count = Some(count);
                        link_data.extend([(symbols, count * 16), (strings, string_bytes)]);
                    }
                    0xb => {
                        let mut values = [0u64; 18];
                        for (i, value) in values.iter_mut().enumerate() {
                            *value = word(data, position + 8 + i as u64 * 4)?;
                        }
                        for (pair, entry_size) in
                            [(6, 8), (8, 56), (10, 4), (12, 4), (14, 8), (16, 8)]
                        {
                            link_data.push((values[pair], values[pair + 1] * entry_size));
                        }
                        dynamic_symbols = Some(values);
                    }
                    0x8000_0022 => {
                        for pair in 0..5 {
                            link_data.push((
                                word(data, position + 8 + pair * 8)?,
                                word(data, position + 12 + pair * 8)?,
                            ));
                        }
                    }
                    0x2c => {
                        let cryptoff = word(data, position + 8)?;
                        let cryptsize = word(data, position + 12)?;
                        need(
                            word(data, position + 16)? == 0 && word(data, position + 20)? == 0,
                            "encrypted_image_unsupported",
                        )?;
                        encryption = Some((cryptoff, cryptsize));
                    }
                    _ => {}
                }
            }
        }
        position += size;
    }
    need(position == end, "load_command_count")?;
    need(
        singleton.contains(&0x32) && singleton.contains(&0xd),
        "required_load_command",
    )?;
    need(
        !segments.is_empty()
            && segments[0].name == b"__TEXT"
            && segments.last().unwrap().name == b"__LINKEDIT",
        "segment_order",
    )?;
    let text = &segments[0];
    let linkedit = segments.last().unwrap();
    need(
        text.fileoff == 0 && first_data.is_some(),
        "text_header_layout",
    )?;
    let first_data = first_data.unwrap();
    need(
        end <= first_data && first_data <= text.filesize,
        "header_padding_bounds",
    )?;
    need(
        bytes(data, end, first_data - end)?
            .iter()
            .all(|&byte| byte == 0),
        "nonzero_header_padding",
    )?;
    if let Some((cryptoff, cryptsize)) = encryption {
        need(
            (cryptsize == 0 && cryptoff == 0)
                || (cryptsize > 0
                    && first_data <= cryptoff
                    && cryptoff + cryptsize <= text.filesize),
            "unencrypted_range_must_be_text_payload",
        )?;
    }
    need(
        linkedit.nsects == 0
            && linkedit.fileoff > 0
            && linkedit.fileoff + linkedit.filesize == data.len() as u64,
        "linkedit_not_terminal",
    )?;
    for pair in segments.windows(2) {
        need(
            u128::from(pair[0].vmaddr) + u128::from(pair[0].vmsize) <= u128::from(pair[1].vmaddr),
            "segment_overlap_or_order",
        )?;
    }
    let mut previous = None::<&Segment>;
    for segment in segments.iter().filter(|segment| segment.filesize != 0) {
        if let Some(left) = previous {
            need(
                left.fileoff + left.filesize <= segment.fileoff,
                "segment_overlap_or_order",
            )?;
        }
        previous = Some(segment);
    }
    let payload_end = signature
        .as_ref()
        .map_or(data.len() as u64, |value| value.dataoff);
    for (offset, size) in link_data {
        if size != 0 {
            // Both fields are at most u32-derived count*56, so this fits u64.
            need(
                linkedit.fileoff <= offset && offset + size <= payload_end,
                "linkedit_data_bounds",
            )?;
        }
    }
    if let Some(values) = dynamic_symbols {
        let count = symbol_count.ok_or("dysymtab_without_symtab")?;
        for pair in [0, 2, 4] {
            need(
                values[pair] + values[pair + 1] <= count,
                "dysymtab_symbol_bounds",
            )?;
        }
    }
    need(signature.is_some() == signed, "signature_presence")?;
    Ok(Image {
        ncmds,
        sizeofcmds,
        first_data,
        linkedit: LinkEdit {
            command_offset: linkedit.offset,
            vmaddr: linkedit.vmaddr,
            vmsize: linkedit.vmsize,
            fileoff: linkedit.fileoff,
            filesize: linkedit.filesize,
        },
        signature,
    })
}
