"""Bounded lock samples and production-emitter privacy/schema round trip."""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import unittest
import tempfile

ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ROOT / 'ios/runtime-port'


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    sys.modules[name] = result
    spec.loader.exec_module(result)
    return result


def sanitizer_flags():
    mode = os.getenv('SKYRIM_TEST_SANITIZERS')
    if mode == 'thread':
        flags = ['-fsanitize=thread', '-fno-omit-frame-pointer']
        # GCC TSan's shadow address space can collide with a PIE under WSL.
        if sys.platform.startswith('linux'):
            flags += ['-fno-pie', '-no-pie']
        return flags
    return ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if mode == '1' else []


class LockMetricsTests(unittest.TestCase):
    def test_actual_recording_bounds_concurrency_and_export(self):
        cc = shutil.which(os.environ.get('CC', 'cc'))
        if not cc:
            self.skipTest('Native C compiler required; run on WSL/Mac')
        with tempfile.TemporaryDirectory(prefix='lock-metrics-') as temporary:
            binary = Path(temporary) / 'lock-metrics'
            flags = ['-std=c11', '-D_POSIX_C_SOURCE=200809L', '-O2',
                     '-Wall', '-Wextra', '-Werror', '-pthread']
            flags += sanitizer_flags()
            subprocess.run([cc, *flags, '-I', str(RUNTIME),
                            str(ROOT/'tests/native/test_lock_metrics.c'), '-o', str(binary)],
                           check=True, timeout=60)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=25)
        self.assertIn('concurrent_accounting=PASS', result.stdout)
        self.assertIn('bounds_lifecycle_errno=PASS', result.stdout)
        lines = result.stderr.splitlines()
        self.assertEqual(len(lines), 2)
        analysis = module('lock_metrics_analysis', ROOT/'scripts/analyze-frame-timing.py')
        receiver = module('lock_metrics_receiver', ROOT/'scripts/receive_private_diagnostics.py')
        schema = receiver.Schema()
        for line in lines:
            self.assertTrue(line.startswith('IOS_LOCK '))
            self.assertLessEqual(len(line), 8191)
            schema.line(line)
            analysis.locks(line.split(' ', 1)[1])
        self.assertIn('count=0 dropped_busy=0 dropped_full=0', lines[0])
        self.assertIn('count=16 dropped_busy=0 dropped_full=0', lines[1])
        self.assertIn(str((1 << 64) - 1), lines[1])

    def test_c_core_is_callable_from_cpp(self):
        cc = shutil.which(os.environ.get('CC', 'cc'))
        cxx = shutil.which(os.environ.get('CXX', 'c++'))
        if not cc or not cxx:
            self.skipTest('Native C and C++ compilers required; run on WSL/Mac')
        with tempfile.TemporaryDirectory(prefix='lock-metrics-cpp-') as temporary:
            root = Path(temporary)
            objects = []
            for name in ('performance_metrics', 'lock_metrics'):
                target = root/(name+'.o'); objects.append(str(target))
                subprocess.run([cc, '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-O2', *sanitizer_flags(),
                    '-Wall', '-Wextra', '-Werror', '-I', str(RUNTIME),
                    '-c', str(RUNTIME/(name+'.c')), '-o', str(target)], check=True, timeout=45)
            source = root/'caller.cpp'
            source.write_text('''#include "lock_metrics.h"
#include "performance_metrics.h"
#include <cassert>
int main() {
    SkyrimLockEvent event{};
    event.start_ns=100; event.end_ns=100+SKYRIM_LOCK_THRESHOLD_NS;
    event.lock=SKYRIM_LOCK_D3D9; event.phase=SKYRIM_LOCK_WAIT;
    event.operation=SKYRIM_LOCK_OP_D3D9_PRESENT;
    event.thread=skyrim_perf_thread_ordinal();
    skyrim_lock_record(&event);
    SkyrimLockWindow window{};
    assert(skyrim_lock_take(&window) && window.count==1);
    assert(window.events[0].thread==event.thread);
}
''')
            binary = root/'caller'
            subprocess.run([cxx, '-std=c++17', '-O2', *sanitizer_flags(), '-Wall', '-Wextra', '-Werror',
                '-pthread', '-I', str(RUNTIME), str(source), *objects, '-o', str(binary)],
                check=True, timeout=45)
            subprocess.run([str(binary)], check=True, timeout=15)


if __name__ == '__main__':
    unittest.main()
