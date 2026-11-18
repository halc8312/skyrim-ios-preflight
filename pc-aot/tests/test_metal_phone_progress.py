"""Actual portable checkpoint policy; native host controls test stop/callback behavior."""
from pathlib import Path
import os
import shutil
import subprocess
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
CXX = shutil.which(os.environ.get("CXX", "c++"))


@unittest.skipUnless(os.name == "posix" and CXX, "requires a POSIX C++17 compiler")
class PhoneProgressPolicyTests(unittest.TestCase):
    def test_fixed_completed_sample_recovery_bound_and_terminal_coverage(self):
        output = ROOT / "local/tests" / f"phone-progress-{time.time_ns()}-{os.getpid()}"
        output.mkdir(parents=True, exist_ok=False)
        binary = output / "progress-policy"
        commands = [
            [CXX, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", "-I",
             str(ROOT / "experiments/metal-compile"), str(ROOT / "tests/native/test_metal_phone_progress.cpp"),
             "-o", str(binary)], [str(binary)]]
        for name, command in zip(("compile", "run"), commands):
            with (output / f"{name}.stdout.log").open("wb") as stdout, \
                    (output / f"{name}.stderr.log").open("wb") as stderr:
                result = subprocess.run(command, stdout=stdout, stderr=stderr, stdin=subprocess.DEVNULL,
                                        timeout=60, check=False)
            self.assertEqual(result.returncode, 0, (output / f"{name}.stderr.log").read_text(errors="replace"))
            self.assertEqual((output / f"{name}.stderr.log").stat().st_size, 0)
        self.assertEqual((output / "run.stdout.log").read_text().strip(),
                         "PHONE_PROGRESS_POLICY considered=601 snapshots=73 skipped=528 suffix=4 gpu_execution_verified=0")


if __name__ == "__main__":
    unittest.main()
