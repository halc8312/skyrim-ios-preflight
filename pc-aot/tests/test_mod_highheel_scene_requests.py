"""Run the hosted HighHeels request queue with an authored scene owner."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from test_mod_hooks_binding import write_fixture_headers

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / "ios/runtime-port"
HIGHHEELS = ROOT / "experiments/classic-highheels"


@unittest.skipUnless(os.name == "posix", "Requires a POSIX C compiler")
class HighHeelSceneRequestsTests(unittest.TestCase):
    def test_coalesces_current_requests_and_reverses_only_owned_height(self):
        compiler = shutil.which(os.environ.get("CC", "cc"))
        if not compiler:
            self.skipTest("C compiler unavailable")
        flags = ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O2", "-pthread"]
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            flags += ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            if sys.platform.startswith("linux"):
                flags += ["-fno-pie", "-no-pie"]
        with tempfile.TemporaryDirectory() as temp:
            stage = Path(temp)
            write_fixture_headers(stage)
            binary = stage / "highheel-scene-requests"
            includes = (stage, PORT, HIGHHEELS, ROOT / "ios/private-abi",
                        ROOT / "experiments/fuz-voice", ROOT / "experiments/skse-cosave")
            for path in includes:
                flags += ["-I", path]
            subprocess.run([compiler, *flags,
                            ROOT / "tests/native/test_mod_highheel_scene_requests.c",
                            PORT / "mod_highheel_scene_requests.c", HIGHHEELS / "highheel_height.c",
                            "-lm", "-o", binary], check=True, timeout=60)
            subprocess.run([binary], check=True, timeout=10)
