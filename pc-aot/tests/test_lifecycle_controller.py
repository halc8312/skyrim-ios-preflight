"""Higher-level admission; keep the rejected native GPU gate excluded."""
import importlib.util
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def flags():
    result = ['-O2', '-Wall', '-Wextra', '-Werror', '-pthread']
    if os.environ.get('SKYRIM_TEST_THREAD_SANITIZER') == '1':
        result += ['-fsanitize=thread', '-fno-omit-frame-pointer']
    elif os.environ.get('SKYRIM_TEST_SANITIZERS') == '1':
        result += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    return result


def function(text, signature):
    start = text.index(signature)
    begin = text.index('{', start)
    depth = 1
    end = begin + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


class LifecycleTests(unittest.TestCase):
    def test_actual_mixer_pause_preserves_queue_and_guest_engine_state(self):
        cc = shutil.which(os.environ.get('CC', 'cc'))
        if not cc:
            self.skipTest('POSIX C compiler required')
        prep = load('lifecycle_audio_prepare', ROOT/'ios/runtime-port/prepare_runtime.py')
        adapter = load('lifecycle_audio_adapter', ROOT/'ios/runtime-port/lifecycle_adapters.py')
        source = (ROOT/'vendor/skydream/runtime/xaudio2.c').read_text()
        source = adapter.adapt('xaudio2.c', prep.patch_audio(source))
        body = function(source, 'static void *engine_main(void *arg)')
        helper = (ROOT/'ios/runtime-port/audio_backpressure.c.in').read_text()
        fixture = (ROOT/'tests/native/lifecycle_audio_fixture.c').read_text()
        with tempfile.TemporaryDirectory(prefix='lifecycle-audio-') as tmp:
            unit, binary = Path(tmp)/'audio.c', Path(tmp)/'audio'
            unit.write_text(fixture.replace('/* ACTUAL_HELPER */', helper).replace('/* ACTUAL_ENGINE */', body))
            subprocess.run([cc, '-std=c11', '-D_DEFAULT_SOURCE', *flags(), str(unit),
                '-o', str(binary)], check=True, timeout=60)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('queued_audio_preserved=PASS guest_running_preserved=PASS', result.stdout)

    def test_actual_controller_preserves_producers_and_resume_races(self):
        cc = shutil.which(os.environ.get('CC', 'cc'))
        if not cc:
            self.skipTest('POSIX C compiler required')
        with tempfile.TemporaryDirectory(prefix='lifecycle-controller-') as tmp:
            binary = Path(tmp) / 'test'
            subprocess.run([cc, '-std=c11', '-D_POSIX_C_SOURCE=200809L', *flags(),
                '-I', str(ROOT/'ios/runtime-port'), str(ROOT/'ios/runtime-port/lifecycle_controller.c'),
                str(ROOT/'tests/native/test_lifecycle_controller.c'), '-o', str(binary)], check=True, timeout=60)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('producer_dependency=PASS', result.stdout)
            self.assertIn('admission_drain_exclusive=PASS', result.stdout)

    def test_generated_frontend_coverage_and_ui_pump_outside_admission(self):
        prep = load('lifecycle_runtime_prepare', ROOT/'ios/runtime-port/prepare_runtime.py')
        adapter = load('lifecycle_adapter', ROOT/'ios/runtime-port/lifecycle_adapters.py')
        staged = prep.prepare(ROOT/'vendor/skydream')
        # Pin all entry sets, including COM release, BC emulation and D3DX
        # texture/surface uploads; unsupported new paths fail closed at staging.
        for name, count in adapter.COUNTS.items():
            entries = list(adapter.ENTRY.finditer(staged[name]))
            self.assertEqual(len(entries), count)
            for match in entries:
                if match.group(1) in adapter.PRESENT:
                    continue
                self.assertTrue(staged[name][match.end():].startswith('\n    SkyrimLifecycleCall iosLifecycle;'))
        bridge = staged['d3d9_bridge.cpp']
        for function, obj in [('B_IDirect3DDevice9_Present', 'd'), ('B_IDirect3DSwapChain9_Present', 's')]:
            body = bridge[bridge.index('void '+function+'('):]
            body = body[:body.index('\n}\n')]
            self.assertLess(body.index('skyrim_ios_pace_frame();'), body.index('SkyrimLifecycleCall'))
            self.assertLess(body.index('SkyrimLifecycleCall'), body.index(f'hr = {obj}->Present('))
            after = body[body.index(f'hr = {obj}->Present('):]
            self.assertLess(after.index('\n    }'), after.index('platform_present_hook();'))
            if obj == 'd':
                self.assertLess(body.index('SkyrimLifecycleCall'), body.index('capture_frame(d);'))
        self.assertEqual(staged['d3dx_native.cpp'].count('SkyrimLifecycleCall'), 6)
        self.assertNotIn('skyrim_gpu_', ''.join(staged.values()))

    def test_actual_dxvk_callback_flushes_before_wait_and_preserves_error(self):
        cxx = shutil.which(os.environ.get('CXX', 'c++'))
        if not cxx:
            self.skipTest('POSIX C++ compiler required')
        prep = load('lifecycle_dxvk_prepare', ROOT/'ios/dependencies/prepare_dxvk.py')
        text = prep.prepare(ROOT/'vendor/skydream/third_party/dxvk')['src/d3d9/d3d9_device.cpp'].decode()
        start = text.index('  static int32_t iosLifecycleDrain(')
        end = text.index('\n  D3D9DeviceEx::D3D9DeviceEx(', start)
        callback = text[start:end]
        fixture = (ROOT/'tests/native/lifecycle_dxvk_fixture.cpp').read_text()
        with tempfile.TemporaryDirectory(prefix='lifecycle-dxvk-') as tmp:
            source, binary = Path(tmp)/'test.cpp', Path(tmp)/'test'
            source.write_text(fixture.replace('/* ACTUAL_CALLBACK */', callback))
            subprocess.run([cxx, '-std=c++17', *flags(), str(source), '-o', str(binary)], check=True, timeout=60)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('producer_flush_before_wait=PASS', result.stdout)

    def test_complete_dxvk_device_translation_unit_syntax(self):
        cxx = shutil.which(os.environ.get('CXX', 'clang++')) or shutil.which('clang++-18') or shutil.which('c++')
        if not cxx:
            self.skipTest('Native Clang C++ compiler required')
        prep = load('lifecycle_dxvk_syntax', ROOT/'ios/dependencies/prepare_dxvk.py')
        vendor = ROOT/'vendor/skydream/third_party/dxvk'
        relative = 'src/d3d9/d3d9_device.cpp'
        source = prep.adapt(relative, (vendor/relative).read_text())
        with tempfile.TemporaryDirectory(prefix='lifecycle-dxvk-syntax-') as tmp:
            unit = Path(tmp)/'d3d9_device.cpp'
            unit.write_text(source)
            command = [cxx, '-std=c++17', '-fsyntax-only', '-include', 'cstdint', '-include', 'cstring',
                '-Wno-deprecated-declarations', '-DDXVK_NATIVE', '-DNOMINMAX', '-D_WIN32_WINNT=0xa00',
                '-I', str(ROOT/'ios/runtime-port')]
            for folder in ('src', 'src/d3d9', 'src/util', 'include', 'include/native',
                           'include/native/windows', 'include/native/directx'):
                command += ['-I', str(vendor/folder)]
            result = subprocess.run([*command, str(unit)], capture_output=True, text=True, timeout=90)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    @unittest.skipUnless(sys.platform == 'darwin', 'Real Core Foundation/Metal fixture requires approved Mac validation')
    def test_real_runloop_and_metal_dependency(self):
        with tempfile.TemporaryDirectory(prefix='lifecycle-metal-') as tmp:
            obj, binary = Path(tmp)/'controller.o', Path(tmp)/'test'
            subprocess.run(['clang', '-std=c11', *flags(), '-c',
                str(ROOT/'ios/runtime-port/lifecycle_controller.c'), '-o', str(obj)], check=True, timeout=60)
            subprocess.run(['clang', '-std=gnu11', *flags(), '-fobjc-arc',
                '-I', str(ROOT/'ios/runtime-port'), str(ROOT/'ios/runtime-port/lifecycle_runtime.m'),
                str(ROOT/'tests/native/test_lifecycle_runtime.m'), str(obj),
                '-framework', 'Foundation', '-framework', 'CoreFoundation', '-framework', 'Metal',
                '-o', str(binary)], check=True, timeout=60)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('main_runloop_live=PASS', result.stdout)
            self.assertIn('event_dependency_bytes=PASS', result.stdout)


if __name__ == '__main__':
    unittest.main()
