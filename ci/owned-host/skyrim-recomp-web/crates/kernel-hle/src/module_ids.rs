//! Dependency-free import-library identity shared by runtime and tools.
use std::collections::HashMap;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct ModuleId(pub u32);

impl ModuleId {
    pub const XBOXKRNL: ModuleId = ModuleId(0);
    pub const XAM: ModuleId = ModuleId(1);
    /// Ids from 2 up are other import libraries, numbered by guest-sys in the
    /// order they appear in the XEX.
    pub const FIRST_OTHER: u32 = 2;

    pub fn for_library(name: &str) -> Option<ModuleId> {
        match name.to_ascii_lowercase().as_str() {
            "xboxkrnl.exe" => Some(ModuleId::XBOXKRNL),
            "xam.xex" => Some(ModuleId::XAM),
            _ => None,
        }
    }
}

/// Module ids for a XEX's import libraries, in library order: xboxkrnl.exe is 0,
/// xam.xex is 1, any other library gets the next id from 2 upwards. guest-sys
/// uses this same function when generating the import thunks.
pub fn assign_module_ids(library_names: &[String]) -> Vec<ModuleId> {
    let mut next = ModuleId::FIRST_OTHER;
    let mut others: HashMap<String, ModuleId> = HashMap::new();
    library_names
        .iter()
        .map(|n| {
            ModuleId::for_library(n).unwrap_or_else(|| {
                *others.entry(n.to_ascii_lowercase()).or_insert_with(|| {
                    let id = ModuleId(next);
                    next += 1;
                    id
                })
            })
        })
        .collect()
}
