//! Bounded metadata reader for the deliberately narrow Apple SDK fixture.
//!
//! Only thin little-endian ARM64_ALL MH_DYLIB images for iOS/device or
//! iOS/Simulator are accepted. This is not a dyld implementation, signature
//! authenticator, normalization policy, or permission to execute an image.
//! The caller must separately bind the complete file hash/identity and actual
//! dyld image, retain its handle, and compare loaded headers to these bytes.
//! Layout constants follow Apple's public mach-o/loader.h, not a private image.

use std::collections::BTreeSet;

type Check<T> = Result<T, String>;
const MAX_FILE: usize = 1 << 30;
const MAX_COMMANDS: u32 = 512;
const MAX_SECTIONS: usize = 4096;
const READ: u32 = 1;
const WRITE: u32 = 2;
const EXECUTE: u32 = 4;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct FileRange {
    pub fileoff: u64,
    pub filesize: u64,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Segment {
    pub name: String,
    pub vmaddr: u64,
    pub vmsize: u64,
    pub fileoff: u64,
    pub filesize: u64,
    pub initprot: u32,
}

#[derive(Clone, Debug)]
pub struct Image {
    pub uuid: [u8; 16],
    pub platform: u32,
    pub min_os: u32,
    pub header_vmaddr: u64,
    pub header_and_commands_size: usize,
    pub segments: Vec<Segment>,
    /// Opaque bounded bytes. Presence does not authenticate any signature.
    pub code_signature: Option<FileRange>,
    pub file_size: usize,
}

impl Image {
    /// Checks one contiguous file-backed segment using a checked signed slide.
    /// This does not query VM mappings: actual dyld/VM identity is a caller gate.
    /// Executable APIs must be RX and not writable. Read-only tables must be R
    /// and not W. Those two uses also reject header/load-command byte aliases.
    pub fn contains_loaded(
        &self,
        header_base: usize,
        pointer: usize,
        bytes: usize,
        require_exec: bool,
        require_readonly: bool,
    ) -> bool {
        if header_base == 0 || pointer == 0 || bytes == 0 || bytes > isize::MAX as usize {
            return false;
        }
        let Some(query_end) = pointer.checked_add(bytes) else {
            return false;
        };
        let slide = header_base as i128 - i128::from(self.header_vmaddr);
        self.segments.iter().any(|segment| {
            if segment.initprot & READ == 0
                || (require_exec && segment.initprot & (READ | WRITE | EXECUTE) != READ | EXECUTE)
                || (require_readonly && segment.initprot & WRITE != 0)
            {
                return false;
            }
            let start = i128::from(segment.vmaddr) + slide;
            let end = start + i128::from(segment.filesize);
            if start < 0 || end > usize::MAX as i128 || end <= start {
                return false;
            }
            if require_exec || require_readonly {
                let Some(header_end) = header_base.checked_add(self.header_and_commands_size)
                else {
                    return false;
                };
                if pointer < header_end && query_end > header_base {
                    return false;
                }
            }
            pointer >= start as usize && query_end <= end as usize
        })
    }
}

fn need(value: bool, category: &'static str) -> Check<()> {
    if value { Ok(()) } else { Err(category.into()) }
}
fn extent(data: &[u8], offset: u64, bytes: u64) -> Check<&[u8]> {
    let start = usize::try_from(offset).map_err(|_| "file offset overflow")?;
    let len = usize::try_from(bytes).map_err(|_| "file extent overflow")?;
    let end = start.checked_add(len).ok_or("file range overflow")?;
    data.get(start..end)
        .ok_or_else(|| "file range outside input".into())
}
fn u32_at(data: &[u8], at: usize) -> Check<u32> {
    Ok(u32::from_le_bytes(
        extent(data, at as u64, 4)?.try_into().unwrap(),
    ))
}
fn u64_at(data: &[u8], at: usize) -> Check<u64> {
    Ok(u64::from_le_bytes(
        extent(data, at as u64, 8)?.try_into().unwrap(),
    ))
}
fn name(raw: &[u8]) -> Check<String> {
    let end = raw.iter().position(|&byte| byte == 0).unwrap_or(raw.len());
    need(
        end > 0 && raw[..end].iter().all(|byte| (0x21..=0x7e).contains(byte)),
        "unsupported segment/section name",
    )?;
    need(
        raw[end..].iter().all(|&byte| byte == 0),
        "nonzero fixed-name padding",
    )?;
    String::from_utf8(raw[..end].to_vec()).map_err(|_| "invalid name".into())
}
fn command_string(command: &[u8], minimum: usize) -> Check<String> {
    let offset = u32_at(command, 8)? as usize;
    need(
        offset >= minimum && offset < command.len(),
        "load-command string bounds",
    )?;
    need(
        command[minimum..offset].iter().all(|&byte| byte == 0),
        "load-command string prefix padding",
    )?;
    let tail = &command[offset..];
    let end = tail
        .iter()
        .position(|&byte| byte == 0)
        .ok_or("unterminated load-command string")?;
    need(
        end > 0 && tail[end..].iter().all(|&byte| byte == 0),
        "load-command string padding",
    )?;
    std::str::from_utf8(&tail[..end])
        .map(str::to_owned)
        .map_err(|_| "load-command string encoding".into())
}
fn reference(references: &mut Vec<FileRange>, offset: u32, count: u32, stride: u64) -> Check<()> {
    let bytes = u64::from(count)
        .checked_mul(stride)
        .ok_or("metadata count overflow")?;
    if bytes != 0 {
        references.push(FileRange {
            fileoff: u64::from(offset),
            filesize: bytes,
        });
    }
    Ok(())
}
fn fits(offset: u64, bytes: u64, start: u64, end: u64) -> bool {
    offset >= start && offset.checked_add(bytes).is_some_and(|value| value <= end)
}

#[derive(Clone, Copy)]
struct Section {
    address: u64,
    size: u64,
}

/// Parses metadata only. Unknown commands, unsupported architecture/platform,
/// duplicate singleton metadata, ambiguous overlaps and invalid ranges refuse.
/// Relocation/fixup bytecode and signature child bodies are not interpreted;
/// their complete file extents are checked and dyld/full-hash gates remain
/// mandatory. No unsafe dereference, file access or mutation occurs here.
pub fn parse(data: &[u8], expected_platform: u32) -> Check<Image> {
    need(
        matches!(expected_platform, 2 | 7),
        "unsupported expected platform",
    )?;
    need((32..=MAX_FILE).contains(&data.len()), "Mach-O input size")?;
    need(
        u32_at(data, 0)? == 0xfeed_facf,
        "thin little-endian Mach-O required",
    )?;
    need(
        u32_at(data, 4)? == 0x0100_000c && u32_at(data, 8)? == 0,
        "plain ARM64_ALL required",
    )?;
    need(
        u32_at(data, 12)? == 6 && u32_at(data, 28)? == 0,
        "MH_DYLIB/reserved field required",
    )?;
    need(
        u32_at(data, 24)? & 0x8000_0000 == 0,
        "shared-cache image unsupported",
    )?;
    let ncmds = u32_at(data, 16)?;
    let command_bytes = u32_at(data, 20)?;
    need(
        (1..=MAX_COMMANDS).contains(&ncmds) && (8..=65536).contains(&command_bytes),
        "load-command limits",
    )?;
    let end = 32usize
        .checked_add(command_bytes as usize)
        .ok_or("load-command extent overflow")?;
    extent(data, 32, u64::from(command_bytes))?;
    let mut at: usize = 32;
    let mut singleton = BTreeSet::new();
    let mut segment_names = BTreeSet::new();
    let mut dependencies = BTreeSet::new();
    let mut sections = Vec::<Section>::new();
    let mut references = Vec::<FileRange>::new();
    let mut segments = Vec::<Segment>::new();
    let mut uuid = None;
    let mut build = None;
    let mut signature = None;
    let mut symtab = None;
    let mut dysymtab = None;
    let mut encryption = None;
    let mut first_text_data = None;
    let mut indirect_sections = Vec::<(u32, u64)>::new();
    for _ in 0..ncmds {
        need(
            at.checked_add(8).is_some_and(|value| value <= end),
            "load-command header bounds",
        )?;
        let kind = u32_at(data, at)?;
        let size = u32_at(data, at + 4)? as usize;
        need(
            size >= 8 && size % 8 == 0 && size <= end - at,
            "load-command size",
        )?;
        let command = &data[at..at + size];
        match kind {
            0x19 => {
                need(size >= 72 && segments.len() < 64, "segment limits")?;
                let segment_name = name(&command[8..24])?;
                need(
                    segment_names.insert(segment_name.clone()),
                    "duplicate segment",
                )?;
                let vmaddr = u64_at(command, 24)?;
                let vmsize = u64_at(command, 32)?;
                let fileoff = u64_at(command, 40)?;
                let filesize = u64_at(command, 48)?;
                let maxprot = u32_at(command, 56)?;
                let initprot = u32_at(command, 60)?;
                let nsects = u32_at(command, 64)? as usize;
                need(
                    nsects <= MAX_SECTIONS - sections.len() && size == 72 + 80 * nsects,
                    "segment section count",
                )?;
                need(
                    vmsize >= filesize && vmaddr.checked_add(vmsize).is_some(),
                    "segment VM extent",
                )?;
                need(
                    maxprot & !7 == 0 && initprot & !7 == 0 && initprot & !maxprot == 0,
                    "segment protections",
                )?;
                need(
                    initprot & (WRITE | EXECUTE) != WRITE | EXECUTE,
                    "writable executable segment",
                )?;
                // SG_HIGHVM and other layout-affecting segment flags require a
                // different mapping interpretation and are intentionally refused.
                need(
                    u32_at(command, 68)? & !0x10 == 0,
                    "unsupported segment flags",
                )?;
                extent(data, fileoff, filesize)?;
                let mut section_ranges = Vec::new();
                for number in 0..nsects {
                    let s = 72 + number * 80;
                    name(&command[s..s + 16])?;
                    need(
                        name(&command[s + 16..s + 32])? == segment_name,
                        "section owner mismatch",
                    )?;
                    let address = u64_at(command, s + 32)?;
                    let bytes = u64_at(command, s + 40)?;
                    let offset = u32_at(command, s + 48)?;
                    let exponent = u32_at(command, s + 52)?;
                    let flags = u32_at(command, s + 64)?;
                    let section_type = flags & 0xff;
                    need(
                        exponent <= 31 && section_type <= 0x16,
                        "unsupported section alignment/type",
                    )?;
                    need(
                        address % (1u64 << exponent) == 0
                            && fits(address, bytes, vmaddr, vmaddr + vmsize),
                        "section VM bounds/alignment",
                    )?;
                    reference(
                        &mut references,
                        u32_at(command, s + 56)?,
                        u32_at(command, s + 60)?,
                        8,
                    )?;
                    if bytes != 0 {
                        section_ranges.push((address, address + bytes));
                        if matches!(section_type, 1 | 0xc | 0x12) {
                            need(
                                offset == 0 && address >= vmaddr + filesize,
                                "zero-fill aliases file-backed bytes",
                            )?;
                        } else {
                            need(
                                fits(u64::from(offset), bytes, fileoff, fileoff + filesize),
                                "section file bounds",
                            )?;
                            need(
                                address - vmaddr == u64::from(offset) - fileoff,
                                "section file/VM mapping",
                            )?;
                            if segment_name == "__TEXT" {
                                first_text_data =
                                    Some(first_text_data.map_or(u64::from(offset), |old: u64| {
                                        old.min(u64::from(offset))
                                    }));
                            }
                        }
                    }
                    if flags & (0x8000_0000 | 0x400) != 0 {
                        need(
                            initprot & (READ | WRITE | EXECUTE) == READ | EXECUTE,
                            "instruction section is not RX",
                        )?;
                    }
                    if matches!(section_type, 6 | 7 | 8 | 0x10 | 0x14) && bytes != 0 {
                        let stride = if section_type == 8 {
                            u64::from(u32_at(command, s + 72)?)
                        } else {
                            8
                        };
                        need(
                            stride != 0 && bytes % stride == 0,
                            "indirect section stride",
                        )?;
                        indirect_sections.push((u32_at(command, s + 68)?, bytes / stride));
                    }
                    sections.push(Section {
                        address,
                        size: bytes,
                    });
                }
                section_ranges.sort_unstable();
                need(
                    section_ranges.windows(2).all(|p| p[0].1 <= p[1].0),
                    "overlapping sections",
                )?;
                need(
                    segment_name != "__LINKEDIT" || nsects == 0,
                    "LINKEDIT sections unsupported",
                )?;
                segments.push(Segment {
                    name: segment_name,
                    vmaddr,
                    vmsize,
                    fileoff,
                    filesize,
                    initprot,
                });
            }
            0xc | 0xd | 0x8000_0018 | 0x8000_001f | 0x20 | 0x8000_0023 => {
                need(size >= 32, "dylib command size")?;
                let path = command_string(command, 24)?;
                if kind == 0xd {
                    need(singleton.insert(kind), "duplicate dylib identity")?;
                } else {
                    need(dependencies.insert(path), "duplicate dylib dependency")?;
                }
            }
            0x8000_001c => {
                need(size >= 16, "rpath command size")?;
                command_string(command, 12)?;
            }
            _ => {
                need(singleton.insert(kind), "duplicate singleton command")?;
                match kind {
                    0x1b => {
                        need(size == 24, "UUID command size")?;
                        let value: [u8; 16] = command[8..24].try_into().unwrap();
                        need(value.iter().any(|&byte| byte != 0), "empty UUID")?;
                        uuid = Some(value);
                    }
                    0x32 => {
                        need(size >= 24, "build-version command size")?;
                        let tools = u32_at(command, 20)? as usize;
                        need(tools <= 64 && size == 24 + tools * 8, "build-tool extent")?;
                        let platform = u32_at(command, 8)?;
                        need(platform == expected_platform, "build platform mismatch")?;
                        build = Some((platform, u32_at(command, 12)?));
                    }
                    0x2a => {
                        need(size == 16, "source-version command size")?;
                    }
                    0x2c => {
                        need(
                            size == 24 && u32_at(command, 16)? == 0 && u32_at(command, 20)? == 0,
                            "encrypted image unsupported",
                        )?;
                        encryption = Some((u32_at(command, 8)?, u32_at(command, 12)?));
                    }
                    0x2 => {
                        need(size == 24, "symbol table command size")?;
                        let values = [
                            u32_at(command, 8)?,
                            u32_at(command, 12)?,
                            u32_at(command, 16)?,
                            u32_at(command, 20)?,
                        ];
                        need(values[1] <= 1_000_000, "symbol count limit")?;
                        reference(&mut references, values[0], values[1], 16)?;
                        reference(&mut references, values[2], values[3], 1)?;
                        symtab = Some(values);
                    }
                    0xb => {
                        need(size == 80, "dynamic-symbol table command size")?;
                        let mut values = [0u32; 18];
                        for (i, value) in values.iter_mut().enumerate() {
                            *value = u32_at(command, 8 + i * 4)?;
                        }
                        for (pair, stride) in [(6, 8), (8, 56), (10, 4), (12, 4), (14, 8), (16, 8)]
                        {
                            reference(&mut references, values[pair], values[pair + 1], stride)?;
                        }
                        dysymtab = Some(values);
                    }
                    0x22 | 0x8000_0022 => {
                        need(
                            size == 48
                                && !(singleton.contains(&0x22) && singleton.contains(&0x8000_0022)),
                            "dyld-info command shape",
                        )?;
                        for pair in 0..5 {
                            reference(
                                &mut references,
                                u32_at(command, 8 + pair * 8)?,
                                u32_at(command, 12 + pair * 8)?,
                                1,
                            )?;
                        }
                    }
                    0x1d | 0x1e | 0x26 | 0x29 | 0x2b | 0x2e | 0x8000_0033 | 0x8000_0034 => {
                        need(size == 16, "linkedit-data command size")?;
                        let offset = u32_at(command, 8)?;
                        let bytes = u32_at(command, 12)?;
                        if kind == 0x1d {
                            need(
                                bytes >= 12 && bytes <= 16 << 20 && offset % 16 == 0,
                                "code-signature extent",
                            )?;
                            signature = Some(FileRange {
                                fileoff: u64::from(offset),
                                filesize: u64::from(bytes),
                            });
                        } else {
                            reference(&mut references, offset, bytes, 1)?;
                        }
                    }
                    _ => return Err("unsupported load command".into()),
                }
            }
        }
        at += size;
    }
    need(at == end, "load-command count/size mismatch")?;
    need(singleton.contains(&0xd), "missing dylib identity")?;
    let uuid = uuid.ok_or("missing UUID")?;
    let (platform, min_os) = build.ok_or("missing build version")?;
    need(
        segments.len() >= 2
            && segments[0].name == "__TEXT"
            && segments.last().unwrap().name == "__LINKEDIT",
        "segment order",
    )?;
    let text = &segments[0];
    let link = segments.last().unwrap();
    need(
        text.fileoff == 0
            && text.filesize >= end as u64
            && text.initprot & (READ | WRITE | EXECUTE) == READ | EXECUTE,
        "header TEXT mapping",
    )?;
    let first_data = first_text_data.ok_or("missing file-backed TEXT section")?;
    need(
        first_data >= end as u64 && first_data <= text.filesize,
        "TEXT section aliases header",
    )?;
    need(
        link.fileoff > 0
            && link.fileoff + link.filesize == data.len() as u64
            && link.initprot == READ,
        "terminal readonly LINKEDIT required",
    )?;
    for pair in segments.windows(2) {
        need(
            pair[0].vmaddr + pair[0].vmsize <= pair[1].vmaddr,
            "segment VM overlap/order",
        )?;
    }
    let backed: Vec<_> = segments
        .iter()
        .filter(|segment| segment.filesize != 0)
        .collect();
    for pair in backed.windows(2) {
        need(
            pair[0].fileoff + pair[0].filesize <= pair[1].fileoff,
            "segment file overlap/order",
        )?;
    }
    let metadata_end = signature
        .as_ref()
        .map_or(data.len() as u64, |value| value.fileoff);
    if let Some(value) = &signature {
        need(
            fits(
                value.fileoff,
                value.filesize,
                link.fileoff,
                data.len() as u64,
            ) && value.fileoff + value.filesize == data.len() as u64,
            "signature outside terminal LINKEDIT",
        )?;
    }
    for value in &references {
        need(
            fits(value.fileoff, value.filesize, link.fileoff, metadata_end),
            "metadata outside LINKEDIT payload",
        )?;
        extent(data, value.fileoff, value.filesize)?;
    }
    if let Some((offset, bytes)) = encryption {
        need(
            (offset == 0 && bytes == 0)
                || (bytes > 0
                    && fits(
                        u64::from(offset),
                        u64::from(bytes),
                        first_data,
                        text.filesize,
                    )),
            "inactive encryption range aliases header/outside TEXT",
        )?;
    }
    if let Some(values) = symtab {
        let [symbols, count, strings, string_bytes] = values;
        let pool = extent(data, u64::from(strings), u64::from(string_bytes))?;
        let last_nul = pool.iter().rposition(|&byte| byte == 0);
        for i in 0..count {
            let nlist = extent(data, u64::from(symbols) + u64::from(i) * 16, 16)?;
            let index = u32_at(nlist, 0)? as usize;
            need(
                last_nul.is_some_and(|last| index <= last),
                "symbol name outside terminated string table",
            )?;
            if nlist[4] & 0xe0 == 0 {
                match nlist[4] & 0xe {
                    0 | 2 => {
                        need(nlist[5] == 0, "nonsection symbol has section index")?;
                    }
                    0xe => {
                        let section = nlist[5] as usize;
                        need(
                            section > 0 && section <= sections.len(),
                            "symbol section index",
                        )?;
                        let owner = sections[section - 1];
                        need(
                            fits(
                                u64_at(nlist, 8)?,
                                0,
                                owner.address,
                                owner.address + owner.size,
                            ),
                            "section symbol VM extent",
                        )?;
                    }
                    0xa => {
                        need(
                            last_nul.is_some_and(|last| {
                                u64_at(nlist, 8).is_ok_and(|v| v <= last as u64)
                            }),
                            "indirect symbol name extent",
                        )?;
                    }
                    _ => return Err("unsupported symbol kind".into()),
                }
            }
        }
    }
    if let Some(values) = dysymtab {
        let count = symtab.ok_or("DYSYMTAB without SYMTAB")?[1];
        let mut groups = Vec::new();
        for pair in [0, 2, 4] {
            let start = u64::from(values[pair]);
            let end = start + u64::from(values[pair + 1]);
            need(end <= u64::from(count), "dynamic symbol index extent")?;
            if start != end {
                groups.push((start, end));
            }
        }
        groups.sort_unstable();
        need(
            groups.windows(2).all(|pair| pair[0].1 <= pair[1].0),
            "overlapping dynamic symbol groups",
        )?;
        for (first, entries) in indirect_sections {
            need(
                u64::from(first)
                    .checked_add(entries)
                    .is_some_and(|value| value <= u64::from(values[13])),
                "indirect section table extent",
            )?;
        }
        for i in 0..values[13] {
            let index = u32_at(
                extent(data, u64::from(values[12]) + u64::from(i) * 4, 4)?,
                0,
            )?;
            need(
                index & 0xc000_0000 != 0 || index < count,
                "indirect symbol index extent",
            )?;
        }
    } else {
        need(
            indirect_sections.is_empty(),
            "indirect sections without DYSYMTAB",
        )?;
    }
    Ok(Image {
        uuid,
        platform,
        min_os,
        header_vmaddr: text.vmaddr,
        header_and_commands_size: end,
        segments,
        code_signature: signature,
        file_size: data.len(),
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    fn put32(bytes: &mut [u8], at: usize, value: u32) {
        bytes[at..at + 4].copy_from_slice(&value.to_le_bytes());
    }
    fn put64(bytes: &mut [u8], at: usize, value: u64) {
        bytes[at..at + 8].copy_from_slice(&value.to_le_bytes());
    }
    fn segment(name: &str, vm: u64, off: u64, len: u64, prot: u32, section: bool) -> Vec<u8> {
        let mut result = vec![0u8; 72 + if section { 80 } else { 0 }];
        let size = result.len() as u32;
        put32(&mut result, 0, 0x19);
        put32(&mut result, 4, size);
        result[8..8 + name.len()].copy_from_slice(name.as_bytes());
        put64(&mut result, 24, vm);
        put64(&mut result, 32, 0x1000);
        put64(&mut result, 40, off);
        put64(&mut result, 48, len);
        put32(&mut result, 56, prot);
        put32(&mut result, 60, prot);
        if section {
            put32(&mut result, 64, 1);
            result[72..78].copy_from_slice(b"__text");
            result[88..94].copy_from_slice(b"__TEXT");
            put64(&mut result, 104, vm + 0x400);
            put64(&mut result, 112, 0x100);
            put32(&mut result, 120, 0x400);
            put32(&mut result, 124, 2);
            put32(&mut result, 136, 0x8000_0400);
        }
        result
    }
    fn fixture(extra: Vec<Vec<u8>>) -> Vec<u8> {
        let mut commands = vec![
            segment("__TEXT", 0x1000_0000, 0, 0x1000, 5, true),
            segment("__LINKEDIT", 0x1000_1000, 0x1000, 0x100, 1, false),
        ];
        let mut uuid = vec![0; 24];
        put32(&mut uuid, 0, 0x1b);
        put32(&mut uuid, 4, 24);
        uuid[8..].copy_from_slice(&[1; 16]);
        commands.push(uuid);
        let mut version = vec![0; 24];
        put32(&mut version, 0, 0x32);
        put32(&mut version, 4, 24);
        put32(&mut version, 8, 7);
        put32(&mut version, 12, 26 << 16);
        commands.push(version);
        let mut identity = vec![0; 56];
        put32(&mut identity, 0, 0xd);
        put32(&mut identity, 4, 56);
        put32(&mut identity, 8, 24);
        let id = b"@rpath/Public.dylib";
        identity[24..24 + id.len()].copy_from_slice(id);
        commands.push(identity);
        commands.extend(extra);
        let mut data = vec![0; 0x1100];
        put32(&mut data, 0, 0xfeed_facf);
        put32(&mut data, 4, 0x0100_000c);
        put32(&mut data, 12, 6);
        put32(&mut data, 16, commands.len() as u32);
        put32(
            &mut data,
            20,
            commands.iter().map(Vec::len).sum::<usize>() as u32,
        );
        let mut at = 32;
        for command in commands {
            data[at..at + command.len()].copy_from_slice(&command);
            at += command.len();
        }
        data
    }
    #[test]
    fn valid_image_and_loaded_ranges_require_file_backing_and_protection() {
        let image = parse(&fixture(vec![]), 7).unwrap();
        assert_eq!(
            (image.platform, image.min_os, image.header_and_commands_size),
            (7, 26 << 16, 360)
        );
        assert!(image.contains_loaded(0x100000, 0x100400, 0x100, true, false));
        assert!(image.contains_loaded(0x100000, 0x101000, 16, false, true));
        assert!(!image.contains_loaded(0x100000, 0x100020, 4, true, false));
        assert!(!image.contains_loaded(0x100000, 0x100ff0, 32, false, true));
        assert!(!image.contains_loaded(0x100000, 0x101100, 1, false, true));
        assert!(!image.contains_loaded(usize::MAX - 16, usize::MAX - 4, 8, true, false));
        assert!(!image.contains_loaded(0x100000, 0x100400, 0, true, false));
        let mut writable = image.clone();
        writable.segments[0].initprot = 7;
        assert!(!writable.contains_loaded(0x100000, 0x100400, 4, true, false));
        assert!(!writable.contains_loaded(0x100000, 0x100400, 4, false, true));
    }
    #[test]
    fn architecture_platform_and_duplicate_metadata_fail_closed() {
        let original = fixture(vec![]);
        for (offset, value) in [(0, 0xcafe_babe), (4, 0x0100_0007), (8, 2), (12, 2), (28, 1)] {
            let mut bad = original.clone();
            put32(&mut bad, offset, value);
            assert!(parse(&bad, 7).is_err());
        }
        assert!(parse(&original, 2).is_err());
        assert!(parse(&original, 1).is_err());
        let uuid = original[256..280].to_vec();
        assert!(parse(&fixture(vec![uuid]), 7).is_err());
    }
    #[test]
    fn all_truncations_and_bad_command_extents_refuse_without_panicking() {
        let original = fixture(vec![]);
        for end in 0..original.len() {
            assert!(parse(&original[..end], 7).is_err());
        }
        for (offset, value) in [
            (16, 0),
            (16, 513),
            (20, u32::MAX),
            (36, 0),
            (36, 73),
            (36, u32::MAX),
        ] {
            let mut bad = original.clone();
            put32(&mut bad, offset, value);
            assert!(parse(&bad, 7).is_err());
        }
        let mut unknown = vec![0; 8];
        put32(&mut unknown, 0, 0x8000_ffff);
        put32(&mut unknown, 4, 8);
        assert!(parse(&fixture(vec![unknown]), 7).is_err());
    }
    #[test]
    fn section_alias_overlap_overflow_and_zero_fill_header_refuse() {
        for (offset, value) in [
            (56, u64::MAX),
            (64, u64::MAX),
            (104 + 32, 0),
            (104 + 40, u64::MAX),
        ] {
            let mut bad = fixture(vec![]);
            put64(&mut bad, offset, value);
            assert!(parse(&bad, 7).is_err());
        }
        let mut zero = fixture(vec![]);
        put32(&mut zero, 32 + 72 + 64, 1);
        put32(&mut zero, 32 + 72 + 48, 0);
        assert!(parse(&zero, 7).is_err());
        let mut owner = fixture(vec![]);
        owner[32 + 72 + 16] ^= 1;
        assert!(parse(&owner, 7).is_err());
    }
    #[test]
    fn linkedit_tables_and_symbol_indices_cannot_reference_headers_or_escape() {
        let mut symbols = vec![0; 24];
        put32(&mut symbols, 0, 2);
        put32(&mut symbols, 4, 24);
        put32(&mut symbols, 8, 0x1000);
        put32(&mut symbols, 12, 1);
        put32(&mut symbols, 16, 0x1010);
        put32(&mut symbols, 20, 16);
        let mut data = fixture(vec![symbols.clone()]);
        data[0x1000 + 4] = 0xf;
        data[0x1000 + 5] = 1;
        put64(&mut data, 0x1008, 0x1000_0400);
        assert!(parse(&data, 7).is_ok());
        for (field, value) in [
            (8, 32),
            (8, 0x10ff),
            (12, u32::MAX),
            (16, 0x1100),
            (20, u32::MAX),
        ] {
            let mut bad = symbols.clone();
            put32(&mut bad, field, value);
            assert!(parse(&fixture(vec![bad]), 7).is_err());
        }
        let mut bad = data.clone();
        put32(&mut bad, 0x1000, 16);
        assert!(parse(&bad, 7).is_err());
        let mut dynamic = vec![0; 80];
        put32(&mut dynamic, 0, 0xb);
        put32(&mut dynamic, 4, 80);
        put32(&mut dynamic, 12, 2);
        assert!(parse(&fixture(vec![symbols, dynamic]), 7).is_err());
    }
    #[test]
    fn code_signature_is_only_an_opaque_terminal_bounded_range() {
        let mut signature = vec![0; 16];
        put32(&mut signature, 0, 0x1d);
        put32(&mut signature, 4, 16);
        put32(&mut signature, 8, 0x10f0);
        put32(&mut signature, 12, 16);
        let data = fixture(vec![signature.clone()]);
        let image = parse(&data, 7).unwrap();
        assert_eq!(
            image.code_signature,
            Some(FileRange {
                fileoff: 0x10f0,
                filesize: 16
            })
        );
        for (field, value) in [(8, 32), (8, 0x10f1), (12, 32), (12, 0)] {
            let mut bad = signature.clone();
            put32(&mut bad, field, value);
            assert!(parse(&fixture(vec![bad]), 7).is_err());
        }
        let mut refs = vec![0; 16];
        put32(&mut refs, 0, 0x26);
        put32(&mut refs, 4, 16);
        put32(&mut refs, 8, 0x10ef);
        put32(&mut refs, 12, 2);
        assert!(parse(&fixture(vec![signature, refs]), 7).is_err());
    }
    #[test]
    fn zero_file_size_data_segment_has_only_unbacked_vm_tail() {
        let mut data = fixture(vec![]);
        let original = data[32..360].to_vec();
        let mut bss = segment("__DATA", 0x1000_1000, 0, 0, 3, true);
        bss[88..104].fill(0);
        bss[88..94].copy_from_slice(b"__DATA");
        put64(&mut bss, 104, 0x1000_1800);
        put32(&mut bss, 120, 0);
        put32(&mut bss, 136, 1);
        let mut commands = original[..152].to_vec();
        commands.extend_from_slice(&bss);
        commands.extend_from_slice(&original[152..]);
        put64(&mut commands, 152 + bss.len() + 24, 0x1000_2000);
        data[32..32 + commands.len()].copy_from_slice(&commands);
        put32(&mut data, 16, 6);
        put32(&mut data, 20, commands.len() as u32);
        let image = parse(&data, 7).unwrap();
        assert_eq!(
            (image.segments[1].fileoff, image.segments[1].filesize),
            (0, 0)
        );
        assert!(!image.contains_loaded(0x100000, 0x101800, 4, false, false));
        put64(&mut data, 32 + 152 + 24, 0x1000_0800);
        assert!(parse(&data, 7).is_err());
    }
    #[test]
    fn every_supported_metadata_pair_is_bounded_to_linkedit_and_not_header() {
        let mut dyld = vec![0; 48];
        put32(&mut dyld, 0, 0x8000_0022);
        put32(&mut dyld, 4, 48);
        for pair in 0..5 {
            put32(&mut dyld, 8 + pair * 8, 0x1000 + pair as u32 * 4);
            put32(&mut dyld, 12 + pair * 8, 4);
        }
        assert!(parse(&fixture(vec![dyld.clone()]), 7).is_ok());
        for pair in 0..5 {
            let mut bad = dyld.clone();
            put32(&mut bad, 8 + pair * 8, 32);
            assert!(parse(&fixture(vec![bad]), 7).is_err());
        }
        for pair in [6, 8, 10, 12, 14, 16] {
            let mut symbols = vec![0; 24];
            put32(&mut symbols, 0, 2);
            put32(&mut symbols, 4, 24);
            let mut dynamic = vec![0; 80];
            put32(&mut dynamic, 0, 0xb);
            put32(&mut dynamic, 4, 80);
            put32(&mut dynamic, 8 + pair * 4, 32);
            put32(&mut dynamic, 12 + pair * 4, 1);
            assert!(parse(&fixture(vec![symbols, dynamic]), 7).is_err());
        }
        let mut reloc = fixture(vec![]);
        put32(&mut reloc, 32 + 72 + 56, 32);
        put32(&mut reloc, 32 + 72 + 60, 1);
        assert!(parse(&reloc, 7).is_err());
        let mut device = fixture(vec![]);
        put32(&mut device, 280 + 8, 2);
        assert!(parse(&device, 2).is_ok());
    }
    #[test]
    fn single_byte_mutations_do_not_panic_or_dereference_untrusted_data() {
        let original = fixture(vec![]);
        for i in 0..original.len() {
            let mut changed = original.clone();
            changed[i] ^= 0xff;
            assert!(std::panic::catch_unwind(|| parse(&changed, 7)).is_ok());
        }
    }
}
