"""Lightweight mode: the portable core, the staged ini reader/writer it hooks, the off path and the wiring.

No game data and no device. What is run here is the real lightweight_core.c and the real, patched ini
functions of the staged runtime; the Apple-only settings screen is checked by reading its source.
"""
import hashlib
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
PORT = ROOT / 'ios/runtime-port'
BASELINE_COMMIT = 'c364281f'      # build 24: the last tree without lightweight mode
# SHA-256 of the explicit-override assembly in mobile_ui.m (iniCount = 0 ... the decals block) at build 24,
# with line endings normalised. Lightweight mode must leave it untouched: that is what "off = as before" rests on.
OVERRIDE_ASSEMBLY_SHA256 = '64140e6c2acf827a4eb1b59ab9eb6579141d5791c080caa40b8294b71d2546ec'
ASSEMBLY_START = '    iniCount = 0;\n'
ASSEMBLY_END = 'integerINI("Decals","uMaxSkinDecalPerActor",enabled ? 20 : 0);\n    }\n'

stage_spec = importlib.util.spec_from_file_location('lightweight_stage', PORT / 'prepare_runtime.py')
stage = importlib.util.module_from_spec(stage_spec)
stage_spec.loader.exec_module(stage)
light_spec = importlib.util.spec_from_file_location('lightweight_prepare', PORT / 'prepare_lightweight.py')
light = importlib.util.module_from_spec(light_spec)
light_spec.loader.exec_module(light)
receiver_spec = importlib.util.spec_from_file_location('lightweight_receiver', ROOT / 'scripts/receive_private_diagnostics.py')
receiver = importlib.util.module_from_spec(receiver_spec)
sys.modules[receiver_spec.name] = receiver
receiver_spec.loader.exec_module(receiver)


def text(path):
    return Path(path).read_text(encoding='utf-8').replace('\r\n', '\n')


def assembly(source):
    start = source.index(ASSEMBLY_START)
    return source[start:source.index(ASSEMBLY_END, start) + len(ASSEMBLY_END)]


def compiler(case):
    cc = shutil.which(os.environ.get('CC', 'cc'))
    if not cc:
        case.skipTest('Native C compiler needed; run on WSL/Mac')
    return cc


def build_and_run(case, sources, extra_source=None, cwd=None, defines=()):
    cc = compiler(case)
    with tempfile.TemporaryDirectory(prefix='lightweight-') as tmp:
        binary = Path(tmp) / 'run'
        flags = ['-std=c11', '-D_POSIX_C_SOURCE=200809L', '-O2', '-Wall', '-Wextra', '-Werror', *defines]
        if os.getenv('SKYRIM_TEST_SANITIZERS') == '1':
            flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run([cc, *flags, '-I', str(PORT), *map(str, sources), '-o', str(binary), '-lm', '-pthread'],
                       check=True, capture_output=True, text=True, timeout=120)
        work = Path(tmp) / 'work'
        work.mkdir()
        return subprocess.run([str(binary)], cwd=work, capture_output=True, text=True, timeout=60)


