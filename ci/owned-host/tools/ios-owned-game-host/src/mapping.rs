//! Additional loader mapping policy, AFTER complete candidate content binding.
//! The signed-image-policy parser remains responsible for all file/section/
//! metadata bounds and normalization. This layer records checked protections,
//! platform and SDK dyld ranges; it is not a replacement signing verifier.
use crate::Check;
pub use ios_resident_apple_backend::macho::{FileRange, Image, Segment};
fn word(d: &[u8], p: usize) -> Check<u32> {
    Ok(u32::from_le_bytes(
        d.get(p..p.checked_add(4).ok_or("mapping overflow")?)
            .ok_or("mapping bounds")?
            .try_into()
            .unwrap(),
    ))
}
fn wide(d: &[u8], p: usize) -> Check<u64> {
    Ok(u64::from_le_bytes(
        d.get(p..p.checked_add(8).ok_or("mapping overflow")?)
            .ok_or("mapping bounds")?
            .try_into()
            .unwrap(),
    ))
}
fn name(d: &[u8]) -> Check<String> {
    let end = d.iter().position(|v| *v == 0).unwrap_or(d.len());
    if end == 0
        || !d[..end].iter().all(|b| (0x21..=0x7e).contains(b))
        || d[end..].iter().any(|b| *b != 0)
    {
        return Err("mapping name");
    }
    String::from_utf8(d[..end].to_vec()).map_err(|_| "mapping name")
}
pub fn parse_after_content_binding(d: &[u8], platform: u32) -> Check<Image> {
    if !matches!(platform, 2 | 7) {
        return Err("unsupported Apple platform");
    }
    // Structural acceptance includes the explicit __DATA zero-padding rule.
    let structural = ios_signed_image_policy::macho::parse_lld_candidate(d, true)?;
    let end = 32usize
        .checked_add(structural.sizeofcmds as usize)
        .ok_or("mapping commands overflow")?;
    let mut at = 32;
    let mut segments = Vec::new();
    let mut uuid = None;
    let mut build = None;
    let mut system_loads = 0;
    for _ in 0..structural.ncmds {
        let kind = word(d, at)?;
        let size = word(d, at + 4)? as usize;
        let c = d
            .get(at..at.checked_add(size).ok_or("mapping command overflow")?)
            .ok_or("mapping command bounds")?;
        match kind {
            0x19 => {
                let segment_name = name(&c[8..24])?;
                let (vmaddr, vmsize, fileoff, filesize) =
                    (wide(c, 24)?, wide(c, 32)?, wide(c, 40)?, wide(c, 48)?);
                let (maxprot, initprot) = (word(c, 56)?, word(c, 60)?);
                if maxprot & !7 != 0
                    || initprot & !7 != 0
                    || initprot & !maxprot != 0
                    || initprot & 6 == 6
                    || word(c, 68)? & !0x10 != 0
                    || vmaddr.checked_add(vmsize).is_none()
                {
                    return Err("unsupported segment mapping or protections");
                }
                let n = word(c, 64)? as usize;
                for i in 0..n {
                    let s = 72 + i * 80;
                    let kind = word(c, s + 64)? & 0xff;
                    // No image constructors/destructors, TLS runtime, interpose
                    // or initializer offsets may run before host binding.
                    if matches!(kind, 9 | 10 | 13 | 0x11..=0x16) {
                        return Err("early image initialization unsupported");
                    }
                    if word(c, s + 64)? & (0x80000000 | 0x400) != 0 && initprot != 5 {
                        return Err("instruction section is not RX");
                    }
                }
                segments.push(Segment {
                    name: segment_name,
                    vmaddr,
                    vmsize,
                    fileoff,
                    filesize,
                    initprot,
                });
            }
            0x1b => {
                if uuid.is_some() || c.len() != 24 {
                    return Err("mapping UUID");
                }
                uuid = Some(c[8..24].try_into().unwrap());
            }
            0x32 => {
                if build.is_some() || c.len() < 24 || word(c, 8)? != platform {
                    return Err("mapping platform");
                }
                build = Some(word(c, 12)?);
            }
            0xc => {
                let p = word(c, 8)? as usize;
                let tail = c.get(p..).ok_or("dependency name bounds")?;
                let z = tail
                    .iter()
                    .position(|b| *b == 0)
                    .ok_or("dependency name terminator")?;
                if &tail[..z] != b"/usr/lib/libSystem.B.dylib"
                    || word(c, 16)? < 0x10000
                    || word(c, 20)? < 0x10000
                {
                    return Err("unexpected guest system dependency");
                }
                system_loads += 1;
            }
            0x80000018 | 0x8000001f | 0x20 | 0x80000023 | 0x80000028 => {
                return Err("unsupported weak/reexport/early entry dependency");
            }
            _ => {}
        }
        at += size;
    }
    if at != end || system_loads != 1 {
        return Err("guest requires exactly one ordinary libSystem load");
    }
    let header = segments
        .iter()
        .filter(|s| {
            s.fileoff == 0 && s.filesize >= end as u64 && s.name == "__TEXT" && s.initprot == 5
        })
        .collect::<Vec<_>>();
    if header.len() != 1 {
        return Err("unique RX header segment required");
    }
    let header_vmaddr = header[0].vmaddr;
    Ok(Image {
        uuid: uuid.ok_or("guest UUID missing")?,
        platform,
        min_os: build.ok_or("guest platform missing")?,
        header_vmaddr,
        header_and_commands_size: end,
        segments,
        code_signature: structural.signature.map(|s| FileRange {
            fileoff: s.dataoff,
            filesize: s.datasize,
        }),
        file_size: d.len(),
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    fn put(d: &mut [u8], p: usize, v: u32) {
        d[p..p + 4].copy_from_slice(&v.to_le_bytes());
    }
    fn put64(d: &mut [u8], p: usize, v: u64) {
        d[p..p + 8].copy_from_slice(&v.to_le_bytes());
    }
    fn segment(n: &[u8], vm: u64, off: u64, size: u64, prot: u32, section: bool) -> Vec<u8> {
        let mut d = vec![0; 72 + if section { 80 } else { 0 }];
        let len = d.len();
        put(&mut d, 0, 0x19);
        put(&mut d, 4, len as u32);
        d[8..8 + n.len()].copy_from_slice(n);
        put64(&mut d, 24, vm);
        put64(&mut d, 32, 0x4000);
        put64(&mut d, 40, off);
        put64(&mut d, 48, size);
        put(&mut d, 56, 7);
        put(&mut d, 60, prot);
        put(&mut d, 64, section as u32);
        if section {
            d[72..78].copy_from_slice(b"__part");
            d[88..88 + n.len()].copy_from_slice(n);
            let delta = if off == 0 { 0x800 } else { 0 };
            put64(&mut d, 104, vm + delta);
            put64(&mut d, 112, 16);
            put(&mut d, 120, (off + delta) as u32);
            put(&mut d, 124, 4);
        }
        d
    }
    fn original() -> Vec<u8> {
        let vm = 0x100000000;
        let mut id = vec![0; 64];
        put(&mut id, 0, 0xd);
        put(&mut id, 4, 64);
        put(&mut id, 8, 24);
        let idname = b"@rpath/Public.framework/Test";
        id[24..24 + idname.len()].copy_from_slice(idname);
        let mut uuid = vec![0; 24];
        put(&mut uuid, 0, 0x1b);
        put(&mut uuid, 4, 24);
        uuid[8..24].fill(0x42);
        let mut build = vec![0; 24];
        put(&mut build, 0, 0x32);
        put(&mut build, 4, 24);
        put(&mut build, 8, 2);
        put(&mut build, 12, 0x1a0000);
        let mut load = vec![0; 56];
        put(&mut load, 0, 0xc);
        put(&mut load, 4, 56);
        put(&mut load, 8, 24);
        put(&mut load, 16, 0x10000);
        put(&mut load, 20, 0x10000);
        let system = b"/usr/lib/libSystem.B.dylib";
        load[24..24 + system.len()].copy_from_slice(system);
        let cmds = [
            segment(b"__TEXT", vm, 0, 0x1000, 5, true),
            segment(b"__DATA", vm + 0x4000, 0x1000, 0x100, 3, true),
            segment(b"__LINKEDIT", vm + 0x8000, 0x1100, 0x31, 1, false),
            id,
            uuid,
            build,
            load,
        ];
        let mut d = vec![0; 0x1131];
        for (p, v) in [
            (0, 0xfeedfacf),
            (4, 0x100000c),
            (12, 6),
            (16, cmds.len() as u32),
            (20, cmds.iter().map(Vec::len).sum::<usize>() as u32),
        ] {
            put(&mut d, p, v);
        }
        let mut at = 32;
        for c in cmds {
            d[at..at + c.len()].copy_from_slice(&c);
            at += c.len();
        }
        d[0x800..0x810].fill(0x7f);
        d
    }
    fn envelope(d: &[u8]) -> (Vec<u8>, ios_signed_image_policy::Manifest) {
        let m = ios_signed_image_policy::candidate_make_manifest(d).unwrap();
        let at = (d.len() + 15) & !15;
        let mut s = d.to_vec();
        s.resize(at + 64, 0);
        let mut be = |p: usize, v: u32| {
            s[at + p..at + p + 4].copy_from_slice(&v.to_be_bytes());
        };
        be(0, 0xfade0cc0);
        be(4, 64);
        be(8, 1);
        be(16, 20);
        be(20, 0xfade0c02);
        be(24, 44);
        put(&mut s, 16, m.ncmds as u32 + 1);
        put(&mut s, 20, m.sizeofcmds as u32 + 16);
        let lc = 32 + m.sizeofcmds as usize;
        for (i, v) in [0x1d, 16, at as u32, 64].into_iter().enumerate() {
            put(&mut s, lc + i * 4, v);
        }
        let filesize = s.len() as u64 - m.linkedit_fileoff;
        put64(&mut s, m.linkedit_command_offset as usize + 48, filesize);
        (s, m)
    }
    #[test]
    fn complete_candidate_binding_then_loader_mapping() {
        let d = original();
        let (s, m) = envelope(&d);
        ios_signed_image_policy::candidate_verify_binding(
            &s,
            &m,
            &ios_signed_image_policy::sha(&d),
        )
        .unwrap();
        let image = parse_after_content_binding(&s, 2).unwrap();
        assert_eq!(image.platform, 2);
        assert_eq!(image.min_os, 0x1a0000);
        assert!(image.code_signature.is_some());
        assert!(parse_after_content_binding(&s, 7).is_err());
        let mut changed = s;
        changed[0x800] ^= 1;
        assert!(
            ios_signed_image_policy::candidate_verify_binding(
                &changed,
                &m,
                &ios_signed_image_policy::sha(&d)
            )
            .is_err()
        );
    }
    #[test]
    fn mapping_rejects_bound_wx_and_early_initializers() {
        let mut d = original();
        put(&mut d, 32 + 60, 7);
        let (s, m) = envelope(&d);
        ios_signed_image_policy::candidate_verify_binding(
            &s,
            &m,
            &ios_signed_image_policy::sha(&d),
        )
        .unwrap();
        assert!(parse_after_content_binding(&s, 2).is_err());
        let mut d = original();
        put(&mut d, 32 + 152 + 72 + 64, 9);
        let (s, m) = envelope(&d);
        ios_signed_image_policy::candidate_verify_binding(
            &s,
            &m,
            &ios_signed_image_policy::sha(&d),
        )
        .unwrap();
        assert!(parse_after_content_binding(&s, 2).is_err());
    }
    #[test]
    fn range_permissions_and_header_aliases() {
        let image = Image {
            uuid: [0; 16],
            platform: 7,
            min_os: 0,
            header_vmaddr: 0x1000,
            header_and_commands_size: 256,
            segments: vec![
                Segment {
                    name: "__TEXT".into(),
                    vmaddr: 0x1000,
                    vmsize: 4096,
                    fileoff: 0,
                    filesize: 1024,
                    initprot: 5,
                },
                Segment {
                    name: "__DATA".into(),
                    vmaddr: 0x3000,
                    vmsize: 4096,
                    fileoff: 1024,
                    filesize: 128,
                    initprot: 3,
                },
            ],
            code_signature: None,
            file_size: 1152,
        };
        assert!(image.contains_loaded(0x4000, 0x4100, 4, true, false));
        assert!(!image.contains_loaded(0x4000, 0x40ff, 4, true, false));
        assert!(!image.contains_loaded(0x4000, 0x6000, 4, false, true));
        assert!(!image.contains_loaded(0x4000, 0x4400, 4, true, false));
        assert!(!image.contains_loaded(0x4000, usize::MAX - 1, 4, true, false));
    }
}
