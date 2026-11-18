"""Exercise the production cache owner/file format with authored Vulkan functions."""
import os
from pathlib import Path
import platform
import re
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
            elif os.environ.get("SKYRIM_TEST_SANITIZERS") == "thread":
                common += ["-fsanitize=thread", "-fno-omit-frame-pointer"]
            metrics = folder / "metrics.o"
            subprocess.run([cc, "-std=c11", *common, "-c", str(runtime / "performance_metrics.c"),
                "-I", str(runtime), "-o", str(metrics)], check=True, timeout=60)
            binary = folder / "cache"
            cache_object = folder / "cache.o"
            # cstdio deliberately undefines a command-line rename macro. Load
            # it before the test interposition instead of modifying libc names.
            io_shim = folder / "cache_io_shim.h"
            io_shim.write_text('#include <cstdio>\nextern "C" int fixture_cache_rename(const char *, const char *);\n#define rename fixture_cache_rename\n')
            subprocess.run([cxx, "-std=c++17", *common, "-c", str(dependency / "pipeline_cache.cpp"),
                "-Dskyrim_perf_now_ns=fixture_cache_now_ns", "-Dlink=fixture_cache_link", "-Dunlink=fixture_cache_unlink",
                "-Dwrite=fixture_cache_write", "-Dfsync=fixture_cache_fsync", "-include", str(io_shim),
                "-I", str(dependency), "-I", str(runtime),
                "-I", str(ROOT / "vendor/skydream/third_party/dxvk/include"), "-o", str(cache_object)], check=True, timeout=60)
            subprocess.run([cxx, "-std=c++17", *common,
                str(ROOT / "tests/native/test_pipeline_cache.cpp"), str(cache_object), str(metrics),
                "-I", str(dependency), "-I", str(runtime),
                "-I", str(ROOT / "vendor/skydream/third_party/dxvk/include"),
                "-o", str(binary)], check=True, timeout=90)
            command = [str(binary), str(folder / "cache-files")]
            if os.environ.get("SKYRIM_TEST_SANITIZERS") == "thread" and os.environ.get("SKYRIM_TSAN_DISABLE_ASLR") == "1":
                setarch = shutil.which("setarch")
                self.assertIsNotNone(setarch, "Explicit process-local TSan layout option requires setarch")
                command = [setarch, platform.machine(), "-R", *command]
            result = subprocess.run(command, check=False,
                capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("dirty_generation_lifetime=PASS", result.stdout)
            self.assertIn("bounded_snapshot_retry=PASS", result.stdout)
            self.assertIn("validated_absent_target_import=PASS", result.stdout)
            self.assertIn("foreground_admission_cancel_singleflight=PASS", result.stdout)
            self.assertIn("monotonic_completion_and_clock_failure=PASS", result.stdout)
            self.assertIn("interrupted_sync_bounded_retry_and_io_failure_recovery=PASS", result.stdout)
            # Each invocation is delimited by the test, including failures and exceptions.
            for section in result.stderr.split("CACHE_CASE_BEGIN\n")[1:]:
                body = section.split("CACHE_CASE_END\n", 1)[0]
                rows = [line for line in body.splitlines() if "event=checkpoint " in line]
                self.assertEqual(len(rows), 1, body)
                # Even a throwing/cancelled driver must be preceded by a stage
                # record. The former end-only report could not identify a stuck
                # or killed call. Exercise real owner calls, not a mock logger.
                boundary, current_stage, stage_count = [], None, 0
                for line in body.splitlines():
                    if "event=checkpoint_boundary " in line:
                        self.assertRegex(line, r" monotonic_ns=[0-9]+$")
                        boundary.append(int(re.search(r"boundary=(\d+)", line)[1]))
                    elif "event=checkpoint_stage " in line:
                        self.assertRegex(line, r" monotonic_ns=[0-9]+$")
                        current_stage = int(re.search(r"stage=(\d+)", line)[1])
                        stage_count += 1
                    elif line in ("CACHE_DRIVER_SIZE_ENTER", "CACHE_DRIVER_DATA_ENTER"):
                        self.assertEqual(boundary, [0, 1, 2], body)
                        self.assertEqual(current_stage, 1 if "SIZE" in line else 2, body)
                self.assertEqual(boundary, [0, 1, 2], body)
                self.assertLessEqual(stage_count, 12, body)  # 3 attempts + one file publication
            self.assertIn("outcome=0 stage=9 attempts=2", result.stderr)
            self.assertIn("outcome=6 stage=2 attempts=3", result.stderr)
            self.assertIn("budget_expired=1", result.stderr)
            self.assertIn("outcome=8", result.stderr)
            self.assertIn("outcome=7 stage=10", result.stderr)
            self.assertIn("import_publish=1 cleanup_errno=13", result.stderr)
            self.assertIn("offered_bytes=96 empty_retry=0 result=0 hit_count_unknown=1", result.stderr)
            self.assertIn("empty_retry=1 result=0", result.stderr)
            self.assertIn("empty_retry=1 result=-1", result.stderr)
            self.assertEqual(sorted(p.name for p in (folder / "cache-files").iterdir()), ["pipeline-v1.cache"])


if __name__ == "__main__":
    unittest.main()
