"""Opt-in 'present in step with the display': decisions, wiring and the off path. No device needed.

The Metal/UIKit calls live in display_timing.m (Apple SDK only). Everything that decides
what they do is in present_pacing_core.c and is run here for real.
"""
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


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


receiver = load('receiver_for_pacing', ROOT / 'scripts/receive_private_diagnostics.py')


class PresentPacingTests(unittest.TestCase):
    def run_native(self, argument=None, environment=None):
        cc = shutil.which(os.environ.get('CC', 'cc'))
        if not cc:
            self.skipTest('Native C compiler needed; run on WSL/Mac')
        with tempfile.TemporaryDirectory(prefix='present-pacing-') as tmp:
            binary = Path(tmp) / 'pacing'
            flags = ['-std=c11', '-D_POSIX_C_SOURCE=200809L', '-O2', '-Wall', '-Wextra', '-Werror']
            if os.getenv('SKYRIM_TEST_SANITIZERS') == '1':
                flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
            subprocess.run([cc, *flags, '-I', str(PORT), str(PORT / 'present_pacing_core.c'),
                            str(PORT / 'mobile_core.c'), str(ROOT / 'tests/native/test_present_pacing_core.c'),
                            '-o', str(binary), '-lm', '-pthread'], check=True, timeout=60)
            env = {k: v for k, v in os.environ.items() if k != 'SKYRIM_IOS_DISPLAY_SYNC'}
            env.update(environment or {})
            return subprocess.run([str(binary), *([argument] if argument else [])], check=True,
                                  capture_output=True, text=True, timeout=30, env=env)

    def test_decisions_off_path_and_deadline_clock(self):
        self.assertIn('present_pacing_core=PASS', self.run_native().stdout)

    def test_only_an_exact_one_enables_it(self):
        self.assertIn('PASS', self.run_native('1', {'SKYRIM_IOS_DISPLAY_SYNC': '1'}).stdout)
        for value in ('0', '', 'true', '2', ' 1'):
            with self.subTest(value=value):
                cc = shutil.which(os.environ.get('CC', 'cc'))
                with tempfile.TemporaryDirectory(prefix='present-pacing-') as tmp:
                    source = Path(tmp) / 'probe.c'
                    source.write_text('#include "present_pacing_core.h"\n#include <stdio.h>\n'
                                      'int main(void){printf("%d",skyrim_present_sync_enabled());return 0;}\n')
                    binary = Path(tmp) / 'probe'
                    subprocess.run([cc, '-std=c11', '-I', str(PORT), str(source), str(PORT / 'present_pacing_core.c'),
                                    '-o', str(binary)], check=True, timeout=60)
                    out = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10,
                                         env={**os.environ, 'SKYRIM_IOS_DISPLAY_SYNC': value})
                    self.assertEqual(out.stdout, '0')

    def test_pacer_changes_nothing_unless_the_option_is_on(self):
        runtime = (PORT / 'ios_runtime.c').read_text()
        start = runtime.index('void skyrim_ios_pace_frame(void) {')
        body = runtime[start:runtime.index('void skyrim_ios_watchdog', start)]
        self.assertIn('uint64_t deadline = skyrim_mobile_frame_deadline(&clock, started, fps);', body)
        guarded = body[body.index('if (skyrim_present_sync_enabled()) {'):]
        guarded = guarded[:guarded.index('\n    }\n') + 7]
        for call in ('skyrim_present_set_target', 'skyrim_present_refresh_hint', 'skyrim_present_cpu_deadline'):
            self.assertEqual(body.count(call), 1, call)
            self.assertIn(call, guarded, call)
        # The rest of the pacer is the established loop, untouched.
        self.assertIn('sleeps += skyrim_device_platform_wait(remaining);', body)
        self.assertIn('SKYRIM_PERF_PACER_OVERRUN, deadline, ended, 0, 0', body)

    def test_metal_hook_is_installed_only_for_the_option_and_never_loses_the_original_present(self):
        source = (PORT / 'display_timing.m').read_text()
        self.assertIn('if (drawable && skyrim_present_sync_enabled()) hook_present_once(drawable);', source)
        self.assertIn('presentAfterMinimumDuration:', source)
        self.assertIn('original_present)(self, command)', source)
        self.assertIn('!inside_present', source)           # a nested -present is never re-routed
        self.assertIn('if (!skyrim_present_sync_enabled()) return;', source)
        self.assertEqual(source.count('@selector(present)'), 1)
        # No cap (target 0) must reach the original -present.
        self.assertIn('const uint64_t minimum = skyrim_present_target();\n    if (minimum &&', source)

    def test_option_is_default_off_labelled_and_wired_to_the_environment(self):
        options = (PORT / 'shader_options.h').read_text()
        self.assertIn('SKYRIM_SHADER_DISPLAY_SYNC,', options)
        self.assertIn('"displaySync"', options)
        self.assertIn('"SKYRIM_IOS_DISPLAY_SYNC"', options)
        ui = (PORT / 'mobile_ui.m').read_text()
        self.assertIn('L(@"Pace frames to the display (experimental)", @"表示に合わせてフレームを出す（試験）")', ui)
        self.assertIn('@"default":@0', ui)                 # every shader/trial row defaults to off
        entry = (PORT / 'ios_entry.m').read_text()         # the generic loop exports every option
        self.assertIn('setenv(skyrim_shader_option_environment(i), enabled ? "1" : "0", 1)', entry)

    def test_uneven_cadence_is_a_note_in_both_languages_and_never_rewrites_the_setting(self):
        ui = (PORT / 'mobile_ui.m').read_text()
        start = ui.index('static NSString *unevenCadenceNote(void) {')
        note = ui[start:ui.index('\n}\n', start)]
        self.assertIn('so a %u FPS limit cannot be shown evenly', note)
        self.assertIn('この画面の更新は%uHzのため、%uFPS上限では均等にならず', note)
        self.assertNotIn('setObject', note)
        self.assertNotIn('settings[', note)
        self.assertNotIn('persist', note)
        self.assertIn('if (value(@"displaySync", 0) != 1) return nil;', note)
        self.assertIn('skyrim_present_even_cadence(hz, fps)', note)
        # Same count of format arguments in both languages (hz, fps).
        texts = re.findall(r'@"([^"]*%u[^"]*)"', note)
        self.assertEqual([t.count('%u') for t in texts], [2, 2])

    def test_info_plist_allows_the_high_refresh_request(self):
        plist = (PORT / 'Info.plist.in').read_text()
        self.assertIn('<key>CADisableMinimumFrameDurationOnPhone</key><true/>', plist)

    def test_new_status_lines_pass_the_wire_allowlist(self):
        schema = receiver.Schema()
        for line in ('IOS_DISPLAY_SYNC schema=1 enabled=0', 'IOS_DISPLAY_SYNC schema=1 enabled=1',
                     'IOS_DISPLAY_SYNC_HOOK schema=1 hooked=1', 'IOS_SHADER_OPTION option=7 enabled=1'):
            schema.line(line)
        for line in ('IOS_DISPLAY_SYNC schema=1 enabled=2', 'IOS_DISPLAY_SYNC_HOOK schema=1 hooked=1 class=X',
                     'IOS_SHADER_OPTION option=8 enabled=1'):
            with self.assertRaises(Exception, msg=line):
                schema.line(line)

    def test_nothing_that_shapes_the_shader_cache_identity_changed(self):
        names = [n for n in ('present_pacing_core', 'display_timing', 'cpu_sampler')]
        namespace = load('namespace_for_pacing', ROOT / 'ios/dependencies/pipeline_cache_namespace.py')
        for name in names:
            self.assertFalse(any(name in item for item in namespace.INPUTS), name)
        build = (ROOT / 'ios/dependencies/CMakeLists.txt').read_text()
        self.assertIn('present_pacing_core.c', build)


if __name__ == '__main__':
    unittest.main()
