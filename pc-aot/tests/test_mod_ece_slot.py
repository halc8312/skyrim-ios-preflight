"""Read-only tests for the standard NoseUpDown value saved by ECE LE."""
from hashlib import sha256
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / "ios/runtime-port"
HEADER = b"# CharacterMakingExtender face data\n# 'uniqueID, value' pair\n"
OK, TOO_LARGE, MALFORMED, NO_NOSE, BAD_NOSE = range(5)


def instructions(objdump, dll, first, end):
    text = subprocess.check_output(
        [objdump, "-d", "-M", "intel", f"--start-address={first:#x}", f"--stop-address={end:#x}", dll],
        text=True, timeout=10, env=dict(os.environ, LC_ALL="C"),
    )
    decoded = {}
    for line in text.splitlines():
        fields = line.strip().split("\t")
        if len(fields) == 3 and fields[0].endswith(":"):
            decoded[int(fields[0][:-1], 16)] = " ".join(fields[-1].lower().split())
    return decoded


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

    def test_owned_ece_binary_slot_value_and_default_descriptor(self):
        owned_dll = os.environ.get("SKYRIM_TEST_ECE_CME_LIBRARY")
        objdump = shutil.which("objdump")
        if not owned_dll or not objdump:
            self.skipTest("Set SKYRIM_TEST_ECE_CME_LIBRARY and install GNU objdump")
        dll = Path(owned_dll)
        with dll.open("rb") as stream:
            contents = stream.read(1 << 20)
        self.assertEqual(sha256(contents).hexdigest(),
                         "27522cfe66f2e404c505cd43c79aa618e27ef73221a8e38e17243244f4d038a2")
        # These offsets are in .rdata of the exact hashed release DLL.
        minus_one, = struct.unpack_from("<f", contents, 0x51E5C)
        self.assertEqual(minus_one, -1.0)
        self.assertEqual(contents[0x50430:0x50438], b"%d, %f\n\0")
        self.assertEqual(contents[0x5102C:0x51035], b"position\0")

        descriptor = instructions(objdump, dll, 0x1000E01D, 0x1000E0D1)
        self.assertEqual(descriptor[0x1000E01D], "cmp eax,0xc8")
        self.assertEqual(descriptor[0x1000E024], "sub eax,0xc8")
        self.assertEqual(descriptor[0x1000E029], "mov dword ptr [ebp+0xc0],0x3")
        self.assertEqual(descriptor[0x1000E0C3], "fld dword ptr ds:0x10052c5c")
        self.assertEqual(descriptor[0x1000E0C9], "fstp dword ptr [ebp+0x28]")
        self.assertEqual(descriptor[0x1000E0CC], "fld1")
        self.assertEqual(descriptor[0x1000E0CE], "fstp dword ptr [ebp+0x2c]")

        written = instructions(objdump, dll, 0x10007049, 0x10007065)
        self.assertEqual(written[0x10007049], "fld qword ptr [edi+eax*1+0x8]")
        self.assertEqual(written[0x10007056], "fstp qword ptr [esp]")
        self.assertEqual(written[0x1000705A], "push 0x10051230")
        read = instructions(objdump, dll, 0x10013203, 0x1001321C)
        self.assertEqual(read[0x10013203], "fld qword ptr [ebx+ecx*1+0x8]")
        self.assertEqual(read[0x1001320A], "fstp qword ptr [esp]")
        self.assertEqual(read[0x10013211], "push 0x10051e2c")
        self.assertEqual(dll.read_bytes(), contents)


if __name__ == "__main__":
    unittest.main()
