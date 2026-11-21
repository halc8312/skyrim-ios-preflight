"""Exercise the hosted profile copy with authored saves and injected I/O failures."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ProfileCopyTests(unittest.TestCase):
    def test_explicit_copy_and_failure_paths(self):
        compiler = shutil.which(os.environ.get("CC", "cc"))
        if not compiler or sys.platform not in ("linux", "darwin"):
            self.skipTest("Run the native copy test on macOS or Linux/WSL")
        flags = ["-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror"]
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        with tempfile.TemporaryDirectory(prefix="skyrim-profile-copy-") as tmp:
            workspace = Path(tmp)
            data = workspace / "authored-saves"
            data.mkdir()
            binary = workspace / "profile-copy-test"
            subprocess.run([compiler, *flags, str(ROOT / "tests/native/test_skse_copy_pair.c"),
                            "-o", str(binary)], check=True, timeout=60)
            result = subprocess.run([str(binary), str(data)], check=True, capture_output=True,
                                    text=True, timeout=60)
            self.assertEqual(result.stdout.strip(), "classic_profile_copy=PASS")


if __name__ == "__main__":
    unittest.main()
