//! Separately packaged expected metadata. This module never opens files,
//! discovers expectations from observed game/framework bytes, normalizes an
//! image, authenticates a signer, or grants permission to execute anything.
//! Callers hash the same owned XEX buffer before parsing it; framework content
//! reconstruction remains the explicit signed-image-policy candidate verifier.
use crate::guest_abi::GuestAbi;
use ios_signed_image_policy::{CANDIDATE_SCHEMA, MAX_IMAGE, Manifest, PAGE};
use serde_core::de::{self, MapAccess, Visitor};
use sha2::{Digest, Sha256};
use std::{collections::BTreeSet, fmt};

pub type Check<T> = Result<T, &'static str>;
pub const SCHEMA: &str = "ios_owned_game_trusted_binding_v1";
pub const ABI_VERSION: u32 = 0x0001_0000;
pub const MAX_BINDING_BYTES: usize = 16 * 1024;
pub const MAX_FUNCTIONS: usize = 1 << 20;
const MAX_CONTEXT_BYTES: u64 = 1 << 20;
const FIELD_COUNT: usize = 27;

#[derive(Clone, Debug, Default)]
pub struct TrustedBinding {
    pub unsigned_framework_sha256: String,
    pub normalization: Manifest,
    pub xex_relative_path: String,
    pub xex_sha256: String,
    pub xex_bytes: u64,
    pub xex_title_id: u32,
    pub xex_media_id: u32,
    pub xex_version: u32,
    pub xex_base_version: u32,
    /// Image base, image size, code base, code size, in this fixed order.
    pub ppc_config: [u32; 4],
    pub function_count: usize,
    /// SHA-256 over strictly increasing function addresses, each LE uint32.
    pub function_table_sha256: String,
    /// r3,r0,r1,r2,r4,r13,r31,lr,ctr,xer,reserved,msr,cr0,fpscr,
    /// f0,f1,v0,v127,sizeof(PPCContext),alignof(PPCContext).
    pub context_layout: [u64; 20],
    pub abi_version: u32,
}

fn need(condition: bool, category: &'static str) -> Check<()> {
    if condition { Ok(()) } else { Err(category) }
}
fn hash(bytes: &[u8]) -> String {
    Sha256::digest(bytes)
        .iter()
        .map(|byte| format!("{byte:02x}"))
        .collect()
}
fn is_hash(value: &str) -> bool {
    value.len() == 64
        && value
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
}
fn config_bounds(config: [u32; 4]) -> Check<()> {
    let [image, size, code, length] = config.map(u64::from);
    need(
        image != 0
            && size != 0
            && code != 0
            && length != 0
            && code % 4 == 0
            && length % 4 == 0
            && image + size <= 1u64 << 32
            && image <= code
            && code + length <= image + size,
        "binding_config_extent",
    )
}
fn layout_bounds(layout: [u64; 20]) -> Check<()> {
    let size = layout[18];
    let alignment = layout[19];
    need(
        (16..=MAX_CONTEXT_BYTES).contains(&size)
            && (16..=4096).contains(&alignment)
            && alignment.is_power_of_two()
            && size % alignment == 0,
        "binding_context_shape",
    )?;
    // Bounds for the selected public ABI fields, not an inferred C layout.
    // Equality with both the native query and actual Rust layout is still
    // required before any context pointer crosses the ABI.
    let widths = [8u64, 8, 8, 8, 8, 8, 8, 8, 8, 3, 8, 4, 4, 4, 8, 8, 16, 16];
    let mut ranges = Vec::with_capacity(18);
    for (&offset, width) in layout[..18].iter().zip(widths) {
        need(
            offset <= size && width <= size - offset,
            "binding_context_extent",
        )?;
        ranges.push((offset, offset + width));
    }
    ranges.sort_unstable();
    need(
        ranges.windows(2).all(|pair| pair[0].1 <= pair[1].0),
        "binding_context_alias",
    )
}

impl TrustedBinding {
    pub fn guest_abi(&self) -> Check<GuestAbi> {
        GuestAbi::from_version(self.abi_version)
    }

