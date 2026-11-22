"""Join the exact released HighHeels assets with the copied active plugin catalogue."""
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / "ios/runtime-port"
EXPERIMENT = ROOT / "experiments/classic-highheels"
CATALOG = ROOT / "experiments/skse-cosave"
PROBE = ROOT / "experiments/skse-plugin-vm"
ESM = "hdtHighHeel.esm"
PEX = "Scripts/hdtHighHeelEffect.pex"
MATCH, NOT_LOADED, DIFFERENT, UNREADABLE, POSSIBLE_OVERRIDE = range(5)


def plugin_header(*masters):
    fields = b""
    for master in masters:
        encoded = master.encode() + b"\0"
        fields += struct.pack("<4sH", b"MAST", len(encoded)) + encoded
        fields += struct.pack("<4sHQ", b"DATA", 8, 0)
    return struct.pack("<4sIIIIHH", b"TES4", len(fields), 0, 0, 0, 43, 0) + fields


@unittest.skipUnless(os.name == "posix", "Requires POSIX directory operations and a C compiler")
class HighHeelIdentityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which(os.environ.get("CC", "cc"))
        if not cc:
            raise unittest.SkipTest("C compiler unavailable")
        build = tempfile.TemporaryDirectory()
        cls.addClassCleanup(build.cleanup)
        cls.binary = Path(build.name) / "highheel-identity"
        flags = ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O2", "-I", PORT,
                 "-I", EXPERIMENT, "-I", CATALOG, "-I", PROBE]
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            flags += ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            if sys.platform.startswith("linux"):
                flags += ["-fno-pie", "-no-pie"]
        subprocess.run([cc, *flags, ROOT / "tests/native/test_mod_highheel_identity.c",
                        EXPERIMENT / "highheel_identity.c", PORT / "classic_mod_catalog.c",
                        PORT / "mod_plugins.c", PROBE / "probe_asset_file.c", "-o", cls.binary],
                       check=True, timeout=60)

    def setUp(self):
        work = tempfile.TemporaryDirectory()
        self.addCleanup(work.cleanup)
        self.data = Path(work.name)
        self.write_marker(ESM, 1934, "7c7b6bb6ba4585e9f55a5b0459bb275a2f6d7f37542f3f12239f5b1c32c52c32")
        self.write_marker(PEX, 5875, "01e4816a1ea6c5bcb6a4c3bf62c9cdca6574600ddc441b95341b4942df3bdf03")

    def write_marker(self, name, length, digest):
        path = self.data / name
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("wb") as stream:
            stream.write(bytes.fromhex(digest))
            stream.truncate(length)

    def check(self, *rows, slot=255, real=False):
        command = [self.binary, self.data, sys.executable if real else "-", str(slot), *rows]
        return tuple(map(int, subprocess.check_output(command, text=True, timeout=10).split()))

    def test_reads_actual_index_and_only_later_headers(self):
        with (self.data / "Large.esp").open("wb") as stream:
            stream.write(plugin_header("Skyrim.esm"))
            stream.truncate(128 * 1024 * 1024)
        self.assertEqual(self.check("Skyrim.esm", ESM.upper(), "Large.esp", slot=1), (MATCH, 2))
        self.assertEqual(self.check("Skyrim.esm", "Update.esm", "Unwatched.esm", ESM, "Large.esp", slot=3),
                         (MATCH, 2))

    def test_absent_duplicate_and_impossible_catalogue(self):
        self.assertEqual(self.check("Skyrim.esm", "Other.esp"), (NOT_LOADED, 0))
        self.assertEqual(self.check("Skyrim.esm", ESM, ESM.upper()), (UNREADABLE, 0))
        self.assertEqual(self.check("Unexpected.esm", ESM), (UNREADABLE, 0))

    def test_rejects_possible_override_and_unreadable_header(self):
        patch = self.data / "Patch.esp"
        patch.write_bytes(plugin_header("skyrim.ESM", ESM.upper()))
        self.assertEqual(self.check("Skyrim.esm", ESM, patch.name), (POSSIBLE_OVERRIDE, 2))
        self.assertEqual(self.check("Skyrim.esm", ESM, slot=1), (MATCH, 2))
        patch.write_bytes(struct.pack("<4sI", b"TES4", 100_000))
        self.assertEqual(self.check("Skyrim.esm", ESM, patch.name), (UNREADABLE, 2))
        patch.unlink()
        self.assertEqual(self.check("Skyrim.esm", ESM, patch.name), (UNREADABLE, 2))
        replacement = patch.with_suffix(".other")
        replacement.write_bytes(plugin_header("Skyrim.esm"))
        patch.symlink_to(replacement)
        self.assertEqual(self.check("Skyrim.esm", ESM, patch.name), (UNREADABLE, 2))

    def test_refuses_a_modified_esm_or_pex(self):
        pex = self.data / PEX
        original = pex.read_bytes()
        pex.write_bytes(bytes([original[0] ^ 1]) + original[1:])
        self.assertEqual(self.check("Skyrim.esm", ESM), (DIFFERENT, 2))
        pex.write_bytes(original)
        (self.data / ESM).write_bytes(b"wrong size")
        self.assertEqual(self.check("Skyrim.esm", ESM), (DIFFERENT, 0))

    def test_startup_checks_the_selected_view_before_the_catalogue_exists(self):
        self.assertEqual(self.check(), (MATCH, 2))
        (self.data / PEX).unlink()
        self.assertEqual(self.check(), (DIFFERENT, 1))
        (self.data / ESM).unlink()
        self.assertEqual(self.check(), (DIFFERENT, 0))

    def test_refuses_case_conflicts_and_linked_assets(self):
        pex = self.data / PEX
        alias = pex.with_name(pex.name.upper())
        if not alias.exists():
            alias.write_bytes(pex.read_bytes())
            self.assertEqual(self.check("Skyrim.esm", ESM), (UNREADABLE, 1))
            alias.unlink()
        os.link(pex, alias.with_suffix(".other"))
        self.assertEqual(self.check("Skyrim.esm", ESM), (UNREADABLE, 1))
        alias.with_suffix(".other").unlink()
        payload = pex.with_suffix(".other")
        pex.rename(payload)
        pex.symlink_to(payload)
        self.assertEqual(self.check("Skyrim.esm", ESM), (UNREADABLE, 1))

    def test_owned_released_assets(self):
        supplied = [os.environ.get("SKYRIM_TEST_OWNED_HDT_EFFECT_ESM"),
                    os.environ.get("SKYRIM_TEST_OWNED_HDT_EFFECT_PEX")]
        if not all(supplied):
            self.skipTest("Set both owned HighHeels ESM and effect PEX paths")
        for source, name in zip(supplied, (ESM, PEX)):
            shutil.copyfile(source, self.data / name)
        self.assertEqual(self.check(real=True), (MATCH, 2))
        self.assertEqual(self.check("Skyrim.esm", "Update.esm", ESM, slot=2, real=True), (MATCH, 2))
        for source, name in zip(supplied, (ESM, PEX)):
            self.assertEqual(Path(source).read_bytes(), (self.data / name).read_bytes())
