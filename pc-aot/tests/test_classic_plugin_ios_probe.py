"""Exercise the actual Classic Lab object target with the isolated game symbols present."""
from concurrent.futures import ThreadPoolExecutor
import ctypes
from importlib import import_module
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / "experiments/classic-plugin"
sys.path.insert(0, str(PROBE))
try:
    runner = import_module("run")
    serial = import_module("run_serialization")
    translation = import_module("verify_ios_translation")
finally:
    sys.path.pop(0)


class Snapshot(unittest.TestCase):
    def test_fixed_translation_identifies_its_authored_input(self):
        for name in ("serial_fixture_aot.c", "funcs_serial.h", "serial_layout.h"):
            self.assertIn(serial.HASH, (PROBE / "ios" / name).read_text(encoding="ascii"))
        self.assertEqual(len(serial.expected_bytes(16)), 83)

    def test_regenerate_from_real_windows_dll(self):
        work = os.environ.get("CLASSIC_SERIAL_WINDOWS_WORK")
        if not work:
            self.skipTest("set CLASSIC_SERIAL_WINDOWS_WORK for regeneration from the Windows reference")
        skydream = Path(os.environ.get("CLASSIC_PLUGIN_SKYDREAM", ROOT / "vendor/skydream"))
        for name, expected in translation.source_snapshot(Path(work).resolve(), skydream.resolve()).items():
            self.assertEqual((PROBE / "ios" / name).read_text(encoding="ascii"), expected, name)


class Observation(ctypes.Structure):
    _fields_ = [(name, ctypes.c_uint32) for name in ("input", "reverted", "output", "reads", "mask")]


class DeviceProbe(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if os.name != "posix" or not shutil.which("cmake") or not shutil.which("cc"):
            raise unittest.SkipTest("a native POSIX C compiler and CMake are required")
        skydream = Path(os.environ.get("CLASSIC_PLUGIN_SKYDREAM", ROOT / "vendor/skydream")).resolve()
        if not (skydream / "runtime" / "x86.h").is_file():
            raise unittest.SkipTest("stage the pinned public Skydream source to compile the probe")
        temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(temp.cleanup)
        work = Path(temp.name)
        source = work / "upstream"
        source.mkdir()
        runner.stage(skydream, source)
        port = ROOT / "ios/runtime-port"
        (work / "CMakeLists.txt").write_text(f"""
cmake_minimum_required(VERSION 3.24)
project(ProbeTest LANGUAGES C)
include("{(PROBE / 'ios/ClassicPlugin.cmake').as_posix()}")
add_library(probe SHARED test.c "{(port / 'classic_cosave.c').as_posix()}")
target_include_directories(probe PRIVATE "{port.as_posix()}")
skyrim_add_classic_plugin_probe(probe "{(source / 'runtime').as_posix()}")
target_link_libraries(probe PRIVATE m)
set_target_properties(probe PROPERTIES PREFIX "" SUFFIX ".test")
""", encoding="ascii")
        (work / "test.c").write_text("""
#include <stdlib.h>
#include "classic_plugin_probe.h"
/* These names belong to the game. A collision must fail at link time. */
unsigned char *MEM;
void rt_call(void) { abort(); }
void rt_call_ic(void) { abort(); }
int test_run(const char *source, const char *dest, SkyrimClassicFixtureResult *observation) {
    return skyrim_classic_fixture_run(source, dest, observation);
}
""", encoding="ascii")
        build = work / "build"
        for args in (["cmake", "-S", work, "-B", build], ["cmake", "--build", build, "--parallel", "2"]):
            job = subprocess.run(args, capture_output=True, text=True, timeout=120)
            if job.returncode:
                raise AssertionError(job.stdout + job.stderr)
        cls.lib = ctypes.CDLL(str(next(build.rglob("probe.test"))))
        cls.lib.test_run.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.POINTER(Observation)]
        cls.lib.test_run.restype = ctypes.c_int

    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.work = Path(temp.name)
        self.source = self.work / "windows.skse"
        reference = os.environ.get("CLASSIC_SERIAL_WINDOWS_WORK")
        if reference:
            shutil.copyfile(Path(reference) / "windows.skse", self.source)
        else:
            self.source.write_bytes(serial.expected_bytes(16))
        self.output = self.work / "translated.skse"

    def execute(self, source, target):
        observed = Observation(1, 1, 1, 1, 1)
        result = self.lib.test_run(os.fsencode(source), os.fsencode(target), ctypes.byref(observed))
        return result, tuple(getattr(observed, field) for field, _ in observed._fields_)

    def test_repeated_execution_and_existing_export(self):
        for name in ("first.skse", "second.skse"):
            target = self.work / name
            self.assertEqual(self.execute(self.source, target), (0, (16, 5, 21, 1, 7)))
            self.assertEqual(target.read_bytes(), serial.expected_bytes(21))
            self.assertEqual(self.execute(self.source, target), (3, (0,) * 5))
            self.assertEqual(target.read_bytes(), serial.expected_bytes(21))
        self.assertEqual(self.source.read_bytes(), serial.expected_bytes(16))
        self.assertEqual(sorted(p.name for p in self.work.iterdir()), ["first.skse", "second.skse", "windows.skse"])

    def test_rejects_tampered_or_unrelated_input_before_creating_output(self):
        data = self.source.read_bytes()
        self.assertEqual(self.execute(self.source, self.source), (3, (0,) * 5))
        for bad in (data[:-1], data + b"x", data.replace(b"opaque", b"edited"), serial.expected_bytes(21), b"TESV_SAVEGAME"):
            self.source.write_bytes(bad)
            self.assertEqual(self.execute(self.source, self.output), (2, (0,) * 5))
            self.assertFalse(self.output.exists())
        self.source.write_bytes(data)
        link = self.work / "link.skse"
        link.symlink_to(self.source)
        self.assertEqual(self.execute(link, self.output), (2, (0,) * 5))
        self.assertEqual(self.execute(self.source, link), (3, (0,) * 5))
        self.assertEqual(self.execute(self.source, self.output.with_suffix(".ess")), (1, (0,) * 5))
        self.assertEqual(self.source.read_bytes(), data)

    def test_concurrent_publication_keeps_one_complete_output(self):
        with ThreadPoolExecutor(max_workers=4) as workers:
            attempts = list(workers.map(lambda _: self.execute(self.source, self.output), range(10)))
        self.assertEqual(sum(status == 0 for status, _ in attempts), 1)
        self.assertTrue(all(status in (0, 3, 4) for status, _ in attempts))
        self.assertEqual(self.output.read_bytes(), serial.expected_bytes(21))
        self.assertCountEqual([p.name for p in self.work.iterdir()], ["windows.skse", "translated.skse"])


if __name__ == "__main__":
    unittest.main()
