"""Fixed numeric slow-operation samples, using only authored intervals."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PerformanceStallTests(unittest.TestCase):
    def test_actual_recorder_boundaries_lifecycle_and_concurrent_drain(self):
        cc = shutil.which(os.environ.get('CC', 'cc'))
        if not cc: self.skipTest('Native C compiler needed; run on WSL/Mac')
        with tempfile.TemporaryDirectory(prefix='performance-stalls-') as tmp:
            binary = Path(tmp) / 'stalls'
            flags = ['-std=c11', '-D_POSIX_C_SOURCE=200809L', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread']
            if os.getenv('SKYRIM_TEST_SANITIZERS') == '1':
                flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
            subprocess.run([cc, *flags, '-I', str(ROOT/'ios/runtime-port'),
                            str(ROOT/'tests/native/test_performance_stalls.c'), '-o', str(binary)], check=True, timeout=60)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=20)
            self.assertIn('concurrent_accounting=PASS', result.stdout)
            lines = result.stderr.splitlines()
            self.assertEqual(len(lines), 6)
            self.assertIn('shader_waits_foreground_origin_and_concurrent_accounting=PASS', result.stdout)
            self.assertIn('IOS_SHADER_LEAD schema=1 seq=1 result=1 ready_ns=5000000000 queued_ns=5100000000 '
                          'worker_ns=5200000000 compile_ns=5300000000 published_ns=5350000000 '
                          'wait_start_ns=5300000000 wait_end_ns=5350000000', lines)
            self.assertIn('IOS_SHADER_LEAD schema=1 seq=2 result=2 ready_ns=0 queued_ns=0 worker_ns=0 '
                          'compile_ns=0 published_ns=0 wait_start_ns=5300000000 wait_end_ns=5350000000', lines)
            self.assertIn('IOS_SHADER_LEAD_DROPS schema=1 seq=3 busy=1 full=0', lines)
            line = next(line for line in lines if line.startswith('IOS_STALL '))
            self.assertLess(len(line), 8192)
            self.assertIn('count=2 dropped_busy=0 dropped_full=0', line)
            self.assertIn('window_start_ns=0 window_end_ns=4000000000', line)
            spans = [[int(word) for word in span.split(',')] for span in line.split(' spans=')[1].split(';')]
            self.assertIn('schema=3 ', line)
            self.assertEqual(spans, [[0, 0, 5000000000, 5100000000, 0, 0, 0, 0, 0],
                                     [3, 0, 5010000000, 5090000000, 1, 0, 0, 0, 0]])

    def test_runtime_uses_original_endpoints_and_existing_activity_notifications(self):
        runtime = (ROOT/'ios/runtime-port/ios_runtime.c').read_text()
        self.assertIn('skyrim_perf_activity(active);', runtime)
        self.assertIn('SKYRIM_PERF_FRAME_GAP, now - gap, now, 0, 0', runtime)
        self.assertIn('SKYRIM_PERF_PRESENT_HOST, entered, now, 0, 0', runtime)
        self.assertIn('SKYRIM_PERF_PACER, started, ended, 0, sleeps', runtime)
        self.assertIn('SKYRIM_PERF_PACER_OVERRUN, deadline, ended, 0, 0', runtime)
        ui = (ROOT/'ios/runtime-port/mobile_ui.m').read_text()
        self.assertIn('skyrim_ios_metrics_active(0)', ui)
        self.assertIn('skyrim_ios_metrics_active(1)', ui)


if __name__ == '__main__': unittest.main()
