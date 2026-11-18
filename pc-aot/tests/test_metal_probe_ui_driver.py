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
            "schema": 5, "total_cases": 12, "total_samples": 192, "elapsed_ns": 2000,
            "status": "unsupported", "host_build": {"source_revision": "authored-revision"},
            "physical_iphone_verified": False, "gpu_execution_verified": False,
            "metal4_library_verified": False, "samples": [],
            "serial_comparison_verified": False, "contention_control_verified": False,
            "contention_panel_requested": "primary",
            "contention_selector_scope":
                "both APIs in each run; alternate selects a disjoint predeclared source panel and changes starting API; "
                "repeating a selector reuses its sources; OS/compiler cache is uncontrolled",
            "contention_control": {"status": "not_run", "scope": "separate authored MRC scheduling control"},
            "progress_timing": {
                "considered_count": 1, "completed_count": 1, "skipped_count": 0,
                "incomplete_count": 0, "callback_present": True,
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
        for field in ("physical_iphone_verified", "gpu_execution_verified", "metal4_library_verified",
                      "serial_comparison_verified", "contention_control_verified"):
            for value in (True, None, 0):
                with self.subTest(field=field, value=value):
                    bad = copy.deepcopy(self.record)
                    bad["comparison"][field] = value
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
                else: bad["comparison"]["schema"] = 4
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

    def test_current_schema_and_sample_plan_require_exact_integer_fields(self):
        for field, valid in (("schema", 5), ("total_cases", 12), ("total_samples", 192)):
            for value in (None, str(valid), float(valid), True, valid - 1, valid + 1):
                bad = copy.deepcopy(self.record)
                bad["comparison"][field] = value
                self.write(bad)
                with self.subTest(field=field, value=value), self.assertRaisesRegex(RuntimeError, "current balanced"):
                    driver.fresh_unsupported_record(self.path, set(), "authored-revision")

    def test_smoke_panel_and_cache_scope_are_required(self):
        for field, values in (("contention_panel_requested", (None, "alternate", "fresh", True)),
                              ("contention_selector_scope", (None, "guaranteed cold", "", True))):
            for value in values:
                bad = copy.deepcopy(self.record)
                bad["comparison"][field] = value
                self.write(bad)
                with self.subTest(field=field, value=value), self.assertRaisesRegex(RuntimeError, "panel and cache scope"):
                    driver.fresh_unsupported_record(self.path, set(), "authored-revision")

    def test_checkpoint_disposition_counters_are_exact_for_unsupported_path(self):
        changes = ({"considered_count": 2}, {"completed_count": True}, {"skipped_count": -1},
                   {"callback_present": False}, {"callback_present": 1}, {"incomplete_count": None},
                   {"considered_count": 2, "skipped_count": 1},
                   {"considered_count": 2, "incomplete_count": 1})
        for change in changes:
            bad = copy.deepcopy(self.record)
            bad["comparison"]["progress_timing"].update(change)
            self.write(bad)
            with self.subTest(change=change), self.assertRaisesRegex(RuntimeError, "disposition"):
                driver.fresh_unsupported_record(self.path, set(), "authored-revision")

    def test_simulator_cannot_claim_independent_contention_execution(self):
        for value in (None, {}, {"status": "passed", "scope": "separate authored MRC scheduling control"},
                      {"status": "not_run", "scope": "unrelated"}):
            bad = copy.deepcopy(self.record)
            bad["comparison"]["contention_control"] = value
            self.write(bad)
            with self.subTest(value=value), self.assertRaisesRegex(RuntimeError, "contention control"):
                driver.fresh_unsupported_record(self.path, set(), "authored-revision")


class AuthoredLayoutStorageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.records = Path(self.temp.name) / "MetalProbe"
        self.record = self.records / "retained-run" / "result.json"
        self.record.parent.mkdir(parents=True)
        self.record.write_text('{"finished":true,"comparison":{"schema":4}}')
        self.before = driver.saved_record_fingerprints(self.records)

    def test_unchanged_old_report_remains_acceptable_for_layout_only(self):
        driver.require_unchanged_records(self.records, self.before)
        self.assertIn("retained-run/", self.before)
        self.assertEqual(len(self.before["retained-run/result.json"]), 64)

    def test_layout_cannot_replace_delete_or_append_saved_results(self):
        original = self.record.read_bytes()
        for change in ("replace", "delete", "new_file", "new_directory"):
            with self.subTest(change=change):
                added = self.records / "new-run"
                if change == "replace": self.record.write_text("{}")
                elif change == "delete": self.record.unlink()
                elif change == "new_file": added.write_text("{}")
                else: added.mkdir()
                with self.assertRaisesRegex(RuntimeError, "changed saved"):
                    driver.require_unchanged_records(self.records, self.before)
                self.record.write_bytes(original)
                if added.is_dir(): added.rmdir()
                elif added.exists(): added.unlink()

    def test_store_snapshot_is_bounded_and_rejects_links(self):
        self.record.write_bytes(b"x" * ((2 << 20) + 1))
        with self.assertRaisesRegex(RuntimeError, "size limit"):
            driver.saved_record_fingerprints(self.records)
        self.record.write_bytes(b"{}")
        linked = self.records / "linked.json"
        try:
            linked.symlink_to(self.record)
        except (OSError, NotImplementedError):
            self.skipTest("host does not permit synthetic symlinks")
        with self.assertRaisesRegex(RuntimeError, "Linked preview"):
            driver.saved_record_fingerprints(self.records)


if __name__ == "__main__":
    unittest.main()
