"""Actual public I/O shims and authored inputs only; no game files or device."""
import importlib.util
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('save_runtime', ROOT/'ios/runtime-port/prepare_runtime.py')
adapter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(adapter)
RUNTIME = ROOT/'vendor/skydream/runtime/kernel32.c'
METRICS = ROOT/'ios/runtime-port'


class SaveWriteTests(unittest.TestCase):
    def run_c(self, source, extra=()):
        cc = shutil.which(os.environ.get('CC', 'cc'))
        if not cc:
            self.skipTest('POSIX host compiler required; run on WSL/Mac')
        with tempfile.TemporaryDirectory(prefix='save-write-') as tmp:
            c = Path(tmp)/'fixture.c'
            exe = Path(tmp)/'fixture'
            c.write_text(source)
            flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if os.getenv('SKYRIM_TEST_SANITIZERS') == '1' else []
            result = subprocess.run([cc, '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-O2',
                '-Wall', '-Wextra', '-Werror', '-pthread', *flags, '-I', str(METRICS), str(c),
                str(METRICS/'performance_metrics.c'), *extra, '-o', str(exe)],
                capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr[-8000:])
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr[-8000:])
            return result

    def test_observation_reversal_restores_exact_upstream_and_patch_composes(self):
        old = RUNTIME.read_text()
        new = adapter.patch_file_writes(old)
        restored = new.removeprefix('#include "save_write_probe.h"\n')
        restored = restored.replace(' SkyrimSaveWriteProbe ios_write;', '')
        restored = re.sub(r'^ *skyrim_save_write_(?:init|end|close)\([^\n]+\);\n', '', restored, flags=re.M)
        restored = re.sub(r'^ *if \(write\) skyrim_save_write_end\([^\n]+\);\n', '', restored, flags=re.M)
        restored = re.sub(r'^ *const uint64_t ios_write_started = [^\n]+;\n', '', restored, flags=re.M)
        restored = re.sub(r'^ *const uint64_t ios_close_started = [^\n]+;\n', '', restored, flags=re.M)
        restored = re.sub(r'^ *const unsigned ios_save_candidate = [^\n]+;\n', '', restored, flags=re.M)
        self.assertEqual(restored, old)
        self.assertIn('skyrim_save_write_init', adapter.prepare(ROOT/'vendor/skydream')['kernel32.c'])
        with self.assertRaises(ValueError):
            adapter.patch_file_writes(old.replace('r = write(f->fd, MEM + buf, n);', 'r = changed();'))

    def test_real_write_shims_preserve_success_short_failure_positions_and_callbacks(self):
        old = RUNTIME.read_text()
        outputs = []
        variants = [old, adapter.patch_file_writes(old), adapter.patch_kernel(old)]
        for index, text in enumerate(variants):
            body = text[text.index('SHIM(ReadFile) {'):text.index('SHIM(SleepEx) {')]
            body += text[text.index('SHIM(WriteFile) {'):text.index('/* lseek,')]
            body += text[text.index('SHIM(CloseHandle) {'):text.index('static void unix_to_filetime')]
            typedef = re.search(r'typedef struct \{ int fd; char path\[1024\];[^\n]+ GFile;', text)[0]
            source = (ROOT/'tests/native/test_save_write_shims.c').read_text()
            source = source.replace('/* GFILE_TYPE */', typedef).replace('/* ACTUAL_SHIMS */', body)
            result = self.run_c(source, [f'-DEXPECT_METRICS={int(index != 0)}'])
            self.assertEqual(result.stderr, '')
            outputs.append(result.stdout)
        self.assertEqual(outputs[0], outputs[1])
        self.assertEqual(outputs[0], outputs[2])
        self.assertIn('save_write_behavior=PASS', outputs[0])

    def test_candidate_bounds_concurrent_writes_and_handle_lifetime_are_explicit(self):
        result = self.run_c((ROOT/'tests/native/test_save_write_state.c').read_text())
        self.assertIn('save_candidate_state=PASS', result.stdout)
        window = next(x for x in result.stderr.splitlines() if 'kind=save_candidate_window ' in x)
        self.assertIn('failures_known=0', window)
        for kind in ['write_file', 'write_file_ex', 'save_candidate_write', 'save_candidate_write_ex']:
            line = next(x for x in result.stderr.splitlines() if f'kind={kind} ' in x)
            self.assertIn('failures_known=1 failures=1', line)


if __name__ == '__main__': unittest.main()
