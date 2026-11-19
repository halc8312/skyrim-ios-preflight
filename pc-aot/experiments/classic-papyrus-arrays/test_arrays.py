"""Isolated experiment. Run with SKYRIM_SKYDREAM_SOURCE pointing at the pinned checkout."""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
PORT = ROOT / "ios/runtime-port"
sys.path.insert(0, str(PORT))
import prepare_classic_skse as classic

spec = importlib.util.spec_from_file_location("experimental_classic_arrays", HERE / "adapt.py")
experiment = importlib.util.module_from_spec(spec)
spec.loader.exec_module(experiment)


class ClassicArrayExperiment(unittest.TestCase):
    def test_optional_registration_and_vm_ownership(self):
        vendor = Path(os.environ.get("SKYRIM_SKYDREAM_SOURCE", ROOT / "vendor/skydream"))
        with tempfile.TemporaryDirectory() as tmp:
            stage = Path(tmp) / "runtime"
            stage.mkdir()
            for name in (*classic.PINNED, classic.PRESENT_FILE, "skse_translation.c"):
                shutil.copyfile(vendor / "runtime" / name, stage / name)
            classic.prepare(vendor, stage)
            table = stage / "skse_compat_values.c"
            baseline = table.read_text()
            self.assertNotIn('F("Utility","CreateIntArray"', baseline)
            for name in classic.PINNED:
                path = stage / name
                path.write_text(experiment.adapt(name, path.read_text()))
            self.assertEqual(table.read_text().count('    F(') - baseline.count('    F('), 7)
            self.assertIn("skse_classic_array_run(c, fn, args, registry, result);",
                          (stage / "skse_system.c").read_text())
            if os.name != "posix":
                self.skipTest("the synthetic registry fixture uses a POSIX C compiler")
            sanitizers = (["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
                          if os.environ.get("SKYRIM_TEST_SANITIZERS") else [])
            binary = Path(tmp) / "arrays"
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                            *sanitizers, "-I", str(stage), "-I", str(HERE), str(HERE / "test_arrays.c"),
                            str(table), "-lm", "-o", str(binary)], check=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=15)


if __name__ == "__main__":
    unittest.main()
