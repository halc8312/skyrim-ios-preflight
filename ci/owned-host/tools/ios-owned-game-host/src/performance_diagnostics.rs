//! Four bounded diagnostic-only sidecars, separate from the established schema.
use crate::{Check, progress_diagnostics::{Snapshot, FINAL_NAME, MAX_SECONDS}};
use serde_json::{Value, json};

pub fn record(point: Snapshot, observed_ms: u64, renderer: Option<Value>) -> Check<(&'static str, Value)> {
    let name = match (point.name, point.seconds) {
        ("diagnostic-30s.json", 30) => "performance-30s.json",
        ("diagnostic-120s.json", 120) => "performance-120s.json",
        ("diagnostic-300s.json", 300) => "performance-300s.json",
        (FINAL_NAME, second) if point.deadline_seconds.checked_sub(1) == Some(second) => "performance-final.json",
        _ => return Err("performance checkpoint identity invalid"),
    };
    if !(1..=MAX_SECONDS).contains(&point.deadline_seconds)
        || point.seconds >= point.deadline_seconds
        || observed_ms < u64::from(point.seconds) * 1000
        || observed_ms >= u64::from(point.deadline_seconds) * 1000 {
        return Err("performance checkpoint phase invalid");
    }
    if let Some(sample) = &renderer {
        let span = sample["window_ms"].as_u64().ok_or("performance window unavailable")?;
        let end = sample["sample_end_elapsed_ms"].as_u64().ok_or("performance epoch unavailable")?;
        let age = sample["age_ms"].as_u64().ok_or("performance age unavailable")?;
        if span == 0 || end < span || sample["stale"].as_bool() != Some(age > 10_000) {
            return Err("performance sample timing invalid");
        }
    }
    Ok((name, json!({
        "schema":1, "kind":"coarse_renderer_performance", "checkpoint_seconds":point.seconds,
        "deadline_seconds":point.deadline_seconds,
        "observed_guest_elapsed_ms":observed_ms, "renderer_clock_epoch":"renderer_loop_initialization",
        "snapshot_available":renderer.is_some(), "renderer":renderer,
        "timing_kind":"host_elapsed_window", "operation_times_may_overlap":true,
        "cpu_thread_time_measured":false, "gpu_execution_time_measured":false,
        "game_or_texture_bytes_included":false, "diagnostic_only":true,
    })))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::progress_diagnostics::schedule;

    #[test]
    fn checkpoints_are_bounded_and_unavailable_is_not_a_zero_measurement() {
        let points = schedule(600).unwrap();
        let mut names = std::collections::BTreeSet::new();
        for point in points {
            let (name, value) = record(point, u64::from(point.seconds) * 1000, None).unwrap();
            assert!(names.insert(name));
            assert_eq!(value["snapshot_available"], false);
            assert!(value["renderer"].is_null());
            assert_eq!(value["cpu_thread_time_measured"], false);
            assert_eq!(value["gpu_execution_time_measured"], false);
            assert!(record(point, u64::from(point.deadline_seconds) * 1000, None).is_err());
        }
        assert_eq!(names.len(), 4);
        let bad = Snapshot { name:"../game.json", seconds:30, deadline_seconds:600 };
        assert!(record(bad, 30_000, None).is_err());
    }

    #[test]
    fn renderer_and_guest_epochs_are_distinct_and_staleness_is_explicit() {
        let point = schedule(600).unwrap()[0];
        let sample = json!({"window_ms":5000,"sample_end_elapsed_ms":45000,"age_ms":10001,"stale":true});
        let (_, value) = record(point, 30_000, Some(sample.clone())).unwrap();
        assert_eq!(value["renderer"], sample);
        let mut invalid = sample;
        invalid["stale"] = false.into();
        assert!(record(point, 30_000, Some(invalid)).is_err());
    }
}
