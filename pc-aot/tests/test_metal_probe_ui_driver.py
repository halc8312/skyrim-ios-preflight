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
            "status": "unsupported", "host_build": {"source_revision": "authored-revision"},
            "physical_iphone_verified": False, "gpu_execution_verified": False,
            "metal4_library_verified": False, "samples": []}}

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


if __name__ == "__main__":
    unittest.main()
