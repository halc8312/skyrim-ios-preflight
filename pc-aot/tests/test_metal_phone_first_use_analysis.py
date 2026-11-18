import json
from pathlib import Path
import sys
from tempfile import TemporaryDirectory
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "experiments" / "metal-compile"))
import first_use_results as analysis


def fixture(alternate=False, overlap_blocks=(0, 1)):
    first = 41 if alternate else 9
    panel = f"contention_v2_groups_{first}_{first + 31}"
    options = "e" * 64
    trials = []
    for index in range(32):
        block, pos = divmod(index, 4)
        condition = (((0, 1, 3, 2)[pos] + block) % 4) ^ (2 if alternate else 0)
        api, mode = analysis.APIS[condition // 2], analysis.MODES[condition % 2]
        lane = (condition + block // 2) % 4
        group = first + 4 * block + lane
        start = (index + 1) * 100_000_000
        active = mode == "contended" and block in overlap_blocks
        wait = (10 if active else 1) * 1_000_000 + (1_000_000 if condition >= 2 else 0)
        jobs = []
        for role in range(3):
            entered = start if role == 2 else start - (3 - role) * 1_000_000
            if role == 2:
                callback = start + wait
            else:
                # One idle prefetch finishes exactly when the urgent request enters.
                callback = start + (role + 1) * 2000 if active else start - role
            jobs.append({"role_index": role, "status": "passed", "oracle_verified": True,
                         "source_sha256": f"{group * 3 + role:064x}", "options_sha256": options,
                         "enqueued_ns": entered - 1000, "api_enter_ns": entered,
                         "api_return_ns": entered + 500, "callback_enter_ns": callback})
        trials.append({"status": "passed", "api": api, "mode": mode, "panel_namespace": panel,
                       "block_index": block, "position_index": pos, "condition_index": condition,
                       "condition_id": f"{api.removesuffix('_async')}_{mode}", "lane_index": lane,
                       "source_group": group, "first_assigned_in_run": True, "jobs": jobs,
                       "prefetch_qos": "user_initiated", "urgent_qos": "user_initiated",
                       "planned_overlap": mode == "contended", "actual_overlap_evaluated": True,
                       "prefetch_inflight_at_urgent_api_enter": 2 if active else 0,
                       "urgent_prefetch_overlap_ns": 4000 if active else 0,
                       "host_overlap_observed": active, "urgent_request_to_callback_ns": wait,
                       "urgent_queue_delay_ns": 1000, "urgent_enqueue_to_callback_ns": wait + 1000})
    control = {"schema": 2, "physical_iphone_verified": True, "status": "passed", "trials": trials,
               "metal4_first": alternate, "panel_namespace": panel, "source_group_first": first,
               "source_group_last": first + 31, "options_sha256": options}
    return {"status": "passed", "finished": True, "recovered_incomplete": False, "metal4_first": alternate,
            "comparison": {"schema": 5, "status": "passed", "contention_control_verified": True,
                           "metal4_first_requested": alternate, "contention_control": control}}


class FirstUseAnalysisTests(unittest.TestCase):
    def test_both_panels_use_all_assigned_observations(self):
        for alternate in (True, False):
            with self.subTest(alternate=alternate):
                summary = analysis.summarize(fixture(alternate))
                self.assertEqual(summary["normal_source_count"], 96)
                legacy, metal4 = [c for c in summary["cells"] if c["planned_mode"] == "contended"]
                self.assertEqual((legacy["scheduled"], legacy["observed_overlap"]), (8, 2))
                self.assertEqual(legacy["median_urgent_api_to_callback_ms"], 1)
                self.assertEqual(legacy["median_urgent_ms_when_overlap_observed"], 10)
                self.assertEqual(metal4["max_urgent_api_to_callback_ms"], 11)
                self.assertEqual(metal4["median_urgent_queue_wait_ms"], 0.001)
                self.assertEqual(summary["descriptive_contrasts"]["contended"], {
                    "blocks": 8, "median_within_block_metal4_minus_legacy_ms": 1,
                    "blocks_with_shorter_metal4_wait": 0})

    def test_zero_observed_overlap_still_has_a_primary_estimate(self):
        summary = analysis.summarize(fixture(overlap_blocks=()))
        for cell in summary["cells"]:
            self.assertEqual(cell["scheduled"], 8)
            self.assertEqual(cell["observed_overlap"], 0)
            self.assertIsNone(cell["median_urgent_ms_when_overlap_observed"])
            self.assertGreater(cell["median_urgent_api_to_callback_ms"], 0)

    def test_timing_and_assignments_checked_against_records(self):
        for change in ("overlap", "elapsed", "source", "lane", "qos", "panel", "result", "order"):
            with self.subTest(change=change):
                record = fixture()
                comp = record["comparison"]
                control = comp["contention_control"]
                row = control["trials"][1]
                if change == "overlap":
                    row["urgent_prefetch_overlap_ns"] = 6000  # Sum instead of union
                elif change == "elapsed":
                    row["urgent_request_to_callback_ns"] += 1000
                elif change == "source":
                    row["jobs"][2]["source_sha256"] = control["trials"][0]["jobs"][0]["source_sha256"]
                elif change == "lane":
                    row["lane_index"] = 3
                elif change == "qos":
                    row["prefetch_qos"] = "utility"
                elif change == "panel":
                    control["panel_namespace"] = "other"
                elif change == "result":
                    row["jobs"][2]["oracle_verified"] = False
                else:
                    control["trials"][0], control["trials"][1] = control["trials"][1], control["trials"][0]
                with self.assertRaises(ValueError):
                    analysis.summarize(record)

    def test_incomplete_and_earlier_formats_rejected(self):
        for change in ("partial", "legacy", "simulator", "different_selector", "missing_trial"):
            with self.subTest(change=change):
                record = fixture()
                comp = record["comparison"]
                if change == "partial":
                    record["recovered_incomplete"] = True
                elif change == "legacy":
                    comp["schema"] = 4
                elif change == "simulator":
                    comp["contention_control"]["physical_iphone_verified"] = False
                elif change == "different_selector":
                    comp["metal4_first_requested"] = True
                else:
                    comp["contention_control"]["trials"].pop()
                with self.assertRaises(ValueError):
                    analysis.summarize(record)

    def test_file_size_duplicate_keys_and_round_trip(self):
        with TemporaryDirectory() as directory:
            path = Path(directory) / "record.json"
            path.write_text(json.dumps(fixture()), encoding="utf-8")
            self.assertEqual(analysis.summarize(analysis.read_record(path))["normal_source_count"], 96)
            path.write_bytes(b'{"x":1,"x":2}')
            with self.assertRaisesRegex(ValueError, "Duplicate"):
                analysis.read_record(path)
            path.write_bytes(b" " * (analysis.MAX_BYTES + 1))
            with self.assertRaisesRegex(ValueError, "2 MiB"):
                analysis.read_record(path)


if __name__ == "__main__":
    unittest.main()
