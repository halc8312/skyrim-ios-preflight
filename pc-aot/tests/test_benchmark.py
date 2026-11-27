"""Exercise the benchmark scheduler without UIKit or game data."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class BenchmarkTests(unittest.TestCase):
    def test_scheduler_and_frame_intervals(self):
        cc = shutil.which(os.environ.get("CC", "cc"))
        if not cc:
            self.skipTest("C compiler required; run on WSL/Mac")

        runtime = ROOT / "ios/runtime-port"
        flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-O2", "-pthread"]
        if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
            flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        with tempfile.TemporaryDirectory(prefix="skyrim-benchmark-") as tmp:
            binary = Path(tmp) / "benchmark"
            subprocess.run([
                cc, *flags, "-I", str(runtime), str(runtime / "benchmark.c"),
                str(ROOT / "tests/native/test_benchmark.c"), "-o", str(binary),
            ], check=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=15)


if __name__ == "__main__":
    unittest.main()
