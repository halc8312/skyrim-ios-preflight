import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("audit_pe_layout", Path(__file__).parents[1] / "scripts" / "audit_pe_layout.py")
audit = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(audit)


def fixture(path, name=b".text", virtual_size=512, raw_offset=512):
    data = bytearray(1024)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 60, 64)
    data[64:68] = b"PE\0\0"
    struct.pack_into("<HH", data, 68, 0x14C, 1)
    struct.pack_into("<H", data, 84, 224)
    struct.pack_into("<H", data, 88, 0x10B)
    struct.pack_into("<I", data, 104, 0x1000)
    struct.pack_into("<I", data, 116, 0x400000)
    struct.pack_into("<II", data, 144, 0x3000, 512)
    struct.pack_into("<8sIIIIIIHHI", data, 312, name, virtual_size, 0x1000, 512, raw_offset, 0, 0, 0, 0, 0x60000020)
    data[512:544] = bytes(range(32))
    path.write_bytes(data)


class PELayoutTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.path = self.root / "synthetic.exe"
        fixture(self.path)

    def test_address_maps_to_file_offset_and_hash_without_returning_bytes(self):
        before = self.path.read_bytes()
        result = audit.audit_image(self.path, [0x401000])
        row = result["hook_windows"][0]
        self.assertEqual(row["file_offset"], 512)
        self.assertEqual(row["sha256"], audit.hashlib.sha256(bytes(range(32))).hexdigest())
        self.assertEqual(result["compatibility"], "not_reviewed")
        self.assertTrue(result["stable_during_read"])
        json.dumps(result)  # Only JSON metadata/digests, no byte buffers.
        self.assertEqual(self.path.read_bytes(), before)

    def test_unmapped_zero_fill_and_boundary_windows_are_not_fingerprinted(self):
        fixture(self.path, virtual_size=1024)
        result = audit.audit_image(self.path, [0x400FFF, 0x4011F0, 0x401200])
        self.assertEqual([r["status"] for r in result["hook_windows"]],
                         ["unmapped", "window_not_fully_file_backed", "window_not_fully_file_backed"])
        self.assertFalse(any("sha256" in r for r in result["hook_windows"]))

    def test_packed_image_preserved_and_not_used_as_hook_evidence(self):
        fixture(self.path, name=b".bind")
        result = audit.audit_image(self.path, [0x401000])
        self.assertTrue(result["layout"]["has_bind_section"])
        self.assertEqual(result["hook_windows"][0]["status"], "packed_image_not_hook_evidence")
        self.assertEqual(audit.compare(result, result)["status"], "not_comparable")

    def test_truncated_out_of_bounds_and_overlapping_layout_rejected(self):
        for offset, value in ((84, 16), (144, 0xFFFFFFFF), (148, 320), (324, 128), (332, 900), (332, 128)):
            fixture(self.path)
            data = bytearray(self.path.read_bytes())
            struct.pack_into("<H" if offset == 84 else "<I", data, offset, value)
            self.path.write_bytes(data)
            with self.subTest(offset=offset, value=value):
                result = audit.audit_image(self.path, [0x401000])
                self.assertTrue("layout_error" in result or "inspection_error" in result)
        self.path.write_bytes(b"MZ")
        self.assertIn("inspection_error", audit.audit_image(self.path, [0x401000]))

    def test_nonexecutable_section_is_not_hook_evidence(self):
        data = bytearray(self.path.read_bytes())
        struct.pack_into("<I", data, 348, 0x40000040)
        self.path.write_bytes(data)
        result = audit.audit_image(self.path, [0x401000])
        self.assertEqual(result["hook_windows"][0]["status"], "not_in_executable_section")

    def test_matching_or_different_reference_never_approves(self):
        candidate = audit.audit_image(self.path, [0x401000])
        equal = audit.compare(candidate, candidate)
        self.assertTrue(equal["whole_file_equal"])
        self.assertTrue(equal["hook_windows"][0]["matching_fingerprint"])
        self.assertEqual(equal["compatibility"], "not_reviewed")
        data = bytearray(self.path.read_bytes())
        data[512] ^= 1
        self.path.write_bytes(data)
        different = audit.compare(candidate, audit.audit_image(self.path, [0x401000]))
        self.assertFalse(different["whole_file_equal"])
        self.assertFalse(different["hook_windows"][0]["matching_fingerprint"])
        self.assertEqual(different["compatibility"], "not_reviewed")

    def test_unstable_input_cannot_be_compared(self):
        candidate = audit.audit_image(self.path, [0x401000])
        candidate["stable_during_read"] = False
        self.assertEqual(audit.compare(candidate, candidate)["status"], "not_comparable")

    def test_output_private_and_exclusive(self):
        with patch.object(audit, "ROOT", self.root):
            with self.assertRaises(ValueError):
                audit.save_report({}, self.root / "docs" / "run")
            run = self.root / "private" / "run"
            path = audit.save_report({"preserve": True}, run)
            with self.assertRaises(FileExistsError):
                audit.save_report({}, run)
            self.assertEqual(json.loads(path.read_text()), {"preserve": True})

    def test_pinned_source_overrides_read_without_importing_lifter(self):
        if not (audit.ROOT / "vendor" / "skydream" / ".git").exists():
            self.skipTest("optional public source checkout is not included in this test environment")
        addresses, provenance = audit.source_locations()
        self.assertEqual(len(addresses), 16)
        self.assertIn(0xD0DB50, addresses)
        self.assertIn(0xF520F0, addresses)
        self.assertEqual(provenance["revision"], "ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a")
        self.assertEqual(provenance["symbol"], "HOST_OVERRIDES_SKYRIM")
        self.assertEqual(len(provenance["uncovered_motion_contract"]["functions"]), 4)
        self.assertEqual(provenance["uncovered_motion_contract"]["clock_global_va"], "0x012d18dc")
        self.assertEqual(provenance["uncovered_motion_contract"]["status"], "not_evaluated")


if __name__ == "__main__":
    unittest.main()
