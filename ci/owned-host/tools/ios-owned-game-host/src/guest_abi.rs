//! The trusted metadata selects exactly one ABI. Missing or incompatible V2
//! symbols never cause a retry through V1; module observations cannot choose it.
use crate::{Check, abi, abi_v2};
use std::{ffi::CStr, mem::size_of};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum GuestAbi {
    V1,
    V2,
}

impl GuestAbi {
    pub fn from_version(version: u32) -> Check<Self> {
        match version {
            abi::VERSION => Ok(Self::V1),
            abi_v2::VERSION => Ok(Self::V2),
            _ => Err("binding_abi_version"),
        }
    }

    pub const fn version(self) -> u32 {
        match self {
            Self::V1 => abi::VERSION,
            Self::V2 => abi_v2::VERSION,
        }
    }

    pub const fn requirements_symbol(self) -> &'static CStr {
        match self {
            Self::V1 => c"PG_GetRequirementsV1",
            Self::V2 => c"PG_GetRequirementsV2",
        }
    }

    pub const fn bind_symbol(self) -> &'static CStr {
        match self {
            Self::V1 => c"PG_BindHostV1",
            Self::V2 => c"PG_BindHostV2",
        }
    }

    pub const fn layout_words(self) -> usize {
        match self {
            Self::V1 => 49,
            Self::V2 => 51,
        }
    }

    pub fn verify_native_layout(self, returned: i32, words: &[u64]) -> Check<()> {
        let matches = match self {
            Self::V1 => words == abi::layout49(),
            Self::V2 => words == abi_v2::layout51(),
        };
        if returned != self.layout_words() as i32 || !matches {
            return Err("SDK/Rust C ABI layout differs");
        }
        Ok(())
    }

    /// V2 deliberately retains the 216-byte requirements layout. Version must
    /// still match the selected metadata, independently of context equality.
    pub fn verify_requirements(self, r: &abi::PGRequirementsV1, context: [u64; 20]) -> Check<()> {
        if r.abi_version != self.version()
            || r.struct_size as usize != size_of::<abi::PGRequirementsV1>()
            || r.pointer_bits != 64
            || r.little_endian != 1
            || r.plain_arm64 != 1
            || r.reserved != 0
            || r.context_layout != context
            || r.ea_offset_count != 4096
            || r.written_count != 1048576
            || r.jump_buffer_size == 0
            || !r.jump_buffer_alignment.is_power_of_two()
        {
            return Err("guest requirements differ from actual runtime");
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_explicit_versions_select_their_exact_symbol_pair() {
        for (version, expected, requirements, bind) in [
            (
                0x10000,
                GuestAbi::V1,
                c"PG_GetRequirementsV1",
                c"PG_BindHostV1",
            ),
            (
                0x20000,
                GuestAbi::V2,
                c"PG_GetRequirementsV2",
                c"PG_BindHostV2",
            ),
        ] {
            let selected = GuestAbi::from_version(version).unwrap();
            assert_eq!(selected, expected);
            assert_eq!(selected.version(), version);
            assert_eq!(selected.requirements_symbol(), requirements);
            assert_eq!(selected.bind_symbol(), bind);
        }
        for version in [0, 1, 2, 0x10001, 0x1ffff, 0x20001, 0x30000, u32::MAX] {
            assert_eq!(GuestAbi::from_version(version), Err("binding_abi_version"));
        }
    }

    #[test]
    fn native_layout_refuses_crossed_versions_counts_and_field_offsets() {
        let v1 = abi::layout49();
        let v2 = abi_v2::layout51();
        GuestAbi::V1.verify_native_layout(49, &v1).unwrap();
        GuestAbi::V2.verify_native_layout(51, &v2).unwrap();
        assert!(GuestAbi::V1.verify_native_layout(51, &v2).is_err());
        assert!(GuestAbi::V2.verify_native_layout(49, &v1).is_err());
        for (selected, words) in [(GuestAbi::V1, v1.to_vec()), (GuestAbi::V2, v2.to_vec())] {
            for returned in [-1, 0, selected.layout_words() as i32 - 1] {
                assert!(selected.verify_native_layout(returned, &words).is_err());
            }
            assert!(
                selected
                    .verify_native_layout(words.len() as i32, &words[..words.len() - 1])
                    .is_err()
            );
            for index in 0..words.len() {
                let mut changed = words.clone();
                changed[index] ^= 1;
                assert!(
                    selected
                        .verify_native_layout(words.len() as i32, &changed)
                        .is_err()
                );
            }
        }
    }

    #[test]
    fn requirements_preserve_all_existing_checks_for_both_versions() {
        let context = [16; 20]; // Authored equality oracle, not a claimed PPC layout.
        for selected in [GuestAbi::V1, GuestAbi::V2] {
            let valid = abi::PGRequirementsV1 {
                abi_version: selected.version(),
                struct_size: 216,
                pointer_bits: 64,
                little_endian: 1,
                plain_arm64: 1,
                context_layout: context,
                ea_offset_count: 4096,
                written_count: 1048576,
                jump_buffer_size: 192,
                jump_buffer_alignment: 4,
                ..Default::default()
            };
            selected.verify_requirements(&valid, context).unwrap();
            let other = if selected == GuestAbi::V1 {
                GuestAbi::V2
            } else {
                GuestAbi::V1
            };
            assert!(other.verify_requirements(&valid, context).is_err());
            let mutations: [fn(&mut abi::PGRequirementsV1); 11] = [
                |r| r.abi_version = 2,
                |r| r.struct_size = 215,
                |r| r.pointer_bits = 32,
                |r| r.little_endian = 0,
                |r| r.plain_arm64 = 0,
                |r| r.reserved = 1,
                |r| r.context_layout[0] ^= 1,
                |r| r.ea_offset_count -= 1,
                |r| r.written_count -= 1,
                |r| r.jump_buffer_size = 0,
                |r| r.jump_buffer_alignment = 3,
            ];
            for mutate in mutations {
                let mut changed = valid;
                mutate(&mut changed);
                assert!(selected.verify_requirements(&changed, context).is_err());
            }
        }
    }
}
