"""Real Mac callable-host failure controls; not iPhone/Metal 4 GPU evidence."""
from pathlib import Path
import os
import platform
import subprocess
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(platform.system() == "Darwin", "requires Apple SDK and Foundation")
class PhoneRuntimeBoundaryTests(unittest.TestCase):
    def test_callable_host_refuses_main_thread_invalid_repeat_stop_and_non_phone(self):
        output = ROOT / "local/tests" / f"phone-runtime-{time.time_ns()}-{os.getpid()}"
        output.mkdir(parents=True, exist_ok=False)
        executable = output / "phone-runtime-controls"
        commands = [
            ["xcrun", "--sdk", "macosx", "clang++", "-std=c++17", "-O1", "-g",
             "-fobjc-arc", "-fblocks", "-DSKYRIM_METAL_RUNTIME_NO_MAIN",
             "-mmacosx-version-min=26.0", "-Wall", "-Wextra", "-Werror",
             "-I", str(ROOT / "experiments/metal-compile"),
             str(ROOT / "experiments/metal-compile/runtime.mm"),
             str(ROOT / "tests/native/test_metal_phone_runtime.mm"),
             "-framework", "Foundation", "-framework", "Metal", "-o", str(executable)],
            [str(executable)],
        ]
        for label, command in zip(("compile", "run"), commands):
            with (output / f"{label}.stdout.log").open("wb") as stdout, \
                 (output / f"{label}.stderr.log").open("wb") as stderr:
                result = subprocess.run(command, stdout=stdout, stderr=stderr,
                                        stdin=subprocess.DEVNULL, timeout=60, check=False)
            self.assertEqual(result.returncode, 0,
                             (output / f"{label}.stderr.log").read_text(errors="replace"))
        self.assertEqual((output / "run.stdout.log").read_text().strip(),
                         "METAL_PHONE_HOST_CONTROLS cases=4 gpu_execution_verified=0 physical_iphone_verified=0")
        self.assertEqual((output / "run.stderr.log").stat().st_size, 0)


if __name__ == "__main__":
    unittest.main()
