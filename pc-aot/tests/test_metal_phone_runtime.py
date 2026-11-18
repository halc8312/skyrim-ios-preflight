"""Real Mac callable-host failure controls; not iPhone/Metal 4 GPU evidence."""
from pathlib import Path
import os
import platform
import subprocess
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]


def host_build_commands(output, executable):
    source = ROOT / "experiments/metal-compile"
    common = ["xcrun", "--sdk", "macosx", "clang++", "-std=c++17", "-O1", "-g",
              "-fblocks", "-fexceptions", "-fobjc-exceptions", "-mmacosx-version-min=26.0",
              "-Wall", "-Wextra", "-Werror", "-I", str(source)]
    manual_object = output / "phone-contention.o"
    return [
        common + ["-fno-objc-arc", "-c", str(source / "phone_contention.mm"), "-o", str(manual_object)],
        common + ["-fobjc-arc", "-DSKYRIM_METAL_RUNTIME_NO_MAIN",
                  str(ROOT / "tests/native/test_metal_phone_runtime.mm"), str(manual_object),
                  "-framework", "Foundation", "-framework", "Metal", "-o", str(executable)],
    ]


class PhoneRuntimeBuildControls(unittest.TestCase):
    def test_mrc_contention_compiles_separately_from_arc_callable_host(self):
        manual, host = host_build_commands(Path("synthetic-output"), Path("synthetic-output/host"))
        self.assertIn("-fno-objc-arc", manual)
        self.assertNotIn("-fobjc-arc", manual)
        self.assertIn("-fobjc-arc", host)
        self.assertNotIn("-fno-objc-arc", host)
        self.assertIn(manual[-1], host)
        self.assertIn("-DSKYRIM_METAL_RUNTIME_NO_MAIN", host)
        self.assertNotIn(str(ROOT / "experiments/metal-compile/runtime.mm"), host)
        for command in (manual, host):
            self.assertIn("-fblocks", command)
            self.assertIn("-fexceptions", command)
            self.assertIn("-fobjc-exceptions", command)


@unittest.skipUnless(platform.system() == "Darwin", "requires Apple SDK and Foundation")
class PhoneRuntimeBoundaryTests(unittest.TestCase):
    def test_callable_host_refuses_main_thread_invalid_repeat_stop_and_non_phone(self):
        output = ROOT / "local/tests" / f"phone-runtime-{time.time_ns()}-{os.getpid()}"
        output.mkdir(parents=True, exist_ok=False)
        executable = output / "phone-runtime-controls"
        commands = host_build_commands(output, executable) + [[str(executable)]]
        for label, command in zip(("compile-contention", "compile-host", "run"), commands):
            with (output / f"{label}.stdout.log").open("wb") as stdout, \
                 (output / f"{label}.stderr.log").open("wb") as stderr:
                result = subprocess.run(command, stdout=stdout, stderr=stderr,
                                        stdin=subprocess.DEVNULL, timeout=60, check=False)
            self.assertEqual(result.returncode, 0,
                             (output / f"{label}.stderr.log").read_text(errors="replace"))
        self.assertEqual((output / "run.stdout.log").read_text().strip(),
                         "METAL_PHONE_HOST_CONTROLS cases=11 gpu_execution_verified=0 physical_iphone_verified=0")
        self.assertEqual((output / "run.stderr.log").stat().st_size, 0)


if __name__ == "__main__":
    unittest.main()
