//! Independent, bounded causal observations; legacy diagnostic schemas stay fixed.
use crate::{Check, progress_diagnostics::{Snapshot, FINAL_NAME, MAX_SECONDS}};
use serde_json::{Value, json};

pub const MAX_BYTES: usize = 512 * 1024;

pub fn record(point: Snapshot, observed_ms: u64, snapshot: Option<Value>) -> Check<(&'static str, Vec<u8>)> {
    let name = match (point.name, point.seconds) {
        ("diagnostic-30s.json", 30) => "causal-30s.json",
        ("diagnostic-120s.json", 120) => "causal-120s.json",
        ("diagnostic-300s.json", 300) => "causal-300s.json",
        (FINAL_NAME, second) if point.deadline_seconds.checked_sub(1) == Some(second) => "causal-final.json",
        _ => return Err("causal checkpoint identity invalid"),
    };
    if !(1..=MAX_SECONDS).contains(&point.deadline_seconds)
        || point.seconds >= point.deadline_seconds
        || observed_ms < u64::from(point.seconds) * 1000
        || observed_ms >= u64::from(point.deadline_seconds) * 1000 {
        return Err("causal checkpoint phase invalid");
    }
    let data = json!({
        "schema":1, "kind":"frame_stability_causal", "unit":"us",
        "clock":"guest_session_monotonic", "checkpoint_seconds":point.seconds,
        "deadline_seconds":point.deadline_seconds, "observed_guest_elapsed_ms":observed_ms,
        "snapshot_available":snapshot.is_some(), "snapshot":snapshot,
        "diagnostic_only":true, "game_or_texture_bytes_included":false,
        "domains_sampled_independently":true, "display_completion_measured":false,
    });
    let bytes = serde_json::to_vec(&data).map_err(|_| "causal serialization failed")?;
    if bytes.len() > MAX_BYTES { return Err("causal output bound exceeded"); }
    Ok((name, bytes))
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn exact_checkpoints_and_unavailable_values_are_bounded_and_explicit() {
        let points = crate::progress_diagnostics::schedule(600).unwrap();
        let mut names = std::collections::BTreeSet::new();
        for point in points {
            let (name, bytes) = record(point, u64::from(point.seconds) * 1000, None).unwrap();
            assert!(names.insert(name));
            let value: Value = serde_json::from_slice(&bytes).unwrap();
            assert_eq!(value["snapshot_available"], false);
            assert_eq!(value["snapshot"], Value::Null);
            assert_eq!(value["clock"], "guest_session_monotonic");
            assert!(record(point, 600_000, None).is_err());
        }
        assert_eq!(names.len(), 4);
        let point = crate::progress_diagnostics::schedule(120).unwrap()[0];
        assert!(record(point, 30_000, Some(json!("x".repeat(MAX_BYTES)))).is_err());
    }
}
