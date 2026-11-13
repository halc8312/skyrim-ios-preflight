"""Public shim behavior and timing, with no original game inputs."""
import importlib.util
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("runtime_asset_stage", ROOT / "ios/runtime-port/prepare_runtime.py")
adapter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(adapter)
RUNTIME = ROOT / "vendor/skydream/runtime"
NATIVE = ROOT / "tests/native"
METRICS = ROOT / "ios/runtime-port"


def erase_observation(text):
    text = text.removeprefix('#include "performance_metrics.h"\n')
    text = re.sub(r'^ *const uint64_t ios_started = (?:write \? 0 : )?skyrim_perf_now_ns\(\);\n', '', text, flags=re.M)
    return re.sub(r'^ *(?:if \(!write\) )?skyrim_perf_end\(SKYRIM_PERF_\w+, ios_started, [^\n]+\);\n', '', text, flags=re.M)


class AssetTimingTests(unittest.TestCase):
    def test_actual_adapters_only_observe_calls_and_keep_every_original_byte(self):
        for filename, patch in [('kernel32.c', adapter.patch_asset_reads),
                                ('d3d9_bridge_gen.cpp', adapter.patch_shader_creation),
                                ('d3dx_native.cpp', adapter.patch_texture_loading)]:
            original = (RUNTIME / filename).read_text()
            patched = patch(original)
            self.assertEqual(erase_observation(patched), original)
            self.assertEqual(patched.count('skyrim_perf_end('), 2)
        adapter.prepare(ROOT / 'vendor/skydream')

    def compile_run(self, work, source, extra=()):
        compiler = shutil.which(os.environ.get('CC', 'cc'))
        if not compiler:
            self.skipTest('Existing POSIX C compiler required; run on WSL/Mac')
        binary = work / source.stem
        flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if os.getenv('SKYRIM_TEST_SANITIZERS') == '1' else []
        compiled = subprocess.run([compiler, '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-O2', '-Wall', '-Wextra',
                        '-Werror', '-pthread', *flags, '-I', str(METRICS), str(source), *extra,
                        '-o', str(binary)], capture_output=True, text=True, timeout=60)
        self.assertEqual(compiled.returncode, 0, compiled.stderr[-8000:])
        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr[-8000:])
        return result

    def test_real_read_shims_keep_results_file_positions_apc_delivery_and_errno(self):
        original = (RUNTIME / 'kernel32.c').read_text()
        outputs = []
        with tempfile.TemporaryDirectory(prefix='asset-read-') as tmp:
            work = Path(tmp)
            for enabled, text in enumerate([original, adapter.patch_asset_reads(original)]):
                section = text[text.index('SHIM(ReadFile) {'):text.index('SHIM(SleepEx) {')]
                fixture = (NATIVE / 'test_asset_read_timing.c').read_text().replace('/* ACTUAL_UPSTREAM_SHIMS */', section)
                source = work / f'read{enabled}.c'
                source.write_text(fixture)
                result = self.compile_run(work, source, [str(METRICS / 'performance_metrics.c'), f'-DEXPECT_METRICS={enabled}'])
                outputs.append(result.stdout)
                self.assertEqual(result.stderr, '')
            self.assertEqual(outputs[0], outputs[1])
            self.assertIn('callbacks=8 reads=4 preads=12 writes=4', outputs[1])

    def test_clock_failure_is_transparent_and_new_failure_labels_are_explicit(self):
        with tempfile.TemporaryDirectory(prefix='asset-clock-') as tmp:
            result = self.compile_run(Path(tmp), NATIVE / 'test_performance_clock.c')
            lines = result.stderr.splitlines()
            self.assertEqual(len(lines), 4)
            for name in ['read_file', 'read_file_ex', 'd3d_shader_create', 'texture_load']:
                line = next(line for line in lines if f'kind={name} ' in line)
                self.assertIn('samples=1', line)
                self.assertIn('failures_known=1 failures=1', line)
                self.assertIn('total_ns=100000001', line)


if __name__ == '__main__':
    unittest.main()
