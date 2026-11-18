"""Exercise unchanged public recorder with separate per-thread cache-line storage."""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('watchdog_stage', ROOT / 'ios/runtime-port/prepare_runtime.py')
adapter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(adapter)


def fixture(text, aligned):
    section = text[text.index('#define WD_MAX 64'):text.index('/* MSVC RTTI class name')]
    template = (ROOT / 'tests/native/test_watchdog_layout.c').read_text(encoding='utf-8')
    return f'#define EXPECT_ALIGNED {int(aligned)}\n' + template.replace('/* ACTUAL_RECORDER */', section)


@unittest.skipUnless(os.name == 'posix' and shutil.which('cc'), 'Existing POSIX C compiler required')
class WatchdogLayoutTests(unittest.TestCase):
    def test_real_recorder_fields_wrapping_and_concurrent_writers(self):
        old = (ROOT / 'vendor/skydream/runtime/dispatch.c').read_text(encoding='utf-8')
        new = adapter.patch_watchdog_layout(old)
        self.assertEqual(new.replace('struct __attribute__((aligned(128)))', 'struct'), old)
        with tempfile.TemporaryDirectory(prefix='watchdog-slots-') as tmp:
            work = Path(tmp); results = []
            for index, text in enumerate([old, new]):
                source = work / f'watchdog{index}.c'; binary = work / f'watchdog{index}'
                source.write_text(fixture(text, index != 0), encoding='utf-8')
                flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if os.getenv('SKYRIM_TEST_SANITIZERS') == '1' else []
                compiled = subprocess.run([os.getenv('CC', 'cc'), '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-pthread', *flags, str(source), '-o', str(binary)], capture_output=True, text=True, timeout=60)
                self.assertEqual(compiled.returncode, 0, compiled.stderr[-8000:])
                result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stderr[-8000:])
                results.append(result.stdout)
            self.assertEqual(results[0], results[1])
            self.assertIn('thread_fields=PASS unsigned_wrap=PASS', results[1])


if __name__ == '__main__':
    unittest.main()
