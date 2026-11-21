"""Exercise Classic input bindings and typed UI reads on synthetic guest values."""
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


class ClassicControlTests(unittest.TestCase):
    def setUp(self):
        self.vendor = Path(os.environ.get("SKYRIM_SKYDREAM_SOURCE", ROOT / "vendor/skydream"))
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.stage = Path(tmp.name) / "runtime"
        self.stage.mkdir()
        for name in (*classic.PINNED, classic.PRESENT_FILE, "skse_translation.c"):
            shutil.copyfile(self.vendor / "runtime" / name, self.stage / name)
        classic.prepare(self.vendor, self.stage)

    def test_prepared_registration_and_system_dispatch(self):
        table = (self.stage / "skse_compat_values.c").read_text()
        self.assertIn('F("Input","GetMappedControl",2,1,3,0,0,0,S_INPUT_CONTROL)', table)
        self.assertIn('F("Input","GetMappedKey",3,2,2,3,0,0,S_INPUT_KEY)', table)
        system = (self.stage / "skse_system.c").read_text()
        self.assertIn('#include "classic_skse_extra.h"', system)
        self.assertIn("if (op == S_INPUT_CONTROL || op == S_INPUT_KEY)", system)
        self.assertIn("skse_classic_input_query(c, op, args, result);", system)
        self.assertIn("skse_classic_ui_read(c, op, args, result);", system)

    def run_native(self, name):
        compiler = os.environ.get("CC", "cc")
        binary = self.stage.parent / name
        sanitizers = (["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
                      if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1" else [])
        subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                        *sanitizers,
                        "-I", str(self.stage), "-I", str(PORT),
                        str(ROOT / "tests/native" / (name + ".c")),
                        str(self.stage / "skse_compat_values.c"), "-lm", "-o", str(binary)],
                       check=True, timeout=60)
        subprocess.run([str(binary)], check=True, timeout=30)

    @unittest.skipUnless(os.name == "posix", "uses a POSIX C compiler")
    def test_native_engine_tables_and_device_selection(self):
        self.run_native("test_classic_skse_extra")

    @unittest.skipUnless(os.name == "posix", "uses a POSIX C compiler")
    def test_native_ui_types_lookup_success_and_cleanup(self):
        self.run_native("test_classic_skse_ui_values")


class ClassicModEventTests(unittest.TestCase):
    def setUp(self):
        self.vendor = Path(os.environ.get("SKYRIM_SKYDREAM_SOURCE", ROOT / "vendor/skydream"))
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.stage = Path(tmp.name) / "runtime"
        self.stage.mkdir()
        for name in (*classic.PINNED, classic.PRESENT_FILE, "skse_translation.c"):
            shutil.copyfile(self.vendor / "runtime" / name, self.stage / name)
        classic.prepare(self.vendor, self.stage)

    def test_statics_are_registered_only_in_hook_builds(self):
        table = (self.stage / "skse_compat_values.c").read_text()
        block = table[table.index("#ifdef SKYRIM_IOS_MOD_HOOKS\n"):table.index("\n};\n#undef F")]
        for row in ('F("ModEvent","Create",3,1,2,0,0,0,S_MODEVENT_CREATE)',
                    'F("ModEvent","PushBool",0,2,3,5,0,0,S_MODEVENT_PUSH_BOOL)',
                    'F("ModEvent","Send",5,1,3,0,0,0,S_MODEVENT_SEND)',
                    'F("ModEvent","Release",0,1,3,0,0,0,S_MODEVENT_RELEASE)'):
            self.assertIn(row, block)
        self.assertIn("S_MODEVENT_CREATE=1330", (self.stage / "skse_compat.h").read_text())
        system = (self.stage / "skse_system.c").read_text()
        self.assertIn('#include "classic_skse_mod_event.h"', system)
        self.assertIn("return skse_classic_mod_event(c, op, args, registry, result);", system)

    @unittest.skipUnless(os.name == "posix", "uses a POSIX C compiler")
    def test_native_packets_delivery_and_generations(self):
        compiler = os.environ.get("CC", "cc")
        binary = self.stage.parent / "mod_event"
        sanitizers = (["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
                      if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1" else [])
        events = ROOT / "experiments/classic-mod-events"
        subprocess.run([compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-pthread", *sanitizers,
                        "-DSKYRIM_IOS_MOD_HOOKS=1", "-I", str(PORT), "-I", str(events),
                        str(ROOT / "tests/native/test_classic_skse_mod_event.c"), str(events / "mod_event.c"),
                        str(PORT / "classic_vm_epoch.c"), "-o", str(binary)], check=True, timeout=60)
        subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
