"""Read-only tests for the standard NoseUpDown value saved by ECE LE."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / "ios/runtime-port"
HEADER = b"# CharacterMakingExtender face data\n# 'uniqueID, value' pair\n"
OK, TOO_LARGE, MALFORMED, NO_NOSE, BAD_NOSE = range(5)


@unittest.skipUnless(os.name == "posix", "Requires a POSIX C build toolchain")
class EceSlotTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which(os.environ.get("CC", "cc"))
        if not cc:
            raise unittest.SkipTest("C compiler unavailable")
        build = tempfile.TemporaryDirectory()
        cls.addClassCleanup(build.cleanup)
        cls.binary = Path(build.name) / "ece-slot"
        flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-O2", "-I", PORT]
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            flags += ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run([cc, *flags, ROOT / "tests/native/test_mod_ece_slot.c",
                        PORT / "mod_ece_slot.c", "-o", cls.binary], check=True, timeout=60)

    def read_path(self, path):
        status, value = subprocess.check_output([self.binary, path], text=True, timeout=10).split()
        return int(status), float(value)

    def read(self, data):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "preset.slot"
            path.write_bytes(data)
            return self.read_path(path)

    def test_stock_nose_height_uses_id_not_row_position(self):
        entries = b"# character race: Nord\n112, 0.5\n-12, 42\n 12 ,\t-0.625 \n200, 5"
        self.assertEqual(self.read(HEADER + entries), (OK, -0.625))

    def test_bom_crlf_and_version_header(self):
        slot = b"\xef\xbb\xbf" + HEADER.replace(b"# '", b"# version: 2.0\n# '")
        slot += b"\n12,+0.125\n-10680, 8421247.000000\n# Other values may follow\n"
        self.assertEqual(self.read(slot.replace(b"\n", b"\r\n")), (OK, 0.125))

    def test_missing_and_malformed_slot(self):
        self.assertEqual(self.read(HEADER + b"20, 1\n-12, 1"), (NO_NOSE, 123))
        for slot in (b"12, 1", HEADER + b"12, 0\ntrailing garbage", HEADER + b"25, 1\x00\n12, 0", HEADER + b"12,0\n#" + b"x" * 1024,
                     HEADER + b"999999999999999999999, 0", HEADER + b"22 0\n12, 0", HEADER + b"# 'uniqueID, value' pair\n12, 0"):
            with self.subTest(slot=slot[:90]):
                self.assertEqual(self.read(slot), (MALFORMED, 123))

    def test_duplicate_and_invalid_nose_values(self):
        for tail in (b"12,0\n+0012,1", b"12, NaN", b"12, -inf", b"12, 1.2.3", b"12, 0.5junk",
                     b"12, 0,5", b"12, 1000001", b"12, -1000001", b"12, " + b"9" * 128):
            with self.subTest(tail=tail):
                self.assertEqual(self.read(HEADER + tail), (BAD_NOSE, 123))

    def test_file_size_limit(self):
        self.assertEqual(self.read(b" " * (1024 * 1024 + 1)), (TOO_LARGE, 123))

    def test_private_author_and_nose_only_fixture(self):
        directory = os.environ.get("SKYRIM_TEST_ECE_SLOT_FIXTURES")
        if not directory:
            self.skipTest("Set SKYRIM_TEST_ECE_SLOT_FIXTURES to the private ECE LE comparison directory.")
        for name, expected in (("author-baseline", -0.266667), ("nose-fixture", 0.266667)):
            path = Path(directory) / name / "_1_commonTriRaces.slot"
            source = path.read_bytes()
            status, value = self.read_path(path)
            self.assertEqual(status, OK)
            self.assertAlmostEqual(value, expected, places=6)
            self.assertEqual(path.read_bytes(), source)


if __name__ == "__main__":
    unittest.main()
