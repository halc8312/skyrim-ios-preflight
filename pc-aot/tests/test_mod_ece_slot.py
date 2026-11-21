"""Read-only tests for the standard NoseUpDown value saved by ECE LE."""
from hashlib import sha256
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / "ios/runtime-port"
HEADER = b"# CharacterMakingExtender face data\n# 'uniqueID, value' pair\n"
OK, TOO_LARGE, MALFORMED, NO_NOSE, BAD_NOSE, UNSUPPORTED_VERSION = range(6)
V2 = b"# version: 2.0\n"


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


def action_body(code, address, expected_opcode):
    assert code[address] == expected_opcode
    size, = struct.unpack_from("<H", code, address + 1)
    return code[address + 3:address + 3 + size]


def pushed_pool_names(body, pool):
    names = []
    at = 0
    while at < len(body):
        kind = body[at]
        at += 1
        if kind in (8, 9):
            width = kind - 7
            names.append(pool[int.from_bytes(body[at:at + width], "little")])
            at += width
        else:
            at += {4: 1, 6: 8, 7: 4}[kind]
    return names


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

    def read_path(self, path, stock=False):
        command = [self.binary, path]
        if stock:
            command.append("stock")
        status, value = subprocess.check_output(command, text=True, timeout=10).split()
        return int(status), float(value)

    def read(self, data, stock=False):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "preset.slot"
            path.write_bytes(data)
            return self.read_path(path, stock=stock)

    def test_stock_nose_height_uses_id_not_row_position(self):
        entries = b"# character race: Nord\n112, 0.5\n-12, 42\n 12 ,\t-0.625 \n200, 5"
        self.assertEqual(self.read(HEADER + entries), (OK, -0.625))

    def test_bom_crlf_and_version_header(self):
        slot = b"\xef\xbb\xbf" + HEADER.replace(b"# '", b"# version: 2.0\n# '")
        slot += b"\n12,+0.125\n-10680, 8421247.000000\n# Other values may follow\n"
        self.assertEqual(self.read(slot.replace(b"\n", b"\r\n")), (OK, 0.125))

    def test_stock_version_and_line_order(self):
        slot = HEADER + b"112, 0.5\n-12, 42\n12, -0.625\n"
        self.assertEqual(self.read(slot, stock=True), (OK, -0.9375))
        self.assertEqual(self.read(slot + V2, stock=True), (OK, -0.9375))
        self.assertEqual(self.read(slot.replace(b"12, -0.625", V2 + b"12, -0.625"), stock=True), (OK, -0.625))
        exported = b"\xef\xbb\xbf" + HEADER.replace(b"# '", V2 + b"# '") + b"12, 0.625\n"
        self.assertEqual(self.read(exported.replace(b"\n", b"\r\n"), stock=True), (OK, 0.625))
        self.assertEqual(self.read(slot), (OK, -0.625))

    def test_stock_bounds_and_ambiguous_input(self):
        for version, raw, expected in ((b"", b"+0.75", 1), (b"", b"-0.75", -1),
                                        (V2, b"+1.25", 1), (V2, b"-1.25", -1),
                                        (b"", b"+0.5", 0.75), (b"", b"-0", -0.0)):
            with self.subTest(version=version, raw=raw):
                self.assertEqual(self.read(HEADER + version + b"12," + raw, stock=True), (OK, expected))
        for tail in (b"12,0\n" + V2 + b"+0012,1", b"12, 0.1234567"):
            with self.subTest(tail=tail):
                self.assertEqual(self.read(HEADER + tail, stock=True), (BAD_NOSE, 123))
        unknown = b"# version: 3.0\n"
        for tail in (unknown + b"12, 0.5", b"12, 0.5\n" + unknown, V2 + unknown + b"12, 0.5"):
            with self.subTest(tail=tail):
                self.assertEqual(self.read(HEADER + tail, stock=True), (UNSUPPORTED_VERSION, 123))
                self.assertEqual(self.read(HEADER + tail), (OK, 0.5))

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

    def test_owned_released_ece_presets(self):
        directory = os.environ.get("SKYRIM_TEST_ECE_RELEASE_PRESETS")
        if not directory:
            self.skipTest("Set SKYRIM_TEST_ECE_RELEASE_PRESETS to the owned ECE 1.59 Presets directory.")
        for name, version, raw, stock in (
            ("Nordpreset1.slot", b"", -0.266667, -0.4000005),
            ("ygNordpreset1.slot", V2, -0.400000, -0.400000),
            ("ygNordpreset2.slot", V2, 0.200000, 0.200000),
        ):
            with self.subTest(name=name):
                path, = Path(directory).rglob(name)
                source = path.read_bytes()
                self.assertEqual([line for line in source.splitlines() if line.startswith(b"# version:")],
                                 [version.rstrip()] if version else [])
                status, value = self.read_path(path)
                self.assertEqual(status, OK)
                self.assertEqual(struct.pack("<f", value), struct.pack("<f", raw))
                status, value = self.read_path(path, stock=True)
                self.assertEqual(status, OK)
                self.assertEqual(struct.pack("<f", value), struct.pack("<f", stock))
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
        self.assertEqual(contents[0x50344:0x50353], b"# version: 2.0\0")
        self.assertIn(12, struct.unpack_from("<18I", contents, 0x4FBB8))
        self.assertEqual(struct.unpack_from("<d", contents, 0x51E80), (1.5,))

        loader = instructions(objdump, dll, 0x10006192, 0x10006555)
        self.assertEqual(loader[0x10006192], "mov byte ptr [esp+0x2b],0x1")
        self.assertEqual(loader[0x1000619C], "add esp,0x10")
        self.assertEqual(loader[0x10006287], "mov ecx,0x10051144")
        self.assertEqual(loader[0x10006293], "call 0x100239c0")
        self.assertEqual(loader[0x100062A0], "mov byte ptr [esp+0x1b],0x0")
        self.assertEqual(loader[0x10006390], "mov eax,0x100509b8")
        self.assertEqual(loader[0x10006397], "je 0x1000653b")
        self.assertEqual(loader[0x100063A0], "cmp eax,0x10050a00")
        self.assertEqual(loader[0x1000653B], "cmp byte ptr [esp+0x1b],0x0")
        self.assertEqual(loader[0x10006540], "je 0x100063a7")
        self.assertEqual(loader[0x10006546], "fmul qword ptr ds:0x10052c80")
        self.assertEqual(loader[0x1000654C], "fstp qword ptr [esp+0x34]")

        descriptor = instructions(objdump, dll, 0x1000E01D, 0x1000E0D1)
        self.assertEqual(descriptor[0x1000E01D], "cmp eax,0xc8")
        self.assertEqual(descriptor[0x1000E024], "sub eax,0xc8")
        self.assertEqual(descriptor[0x1000E029], "mov dword ptr [ebp+0xc0],0x3")
        self.assertEqual(descriptor[0x1000E091], "mov edi,0x3")
        self.assertEqual(descriptor[0x1000E0A4], "lea ecx,[ebp+0x54]")
        self.assertEqual(descriptor[0x1000E0AC], "mov edi,0x4")
        self.assertEqual(descriptor[0x1000E0BB], "lea ecx,[ebp+0x38]")
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

        blend = instructions(objdump, dll, 0x100078E7, 0x10007B75)
        self.assertEqual(blend[0x100078F8], "fldz")
        self.assertEqual(blend[0x10007900], "fcomp qword ptr [esp+0x14]")
        self.assertEqual(blend[0x10007906], "test ah,0x41")
        self.assertEqual(blend[0x1000790E], "jne 0x10007945")
        self.assertEqual(blend[0x1000792D], "lea eax,[edi+ecx*1+0x54]")
        self.assertEqual(blend[0x10007962], "lea eax,[ecx+edi*1+0x38]")
        self.assertEqual(blend[0x10007ADC], "fabs")
        self.assertEqual(blend[0x10007B69], "fld dword ptr [ebx+ebp*1+0x1c]")
        self.assertEqual(blend[0x10007B71], "fmul qword ptr [esp+0x5c]")
        self.assertEqual(dll.read_bytes(), contents)

    def test_owned_ece_menu_uses_signed_slot_position_and_clamps(self):
        owned = os.environ.get("SKYRIM_TEST_ECE_RELEASE_MENU")
        if not owned:
            self.skipTest("Set SKYRIM_TEST_ECE_RELEASE_MENU to the owned ECE 1.59 racesex_menu.gfx.")
        with Path(owned).open("rb") as stream:
            gfx = stream.read(1 << 20)
        self.assertEqual(sha256(gfx).hexdigest(),
                         "d53811b1c2c5a5b12a10742eb473867e2333113e8cd52e7434a8ddeca74d226d")
        self.assertEqual(gfx[:4], b"CFX\x0a")
        code = zlib.decompress(gfx[8:])

        # AVM1 actions within the pinned shipped menu's DoInitAction block.
        constants = action_body(code, 0x73EC0, 0x88)
        pool = constants[2:-1].split(b"\0")
        self.assertEqual(len(pool), struct.unpack_from("<H", constants)[0])
        for address, name in (
            (0x7AD97, b"uniqueID"), (0x7ADB2, b"position"), (0x7B7F7, b"GetSlotData"),
            (0x7B894, b"position"), (0x7B8B1, b"ExCharGenSearchPosFromUniqueID"),
            (0x7B927, b"sliderMin"), (0x7B998, b"sliderMax"),
            (0x7BC58, b"ExCharGenSetMergedMorph"), (0x7AE11, b"ExCharGenMakeInfoArray"),
            (0x7AEDF, b"SetMergedMorphsMemory"), (0x7F560, b"uniqueID"), (0x7F589, b"position"),
        ):
            self.assertIn(name, pushed_pool_names(action_body(code, address, 0x96), pool))
        # Return the value matched by ID, assign it, and clamp to the actual
        # descriptor. The merge array passes the resulting ID/value pair.
        self.assertEqual(code[0x7ADA3], 0x49)  # Equals2
        self.assertEqual(code[0x7ADB7:0x7ADB9], b"\x4e\x3e")  # GetMember, Return
        self.assertEqual(code[0x7B8C0:0x7B8C2], b"\x52\x4f")  # CallMethod, SetMember
        self.assertEqual(code[0x7B8F6], 0x48)  # Less2
        self.assertEqual(code[0x7B92D], 0x4F)  # SetMember
        self.assertEqual(code[0x7B967], 0x67)  # Greater
        self.assertEqual(code[0x7B99E], 0x4F)  # SetMember
        self.assertEqual(code[0x7AEE5], 0x52)  # CallMethod

    def test_owned_stock_nose_morph_uses_same_direction(self):
        generation = os.environ.get("SKYRIM_TEST_OWNED_CLASSIC_LIFT")
        objdump = shutil.which("objdump")
        if not generation or not objdump:
            self.skipTest("Set SKYRIM_TEST_OWNED_CLASSIC_LIFT and install GNU objdump")
        receipt = json.loads((Path(generation) / "generation.json").read_text(encoding="utf-8"))
        exe = Path(receipt["candidate"])
        with exe.open("rb") as stream:
            data = stream.read(1 << 25)
        self.assertEqual(sha256(data).hexdigest(),
                         "d97eb0cc0ceef0923d2234d42b382c0d033853356528b271bc043542b1f5e605")
        # ECE ID 12 maps by morph names to stock option 1, not option 12.
        kind, down, up = struct.unpack_from("<III", data, 0xE573A8 + 12)
        self.assertEqual((kind, down, up), (2, 0x10A53D4, 0x10A53CC))
        self.assertEqual(data[0xCA3BD4:0xCA3BDD], b"NoseDown\0")
        self.assertEqual(data[0xCA3BCC:0xCA3BD3], b"NoseUp\0")
        select = instructions(objdump, exe, 0x5610F0, 0x561125)
        self.assertEqual(select[0x5610F7], "cmp dword ptr [ecx*4+0x12593a8],0x2")
        self.assertEqual(select[0x5610FF], "mov edx,dword ptr [ecx*4+0x12593ac]")
        self.assertEqual(select[0x56110F], "fld dword ptr [esp+0x8]")
        self.assertEqual(select[0x561113], "fldz")
        self.assertEqual(select[0x561115], "fcompp")
        self.assertEqual(select[0x561119], "test ah,0x41")
        self.assertEqual(select[0x56111C], "jp 0x561122")
        self.assertEqual(select[0x56111E], "mov eax,dword ptr [ecx+0x8]")
        self.assertEqual(select[0x561122], "mov eax,edx")


if __name__ == "__main__":
    unittest.main()
