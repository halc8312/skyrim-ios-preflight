"""Check the shared report contract and the real Windows reread using a synthetic control."""
import hashlib
import importlib
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "experiments/classic-plugin"))
try:
    returned = importlib.import_module("verify_ios_return")
finally:
    sys.path.pop(0)


class ReturnedEvidence(unittest.TestCase):
    def setUp(self):
        area = tempfile.TemporaryDirectory()
        self.addCleanup(area.cleanup)
        self.area = Path(area.name)
        self.save = self.area / "translated.skse"
        self.receipt = self.area / "classic-lab-result.json"
        # This schema control is constructed by the test and never reported as an iPhone run.
        self.control = dict(returned.OBSERVATIONS, run_id="12345678-1234-4baa-abcd-123456789000",
                            os_version="18.0", bundle_id=returned.BUNDLE, build="1", platform="ios_simulator",
                            completed_at="2026-01-01T12:00:00Z", input_sha256=returned.INPUT_HASH,
                            output_sha256=returned.OUTPUT_HASH)
        self.save.write_bytes(returned.expected_bytes(21))
        self.write(self.control)

    def write(self, record):
        self.receipt.write_text(json.dumps(record), encoding="utf-8")

    def test_known_file_hashes_and_separate_simulator_identity(self):
        for value, digest in ((16, returned.INPUT_HASH), (21, returned.OUTPUT_HASH), (26, returned.RETURN_HASH)):
            self.assertEqual(hashlib.sha256(returned.expected_bytes(value)).hexdigest(), digest)
        raw = self.receipt.read_bytes()
        self.assertEqual(returned.read_pair(self.save, self.receipt, "1", "ios_simulator"),
                         (self.save.read_bytes(), raw, self.control))
        with self.assertRaisesRegex(ValueError, "platform"):
            returned.read_pair(self.save, self.receipt, "1", "ios_device")
        self.assertEqual(self.receipt.read_bytes(), raw)

    def test_rejects_missing_changed_or_ill_typed_fields_and_oversized_input(self):
        for field, bad in (("schema", True), ("run_id", "wrong"), ("os_version", "sample"),
                           ("completed_at", "2026-02-30T01:02:03Z"), ("build", "2"),
                           ("bundle_id", "org.skyrimios.pc2011"), ("output_sha256", returned.INPUT_HASH),
                           ("input_sha256", returned.OUTPUT_HASH), ("records_read", True), ("callback_mask", 3),
                           ("first_save_after_revert", 0), ("output_value", "21"), ("input_value", 77)):
            with self.subTest(field=field), self.assertRaises(ValueError):
                self.write(self.control | {field: bad})
                returned.read_pair(self.save, self.receipt, "1", "ios_simulator")
        self.write({k: v for k, v in self.control.items() if k != "completed_at"})
        with self.assertRaisesRegex(ValueError, "schema"):
            returned.read_pair(self.save, self.receipt, "1", "ios_simulator")
        self.receipt.write_bytes(b" " * 4097)
        with self.assertRaisesRegex(ValueError, "limit"):
            returned.read_pair(self.save, self.receipt, "1", "ios_simulator")
        self.receipt.write_text(json.dumps(self.control).replace('"schema": 1', '"schema": 1, "schema": 1'))
        with self.assertRaisesRegex(ValueError, "duplicate"):
            returned.read_pair(self.save, self.receipt, "1", "ios_simulator")
        self.write(self.control)
        self.save.write_bytes(self.save.read_bytes().replace(b"opaque", b"differ"))
        with self.assertRaisesRegex(ValueError, "83-byte"):
            returned.read_pair(self.save, self.receipt, "1", "ios_simulator")

    @unittest.skipUnless(sys.platform == "win32", "run the reference DLL with the real i386 Windows loader")
    def test_native_dll_reopens_control_in_private_evidence_directory(self):
        llvm = Path(os.environ.get("CLASSIC_PLUGIN_LLVM_BIN", "C:/Program Files/LLVM/bin"))
        if not all((llvm / name).is_file() for name in ("clang.exe", "lld-link.exe")):
            self.skipTest("requires the project's Windows LLVM toolchain")
        original = (self.save.read_bytes(), self.receipt.read_bytes())
        with patch.object(returned, "ROOT", self.area):
            result, folder = returned.verify_return(self.save, self.receipt, "1", "ios_simulator", llvm)
        self.assertTrue(folder.is_relative_to(self.area / "local/classic-lab-returns"))
        self.assertEqual(result["app_reported_platform"], "ios_simulator")
        self.assertEqual([result[k] for k in ("windows_fresh_plugin_handle", "windows_loaded", "windows_saved")],
                         [99, 21, 26])
        self.assertEqual((folder / "returned.skse").read_bytes(), returned.expected_bytes(26))
        self.assertEqual(original, (self.save.read_bytes(), self.receipt.read_bytes()))


if __name__ == "__main__":
    unittest.main()
