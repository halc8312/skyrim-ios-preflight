"""Portable oracle checks and real Mac legacy GPU checks; no iPhone claims."""
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


class LoggedNativeTest(unittest.TestCase):
    def directory(self, label):
        result = ROOT / "local/tests" / f"phone-workloads-{label}-{time.time_ns()}-{os.getpid()}"
        result.mkdir(parents=True, exist_ok=False)
        return result

    def run_logged(self, directory, label, command, timeout=120):
        with (directory / f"{label}.stdout.log").open("wb") as stdout, \
                (directory / f"{label}.stderr.log").open("wb") as stderr:
            result = subprocess.run(command, stdout=stdout, stderr=stderr,
                                    stdin=subprocess.DEVNULL, timeout=timeout, check=False)
        self.assertEqual(result.returncode, 0, (directory / f"{label}.stderr.log").read_text(errors="replace"))
        self.assertEqual((directory / f"{label}.stderr.log").stat().st_size, 0)
        return (directory / f"{label}.stdout.log").read_text().strip()


@unittest.skipUnless(os.name == "posix" and CXX, "requires a POSIX C++17 compiler")
class PortableWorkloadOracleTests(LoggedNativeTest):
    def test_golden_vectors_inputs_and_complete_route_balance(self):
        for mode, flags in (("normal", []), ("sanitized", ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])):
            with self.subTest(mode=mode):
                directory = self.directory(mode)
                executable = directory / "oracle-controls"
                self.run_logged(directory, "compile", [CXX, "-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    *flags, "-I", str(EXPERIMENT), str(ROOT / "tests/native/test_metal_phone_workloads.cpp"),
                    "-o", str(executable)])
                output = self.run_logged(directory, "run", [str(executable)])
                self.assertEqual(output, "PHONE_WORKLOAD_CPU families=3 cases=12 stages=2 balanced_samples=192 golden_vectors=24 first_api_balanced=1 gpu_execution_verified=0")


@unittest.skipUnless(platform.system() == "Darwin", "requires actual Apple Metal device and SDK")
class MacLegacyWorkloadGPUTests(LoggedNativeTest):
    def test_exact_render_compute_oracle_with_original_and_changed_inputs(self):
        directory = self.directory("mac-gpu")
        executable = directory / "gpu-controls"
        report = directory / "gpu-report.json"
        self.run_logged(directory, "compile", ["xcrun", "--sdk", "macosx", "clang++", "-std=c++17", "-O1", "-g",
            "-fobjc-arc", "-fblocks", "-mmacosx-version-min=26.0", "-Wall", "-Wextra", "-Werror",
            "-I", str(EXPERIMENT), str(ROOT / "tests/native/test_metal_phone_workloads_gpu.mm"),
            "-framework", "Foundation", "-framework", "Metal", "-o", str(executable)])
        output = self.run_logged(directory, "run", [str(executable), str(EXPERIMENT / "phone_fixture.metal"), str(report)], 180)
        self.assertEqual(output, "PHONE_WORKLOAD_MAC_LEGACY families=3 cases=12 input_variants=2 readbacks=24 physical_iphone_verified=0 metal4_verified=0")
        data = json.loads(report.read_text())
        self.assertEqual(data["status"], "passed")
        self.assertTrue(data["mac_legacy_gpu_verified"])
        self.assertFalse(data["physical_iphone_verified"])
        self.assertFalse(data["metal4_library_verified"])
        self.assertEqual(len(data["samples"]), 24)
        for sample in data["samples"]:
            self.assertEqual(sample["status"], "passed")
            self.assertTrue(sample["oracle_verified"])
            self.assertEqual(sample["command_status"], "completed")
            for key in ("render_words", "compute_words"):
                self.assertEqual(len(sample[key]), 64)


if __name__ == "__main__":
    unittest.main()
