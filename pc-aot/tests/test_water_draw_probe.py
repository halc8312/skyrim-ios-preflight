"""Run the real CTAB classifier and capture lifecycle on authored D3D9 metadata."""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / "ios/runtime-port"


class WaterDrawProbeTests(unittest.TestCase):
    def test_shader_groups_capture_limits_and_diagnostic_transport(self):
        compiler = shutil.which(os.environ.get("CXX", "c++"))
        if not compiler:
            self.skipTest("C++17 compiler required")
        spec = importlib.util.spec_from_file_location(
            "water_diagnostic_receiver", ROOT / "scripts/receive_private_diagnostics.py")
        receiver = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = receiver
        spec.loader.exec_module(receiver)
        schema = receiver.Schema()
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / ("water-probe.exe" if os.name == "nt" else "water-probe")
            flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread"]
            if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
                flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            subprocess.run([compiler, *flags, "-I", str(PORT), str(PORT / "water_probe.cpp"),
                            str(ROOT / "tests/native/test_water_draw_probe.cpp"),
                            "-o", str(binary)], check=True)
            captured = subprocess.run([str(binary), "--records"], capture_output=True, text=True,
                                      check=True, timeout=20)
        self.assertFalse(captured.stdout)
        lines = captured.stderr.splitlines()
        self.assertEqual(lines, [
            "IOS_WATER_DRAW schema=1 scene=2 kind=1 count=1 depth=0,0,1,0 ms_depth=0 "
            "color=0,1,0 params=0,0,1,0",
            "IOS_WATER_CAMERA schema=1 scene=2 count=1 endpoints=0,0,0,0,0,0,0,0,1",
            "IOS_WATER_VIEW schema=1 scene=2 role=1 format=100 aspect=1 width=1564 height=720 flags=3 count=1",
            "IOS_WATER_VIEW schema=1 scene=2 role=2 format=0 aspect=0 width=0 height=0 flags=0 count=1",
            "IOS_WATER_SUMMARY schema=1 scene=2 end=1 frames=3 checked=1 fixed=0 unknown=0 other=0 limited=0",
        ])
        for line in lines:
            schema.line(line)
            with self.assertRaises(receiver.Rejected):
                schema.line(line + " shader=not-an-allowed-field")


if __name__ == "__main__":
    unittest.main()
