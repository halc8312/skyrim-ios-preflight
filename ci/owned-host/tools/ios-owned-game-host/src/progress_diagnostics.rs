//! Bounded numeric progress records. Scheduled time is separate from observed
//! elapsed time; unavailable renderer measurements are omitted, never invented.
use crate::{Check, lifecycle, renderer_policy::OHRendererStatusV1};
use std::mem::size_of;

pub const DIAGNOSTIC_SCHEMA: u32 = 4;
pub const MAX_SECONDS: u32 = 600;
const CHECKPOINTS: [(u32, &str); 3] = [
    (30, "diagnostic-30s.json"),
    (120, "diagnostic-120s.json"),
    (300, "diagnostic-300s.json"),
];
pub const FINAL_NAME: &str = "diagnostic-final.json";

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Snapshot {
    pub seconds: u32,
    pub deadline_seconds: u32,
    pub name: &'static str,
}

/// At most four distinct filenames, all strictly before the process deadline.
/// A checkpoint and final record may share a scheduled second (for example,31).
pub fn schedule(seconds: u32) -> Check<Vec<Snapshot>> {
    if !(1..=MAX_SECONDS).contains(&seconds) {
        return Err("progress duration must be within 1..600 seconds");
    }
    let mut points: Vec<_> = CHECKPOINTS
        .into_iter()
        .filter(|(at, _)| *at < seconds)
        .map(|(at, name)| Snapshot {
            seconds: at,
            deadline_seconds: seconds,
            name,
        })
        .collect();
    points.push(Snapshot {
        seconds: seconds - 1,
        deadline_seconds: seconds,
        name: FINAL_NAME,
    });
    Ok(points)
}

