//! Bounded numeric evidence from the owned guest fatal callback. These flags
//! classify a target against already validated, immutable metadata; they do not
//! identify a valid new entry or make an unregistered target executable.
pub const DIAGNOSTIC_SCHEMA: u32 = 2;
pub const MISSING_FUNCTION: u32 = 3;
pub const HAS_U32_TARGET: u32 = 1;
pub const ALIGNED: u32 = 2;
pub const IN_CODE: u32 = 4;
pub const REGISTERED: u32 = 8;
pub const ZERO: u32 = 16;
pub const REJECTED_HIGH_DETAIL: u32 = 32;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct GuestFatal {
    pub category: u32,
    pub target: u32,
    pub flags: u32,
}

impl GuestFatal {
    /// Only category 3 defines `detail` as a guest u32 dispatch target. Other
    /// reasons can pack unrelated data, so their detail is never persisted.
    /// `functions` is the validated strictly sorted owned copy, not guest RAM.
    pub fn from_callback(
        category: u32,
        detail: u64,
        code_base: u32,
        code_size: u32,
        functions: &[u32],
    ) -> Self {
        let mut evidence = Self {
            category,
            ..Self::default()
        };
        if category != MISSING_FUNCTION {
            return evidence;
        }
        let Ok(target) = u32::try_from(detail) else {
            evidence.flags = REJECTED_HIGH_DETAIL;
            return evidence;
        };
        evidence.target = target;
        evidence.flags = HAS_U32_TARGET;
        if target & 3 == 0 {
            evidence.flags |= ALIGNED;
        }
        if u64::from(target)
            .checked_sub(u64::from(code_base))
            .is_some_and(|offset| offset < u64::from(code_size))
        {
            evidence.flags |= IN_CODE;
        }
        if evidence.flags & (ALIGNED | IN_CODE) == (ALIGNED | IN_CODE)
            && functions.binary_search(&target).is_ok()
        {
            evidence.flags |= REGISTERED;
        }
        if target == 0 {
            evidence.flags |= ZERO;
        }
        evidence
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn missing_target_classifies_exact_membership_and_alignment() {
        let functions = [0x1000, 0x1008];
        let registered = GuestFatal::from_callback(3, 0x1008, 0x1000, 16, &functions);
        assert_eq!(
            registered,
            GuestFatal {
                category: 3,
                target: 0x1008,
                flags: HAS_U32_TARGET | ALIGNED | IN_CODE | REGISTERED
            }
        );
        assert_eq!(
            GuestFatal::from_callback(3, 0x1004, 0x1000, 16, &functions).flags,
            HAS_U32_TARGET | ALIGNED | IN_CODE
        );
        assert_eq!(
            GuestFatal::from_callback(3, 0x1007, 0x1000, 16, &functions).flags,
            HAS_U32_TARGET | IN_CODE
        );
        assert_eq!(
            GuestFatal::from_callback(3, 0x1010, 0x1000, 16, &functions).flags,
            HAS_U32_TARGET | ALIGNED
        );
    }

    #[test]
    fn unrelated_details_and_high_bits_are_not_truncated_into_targets() {
        let packed_import = 0x89ab_cdef_0000_0003;
        assert_eq!(
            GuestFatal::from_callback(2, packed_import, 0, u32::MAX, &[3]),
            GuestFatal {
                category: 2,
                target: 0,
                flags: 0
            }
        );
        assert_eq!(
            GuestFatal::from_callback(3, 0x1_0000_1000, 0, u32::MAX, &[0x1000]),
            GuestFatal {
                category: 3,
                target: 0,
                flags: REJECTED_HIGH_DETAIL
            }
        );
        assert_eq!(
            GuestFatal::from_callback(3, 0xf123_45a7, 0xf123_4000, 0x1000, &[]).target,
            0xf123_45a7
        );
    }

    #[test]
    fn zero_and_high_address_boundaries_are_explicit() {
        assert_eq!(
            GuestFatal::from_callback(3, 0, 0x1000, 16, &[]).flags,
            HAS_U32_TARGET | ALIGNED | ZERO
        );
        assert_eq!(
            GuestFatal::from_callback(3, u64::from(u32::MAX), u32::MAX - 3, 4, &[]).flags,
            HAS_U32_TARGET | IN_CODE
        );
        assert_eq!(
            GuestFatal::from_callback(3, 0, u32::MAX - 3, 4, &[]).flags,
            HAS_U32_TARGET | ALIGNED | ZERO
        );
        assert_eq!(
            GuestFatal::from_callback(3, 0x1000, 0x1000, 0, &[0x1000]).flags,
            HAS_U32_TARGET | ALIGNED
        );
    }
}