    pub fn validate(&self) -> Check<()> {
        self.guest_abi()?;
        need(
            self.xex_relative_path == "default.xex",
            "binding_xex_relative_path",
        )?;
        need(
            is_hash(&self.unsigned_framework_sha256)
                && is_hash(&self.xex_sha256)
                && is_hash(&self.function_table_sha256),
            "binding_hash_shape",
        )?;
        need(
            (24..=MAX_IMAGE as u64).contains(&self.xex_bytes),
            "binding_xex_size",
        )?;
        let m = &self.normalization;
        need(
            m.schema == CANDIDATE_SCHEMA && m.unsigned_sha256 == self.unsigned_framework_sha256,
            "binding_normalization_policy",
        )?;
        // Common bounded-manifest contract mirrors the public normalizer. Its
        // explicit candidate verifier independently validates these fields and
        // reconstructs/reparses the entire original image before use.
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
            .all(|value| *value <= MAX_IMAGE as u64),
            "binding_manifest_integer",
        )?;
        let end = 32 + m.sizeofcmds;
        need(
            (32..=MAX_IMAGE as u64).contains(&m.unsigned_length)
                && m.vm_alignment == PAGE
                && (1..=512).contains(&m.ncmds)
                && (8..=65536).contains(&m.sizeofcmds)
                && end + 16 <= m.first_data
                && m.first_data <= m.linkedit_fileoff
                && m.linkedit_command_offset >= 32
                && m.linkedit_command_offset + 72 <= end
                && m.linkedit_fileoff + m.linkedit_filesize == m.unsigned_length
                && m.linkedit_vmsize >= m.linkedit_filesize,
            "binding_manifest_layout",
        )?;
        ios_signed_image_policy::restoration_ranges(m)
            .map_err(|_| "binding_manifest_restoration_ranges")?;
        config_bounds(self.ppc_config)?;
        need(
            self.function_count > 0
                && self.function_count <= MAX_FUNCTIONS
                && self.function_count as u64 <= u64::from(self.ppc_config[3]) / 4,
            "binding_function_count",
        )?;
        layout_bounds(self.context_layout)
    }

    /// Must receive the exact owned buffer subsequently passed to XEX parsing;
    /// callers must not reopen its path after this check.
    pub fn verify_xex_bytes(&self, bytes: &[u8]) -> Check<()> {
        self.validate()?;
        need(
            bytes.len() as u64 == self.xex_bytes,
            "binding_xex_byte_count",
        )?;
        need(hash(bytes) == self.xex_sha256, "binding_xex_sha256")
    }

    pub fn verify_xex_identity(
        &self,
        title: u32,
        media: u32,
        version: u32,
        base: u32,
    ) -> Check<()> {
        self.validate()?;
        need(
            [title, media, version, base]
                == [
                    self.xex_title_id,
                    self.xex_media_id,
                    self.xex_version,
                    self.xex_base_version,
                ],
            "binding_xex_identity",
        )
    }

    /// Config and function addresses are values copied from the checked module
    /// queries. This never creates the trusted digest from those observations.
    pub fn verify_module(
        &self,
        config: [u32; 4],
        functions: &[u32],
        context_layout: [u64; 20],
        abi_version: u32,
    ) -> Check<()> {
        self.validate()?;
        need(
            abi_version == self.abi_version,
            "binding_module_abi_version",
        )?;
        need(config == self.ppc_config, "binding_module_config")?;
        need(
            context_layout == self.context_layout,
            "binding_module_context_layout",
        )?;
        need(
            functions.len() == self.function_count,
            "binding_module_function_count",
        )?;
        let code = u64::from(config[2]);
        let end = code + u64::from(config[3]);
        need(
            functions.iter().all(|address| {
                *address != 0
                    && address % 4 == 0
                    && u64::from(*address) >= code
                    && u64::from(*address) < end
            }) && functions.windows(2).all(|pair| pair[0] < pair[1]),
            "binding_module_function_extent_or_order",
        )?;
        let mut digest = Sha256::new();
        for address in functions {
            digest.update(address.to_le_bytes());
        }
        let digest: String = digest
            .finalize()
            .iter()
            .map(|byte| format!("{byte:02x}"))
            .collect();
        need(
            digest == self.function_table_sha256,
            "binding_module_function_sha256",
        )
    }

    /// Serializes already supplied expected metadata; performs no discovery or
    /// filesystem writes. Useful for independently prepared packaging tools.
    pub fn to_json(&self) -> Check<serde_json::Value> {
        self.validate()?;
        let m = &self.normalization;
        Ok(serde_json::json!({
            "schema":SCHEMA,"abi_version":self.abi_version,"normalization_schema":m.schema,
            "unsigned_framework_sha256":self.unsigned_framework_sha256,"unsigned_framework_bytes":m.unsigned_length,
            "normalization_ncmds":m.ncmds,"normalization_sizeofcmds":m.sizeofcmds,"normalization_first_data":m.first_data,
            "normalization_linkedit_command_offset":m.linkedit_command_offset,"normalization_linkedit_fileoff":m.linkedit_fileoff,
            "normalization_linkedit_filesize":m.linkedit_filesize,"normalization_linkedit_vmsize":m.linkedit_vmsize,
            "normalization_vm_alignment":m.vm_alignment,"xex_relative_path":self.xex_relative_path,
            "xex_sha256":self.xex_sha256,"xex_bytes":self.xex_bytes,"xex_title_id":self.xex_title_id,
            "xex_media_id":self.xex_media_id,"xex_version":self.xex_version,"xex_base_version":self.xex_base_version,
            "ppc_image_base":self.ppc_config[0],"ppc_image_size":self.ppc_config[1],"ppc_code_base":self.ppc_config[2],
            "ppc_code_size":self.ppc_config[3],"function_count":self.function_count,
            "function_table_sha256":self.function_table_sha256,"context_layout":self.context_layout
        }))
    }
}

