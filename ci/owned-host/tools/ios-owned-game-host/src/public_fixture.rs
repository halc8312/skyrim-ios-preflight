//! Independently declared self-authored fixture expectations. No game input,
//! observed loaded API, private framework or runtime discovery defines them.
use crate::{Check, binding::TrustedBinding};
use xex::builder::*;
pub const CONFIG: [u32; 4] = [0x82000000, 0x2000, 0x82001000, 0x10];
pub const FUNCTIONS: [u32; 3] = [0x82001000, 0x82001004, 0x82001008];
pub const TITLE: u32 = 0x12345678;
pub const MEDIA: u32 = 0x11223344;
pub const VERSION: u32 = 7;
pub const BASE_VERSION: u32 = 2;
pub const FREQUENCY: u32 = CONFIG[0] + 0x1800;
pub const CLEANUP: u32 = CONFIG[0] + 0x1804;
pub fn xex_bytes() -> Vec<u8> {
    let mut data = vec![0u8; 16];
    data[..4].copy_from_slice(&0x83u32.to_be_bytes());
    data[4..8].copy_from_slice(&0x19u32.to_be_bytes());
    let mut code = vec![0u8; 16];
    code[..4].copy_from_slice(&0x4e800020u32.to_be_bytes());
    code[4..8].copy_from_slice(&0x01000083u32.to_be_bytes());
    code[8..12].copy_from_slice(&0x01000019u32.to_be_bytes());
    let image = build_pe_image(
        &[
            PeSection {
                name: ".rdata".into(),
                rva: 0x800,
                virtual_size: 16,
                characteristics: IMAGE_SCN_CNT_INITIALIZED_DATA
                    | IMAGE_SCN_MEM_READ
                    | IMAGE_SCN_MEM_WRITE,
                data,
            },
            PeSection {
                name: ".text".into(),
                rva: 0x1000,
                virtual_size: 16,
                characteristics: IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE,
                data: code,
            },
            PeSection {
                name: ".data".into(),
                rva: 0x1800,
                virtual_size: 16,
                characteristics: IMAGE_SCN_CNT_INITIALIZED_DATA
                    | IMAGE_SCN_MEM_READ
                    | IMAGE_SCN_MEM_WRITE,
                data: vec![0; 16],
            },
        ],
        CONFIG[1],
    );
    XexBuilder {
        image,
        load_address: CONFIG[0],
        entry_point: FUNCTIONS[0],
        module_flags: 1,
        imports: vec![ImportLibrarySpec {
            name: "xboxkrnl.exe".into(),
            id: 0,
            version: 0x20004497,
            min_version: 0x20000000,
            records: vec![
                CONFIG[0] + 0x800,
                FUNCTIONS[1],
                CONFIG[0] + 0x804,
                FUNCTIONS[2],
            ],
        }],
        tls: Some(xex::TlsInfo {
            slot_count: 64,
            raw_data_address: 0,
            data_size: 0,
            raw_data_size: 0,
        }),
        system_flags: Some(0x400),
        default_stack_size: Some(0x40000),
        execution_info: Some(xex::ExecutionInfo {
            media_id: MEDIA,
            version: VERSION,
            base_version: BASE_VERSION,
            title_id: TITLE,
            platform: 2,
            executable_type: 0,
            disc_number: 1,
            disc_count: 1,
            savegame_id: 0,
        }),
        basic_compression: false,
    }
    .build()
}
pub fn layout() -> [u64; 20] {
    use runtime_core::context::offsets as o;
    [
        o::R3,
        o::R0,
        o::R1,
        o::R2,
        o::R4,
        o::R13,
        o::R31,
        o::LR,
        o::CTR,
        o::XER,
        o::RESERVED,
        o::MSR,
        o::CR0,
        o::FPSCR,
        o::F0,
        o::F1,
        o::V0,
        o::V127,
        o::SIZE,
        o::ALIGN,
    ]
    .map(|n| n as u64)
}
pub fn expected_binding(unsigned_public_framework: &[u8]) -> Check<TrustedBinding> {
    if unsigned_public_framework.len() > 16 * 1024 * 1024 {
        return Err("public fixture framework exceeds 16 MiB limit");
    }
    let normalization =
        ios_signed_image_policy::candidate_make_manifest(unsigned_public_framework)?;
    let xex = xex_bytes();
    let function_bytes = FUNCTIONS
        .iter()
        .flat_map(|n| n.to_le_bytes())
        .collect::<Vec<_>>();
    let binding = TrustedBinding {
        unsigned_framework_sha256: normalization.unsigned_sha256.clone(),
        normalization,
        xex_relative_path: "default.xex".into(),
        xex_sha256: ios_signed_image_policy::sha(&xex),
        xex_bytes: xex.len() as u64,
        xex_title_id: TITLE,
        xex_media_id: MEDIA,
        xex_version: VERSION,
        xex_base_version: BASE_VERSION,
        ppc_config: CONFIG,
        function_count: FUNCTIONS.len(),
        function_table_sha256: ios_signed_image_policy::sha(&function_bytes),
        context_layout: layout(),
        abi_version: crate::abi::VERSION,
    };
    binding.validate()?;
    Ok(binding)
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn fixed_plaintext_image_and_expected_identity() {
        let bytes = xex_bytes();
        crate::xex_envelope::validate(&bytes, CONFIG[1]).unwrap();
        let xex = xex::Xex::parse_owned(bytes).unwrap();
        let e = xex.execution_info().unwrap();
        assert_eq!(
            [e.title_id, e.media_id, e.version, e.base_version],
            [TITLE, MEDIA, VERSION, BASE_VERSION]
        );
        assert_eq!(xex.image_base(), CONFIG[0]);
        assert_eq!(xex.image_size(), CONFIG[1]);
        assert_eq!(xex.entry_point(), Some(FUNCTIONS[0]));
        let imports = xex.imports().unwrap();
        assert_eq!(imports.len(), 2);
        assert!(imports.iter().all(|i| i.is_function()));
        assert_eq!(layout()[18..], [2688, 64]);
    }
}
