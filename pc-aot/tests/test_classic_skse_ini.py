"""Check typed Classic INI reads, the real registration table and the opt-in overlay."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / "ios/runtime-port"
sys.path.insert(0, str(PORT))
import prepare_classic_skse as classic


class ClassicIniTests(unittest.TestCase):
    def setUp(self):
        self.vendor = Path(os.environ.get("SKYRIM_SKYDREAM_SOURCE", ROOT / "vendor/skydream"))
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.stage = Path(tmp.name) / "runtime"
        self.stage.mkdir()
        self.files = (*classic.PINNED, "skse_translation.c")
        for name in self.files:
            shutil.copyfile(self.vendor / "runtime" / name, self.stage / name)

    def test_ordinary_table_and_sources_are_preserved_and_staging_is_repeatable(self):
        before = [(self.vendor / "runtime" / name).read_bytes() for name in self.files]
        self.assertNotIn('F("Utility","GetINIBool"', (self.stage / "skse_compat_values.c").read_text())
        self.assertEqual(classic.prepare(self.vendor, self.stage), len(self.files))
        self.assertEqual(classic.prepare(self.vendor, self.stage), 0)
        self.assertEqual([(self.vendor / "runtime" / name).read_bytes() for name in self.files], before)
        staged = (self.stage / "skse_system.c").read_text()
        self.assertIn("skse_classic_ini_dispatch(c, op, args, result);", staged)
        self.assertNotIn("double number=name[0]", staged)

    def test_rejects_vendor_target_and_staged_drift_before_publishing(self):
        with self.assertRaisesRegex(ValueError, "outside the vendor"):
            classic.prepare(self.vendor, self.vendor / "runtime")
        (self.stage / "skse_system.c").write_text("staged local edit")
        with self.assertRaisesRegex(ValueError, "unrecognized contents"):
            classic.prepare(self.vendor, self.stage)
        self.assertEqual((self.stage / "skse_compat.h").read_bytes(),
                         (self.vendor / "runtime/skse_compat.h").read_bytes())

    @unittest.skipUnless(os.name == "posix", "needs a POSIX C compiler")
    def test_real_registration_table_dispatches_typed_results(self):
        classic.prepare(self.vendor, self.stage)
        executable = self.stage.parent / "classic-ini"
        sanitizers = []
        if os.environ.get("SKYRIM_TEST_SANITIZERS"):
            sanitizers = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        *sanitizers, "-I", str(self.stage), "-I", str(PORT),
                        str(ROOT / "tests/native/test_classic_skse_ini.c"),
                        str(self.stage / "skse_compat_values.c"), "-lm", "-o", str(executable)],
                       check=True, timeout=60)
        subprocess.run([str(executable)], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