struct FlatBinding(TrustedBinding);
impl<'de> de::Deserialize<'de> for FlatBinding {
    fn deserialize<D: de::Deserializer<'de>>(decoder: D) -> Result<Self, D::Error> {
        struct FlatVisitor;
        impl<'de> Visitor<'de> for FlatVisitor {
            type Value = FlatBinding;
            fn expecting(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
                write!(formatter, "an exact flat expected binding object")
            }
            fn visit_map<M: MapAccess<'de>>(self, mut map: M) -> Result<Self::Value, M::Error> {
                let mut value = TrustedBinding::default();
                let mut seen = BTreeSet::new();
                while let Some(key) = map.next_key::<String>()? {
                    if !seen.insert(key.clone()) {
                        return Err(de::Error::custom("duplicate_binding_key"));
                    }
                    match key.as_str() {
                        "schema" => {
                            let schema: String = map.next_value()?;
                            if schema != SCHEMA {
                                return Err(de::Error::custom("binding_schema"));
                            }
                        }
                        "abi_version" => value.abi_version = map.next_value()?,
                        "normalization_schema" => value.normalization.schema = map.next_value()?,
                        "unsigned_framework_sha256" => {
                            value.unsigned_framework_sha256 = map.next_value()?;
                            value.normalization.unsigned_sha256 =
                                value.unsigned_framework_sha256.clone();
                        }
                        "unsigned_framework_bytes" => {
                            value.normalization.unsigned_length = map.next_value()?
                        }
                        "normalization_ncmds" => value.normalization.ncmds = map.next_value()?,
                        "normalization_sizeofcmds" => {
                            value.normalization.sizeofcmds = map.next_value()?
                        }
                        "normalization_first_data" => {
                            value.normalization.first_data = map.next_value()?
                        }
                        "normalization_linkedit_command_offset" => {
                            value.normalization.linkedit_command_offset = map.next_value()?
                        }
                        "normalization_linkedit_fileoff" => {
                            value.normalization.linkedit_fileoff = map.next_value()?
                        }
                        "normalization_linkedit_filesize" => {
                            value.normalization.linkedit_filesize = map.next_value()?
                        }
                        "normalization_linkedit_vmsize" => {
                            value.normalization.linkedit_vmsize = map.next_value()?
                        }
                        "normalization_vm_alignment" => {
                            value.normalization.vm_alignment = map.next_value()?
                        }
                        "xex_relative_path" => value.xex_relative_path = map.next_value()?,
                        "xex_sha256" => value.xex_sha256 = map.next_value()?,
                        "xex_bytes" => value.xex_bytes = map.next_value()?,
                        "xex_title_id" => value.xex_title_id = map.next_value()?,
                        "xex_media_id" => value.xex_media_id = map.next_value()?,
                        "xex_version" => value.xex_version = map.next_value()?,
                        "xex_base_version" => value.xex_base_version = map.next_value()?,
                        "ppc_image_base" => value.ppc_config[0] = map.next_value()?,
                        "ppc_image_size" => value.ppc_config[1] = map.next_value()?,
                        "ppc_code_base" => value.ppc_config[2] = map.next_value()?,
                        "ppc_code_size" => value.ppc_config[3] = map.next_value()?,
                        "function_count" => value.function_count = map.next_value()?,
                        "function_table_sha256" => {
                            value.function_table_sha256 = map.next_value()?
                        }
                        "context_layout" => value.context_layout = map.next_value()?,
                        _ => return Err(de::Error::custom("unknown_binding_field")),
                    }
                }
                if seen.len() != FIELD_COUNT {
                    return Err(de::Error::custom("missing_binding_field"));
                }
                Ok(FlatBinding(value))
            }
        }
        decoder.deserialize_map(FlatVisitor)
    }
}