class CoreTests(unittest.TestCase):
    def test_core_rules_off_path_and_threads(self):
        result = build_and_run(self, [PORT / 'lightweight_core.c', ROOT / 'tests/native/test_lightweight_core.c'])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('lightweight_core=PASS', result.stdout)

    def test_every_diagnostic_line_the_core_prints_is_allowlisted(self):
        result = build_and_run(self, [PORT / 'lightweight_core.c', ROOT / 'tests/native/test_lightweight_core.c'])
        self.assertEqual(result.returncode, 0, result.stderr)
        schema = receiver.Schema()
        lines = [line for line in result.stderr.splitlines() if line.startswith('IOS_LIGHT ')]
        kinds = {line.split(' ')[1] for line in lines}
        self.assertEqual(kinds, {'read', 'write_kept', 'summary'})
        for line in lines:
            with self.subTest(line=line):
                schema.line(line)

    def test_schema_accepts_new_records_and_rejects_neighbours(self):
        schema = receiver.Schema()
        for line in ('IOS_GRAPHICS override_count=27 resolution=956x440',
                     'IOS_GRAPHICS override_count=27 resolution=956x440 lightweight=0',
                     'IOS_GRAPHICS override_count=0 resolution=0x0 lightweight=2',
                     'IOS_DIAG_SETTING key=19 value=2',
                     'IOS_LIGHT read file=prefs section=Display key=fShadowDistance from=8000.0000 src=ini to=3000.0000',
                     'IOS_LIGHT read file=ini section=Grass key=iMinGrassSize from=20.0000 src=default to=40',
                     'IOS_LIGHT read file=prefs section=Water key=iWaterReflectWidth from=512.0000 src=default to=same',
                     'IOS_LIGHT write_kept section=Display key=iMaxAnisotropy',
                     'IOS_LIGHT summary mode=1 keys=23 read=20 changed=15 exempt=1 unread=iShadowFilter,bDoDepthOfField',
                     'IOS_LIGHT summary mode=0 keys=0 read=0 changed=0 exempt=0 unread='):
            with self.subTest(line=line):
                schema.line(line)
        for line in ('IOS_GRAPHICS override_count=27 resolution=956x440 lightweight=3',
                     'IOS_GRAPHICS override_count=27 resolution=956x440 lightweight=1 extra=1',
                     'IOS_DIAG_SETTING key=20 value=1',
                     'IOS_LIGHT read file=other section=Display key=x from=1.0000 src=ini to=1',
                     'IOS_LIGHT read file=prefs section=Display key=fShadowDistance from=8000.0000 src=ini to=/private/path',
                     'IOS_LIGHT write_kept section=Display key=a/b',
                     'IOS_LIGHT summary mode=3 keys=1 read=1 changed=1 exempt=0 unread=',
                     'IOS_LIGHT summary mode=1 keys=1 read=1 changed=1 exempt=0 unread=/etc/passwd',
                     'IOS_LIGHT secret=1'):
            with self.subTest(line=line):
                with self.assertRaises(receiver.Rejected):
                    schema.line(line)


def staged_shims():
    return light.patch_shims(stage.patch_preferences((ROOT / 'vendor/skydream/runtime/skyrim_shims.c').read_text()))


