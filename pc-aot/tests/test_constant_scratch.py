"""Differential execution of the actual public D3DX constant conversion path."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('constant_stage', ROOT/'ios/runtime-port/prepare_runtime.py')
adapter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(adapter)


def source(patched):
    original = (ROOT/'vendor/skydream/runtime/d3dx_shader.cpp').read_text(encoding='utf-8')
    assert hashlib.sha256(original.encode()).hexdigest() == adapter.EXPECTED['d3dx_shader.cpp']
    text = adapter.patch_shader(original) if patched else original
    # Entire original conversion, handle resolution and device forwarding bodies.
    sections = [text[text.index('struct Constant {'):text.index('static uint32_t buffer_vt')],
                text[text.index('static Constant *constant('):text.index('static uint32_t find_constant(')],
                text[text.index('static HRESULT set_registers('):text.index('SHIM(SetDefaults)')],
                text[text.index('static void set_value('):text.index('static uint32_t make_table(')]]
    fixture = (ROOT/'tests/native/test_constant_scratch.cpp').read_text(encoding='utf-8')
    return fixture.replace('// ACTUAL_CONSTANT_PATH', '\n'.join(sections))


def compile_fixture(folder, patched):
    path = folder/'fixture.cpp'
    path.write_text(source(patched), encoding='utf-8')
    flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if os.getenv('SKYRIM_TEST_SANITIZERS') == '1' else []
    command = ['c++', '-std=c++17', '-O2', '-g', '-pthread', '-Wall', '-Wextra', '-Werror',
               '-Wno-mismatched-new-delete', *flags, str(path), '-o', str(folder/'fixture')]
    result = subprocess.run(command, capture_output=True, text=True, timeout=60)
    if result.returncode:
        raise AssertionError(result.stderr[-8000:])
    return folder/'fixture'


@unittest.skipUnless(os.name == 'posix' and shutil.which('c++'), 'Existing POSIX C++ compiler required')
class ConstantScratchTests(unittest.TestCase):
    def test_actual_conversions_padding_failure_reentrancy_and_small_allocations(self):
        outputs = []
        for patched in (False, True):
            with tempfile.TemporaryDirectory(prefix='constant-scratch-') as tmp:
                binary = compile_fixture(Path(tmp), patched)
                result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=25)
                self.assertEqual(result.returncode, 0, result.stderr[-8000:])
                outputs.append(json.loads(result.stdout))
        before, after = outputs
        self.assertGreater(before['cases'], 10000)
        for name in ('cases', 'digest', 'device_calls', 'reentrant', 'concurrent'):
            self.assertEqual(before[name], after[name], name)
        self.assertEqual(before['small_allocations'], 2)
        self.assertEqual(after['small_allocations'], 0)
        self.assertEqual(before['large_allocations'], 2)
        self.assertEqual(after['large_allocations'], 1)


if __name__ == '__main__':
    unittest.main()