impl Snapshot {
    pub fn append_numeric_fields(
        self,
        value: &mut serde_json::Value,
        renderer: Option<&OHRendererStatusV1>,
    ) -> Check<()> {
        let valid_name = if self.name == FINAL_NAME {
            self.deadline_seconds.checked_sub(1) == Some(self.seconds)
        } else {
            CHECKPOINTS.contains(&(self.seconds, self.name))
        };
        let elapsed = value["elapsed_ms"]
            .as_u64()
            .ok_or("progress elapsed time absent")?;
        if !(1..=MAX_SECONDS).contains(&self.deadline_seconds)
            || self.seconds >= self.deadline_seconds
            || !valid_name
            || value["diagnostic_schema"].as_u64() != Some(2)
            || !matches!(value["state"].as_u64(), Some(4..=6))
            || elapsed < u64::from(self.seconds) * 1000
            || elapsed >= u64::from(self.deadline_seconds) * 1000
        {
            return Err("progress phase or schedule invalid");
        }
        // Failed/stopped renderers remain measurable. Initialization/ownership
        // and the existing C layout must be valid before exposing their counts.
        let renderer = renderer.filter(|s| {
            s.abi_version == lifecycle::VERSION
                && s.struct_bytes as usize == size_of::<OHRendererStatusV1>()
                && s.attachment == 2
                && s.initialized == 1
                && s.stopped <= 1
                && s.failed <= 1
        });
        value["diagnostic_schema"] = DIAGNOSTIC_SCHEMA.into();
        value["deadline_seconds"] = self.deadline_seconds.into();
        value["snapshot_seconds"] = self.seconds.into();
        value["renderer_available"] = u32::from(renderer.is_some()).into();
        if let Some(s) = renderer {
            value["decoded_frames"] = s.decoded_frames.into();
            value["gpu_errors"] = s.gpu_errors.into();
            value["renderer_failed"] = s.failed.into();
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn base(state: u32, elapsed_ms: u64) -> serde_json::Value {
        serde_json::json!({"version":65536,"state":state,"failure":0,"module":0,"ordinal":0,
            "exit_code":0,"elapsed_ms":elapsed_ms,"diagnostic_schema":2,"guest_fatal_category":0,
            "guest_target":0,"guest_target_flags":0,"ready_to_play":false,
            "signature_authenticated":false,"iphone_game_verified":false})
    }

    fn renderer() -> OHRendererStatusV1 {
        OHRendererStatusV1 {
            abi_version: lifecycle::VERSION,
            struct_bytes: size_of::<OHRendererStatusV1>() as u32,
            attachment: 2,
            initialized: 1,
            decoded_frames: u64::MAX,
            gpu_errors: 7,
            ..Default::default()
        }
    }

    #[test]
    fn schedule_boundaries_and_fixed_names() {
        for (deadline, expected) in [
            (1, vec![0]),
            (30, vec![29]),
            (31, vec![30, 30]),
            (120, vec![30, 119]),
            (121, vec![30, 120, 120]),
            (300, vec![30, 120, 299]),
            (301, vec![30, 120, 300, 300]),
            (600, vec![30, 120, 300, 599]),
        ] {
            let points = schedule(deadline).unwrap();
            assert_eq!(
                points.iter().map(|p| p.seconds).collect::<Vec<_>>(),
                expected
            );
            assert!(
                points
                    .iter()
                    .all(|p| p.seconds < deadline && p.deadline_seconds == deadline)
            );
            let names: std::collections::HashSet<_> = points.iter().map(|p| p.name).collect();
            assert_eq!(names.len(), points.len());
            assert_eq!(points.last().unwrap().name, FINAL_NAME);
        }
        for deadline in [0, 601, u32::MAX] {
            assert!(schedule(deadline).is_err());
        }
    }

    #[test]
    fn available_failed_and_unavailable_renderer_are_distinct() {
        let point = schedule(600).unwrap()[0];
        for state in [lifecycle::RUNNING, lifecycle::EXITED, lifecycle::FAILED] {
            let mut value = base(state, 30_001);
            let mut status = renderer();
            status.failed = 1;
            status.stopped = 1;
            point
                .append_numeric_fields(&mut value, Some(&status))
                .unwrap();
            assert_eq!(value["diagnostic_schema"], 4);
            assert_eq!(value["renderer_available"], 1);
            assert_eq!(value["decoded_frames"], u64::MAX);
            assert_eq!(value["gpu_errors"], 7);
            assert_eq!(value["renderer_failed"], 1);
            assert_eq!(value["state"], state);
        }
        let mut invalid = renderer();
        invalid.attachment = 1;
        let mut wrong_abi = renderer();
        wrong_abi.struct_bytes = 0;
        let mut bad_flags = renderer();
        bad_flags.failed = 2;
        for status in [None, Some(&invalid), Some(&wrong_abi), Some(&bad_flags)] {
            let mut value = base(lifecycle::RUNNING, 30_000);
            point.append_numeric_fields(&mut value, status).unwrap();
            assert_eq!(value["renderer_available"], 0);
            for key in ["decoded_frames", "gpu_errors", "renderer_failed"] {
                assert!(value.get(key).is_none());
            }
        }
    }

    #[test]
    fn invalid_phase_time_or_filename_never_changes_base_record() {
        let point = schedule(600).unwrap()[0];
        for (point, state, elapsed) in [
            (point, lifecycle::PREPARED, 30_000),
            (point, lifecycle::RUNNING, 29_999),
            (point, lifecycle::RUNNING, 600_000),
            (
                Snapshot {
                    name: "first-failure.json",
                    ..point
                },
                lifecycle::RUNNING,
                30_000,
            ),
            (
                Snapshot {
                    seconds: 120,
                    ..point
                },
                lifecycle::RUNNING,
                120_000,
            ),
        ] {
            let mut value = base(state, elapsed);
            let before = value.clone();
            assert!(point.append_numeric_fields(&mut value, None).is_err());
            assert_eq!(value, before);
        }
        let mut value = base(lifecycle::RUNNING, 0);
        schedule(1).unwrap()[0]
            .append_numeric_fields(&mut value, None)
            .unwrap();
        assert_eq!(value["snapshot_seconds"], 0);
    }
}
