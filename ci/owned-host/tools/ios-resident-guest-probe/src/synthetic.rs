//! The sole accepted image is generated in memory from public, self-authored
//! PE/XEX fixture data. It is not a game or decrypted from any owned input.
use xex::builder::*;
pub const CONFIG: [u32; 4] = [0x82000000, 0x2000, 0x82001000, 0x10];
pub const FUNCTIONS: [u32; 3] = [0x82001000, 0x82001004, 0x82001008];
pub const FREQUENCY: u32 = CONFIG[0] + 0x1800;
pub const CLEANUP: u32 = CONFIG[0] + 0x1804;
pub fn xex() -> xex::Xex {
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
    let bytes = XexBuilder {
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
        execution_info: None,
        basic_compression: false,
    }
    .build();
    xex::Xex::parse_owned(bytes).expect("self-authored public XEX fixture is malformed")
}
