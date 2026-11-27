"""Pinned D3D9 lock semantics, clock-free hot path and complete source checks."""
import importlib.util
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'vendor/skydream/third_party/dxvk'
spec = importlib.util.spec_from_file_location('d3d9_observed_stage', ROOT / 'ios/dependencies/prepare_dxvk.py')
adapter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(adapter)


def flags():
    mode = os.environ.get('SKYRIM_TEST_SANITIZERS')
    if mode == 'thread':
        return ['-fsanitize=thread', '-fno-omit-frame-pointer']
    if mode == '1':
        return ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    return []


def includes(stage):
    result = ['-I', str(ROOT / 'ios/runtime-port'), '-I', str(ROOT / 'ios/dependencies')]
    for folder in ('src', 'src/d3d9', 'src/util', 'include', 'include/native',
                   'include/native/windows', 'include/native/directx'):
        result += ['-I', str(stage / folder)]
    return result


class D3D9LockAdapterTests(unittest.TestCase):
    def test_present_observers_leave_swapchain_behavior_unchanged(self):
        path = 'src/d3d9/d3d9_swapchain.cpp'
        original = (SOURCE / path).read_text(encoding='utf-8')
        observed = adapter.adapt(path, original)
        self.assertEqual(observed.count('LockDevice(SKYRIM_LOCK_OP_D3D9_PRESENT)'), 1)
        self.assertEqual(observed.count('skyrim_water::present(m_parent, this);'), 1)
        after_present = ('      PresentImage(presentInterval);\n'
            '      if (unlikely(skyrim_water::phase.load(std::memory_order_relaxed) != skyrim_water::Phase::Idle))\n'
            '        skyrim_water::present(m_parent, this);')
        after_submit = ('      SubmitPresent(sync, i);\n'
            '#if defined(SKYRIM_IOS_RENDER_SUBMISSION)\n'
            '      if (!i) {\n'
            '        const VkExtent3D size = swapImage->info().extent;\n'
            '        skyrim_ios_render_submitted(size.width, size.height);\n'
            '      }\n'
            '#endif')
        expected = '#include "ios_runtime.h"\n#include "water_probe.h"\n' + original.replace(
            '      PresentImage(presentInterval);', after_present).replace('      SubmitPresent(sync, i);', after_submit)
        self.assertEqual(observed.replace('LockDevice(SKYRIM_LOCK_OP_D3D9_PRESENT)', 'LockDevice()'), expected)
        # The resource/generic synchronization implementation is untouched.
        self.assertNotIn('src/util/sync/sync_recursive.cpp', adapter.EXPECTED)
        self.assertNotIn('src/util/sync/sync_recursive.h', adapter.EXPECTED)

    def test_changed_device_lock_anchor_is_refused(self):
        path = 'src/d3d9/d3d9_device.h'
        original = (SOURCE / path).read_text(encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'D3D9 lock observer anchor changed'):
            adapter.adapt(path, original.replace('return m_multithread.AcquireLock();', 'return {};'))


@unittest.skipUnless(os.name == 'posix' and shutil.which(os.environ.get('CXX', 'c++')),
                     'Native POSIX compiler required')
class D3D9LockNativeTests(unittest.TestCase):
    def test_actual_lock_recursion_contention_and_hot_clock_budget(self):
        with tempfile.TemporaryDirectory(prefix='d3d9-lock-') as tmp:
            folder = Path(tmp)
            stage = folder / 'dxvk'
            adapter.stage(SOURCE, stage)
            # Use the unchanged real constructor, replacing only its overly broad
            # device header include with the class's own complete declaration.
            constructor = (stage / 'src/d3d9/d3d9_multithread.cpp').read_text(encoding='utf-8')
            constructor = constructor.replace('#include "d3d9_device.h"', '#include "d3d9_multithread.h"')
            ctor = folder / 'constructor.cpp'
            ctor.write_text(constructor, encoding='utf-8')
            executable = folder / 'fixture'
            command = [shutil.which(os.environ.get('CXX', 'c++')), '-std=c++17', '-O2', '-g', '-pthread', *flags(),
                # Pinned log_debug.h has an unused no-op traceArgs parameter.
                '-Wall', '-Wextra', '-Werror', '-Wno-deprecated-declarations', '-Wno-unused-parameter',
                '-DDXVK_NATIVE', '-DNOMINMAX', '-D_WIN32_WINNT=0xa00',
                *includes(stage), str(ROOT / 'tests/native/d3d9_lock_fixture.cpp'),
                str(ctor), str(stage / 'src/util/platform/thread_native.cpp'), '-o', str(executable)]
            result = subprocess.run(command, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            command = [str(executable)]
            if os.environ.get('SKYRIM_TEST_SANITIZERS') == 'thread' and os.environ.get('SKYRIM_TSAN_DISABLE_ASLR') == '1':
                command = [shutil.which('setarch'), platform.machine(), '-R', *command]
            result = subprocess.run(command, capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            for marker in ('hot_recursion_unprotected_clocks_zero', 'contended_present_owner_recursion_moves',
                           'exception_outer_hold_distinct_device', 'concurrent_recursive_ordering'):
                self.assertIn(marker + '=PASS', result.stdout)

    def test_complete_device_swapchain_multithread_translation_units(self):
        with tempfile.TemporaryDirectory(prefix='d3d9-lock-syntax-') as tmp:
            stage = Path(tmp) / 'dxvk'
            adapter.stage(SOURCE, stage)
            command = [shutil.which(os.environ.get('CXX', 'c++')), '-std=c++17', '-fsyntax-only', '-include', 'cstdint', '-include', 'cstring',
                '-Wno-deprecated-declarations', '-DDXVK_NATIVE', '-DNOMINMAX', '-D_WIN32_WINNT=0xa00', '-DSKYRIM_IOS_RENDER_SUBMISSION=1',
                *includes(stage)]
            for name in ('d3d9_device.cpp', 'd3d9_swapchain.cpp', 'd3d9_multithread.cpp'):
                with self.subTest(name=name):
                    result = subprocess.run([*command, str(stage / 'src/d3d9' / name)],
                        capture_output=True, text=True, timeout=90)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