pub fn parse(bytes: &[u8]) -> Check<TrustedBinding> {
    need(
        !bytes.is_empty() && bytes.len() <= MAX_BINDING_BYTES,
        "binding_byte_limit",
    )?;
    let FlatBinding(value) = serde_json::from_slice(bytes).map_err(|_| "binding_json_or_schema")?;
    value.validate()?;
    Ok(value)
}

#[cfg(test)]
mod tests {
    use super::*;
    // Self-authored non-executable bytes and invented metadata. No game/XEX or
    // framework sample is read. SHA expectations below were computed separately
    // with Python hashlib over the explicit byte sequences.
    const FUNCTIONS: [u32; 3] = [0x1010, 0x1014, 0x101c];
    const LAYOUT: [u64; 20] = [
        0, 8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 92, 96, 104, 112, 128, 144, 192, 16,
    ];
    fn example() -> TrustedBinding {
        let framework_sha = "a".repeat(64);
        TrustedBinding {
            unsigned_framework_sha256: framework_sha.clone(),
            normalization: Manifest {
                schema: CANDIDATE_SCHEMA.into(),
                unsigned_sha256: framework_sha,
                unsigned_length: 1024,
                ncmds: 4,
                sizeofcmds: 352,
                first_data: 512,
                linkedit_command_offset: 232,
                linkedit_fileoff: 768,
                linkedit_filesize: 256,
                linkedit_vmsize: 16384,
                vm_alignment: PAGE,
            },
            xex_relative_path: "default.xex".into(),
            xex_sha256: "3eb88535070792a177d2b64034891c00bcb525544675794b74f07fa82bf668fb".into(),
            xex_bytes: 34,
            xex_title_id: 0x12345678,
            xex_media_id: 0x11223344,
            xex_version: 7,
            xex_base_version: 2,
            ppc_config: [0x1000, 0x100, 0x1010, 0x40],
            function_count: 3,
            function_table_sha256:
                "623990a05b93c67d9ca16daeb61e031338bb77164fcce5bd5774a4fa00a61a65".into(),
            context_layout: LAYOUT,
            abi_version: ABI_VERSION,
        }
    }
    fn json(value: &TrustedBinding) -> serde_json::Value {
        value.to_json().unwrap()
    }
    fn encoded(value: &serde_json::Value) -> Vec<u8> {
        serde_json::to_vec(value).unwrap()
    }

    #[test]
    fn independently_expected_function_digest_and_whole_byte_buffer_match() {
        let expected = example();
        let parsed = parse(&encoded(&json(&expected))).unwrap();
        assert_eq!(parsed.normalization, expected.normalization);
        assert_eq!(parsed.ppc_config, expected.ppc_config);
        assert_eq!(parsed.context_layout, LAYOUT);
        parsed
            .verify_xex_bytes(b"self-authored expected byte buffer")
            .unwrap();
        parsed
            .verify_xex_identity(0x12345678, 0x11223344, 7, 2)
            .unwrap();
        parsed
            .verify_module(expected.ppc_config, &FUNCTIONS, LAYOUT, ABI_VERSION)
            .unwrap();
        assert_eq!(json(&parsed), json(&expected));
    }

    #[test]
    fn only_explicit_integer_v1_and_v2_metadata_versions_are_accepted() {
        for (version, selected) in [
            (ABI_VERSION, GuestAbi::V1),
            (crate::abi_v2::VERSION, GuestAbi::V2),
        ] {
            let mut expected = example();
            expected.abi_version = version;
            let parsed = parse(&encoded(&json(&expected))).unwrap();
            assert_eq!(parsed.guest_abi().unwrap(), selected);
            assert_eq!(json(&parsed), json(&expected));
            assert_eq!(json(&parsed)["schema"], SCHEMA);
        }
        let original = json(&example());
        for bad in [
            serde_json::json!(0),
            serde_json::json!(1),
            serde_json::json!(2),
            serde_json::json!(0x10001),
            serde_json::json!(0x20001),
            serde_json::json!(0x30000),
            serde_json::json!(true),
            serde_json::json!(131072.0),
            serde_json::json!("131072"),
            serde_json::json!(null),
            serde_json::json!(-1),
            serde_json::json!(4294967296u64),
        ] {
            let mut changed = original.clone();
            changed["abi_version"] = bad;
            assert!(parse(&encoded(&changed)).is_err());
        }
    }

