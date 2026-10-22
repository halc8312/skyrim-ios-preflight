//! Numeric ancestry only: raw process PCs never enter the serialized record.
//! A successful capture is not proof of a complete stack or the exact indirect
//! instruction: optimizations can inline frames or replace them with tail calls.
pub const MAX_CAPTURE: usize = 32;
pub const MAX_KEPT: usize = 8;
pub const CAPTURE_RETURNED: u32 = 1;
pub const CAPTURE_INVALID: u32 = 2;
pub const DIAGNOSTIC_SCHEMA: u32 = 3;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct NativeTrace {
    pub status: u32,
    pub captured: u32,
    pub kept: u32,
    pub offsets: [u64; MAX_KEPT],
}
impl NativeTrace {
    /// `verified_rx` must refer only to the already content-bound, retained
    /// image. Return PCs are checked at pc-1 so a return just beyond an RX
    /// segment's final instruction can be classified without reading that PC.
    /// No symbols, stack memory, pointed-to instructions or OS images are read.
    pub fn filter(
        raw: &[usize],
        returned: i32,
        header: usize,
        verified_rx: impl Fn(usize) -> bool,
    ) -> Self {
        let mut result = Self {
            status: CAPTURE_INVALID,
            captured: 0,
            kept: 0,
            offsets: [0; MAX_KEPT],
        };
        let Ok(count) = usize::try_from(returned) else {
            return result;
        };
        if count > MAX_CAPTURE || count > raw.len() || header == 0 {
            return result;
        }
        result.status = CAPTURE_RETURNED;
        result.captured = count as u32;
        for &pc in &raw[..count] {
            if pc == 0 || pc & 3 != 0 {
                continue;
            }
            let Some(offset) = pc.checked_sub(header) else {
                continue;
            };
            if offset == 0 || offset & 3 != 0 || !verified_rx(pc - 1) {
                continue;
            }
            // Do not deduplicate: repeated return PCs preserve recursion order.
            result.offsets[result.kept as usize] = offset as u64;
            result.kept += 1;
            if result.kept as usize == MAX_KEPT {
                break;
            }
        }
        result
    }

    pub fn append_numeric_fields(&self, value: &mut serde_json::Value) {
        value["diagnostic_schema"] = DIAGNOSTIC_SCHEMA.into();
        value["native_trace_status"] = self.status.into();
        value["native_trace_captured"] = self.captured.into();
        value["native_trace_kept"] = self.kept.into();
        for (index, offset) in self.offsets.iter().enumerate() {
            value[format!("native_pc{index}")] = (*offset).into();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn filtered_offsets_preserve_recursion_order_and_segment_end() {
        let header = 0x1000;
        let raw = [0, 0x1000, 0x2001, 0x5000, 0x2004, 0x2004, 0x2100];
        let trace = NativeTrace::filter(&raw, raw.len() as i32, header, |pc| {
            (0x2000..0x2100).contains(&pc)
        });
        assert_eq!(trace.status, CAPTURE_RETURNED);
        assert_eq!(trace.captured, 7);
        assert_eq!(trace.kept, 3);
        assert_eq!(trace.offsets, [0x1004, 0x1004, 0x1100, 0, 0, 0, 0, 0]);
        let more = [0x2004usize; MAX_CAPTURE];
        let trace = NativeTrace::filter(&more, MAX_CAPTURE as i32, header, |_| true);
        assert_eq!(trace.kept as usize, MAX_KEPT);
        assert_eq!(trace.offsets, [0x1004; MAX_KEPT]);
    }

    #[test]
    fn invalid_capture_and_arithmetic_do_not_reach_image_predicate() {
        for (raw, returned, header) in [
            (&[4usize][..], -1, 4),
            (&[4usize][..], 2, 4),
            (&[4usize; 33][..], 33, 4),
            (&[4usize][..], 1, 0),
        ] {
            let trace = NativeTrace::filter(raw, returned, header, |_| {
                panic!("invalid capture must not classify pointers")
            });
            assert_eq!(
                (trace.status, trace.captured, trace.kept),
                (CAPTURE_INVALID, 0, 0)
            );
            assert_eq!(trace.offsets, [0; MAX_KEPT]);
        }
        let trace = NativeTrace::filter(&[0, 4, 8, usize::MAX], 4, 0x1000, |_| {
            panic!("unaligned or underflowing PC must not reach image")
        });
        assert_eq!(
            (trace.status, trace.captured, trace.kept),
            (CAPTURE_RETURNED, 4, 0)
        );
        assert_eq!(
            NativeTrace::filter(&[], 0, 4, |_| false).status,
            CAPTURE_RETURNED
        );
    }

    #[test]
    fn serialization_has_only_fixed_numeric_fields_and_zero_unused_slots() {
        let trace = NativeTrace::filter(&[0x2004], 1, 0x1000, |_| true);
        let mut value = serde_json::json!({"failure":22});
        trace.append_numeric_fields(&mut value);
        let object = value.as_object().unwrap();
        assert_eq!(object.len(), 13);
        assert!(object.values().all(|v| v.as_u64().is_some()));
        assert_eq!(object["native_pc0"].as_u64(), Some(0x1004));
        assert_eq!(object["native_pc7"].as_u64(), Some(0));
        assert!(!serde_json::to_string(&value).unwrap().contains("8196"));
    }
}
