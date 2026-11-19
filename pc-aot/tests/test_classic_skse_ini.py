"""Check Classic Game and Utility queries against the staged registration table."""
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
        registration = (self.stage / "skse_compat.c").read_text()
        self.assertLess(registration.index("probe_native(c,object,fn);"), registration.index("++passed;"))
        self.assertLess(registration.index("++passed;"), registration.index("IOS_CLASSIC_VM registered="))
        staged = (self.stage / "skse_system.c").read_text()
        self.assertIn("skse_classic_ini_dispatch(c, op, args, result);", staged)
        self.assertIn("if (skse_classic_mod_query(c, op, args, result)) return 1;", staged)
        self.assertNotIn("double number=name[0]", staged)

    def test_existing_classic_overlay_is_upgraded_without_replacing_local_edits(self):
        from classic_skse_translation import adapt
        for name in self.files:
            target = self.stage / name
            original = target.read_text()
            previous = adapt(original) if name == "skse_translation.c" else classic.adapt_ini(name, original)
            target.write_text(previous)
        self.assertEqual(classic.prepare(self.vendor, self.stage), len(classic.PINNED))
        self.assertEqual(classic.prepare(self.vendor, self.stage), 0)

    def test_rejects_vendor_target_and_staged_drift_before_publishing(self):
        with self.assertRaisesRegex(ValueError, "outside the vendor"):
            classic.prepare(self.vendor, self.vendor / "runtime")
        (self.stage / "skse_system.c").write_text("staged local edit")
        with self.assertRaisesRegex(ValueError, "unrecognized contents"):
            classic.prepare(self.vendor, self.stage)
        self.assertEqual((self.stage / "skse_compat.h").read_bytes(),
                         (self.vendor / "runtime/skse_compat.h").read_bytes())

    def run_fixture(self, name):
        classic.prepare(self.vendor, self.stage)
        executable = self.stage.parent / name
        sanitizers = []
        if os.environ.get("SKYRIM_TEST_SANITIZERS"):
            sanitizers = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        *sanitizers, "-I", str(self.stage), "-I", str(PORT),
                        str(ROOT / "tests/native" / (name + ".c")),
                        str(self.stage / "skse_compat_values.c"), "-lm", "-o", str(executable)],
                       check=True, timeout=60)
        return subprocess.run([str(executable)], check=True, timeout=30, capture_output=True,
                              text=True).stderr.splitlines()

    @unittest.skipUnless(os.name == "posix", "needs a POSIX C compiler")
    def test_real_registration_table_dispatches_typed_results(self):
        self.run_fixture("test_classic_skse_ini")

    @unittest.skipUnless(os.name == "posix", "needs a POSIX C compiler")
    def test_game_queries_read_loaded_files_and_master_indices(self):
        lines = self.run_fixture("test_classic_skse_mod_list")
        self.assertEqual(len(lines), len(set(lines)))
        for query in range(7):
            self.assertIn(f"IOS_CLASSIC_QUERY id={query} handled=1", lines)
        self.assertIn("IOS_CLASSIC_QUERY id=0 handled=0", lines)
        for line in lines:
            self.assertRegex(line, r"^IOS_CLASSIC_QUERY id=[0-6] handled=[01]$")


if __name__ == "__main__":
    unittest.main()
