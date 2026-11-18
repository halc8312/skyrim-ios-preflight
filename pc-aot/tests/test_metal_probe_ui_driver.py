"""Authored stale-result controls; these do not render UIKit or exercise Metal."""
import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("metal_probe_ui_driver", ROOT / "scripts/check-metal-probe-ios-mac.py")
driver = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(driver)


class PreviewEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "record.json"
        self.record = {"finished": True, "status": "unsupported", "comparison": {
            "schema": 3, "total_cases": 12, "total_samples": 192, "elapsed_ns": 2000,
            "status": "unsupported", "host_build": {"source_revision": "authored-revision"},
            "physical_iphone_verified": False, "gpu_execution_verified": False,
            "metal4_library_verified": False, "samples": [],
            "progress_timing": {
                "snapshot_serialization": {"count": 1, "total_ns": 100, "max_ns": 100},
                "synchronous_callback": {"count": 1, "total_ns": 1000, "max_ns": 1000}},
            "host_timing": {"initial_store_included": False, "final_store_included": False,
                "progress_store": {"count": 1, "successful_count": 1, "failed_count": 0,
                                   "total_ns": 900, "max_ns": 900}}}}

    def write(self, record=None):
        self.path.write_text(json.dumps(record or self.record))

    def test_previous_completed_result_cannot_satisfy_current_run(self):
        self.write()
        self.assertIsNone(driver.fresh_unsupported_record(self.path, {self.path}, "authored-revision"))
        self.assertEqual(driver.fresh_unsupported_record(self.path, set(), "authored-revision"), self.record)

    def test_result_from_different_build_cannot_satisfy_current_run(self):
        self.write()
        self.assertIsNone(driver.fresh_unsupported_record(self.path, set(), "new-revision"))

    def test_running_and_truncated_records_do_not_pass(self):
        self.write({**self.record, "finished": False})
        self.assertIsNone(driver.fresh_unsupported_record(self.path, set(), "authored-revision"))
        self.path.write_text('{"finished":')
        self.assertIsNone(driver.fresh_unsupported_record(self.path, set(), "authored-revision"))

    def test_false_device_claims_and_missing_proof_fields_are_rejected(self):
        for value in (True, None, 0):
            with self.subTest(value=value):
                bad = copy.deepcopy(self.record)
                bad["comparison"]["physical_iphone_verified"] = value
                self.write(bad)
                with self.assertRaises(RuntimeError):
                    driver.fresh_unsupported_record(self.path, set(), "authored-revision")

    def test_missing_or_uncompleted_progress_timing_is_rejected(self):
        for change in ("missing", "wrong_count", "invalid_duration", "old_schema"):
            with self.subTest(change=change):
                bad = copy.deepcopy(self.record)
                if change == "missing": del bad["comparison"]["host_timing"]
                elif change == "wrong_count": bad["comparison"]["progress_timing"]["synchronous_callback"]["count"] = 0
                elif change == "invalid_duration": bad["comparison"]["host_timing"]["progress_store"]["total_ns"] = True
                else: bad["comparison"]["schema"] = 2
                self.write(bad)
                with self.assertRaises(RuntimeError):
                    driver.fresh_unsupported_record(self.path, set(), "authored-revision")

    def test_nested_store_scope_and_runtime_elapsed_are_enforced(self):
        for change in ("store_outside_callback", "callback_outside_runtime", "final_included", "failed_store"):
            with self.subTest(change=change):
                bad = copy.deepcopy(self.record)
                c = bad["comparison"]
                if change == "store_outside_callback":
                    c["host_timing"]["progress_store"].update(total_ns=1500, max_ns=1500)
                elif change == "callback_outside_runtime": c["elapsed_ns"] = 1000
                elif change == "final_included": c["host_timing"]["final_store_included"] = True
                else: c["host_timing"]["progress_store"].update(successful_count=0, failed_count=1)
                self.write(bad)
                with self.assertRaises(RuntimeError):
                    driver.fresh_unsupported_record(self.path, set(), "authored-revision")


if __name__ == "__main__":
    unittest.main()
