"""Real CPU oracle/order controls and Apple-only MRC/legacy GPU controls."""
from pathlib import Path
import json
import os
import platform
import shutil
import subprocess
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
EXPERIMENT = ROOT / "experiments/metal-compile"
CXX = shutil.which(os.environ.get("CXX", "c++"))


class Logged(unittest.TestCase):
    def directory(self, kind):
        directory = ROOT / "local/tests" / f"phone-contention-{kind}-{time.time_ns()}-{os.getpid()}"
        directory.mkdir(parents=True, exist_ok=False)
        return directory

    def execute(self, directory, label, command, timeout=180):
        with (directory / f"{label}.stdout.log").open("wb") as out, \
                (directory / f"{label}.stderr.log").open("wb") as err:
            result = subprocess.run(command, stdout=out, stderr=err, stdin=subprocess.DEVNULL,
                                    timeout=timeout, check=False)
        self.assertEqual(result.returncode, 0, (directory / f"{label}.stderr.log").read_text(errors="replace"))
        self.assertEqual((directory / f"{label}.stderr.log").stat().st_size, 0)
        return (directory / f"{label}.stdout.log").read_text().strip()


@unittest.skipUnless(os.name == "posix" and CXX, "requires a POSIX C++17 compiler")
class CPUControls(Logged):
    def test_frozen_oracle_and_actual_schedule_both_orders(self):
        for mode, flags in (("normal", []), ("sanitized", ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])):
            with self.subTest(mode=mode):
                directory = self.directory(mode)
                exe = directory / "cpu-controls"
                self.execute(directory, "compile", [CXX, "-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    *flags, "-I", str(EXPERIMENT), str(ROOT / "tests/native/test_metal_phone_contention.cpp"), "-o", str(exe)])
                self.assertEqual(self.execute(directory, "run", [str(exe)]),
                    "PHONE_CONTENTION_CPU golden_vectors=24 balanced_trials=32 requests=96 base_orders=2 gpu_execution_verified=0")


@unittest.skipUnless(platform.system() == "Darwin", "requires Apple SDK, Foundation, and actual Metal GPU")
class MacMRCControls(Logged):
    def test_manual_ownership_success_error_partial_stop_and_legacy_gpu(self):
        directory = self.directory("mac-mrc")
        exe = directory / "mrc-controls"
        report = directory / "gpu-report.json"
        self.execute(directory, "compile", ["xcrun", "--sdk", "macosx", "clang++", "-std=c++17", "-O1", "-g",
            "-fno-objc-arc", "-fblocks", "-fexceptions", "-fobjc-exceptions", "-mmacosx-version-min=26.0",
            "-Wall", "-Wextra", "-Werror", "-I", str(EXPERIMENT),
            str(ROOT / "tests/native/test_metal_phone_contention.mm"),
            "-framework", "Foundation", "-framework", "Metal", "-o", str(exe)])
        self.assertEqual(self.execute(directory, "run", [str(exe), str(EXPERIMENT / "phone_fixture.metal"), str(report)]),
            "PHONE_CONTENTION_MAC_MRC boundaries=3 readbacks=6 compiler_errors=1 stop_after_submit=2 physical_iphone_verified=0 metal4_task_verified=0")
        data = json.loads(report.read_text())
        self.assertEqual(data["status"], "passed")
        self.assertFalse(data["physical_iphone_verified"])
        self.assertFalse(data["metal4_task_lifetime_verified"])
        self.assertTrue(data["mac_legacy_mrc_verified"])
        self.assertEqual(len(data["trials"]), 2)
        self.assertEqual(len(data["error_controls"]), 1)
        self.assertEqual(len(data["partial_stop"]), 2)
        for trial in data["trials"]:
            self.assertEqual(len(trial["jobs"]), 3)
            for job in trial["jobs"]:
                self.assertTrue(job["oracle_verified"])
                self.assertEqual(len(job["compute_words"]), 64)
                self.assertTrue(job["owned_input_snapshots_verified"])
                self.assertTrue(job["adapter_input_reference_released_after_drain"])
                self.assertFalse(job["metal4_task_joined"])


if __name__ == "__main__":
    unittest.main()