class StagedIniTests(unittest.TestCase):
    def test_patched_runtime_ini_functions_with_the_real_core(self):
        source = staged_shims()
        code = source[source.index('typedef struct { char *sec'):source.index('/* Copies a double-NUL')]
        code += source[source.index('/* Copies a double-NUL'):source.index('/* ===================== kernel32')]
        fixture = (ROOT / 'tests/native/test_lightweight_shims.c').read_text().replace('/* ACTUAL_INI_CODE */', code)
        with tempfile.TemporaryDirectory(prefix='lightweight-shims-') as tmp:
            c = Path(tmp) / 'fixture.c'
            c.write_text(fixture)
            cc = compiler(self)
            binary = Path(tmp) / 'fixture'
            flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if os.getenv('SKYRIM_TEST_SANITIZERS') == '1' else []
            build = subprocess.run([cc, '-std=gnu11', '-D_GNU_SOURCE', '-DSKYDREAM_MOBILE=1', '-O2', '-Wall', '-Wextra', '-Werror',
                                    '-Wno-unused-function', '-Wno-unused-parameter', '-pthread', *flags, '-I', str(PORT), str(c),
                                    str(PORT / 'lightweight_core.c'), '-o', str(binary), '-lm'],
                                   capture_output=True, text=True, timeout=120)
            self.assertEqual(build.returncode, 0, build.stderr)
            work = Path(tmp) / 'work'
            work.mkdir()
            result = subprocess.run([str(binary)], cwd=work, capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('lightweight_shims=PASS', result.stdout)

    def test_patch_adds_only_the_reviewed_hunks_and_refuses_a_changed_anchor(self):
        prepared = stage.patch_preferences((ROOT / 'vendor/skydream/runtime/skyrim_shims.c').read_text())
        patched = light.patch_shims(prepared)
        for _, after, label in light.HUNKS:
            self.assertEqual(patched.count(after), 1, label)
        for before, after, label in light.HUNKS:
            with self.assertRaises(ValueError, msg=label):
                light.patch_shims(prepared.replace(before, before.replace('(', '[', 1)))
        with self.assertRaises(ValueError):
            light.patch_shims(prepared.replace(light.INCLUDE[0], '#include "other.h"'))
        # Only the include and the three ini functions change; undoing exactly the reviewed hunks restores the input.
        restored = patched.replace(light.INCLUDE[1], light.INCLUDE[0])
        for before, after, _ in light.HUNKS:
            restored = restored.replace(after, before)
        self.assertEqual(restored, prepared)
        self.assertEqual(sum(patched.count(name) for name in ('skyrim_light_read(', 'skyrim_light_write_is_echo(')), 3)

    def test_staging_leaves_prepare_runtime_and_the_cache_identity_inputs_alone(self):
        # prepare_runtime.py is an input of the pipeline-cache identity; lightweight mode must not move it.
        self.assertNotIn('lightweight', (PORT / 'prepare_runtime.py').read_text())
        script = (ROOT / 'scripts/build-title-host.sh').read_text()
        prepare = script.index('ios/runtime-port/prepare_runtime.py --source')
        lightweight = script.index('python3 ios/runtime-port/prepare_lightweight.py "$RUN/overlay/runtime/skyrim_shims.c"')
        self.assertLess(prepare, lightweight)
        self.assertLess(lightweight, script.index('git -C "$RUN/overlay" init -q'))


class OffPathAndWiringTests(unittest.TestCase):
    def test_explicit_override_assembly_is_unchanged_since_build_24(self):
        mine = assembly(text(PORT / 'mobile_ui.m'))
        self.assertEqual(hashlib.sha256(mine.encode()).hexdigest(), OVERRIDE_ASSEMBLY_SHA256)
        git = shutil.which('git')
        if git:
            shown = subprocess.run([git, 'show', f'{BASELINE_COMMIT}:pc-aot/ios/runtime-port/mobile_ui.m'], cwd=ROOT,
                                   capture_output=True, timeout=30)
            if shown.returncode == 0:
                self.assertEqual(mine, assembly(shown.stdout.decode('utf-8').replace('\r\n', '\n')))

    def test_mode_and_exemptions_are_set_after_the_overrides_and_only_from_the_startup_value(self):
        source = text(PORT / 'mobile_ui.m')
        init = source[source.index('void skyrim_mobile_initialize('):source.index('static BOOL applyGameLanguageProfile')]
        self.assertLess(init.index(ASSEMBLY_END), init.index('skyrim_light_set_mode('))
        self.assertLess(init.index('skyrim_light_set_mode('), init.index('skyrim_light_exempt(ini[i].section, ini[i].key)'))
        self.assertIn('startup[@"lightweight"]', init)
        self.assertEqual(source.count('skyrim_light_set_mode('), 1)
        # Nothing in lightweight mode feeds the ini[] list that is written to SkyrimPrefs.ini.
        self.assertNotIn('lightweight', assembly(source))
        self.assertIn('lightweight=%d\\n",iniCount,renderWidth,renderHeight,skyrim_light_mode())', source)

    def test_settings_row_is_separate_from_the_preset_list_and_validated(self):
        source = text(PORT / 'mobile_ui.m')
        rows = source[source.index('static NSArray<NSDictionary *> *graphicsRows(void) {'):source.index('/* Lightweight mode: its own row')]
        self.assertNotIn('lightweight', rows)
        self.assertIn('section==2 ? graphicsRows().count+2 :', source)
        self.assertIn('if (p.section==2 && (NSUInteger)p.row == graphicsRows().count+1) return lightweightRow();', source)
        self.assertIn('@"values":@[@0,@1,@2], @"default":@0', source)
        self.assertIn('[lightweightRow()[@"values"] containsObject:@(value(@"lightweight",0))]', source)
        self.assertIn('"game_text", "game_voices", "lightweight"}', source)
        # The presets still pair graphicsRows() with their tuples by position.
        self.assertEqual(len(re.findall(r'\{[0-9.,]+\}', source[source.index('const double v[5][11]'):source.index('NSArray *rows=graphicsRows();')])), 5)
        self.assertEqual(len(re.findall(r'@\{@"key":', rows)), 11)

    def test_diagnostic_key_is_last_and_summary_is_sent_with_the_settings(self):
        schema = text(PORT / 'diagnostic_schema.h')
        self.assertIn('SKYRIM_DIAG_SETTING_LIGHTWEIGHT = 19,', schema)
        source = text(PORT / 'mobile_ui.m')
        self.assertIn('logDiagnosticSettings();\n        skyrim_light_log_summary();', source)

    def test_sources_are_built_into_the_app_and_the_simulator(self):
        self.assertIn('lightweight_core.c', text(PORT / 'CMakeLists.txt'))
        self.assertIn('lightweight_core.c', text(ROOT / 'tests/mod-simulator/prepare.py'))
        self.assertIn('lightweight_core.h', text(PORT / 'prepare_lightweight.py'))


if __name__ == '__main__':
    unittest.main()
