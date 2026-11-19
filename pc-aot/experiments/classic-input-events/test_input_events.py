"""Exercise the opt-in event observer against the fixed Skydream source."""
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
sys.path.insert(0, str(ROOT / "ios/runtime-port"))
import prepare_classic_skse as classic

spec = importlib.util.spec_from_file_location("experimental_classic_input", HERE / "adapt.py")
experiment = importlib.util.module_from_spec(spec)
spec.loader.exec_module(experiment)


def stage_sources(vendor, stage):
    stage.mkdir()
    for name in (*classic.PINNED, classic.PRESENT_FILE, "skse_translation.c", *experiment.PINNED_INPUT):
        shutil.copyfile(vendor / "runtime" / name, stage / name)
    classic.prepare(vendor, stage)
    table = stage / "skse_compat_values.c"
    before = table.read_text().count('    F(')
    for name in (*classic.PINNED, *experiment.PINNED_INPUT):
        target = stage / name
        target.write_text(experiment.adapt(name, target.read_text()))
    for name in ("classic_input_events.h", "classic_input_receiver.h"):
        shutil.copyfile(HERE / name, stage / name)
    observer = (stage / "skse_input.c").read_text()
    (stage / "fixture_observer.inc").write_text(
        observer[observer.index("static unsigned macro_key("):observer.index("void skse_input_install(")])
    system = (stage / "skse_system.c").read_text()
    (stage / "fixture_copy.inc").write_text(
        system[system.index("static void event_copy("):system.index("static pthread_once_t classic_event_once")])
    dispatcher = system.index("int skse_system_run(")
    normal_path = system.index("    vm_clear(c,result);M32(result)=fn->result;", dispatcher)
    (stage / "fixture_dispatch.inc").write_text(system[dispatcher:normal_path] + "    return -1;\n}\n")
    return table.read_text().count('    F(') - before


class ClassicInputEvents(unittest.TestCase):
    def test_live_observer_and_registration(self):
        vendor = Path(os.environ.get("SKYRIM_SKYDREAM_SOURCE", ROOT / "vendor/skydream"))
        with tempfile.TemporaryDirectory() as tmp:
            runtime = Path(tmp) / "runtime"
            self.assertEqual(stage_sources(vendor, runtime), 9)
            self.assertNotIn("platform_keys(", (runtime / "skse_system.c").read_text())
            if os.name != "posix":
                self.skipTest("the concurrent C fixture uses POSIX threads")
            check = Path(tmp) / "input-events"
            flags = (["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
                     if os.environ.get("SKYRIM_TEST_SANITIZERS") else [])
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-pthread", *flags, "-I", str(runtime), str(HERE / "test_input_events.c"),
                            str(runtime / "skse_compat_values.c"), "-lm", "-o", str(check)], check=True, timeout=60)
            subprocess.run([str(check)], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
