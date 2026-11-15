"""Authored histogram and real pinned-DXVK forwarding checks, no game inputs."""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("dxvk_stage", ROOT / "ios/dependencies/prepare_dxvk.py")
adapter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(adapter)
SOURCE = ROOT / "vendor/skydream/third_party/dxvk"


class PerformanceTests(unittest.TestCase):
    def test_native_boundaries_and_concurrent_drain(self):
        compiler = shutil.which(os.environ.get("CC", "cc"))
        if not compiler:
            self.skipTest("Native C compiler unavailable; run on WSL/Mac")
        with tempfile.TemporaryDirectory(prefix="perf-metrics-") as tmp:
            binary = Path(tmp) / "metrics"
            command = [compiler, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-O2",
                "-Wall", "-Wextra", "-Werror", "-pthread", "-I", str(ROOT / "ios/runtime-port"),
                str(ROOT / "ios/runtime-port/performance_metrics.c"),
                str(ROOT / "tests/native/test_performance_metrics.c"), "-o", str(binary)]
            if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
                command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            subprocess.run(command, check=True, timeout=60)
            result = subprocess.run([str(binary)], check=True, text=True, capture_output=True, timeout=15)
            self.assertIn("concurrent_drain=PASS", result.stdout)
            self.assertEqual(result.stderr.count("IOS_TIMING "), 2)
            self.assertIn("kind=pacer samples=1", result.stderr)
            self.assertIn("sleep_calls=8", result.stderr)
            wait_line = next(line for line in result.stderr.splitlines() if "kind=graphics_compile_wait" in line)
            self.assertIn("samples=1", wait_line)
            self.assertIn("total_ns=120000000", wait_line)
            self.assertIn("failures_known=0", wait_line)

    def test_real_adapter_preserves_every_api_argument_and_failure_branch(self):
        for name in adapter.EXPECTED:
            if not name.endswith(("dxvk_graphics.cpp", "dxvk_compute.cpp", "dxvk_shader.cpp")):
                continue
            old = (SOURCE / name).read_text(encoding="utf-8")
            new = adapter.adapt_timing(name, old)
            # Reverse the timing wrapper, not the rest of the source. Full byte
            # equality after this reversal proves no other pipeline behavior changed.
            import re
            new = new.removeprefix('#include "performance_metrics.h"\n')
            new = new.replace("    if (iosResult == VK_SUCCESS) m_pipeMgr->m_cache->compiled();\n", "")
            pattern = (r"    const uint64_t iosStarted = skyrim_perf_now_ns\(\);\n"
                       r"    const VkResult iosResult = (m_vkd->vkCreate[\s\S]+?);\n"
                       r"    skyrim_perf_end\(SKYRIM_PERF_\w+, iosStarted, iosResult != VK_SUCCESS\);\n"
                       r"    if \(iosResult != VK_SUCCESS\)")
            restored, count = re.subn(pattern, lambda m: "    if (" + m[1] + " != VK_SUCCESS)", new)
            self.assertEqual(count, 1)
            self.assertEqual(restored, old)

    def test_staging_reuses_exact_tree_and_refuses_changed_input(self):
        with tempfile.TemporaryDirectory(prefix="dxvk-stage-") as tmp:
            stage = Path(tmp) / "stage"
            adapter.stage(SOURCE, stage)
            adapter.stage(SOURCE, stage)
            self.assertEqual((stage / "CMakeLists.txt").read_bytes(), (SOURCE / "CMakeLists.txt").read_bytes())
            with self.assertRaisesRegex(ValueError, "Changed DXVK input"):
                adapter.prepare(stage)
            target = stage / "src/dxvk/dxvk_graphics.cpp"
            target.write_bytes(target.read_bytes() + b"\n// changed\n")
            with self.assertRaisesRegex(ValueError, "Existing DXVK stage differs"):
                adapter.stage(SOURCE, stage)


if __name__ == "__main__":
    unittest.main()
