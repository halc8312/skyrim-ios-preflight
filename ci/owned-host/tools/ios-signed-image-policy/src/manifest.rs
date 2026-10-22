use crate::{CANDIDATE_SCHEMA, Check, MAX_IMAGE, PAGE, SCHEMA, macho, need, sha};
use serde_core::de::{self, MapAccess, Visitor};
use std::{collections::BTreeSet, fmt};
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Manifest {
    pub schema: String,
    pub unsigned_sha256: String,
    pub unsigned_length: u64,
    pub ncmds: u64,
    pub sizeofcmds: u64,
    pub first_data: u64,
    pub linkedit_command_offset: u64,
    pub linkedit_fileoff: u64,
    pub linkedit_filesize: u64,
    pub linkedit_vmsize: u64,
    pub vm_alignment: u64,
}
impl Manifest {
    pub fn to_json(&self) -> serde_json::Value {
        serde_json::json!({"schema":self.schema,"unsigned_sha256":self.unsigned_sha256,"unsigned_length":self.unsigned_length,"ncmds":self.ncmds,"sizeofcmds":self.sizeofcmds,"first_data":self.first_data,"linkedit_command_offset":self.linkedit_command_offset,"linkedit_fileoff":self.linkedit_fileoff,"linkedit_filesize":self.linkedit_filesize,"linkedit_vmsize":self.linkedit_vmsize,"vm_alignment":self.vm_alignment})
    }
}
impl<'de> de::Deserialize<'de> for Manifest {
    fn deserialize<D: de::Deserializer<'de>>(decoder: D) -> Result<Self, D::Error> {
        struct ManifestVisitor;
        impl<'de> Visitor<'de> for ManifestVisitor {
            type Value = Manifest;
            fn expecting(&self, f: &mut fmt::Formatter) -> fmt::Result {
                write!(f, "an exact flat unsigned binding manifest")
            }
            fn visit_map<M: MapAccess<'de>>(self, mut map: M) -> Result<Manifest, M::Error> {
                let mut m = Manifest::default();
                let mut seen = BTreeSet::new();
                while let Some(key) = map.next_key::<String>()? {
                    if !seen.insert(key.clone()) {
                        return Err(de::Error::custom("duplicate_manifest_key"));
                    }
                    match key.as_str() {
                        "schema" => m.schema = map.next_value()?,
                        "unsigned_sha256" => m.unsigned_sha256 = map.next_value()?,
                        "unsigned_length" => m.unsigned_length = map.next_value()?,
                        "ncmds" => m.ncmds = map.next_value()?,
                        "sizeofcmds" => m.sizeofcmds = map.next_value()?,
                        "first_data" => m.first_data = map.next_value()?,
                        "linkedit_command_offset" => {
                            m.linkedit_command_offset = map.next_value()?
                        }
                        "linkedit_fileoff" => m.linkedit_fileoff = map.next_value()?,
                        "linkedit_filesize" => m.linkedit_filesize = map.next_value()?,
                        "linkedit_vmsize" => m.linkedit_vmsize = map.next_value()?,
                        "vm_alignment" => m.vm_alignment = map.next_value()?,
                        _ => return Err(de::Error::custom("unknown_manifest_field")),
                    }
                }
                if seen.len() != 11 {
                    return Err(de::Error::custom("missing_manifest_field"));
                }
                Ok(m)
            }
        }
        decoder.deserialize_map(ManifestVisitor)
    }
}
pub fn load_manifest(data: &[u8]) -> Check<Manifest> {
    need(data.len() <= 16384, "manifest_size")?;
    serde_json::from_slice(data).map_err(|_| "manifest_json_or_schema")
}
pub fn validate_manifest(m: &Manifest, expected: &str) -> Check<()> {
    validate_manifest_for(m, expected, false)
}
pub(crate) fn validate_manifest_for(m: &Manifest, expected: &str, candidate: bool) -> Check<()> {
    need(
        expected.len() == 64
            && expected
                .bytes()
                .all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c)),
        "trusted_unsigned_sha256",
    )?;
    need(
        m.schema == if candidate { CANDIDATE_SCHEMA } else { SCHEMA }
            && m.unsigned_sha256 == expected,
        "manifest_expected_sha256",
    )?;
    need(
        [
            m.unsigned_length,
            m.ncmds,
            m.sizeofcmds,
            m.first_data,
            m.linkedit_command_offset,
            m.linkedit_fileoff,
            m.linkedit_filesize,
            m.linkedit_vmsize,
            m.vm_alignment,
        ]
        .iter()
        .all(|v| *v <= MAX_IMAGE as u64),
        "manifest_integer",
    )?;
    need(
        (32..=MAX_IMAGE as u64).contains(&m.unsigned_length) && m.vm_alignment == PAGE,
        "manifest_limits",
    )?;
    let end = 32 + m.sizeofcmds;
    need(
        (1..=512).contains(&m.ncmds)
            && (8..=65536).contains(&m.sizeofcmds)
            && end + 16 <= m.first_data
            && m.first_data <= m.linkedit_fileoff
            && m.linkedit_command_offset >= 32
            && m.linkedit_command_offset + 72 <= end
            && m.linkedit_fileoff + m.linkedit_filesize == m.unsigned_length
            && m.linkedit_vmsize >= m.linkedit_filesize,
        "manifest_layout",
    )
}
pub fn make_manifest(unsigned: &[u8]) -> Check<Manifest> {
    make_manifest_for(unsigned, false)
}
pub fn candidate_make_manifest(unsigned: &[u8]) -> Check<Manifest> {
    make_manifest_for(unsigned, true)
}
pub(crate) fn make_manifest_for(unsigned: &[u8], candidate: bool) -> Check<Manifest> {
    let image = if candidate {
        macho::parse_lld_candidate(unsigned, false)?
    } else {
        macho::parse(unsigned, false)?
    };
    need(
        image.first_data >= 32 + image.sizeofcmds + 16,
        "insufficient_zero_header_padding",
    )?;
    let link = image.linkedit;
    Ok(Manifest {
        schema: if candidate { CANDIDATE_SCHEMA } else { SCHEMA }.into(),
        unsigned_sha256: sha(unsigned),
        unsigned_length: unsigned.len() as u64,
        ncmds: image.ncmds,
        sizeofcmds: image.sizeofcmds,
        first_data: image.first_data,
        linkedit_command_offset: link.command_offset,
        linkedit_fileoff: link.fileoff,
        linkedit_filesize: link.filesize,
        linkedit_vmsize: link.vmsize,
        vm_alignment: PAGE,
    })
}
