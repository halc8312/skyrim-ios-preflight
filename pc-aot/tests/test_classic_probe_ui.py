"""Exercise the acceptance checks used when collecting the real Simulator UI run."""
import datetime
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import tempfile
import unittest
import uuid


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("classic_probe_ui_driver", ROOT / "tests/classic-probe-ui/check_simulator.py")
driver = importlib.util.module_from_spec(spec)
spec.loader.exec_module(driver)


class Evidence(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.documents = Path(directory.name)
        self.original = driver.expected_bytes(16)
        self.translated = driver.expected_bytes(21)
        (self.documents / "ui-check.json").write_text('{"outcome":"passed","stage":6}')
        (self.documents / "windows.skse").write_bytes(self.original)
        (self.documents / "translated.skse").write_bytes(self.translated)
        self.receipt = {"schema": 1, "platform": "ios_simulator", "bundle_id": driver.BUNDLE, "build": "1",
                        "input_value": 16, "first_save_after_revert": 5, "output_value": 21, "records_read": 1,
                        "callback_mask": 7, "input_sha256": hashlib.sha256(self.original).hexdigest(),
                        "output_sha256": hashlib.sha256(self.translated).hexdigest(), "run_id": str(uuid.uuid4()),
                        "completed_at": datetime.datetime.now(datetime.timezone.utc).isoformat(), "os_version": "26.0"}

    def verify(self, receipt):
        (self.documents / "classic-lab-result.json").write_text(json.dumps(receipt))
        driver.verify(self.documents, self.original)

    def test_ui_accepts_exact_fixture_identities(self):
        source = (ROOT / "ios/runtime-port/classic_probe_ui.m").read_text(encoding="utf-8")
        for variable, data in [("inputDigest", self.original), ("outputDigest", self.translated)]:
            match = re.search(variable + r"\[CC_SHA256_DIGEST_LENGTH\]\s*=\s*\{([^}]+)\}", source)
            self.assertIsNotNone(match)
            expected = bytes(int(byte, 16) for byte in match[1].split(","))
            self.assertEqual(expected, hashlib.sha256(data).digest())
        self.verify(self.receipt)

    def test_wrong_platform_failed_callbacks_and_modified_payload_do_not_pass(self):
        for key, value in (("platform", "ios_device"), ("records_read", True), ("callback_mask", 3),
                           ("input_value", 21), ("run_id", "missing"), ("private_path", "/sample")):
            with self.subTest(key=key), self.assertRaises(ValueError):
                self.verify({**self.receipt, key: value})
        (self.documents / "translated.skse").write_bytes(self.translated[:-1])
        with self.assertRaises(ValueError):
            self.verify(self.receipt)

    def test_attempted_ui_run_does_not_pass_as_success(self):
        (self.documents / "ui-check.json").write_text('{"outcome":"missing-share-sheet-or-files"}')
        with self.assertRaisesRegex(ValueError, "did not complete"):
            self.verify(self.receipt)


if __name__ == "__main__":
    unittest.main()
