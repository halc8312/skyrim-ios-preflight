"""Authored profiles through the real staged INI reader/writer/seeder.

No game data, UIKit execution, or device performance is implied.
"""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('autosave_stage', ROOT/'ios/runtime-port/prepare_runtime.py')
stage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stage)


class AutosavePreferencesTests(unittest.TestCase):
    def test_actual_profile_roundtrip_and_preserved_settings(self):
        cc = shutil.which(os.environ.get('CC', 'cc'))
        if not cc:
            self.skipTest('POSIX compiler required; run on WSL/Mac')
        source = stage.patch_preferences((ROOT/'vendor/skydream/runtime/skyrim_shims.c').read_text())
        helpers = source[source.index('typedef struct { char *sec'):source.index('/* Copies a double-NUL')]
        helpers += source[source.index('static void ini_save('):source.index('/* (app, key, value, file)')]
        helpers += source[source.index('static void ini_set('):source.index('void skyrim_shims_init(')]
        fixture = (ROOT/'tests/native/test_autosave_preferences.c').read_text().replace('/* ACTUAL_INI_CODE */', helpers)
        with tempfile.TemporaryDirectory(prefix='autosave-prefs-') as tmp:
            c = Path(tmp)/'fixture.c'
            exe = Path(tmp)/'fixture'
            c.write_text(fixture)
            flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if os.getenv('SKYRIM_TEST_SANITIZERS') == '1' else []
            result = subprocess.run([cc, '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-DSKYDREAM_MOBILE=1',
                '-O2', '-Wall', '-Wextra', '-Werror', '-pthread', *flags,
                '-I', str(ROOT/'ios/runtime-port'), str(c), '-o', str(exe)], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], cwd=tmp, capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('autosave_profile=PASS', result.stdout)


if __name__ == '__main__': unittest.main()
