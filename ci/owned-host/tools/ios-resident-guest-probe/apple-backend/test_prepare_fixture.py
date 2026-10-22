"""Actual CLI regression for the public frozen header; no SDK or game data."""
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
PUBLIC_HEADER = HERE.parents[2] / "skyrim-recomp-web/cpp/ppc_context.h"

class PrepareFixtureTests(unittest.TestCase):
    def prepare(self, raw, root):
        source = root / "public-source"
        (source / "cpp").mkdir(parents=True)
        header = source / "cpp/ppc_context.h"
        header.write_bytes(raw)
        generic = root / "public-generic"
        generic.mkdir()
        output = root / "generated"
        result = subprocess.run([sys.executable, str(HERE / "prepare_fixture.py"),
                                 "--source", str(source), "--generic", str(generic),
                                 "--output", str(output)], capture_output=True, timeout=10)
        self.assertEqual(header.read_bytes(), raw, "Original bytes must never be normalized in place")
        return result, output

    def assert_success(self, raw):
        with tempfile.TemporaryDirectory(prefix="public-prepare-cli-") as temporary:
            result, output = self.prepare(raw, Path(temporary))
            self.assertEqual(result.returncode, 0, "Public prepare CLI failed")
            self.assertEqual(result.stdout.decode().strip(), "public_header_sha256=" + hashlib.sha256(raw).hexdigest())
            generated = (output / "ppc_context.h").read_bytes()
            self.assertNotIn(b"\r\n", generated)
            self.assertIn(b"__atomic_load_n(&offsets[ea >> 20], __ATOMIC_ACQUIRE)", generated)
            self.assertIn(b"__atomic_store_n(&written[ea >> 12], (uint8_t)0, __ATOMIC_RELEASE)", generated)
            self.assertIn(b"returns_twice", generated)

    def test_actual_frozen_header_cli_preserves_original_bytes_and_sha(self):
        original = PUBLIC_HEADER.read_bytes()
        self.assert_success(original)
        self.assertEqual(PUBLIC_HEADER.read_bytes(), original)

    def test_actual_cli_accepts_both_lf_and_crlf_public_headers(self):
        original = PUBLIC_HEADER.read_bytes()
        lf = original.decode("utf-8").replace("\r\n", "\n")
        for raw in (lf.encode(), lf.replace("\n", "\r\n").encode()):
            with self.subTest(crlf=b"\r\n" in raw):
                self.assert_success(raw)
        self.assertEqual(PUBLIC_HEADER.read_bytes(), original)

    def test_changed_anchor_is_still_rejected_before_output_creation(self):
        original = PUBLIC_HEADER.read_bytes()
        raw = original.replace(b'extern "C" uint8_t skyrt_written[];', b'extern "C" uint8_t different_public_name[];')
        self.assertNotEqual(raw, original)
        with tempfile.TemporaryDirectory(prefix="public-prepare-refusal-") as temporary:
            result, output = self.prepare(raw, Path(temporary))
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(output.exists())
        self.assertEqual(PUBLIC_HEADER.read_bytes(), original)

if __name__ == "__main__":
    unittest.main()
