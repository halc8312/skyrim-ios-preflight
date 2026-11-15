"""Exercise the production cache owner/file format with authored Vulkan functions."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PipelineCacheTests(unittest.TestCase):
    def test_actual_cache_owner_and_atomic_file_roundtrip(self):
        cc = shutil.which(os.environ.get("CC", "cc"))
        cxx = shutil.which(os.environ.get("CXX", "c++"))
        if not cc or not cxx:
            self.skipTest("C/C++ compiler unavailable; run on WSL/Mac")
        with tempfile.TemporaryDirectory(prefix="pipeline-cache-") as tmp:
            folder = Path(tmp)
            runtime = ROOT / "ios/runtime-port"
            dependency = ROOT / "ios/dependencies"
            common = ["-O2", "-Wall", "-Wextra", "-Werror", "-pthread"]
            if os.environ.get("SKYRIM_TEST_SANITIZERS") == "1":
                common += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            metrics = folder / "metrics.o"
            subprocess.run([cc, "-std=c11", *common, "-c", str(runtime / "performance_metrics.c"),
                "-I", str(runtime), "-o", str(metrics)], check=True, timeout=60)
            binary = folder / "cache"
            cache_object = folder / "cache.o"
            subprocess.run([cxx, "-std=c++17", *common, "-c", str(dependency / "pipeline_cache.cpp"),
                "-Dskyrim_perf_now_ns=fixture_cache_now_ns", "-I", str(dependency), "-I", str(runtime),
                "-I", str(ROOT / "vendor/skydream/third_party/dxvk/include"), "-o", str(cache_object)], check=True, timeout=60)
            subprocess.run([cxx, "-std=c++17", *common,
                str(ROOT / "tests/native/test_pipeline_cache.cpp"), str(cache_object), str(metrics),
                "-I", str(dependency), "-I", str(runtime),
                "-I", str(ROOT / "vendor/skydream/third_party/dxvk/include"),
                "-o", str(binary)], check=True, timeout=90)
            result = subprocess.run([str(binary), str(folder / "cache-files")], check=True,
                capture_output=True, text=True, timeout=20)
            self.assertIn("dirty_generation_lifetime=PASS", result.stdout)
            self.assertIn("bounded_snapshot_retry=PASS", result.stdout)
            # Each invocation is delimited by the test, including failures and exceptions.
            for section in result.stderr.split("CACHE_CASE_BEGIN\n")[1:]:
                body = section.split("CACHE_CASE_END\n", 1)[0]
                rows = [line for line in body.splitlines() if "event=checkpoint " in line]
                self.assertEqual(len(rows), 1, body)
            self.assertIn("outcome=0 stage=9 attempts=2", result.stderr)
            self.assertIn("outcome=6 stage=2 attempts=3", result.stderr)
            self.assertIn("budget_expired=1", result.stderr)
            self.assertIn("outcome=8", result.stderr)
            self.assertIn("offered_bytes=96 empty_retry=0 result=0 hit_count_unknown=1", result.stderr)
            self.assertIn("empty_retry=1 result=0", result.stderr)
            self.assertIn("empty_retry=1 result=-1", result.stderr)
            self.assertEqual(sorted(p.name for p in (folder / "cache-files").iterdir()), ["pipeline-v1.cache"])


if __name__ == "__main__":
    unittest.main()