    #[test]
    fn v2_keeps_exact_content_identity_layout_and_function_bindings() {
        let mut expected = example();
        expected.abi_version = crate::abi_v2::VERSION;
        let parsed = parse(&encoded(&json(&expected))).unwrap();
        parsed
            .verify_xex_bytes(b"self-authored expected byte buffer")
            .unwrap();
        parsed
            .verify_xex_identity(0x12345678, 0x11223344, 7, 2)
            .unwrap();
        parsed
            .verify_module(
                expected.ppc_config,
                &FUNCTIONS,
                LAYOUT,
                crate::abi_v2::VERSION,
            )
            .unwrap();
        assert!(
            parsed
                .verify_xex_bytes(b"self-authored EXPECTED byte buffer")
                .is_err()
        );
        assert!(
            parsed
                .verify_xex_identity(0x12345678, 0x11223344, 8, 2)
                .is_err()
        );
        assert_eq!(
            parsed.verify_module(expected.ppc_config, &FUNCTIONS, LAYOUT, ABI_VERSION),
            Err("binding_module_abi_version")
        );
        let mut config = expected.ppc_config;
        config[2] += 4;
        assert!(
            parsed
                .verify_module(config, &FUNCTIONS, LAYOUT, crate::abi_v2::VERSION)
                .is_err()
        );
        let mut layout = LAYOUT;
        layout[17] += 16;
        assert!(
            parsed
                .verify_module(
                    expected.ppc_config,
                    &FUNCTIONS,
                    layout,
                    crate::abi_v2::VERSION
                )
                .is_err()
        );
        assert!(
            parsed
                .verify_module(
                    expected.ppc_config,
                    &[0x1010, 0x1014, 0x1020],
                    LAYOUT,
                    crate::abi_v2::VERSION
                )
                .is_err()
        );
        let mut malformed = json(&expected);
        malformed["normalization_linkedit_filesize"] = serde_json::json!(255);
        assert!(parse(&encoded(&malformed)).is_err());
        assert_eq!(
            parsed.function_table_sha256,
            example().function_table_sha256
        );
    }

    #[test]
    fn duplicate_unknown_missing_and_wrong_schema_refuse() {
        let value = json(&example());
        let normal = serde_json::to_string(&value).unwrap();
        let duplicate = normal.replacen("{", "{\"xex_bytes\":34,", 1);
        assert!(parse(duplicate.as_bytes()).is_err());
        let escaped_duplicate = normal.replacen("{", "{\"xex_\\u0062ytes\":34,", 1);
        assert!(parse(escaped_duplicate.as_bytes()).is_err());
        for field in value.as_object().unwrap().keys() {
            let mut changed = value.clone();
            changed.as_object_mut().unwrap().remove(field);
            assert!(parse(&encoded(&changed)).is_err());
        }
        let mut changed = value.clone();
        changed["unknown_field"] = serde_json::json!(0);
        assert!(parse(&encoded(&changed)).is_err());
        let mut changed = value.clone();
        changed["schema"] = serde_json::json!("future");
        assert!(parse(&encoded(&changed)).is_err());
        let mut changed = value;
        changed["normalization_schema"] = serde_json::json!(ios_signed_image_policy::SCHEMA);
        assert!(parse(&encoded(&changed)).is_err());
    }

    #[test]
    fn integer_types_ranges_and_exact_layout_length_are_required() {
        let value = json(&example());
        for bad in [
            serde_json::json!(true),
            serde_json::json!(-1),
            serde_json::json!(7.0),
            serde_json::json!("7"),
            serde_json::json!(null),
            serde_json::json!(4294967296u64),
        ] {
            let mut changed = value.clone();
            changed["xex_version"] = bad;
            assert!(parse(&encoded(&changed)).is_err());
        }
        for count in [19, 21] {
            let mut changed = value.clone();
            changed["context_layout"] = serde_json::json!(vec![0u64; count]);
            assert!(parse(&encoded(&changed)).is_err());
        }
        let mut changed = value;
        changed["function_count"] = serde_json::json!(MAX_FUNCTIONS + 1);
        assert!(parse(&encoded(&changed)).is_err());
    }

