"""Exercise the hosted Classic HighHeels native provider's request lifecycle."""
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
class HighHeelNativeHostTests(unittest.TestCase):
    def test_accepts_current_effect_only_and_preserves_request_on_failed_commit(self):
        cc = shutil.which(os.environ.get("CC", "cc"))
        if not cc:
            self.skipTest("C compiler unavailable")
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            write_fixture_headers(stage)
            binary = stage / "highheel-native-host"
            includes = (stage, PORT, HIGHHEELS, ROOT / "ios/private-abi",
                        ROOT / "experiments/fuz-voice", ROOT / "experiments/skse-cosave")
            flags = ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O2", "-pthread"]
            if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
                flags += ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
                if sys.platform.startswith("linux"):
                    flags += ["-fno-pie", "-no-pie"]
            for path in includes:
                flags += ["-I", path]
            subprocess.run([cc, *flags, ROOT / "tests/native/test_mod_highheel_native_host.c",
                            PORT / "mod_highheel_native_host.c", PORT / "classic_vm_epoch.c",
                            HIGHHEELS / "highheel_native_frame.c", "-o", binary],
                           check=True, timeout=60)
            subprocess.run([binary], check=True, timeout=10)
