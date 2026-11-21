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
        for name in (*classic.PINNED, classic.PRESENT_FILE, "skse_translation.c", classic.INPUT_FILE):
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
        for name in (*classic.PINNED, classic.PRESENT_FILE, "skse_translation.c", classic.INPUT_FILE):
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

    @unittest.skipUnless(os.name == "posix", "uses a POSIX C compiler")
    def test_staged_skyui_registrations_rebind_once_and_survive_revert(self):
        # Compile the whole staged dispatcher. Only the unrelated SDL event
        # dependencies and guest VM boundaries are supplied by the fixture.
        runtime = self.vendor / "runtime"
        shutil.copyfile(runtime / "skse_runtime.h", self.stage / "skse_runtime.h")
        sdl = self.stage / "SDL2"
        sdl.mkdir()
        (sdl / "SDL.h").write_text(
            "#include <stdint.h>\ntypedef uint8_t Uint8;\n"
            "enum { SDL_NUM_SCANCODES = 512 };\nuint32_t SDL_GetTicks(void);\n", encoding="utf-8")
        binary = self.stage.parent / "skyui_registration"
        sanitizers = []
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            sanitizers += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
            if sys.platform.startswith("linux"):
                sanitizers += ["-fno-pie", "-no-pie"]
        events = ROOT / "experiments/classic-mod-events"
        controls = ROOT / "experiments/classic-control-events"
        subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-pthread",
                        *sanitizers, "-DSKYDREAM_SKSE=1", "-DSKYRIM_IOS_MOD_HOOKS=1", "-I", str(self.stage),
                        "-I", str(PORT), "-I", str(events), "-I", str(controls), "-isystem", str(runtime),
                        str(ROOT / "tests/native/test_classic_skyui_registration.c"),
                        str(self.stage / "skse_system.c"), str(self.stage / "skse_compat_values.c"),
                        str(events / "mod_event.c"), str(controls / "control_bridge.c"),
                        str(PORT / "classic_vm_epoch.c"), "-lm", "-o", str(binary)], check=True, timeout=60)
        subprocess.run([str(binary)], check=True, timeout=15)


class ClassicControlEventTests(unittest.TestCase):
    def setUp(self):
        self.vendor = Path(os.environ.get("SKYRIM_SKYDREAM_SOURCE", ROOT / "vendor/skydream"))
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.stage = Path(tmp.name) / "runtime"
        self.stage.mkdir()
        for name in (*classic.PINNED, classic.PRESENT_FILE, "skse_translation.c", classic.INPUT_FILE):
            shutil.copyfile(self.vendor / "runtime" / name, self.stage / name)
        classic.prepare(self.vendor, self.stage)

    def test_methods_dispatch_and_input_edge_are_staged(self):
        table = (self.stage / "skse_compat_values.c").read_text()
        block = table[table.index("#ifdef SKYRIM_IOS_MOD_HOOKS\n"):table.index("\n};\n#undef F")]
        for cls in classic.CONTROL_CLASSES:
            for row in (f'F("{cls}","RegisterForControl",0,1,2,0,0,0,S_METHOD|S_REG_CONTROL)',
                        f'F("{cls}","UnregisterForControl",0,1,2,0,0,0,S_METHOD|S_UNREG_CONTROL)',
                        f'F("{cls}","UnregisterForAllControls",0,0,0,0,0,0,S_METHOD|S_UNREG_CONTROLS)'):
                self.assertIn(row, block)
        self.assertEqual(block.count("Control\",") + block.count("Controls\","), 9)
        system = (self.stage / "skse_system.c").read_text()
        self.assertIn('#include "classic_skse_control.h"', system)
        self.assertIn("return skse_classic_control(c, op, args, registry, base);", system)
        observer = (self.stage / classic.INPUT_FILE).read_text()
        self.assertIn(classic.INPUT_CONTROL_EDGE + classic.INPUT_GFX_CALL, observer)
        self.assertEqual(observer.count("skyrim_classic_control_edge("), 2)  # declaration and call
        self.assertEqual(classic.prepare(self.vendor, self.stage), 0)

    @unittest.skipUnless(os.name == "posix", "uses a POSIX C compiler")
    def test_native_registration_edges_deletion_and_generations(self):
        compiler = os.environ.get("CC", "cc")
        binary = self.stage.parent / "controls"
        sanitizers = (["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
                      if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1" else [])
        controls = ROOT / "experiments/classic-control-events"
        subprocess.run([compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-pthread", *sanitizers,
                        "-DSKYRIM_IOS_MOD_HOOKS=1", "-I", str(PORT), "-I", str(controls),
                        str(ROOT / "tests/native/test_classic_skse_control.c"), str(controls / "control_bridge.c"),
                        str(PORT / "classic_vm_epoch.c"), "-o", str(binary)], check=True, timeout=60)
        subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