    #[test]
    fn byte_limits_nesting_trailing_data_and_nonobjects_refuse() {
        assert!(parse(&[]).is_err());
        assert_eq!(
            parse(&vec![b' '; MAX_BINDING_BYTES + 1]).unwrap_err(),
            "binding_byte_limit"
        );
        for bytes in [
            &b"[]"[..],
            b"null",
            b"true",
            b"{}{}",
            b"{\"schema\":{\"nested\":1}}",
            b"{\"schema\":NaN}",
            b"\xff",
        ] {
            assert!(parse(bytes).is_err());
        }
        let mut bytes = encoded(&json(&example()));
        bytes.push(b'0');
        assert!(parse(&bytes).is_err());
    }

    #[test]
    fn paths_hash_spelling_and_manifest_aliases_refuse() {
        let value = json(&example());
        for path in [
            "../default.xex",
            "DEFAULT.XEX",
            "game/default.xex",
            "default.xex\0hidden",
            "C:\\default.xex",
            "",
        ] {
            let mut changed = value.clone();
            changed["xex_relative_path"] = serde_json::json!(path);
            assert!(parse(&encoded(&changed)).is_err());
        }
        for field in [
            "unsigned_framework_sha256",
            "xex_sha256",
            "function_table_sha256",
        ] {
            for spelling in [
                "A".repeat(64),
                "a".repeat(63),
                format!("{}g", "a".repeat(63)),
            ] {
                let mut changed = value.clone();
                changed[field] = serde_json::json!(spelling);
                assert!(parse(&encoded(&changed)).is_err());
            }
        }
        for (field, bad) in [
            ("normalization_linkedit_command_offset", 16u64),
            ("normalization_sizeofcmds", u64::MAX),
            ("normalization_linkedit_filesize", 255),
            ("normalization_vm_alignment", 4096),
        ] {
            let mut changed = value.clone();
            changed[field] = serde_json::json!(bad);
            assert!(parse(&encoded(&changed)).is_err());
        }
    }

    #[test]
    fn address_extents_layout_aliases_and_abi_refuse_before_use() {
        let mut expected = example();
        expected.ppc_config = [0xfffffff0, 0x20, 0xfffffff0, 4];
        assert!(expected.validate().is_err());
        let mut expected = example();
        expected.ppc_config[2] = 0x100c;
        expected.ppc_config[3] = 0x100;
        assert!(expected.validate().is_err());
        let mut expected = example();
        expected.context_layout[1] = 0;
        assert!(expected.validate().is_err());
        let mut expected = example();
        expected.context_layout[17] = 191;
        assert!(expected.validate().is_err());
        let mut expected = example();
        expected.context_layout[19] = 24;
        assert!(expected.validate().is_err());
        let mut expected = example();
        expected.abi_version = 2;
        assert!(expected.validate().is_err());
    }

    #[test]
    fn observed_changes_never_redefine_expected_identity_config_or_digest() {
        let expected = example();
        assert!(
            expected
                .verify_xex_bytes(b"self-authored EXPECTED byte buffer")
                .is_err()
        );
        assert!(expected.verify_xex_bytes(b"short").is_err());
        for identity in [
            [0x12345679, 0x11223344, 7, 2],
            [0x12345678, 0x11223345, 7, 2],
            [0x12345678, 0x11223344, 8, 2],
            [0x12345678, 0x11223344, 7, 3],
        ] {
            assert!(
                expected
                    .verify_xex_identity(identity[0], identity[1], identity[2], identity[3])
                    .is_err()
            );
        }
        let mut config = expected.ppc_config;
        config[0] += 4;
        assert!(
            expected
                .verify_module(config, &FUNCTIONS, LAYOUT, ABI_VERSION)
                .is_err()
        );
        assert!(
            expected
                .verify_module(expected.ppc_config, &FUNCTIONS, LAYOUT, 2)
                .is_err()
        );
        let mut layout = LAYOUT;
        layout[17] += 16;
        assert!(
            expected
                .verify_module(expected.ppc_config, &FUNCTIONS, layout, ABI_VERSION)
                .is_err()
        );
        for functions in [
            &[0x1010, 0x1014][..],
            &[0x1010, 0x1014, 0x1014],
            &[0x1014, 0x1010, 0x101c],
            &[0x1010, 0x1014, 0x1050],
            &[0x1010, 0x1014, 0x101d],
            &[0x1010, 0x1014, 0x1020],
        ] {
            assert!(
                expected
                    .verify_module(expected.ppc_config, functions, LAYOUT, ABI_VERSION)
                    .is_err()
            );
        }
        assert_eq!(
            expected.function_table_sha256,
            example().function_table_sha256
        );
    }
}
