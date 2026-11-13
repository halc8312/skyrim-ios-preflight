"""Actual public APC and D3D9 methods with authored I/O/compiler fixtures."""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
DXVK = ROOT / 'vendor/skydream/third_party/dxvk'


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


runtime = load('pending_runtime', 'ios/runtime-port/prepare_runtime.py')
dxvk = load('pending_dxvk', 'ios/dependencies/prepare_dxvk.py')


def apc_fixture(patched):
    code = (ROOT / 'vendor/skydream/runtime/kernel32.c').read_text()
    if patched:
        code = runtime.patch_apc_queue(code)
    code = code[code.index('typedef struct { uint32_t fn, err, bytes, ov; } Apc;'):code.index('SHIM(SleepEx) {')]
    return (ROOT / 'tests/native/test_apc_queue.c').read_text().replace('/* ACTUAL_APC */', code)


def shader_fixture(patched):
    header = (DXVK / 'src/d3d9/d3d9_shader.h').read_text()
    code = (DXVK / 'src/d3d9/d3d9_shader.cpp').read_text()
    if patched:
        header = dxvk.adapt_shader_inflight('d3d9_shader.h', header)
        code = dxvk.adapt_shader_inflight('d3d9_shader.cpp', code)
    start = header.index('  class D3D9ShaderModuleSet :')
    header = header[start:header.index('\n  };', start) + 5]
    code = code[code.index('  void D3D9ShaderModuleSet::GetShaderModule('):code.rindex('\n}')]
    # Allocation/publication sites get authored fault injection. All normal
    # runs call the real standard library implementations through the wrappers.
    code = code.replace('std::make_shared<IosShaderFlight>()', 'flight_allocate<IosShaderFlight>()')
    code = code.replace('m_iosCompiling.emplace(lookupKey, flight)', 'flight_insert(m_iosCompiling, lookupKey, flight)')
    code = code.replace('m_modules.insert({ lookupKey, *pShaderModule })',
                        'publish_module(m_modules, lookupKey, *pShaderModule)')
    return (ROOT / 'tests/native/test_shader_inflight.cpp').read_text().replace('// ACTUAL_SHADER_SET', header + '\n' + code)


@unittest.skipUnless(os.name == 'posix' and shutil.which('c++'), 'Existing POSIX compilers required')
class PendingWorkTests(unittest.TestCase):
    def run_fixture(self, work, name, source, patched):
        cpp = name == 'shader'
        path = work / (name + ('.cpp' if cpp else '.c'))
        path.write_text(source)
        flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if os.getenv('SKYRIM_TEST_SANITIZERS') == '1' else []
        extra = []
        if cpp:
            obj = work / 'metrics.o'
            subprocess.run(['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-O2', *flags,
                            '-c', str(ROOT/'ios/runtime-port/performance_metrics.c'), '-o', str(obj)],
                           check=True, capture_output=True, timeout=40)
            extra = [str(obj)]
        cmd = ['c++' if cpp else 'cc', '-std=c++17' if cpp else '-std=c11', '-O2', '-g', '-pthread',
               '-Wall', '-Wextra', '-Werror', f'-DPATCHED={int(patched)}', *flags,
               '-I', str(ROOT/'ios/runtime-port'), str(path), *extra, '-o', str(work / name)]
        built = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
        self.assertEqual(built.returncode, 0, built.stderr)
        ran = subprocess.run([str(work / name)], capture_output=True, text=True, timeout=35)
        self.assertEqual(ran.returncode, 0, ran.stderr)
        if cpp:
            self.assertIn('kind=d3d_shader_wait samples=1 total_ns=123 max_ns=123 failures_known=0', ran.stderr)
        print(name, 'patched' if patched else 'baseline', ran.stdout.strip())
        return ran.stdout

    def test_actual_apc_fifo_wrap_recursion_full_boundary_and_tls(self):
        for patched in (False, True):
            with self.subTest(patched=patched), tempfile.TemporaryDirectory(prefix='apc-work-') as tmp:
                output = self.run_fixture(Path(tmp), 'apc', apc_fixture(patched), patched)
                self.assertIn('semantics=PASS', output)
                self.assertIn(f'shifted_bytes={0 if patched else 32256}', output)

    def test_actual_shader_same_key_different_keys_failure_reentry_and_lifetime(self):
        for patched in (False, True):
            with self.subTest(patched=patched), tempfile.TemporaryDirectory(prefix='shader-work-') as tmp:
                output = self.run_fixture(Path(tmp), 'shader', shader_fixture(patched), patched)
                self.assertIn(f'same_key_compiles={1 if patched else 8}', output)
                self.assertIn('semantics=PASS', output)

    def test_patch_scope_preserves_shader_validation_and_file_io(self):
        name = 'src/d3d9/d3d9_shader.cpp'
        old = (DXVK / name).read_text()
        new = dxvk.adapt(name, old)
        new = new.removeprefix('#include "performance_metrics.h"\n')
        marker = "    // Use the shader's unique key for the lookup"
        self.assertEqual(old[:old.index(marker)], new[:new.index('    // Hot modules')])
        old = (ROOT / 'vendor/skydream/runtime/kernel32.c').read_text()
        new = runtime.patch_apc_queue(old)
        # All syscall arguments, error handling, capacity failure and callback
        # invocation stay byte-identical, aside from the three FIFO operations.
        new = new.replace('\nstatic __thread unsigned t_apc_head;', '')
        new = new.replace('    t_apc[(t_apc_head + (unsigned)t_napc) & 63u] = (Apc){fn, err, (uint32_t)r, ov};\n    ++t_napc;',
                          '    t_apc[t_napc++] = (Apc){fn, err, (uint32_t)r, ov};')
        new = new.replace('        Apc a = t_apc[t_apc_head];\n        t_apc_head = (t_apc_head + 1u) & 63u;\n        --t_napc;',
                          '        Apc a = t_apc[0];\n        memmove(t_apc, t_apc + 1, --t_napc * sizeof(Apc));')
        self.assertEqual(new, old)
        runtime.prepare(ROOT / 'vendor/skydream')


if __name__ == '__main__':
    unittest.main()
