"""Actual prepared runtime lock boundaries, authored audio and COM objects."""
import importlib.util
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / 'ios/runtime-port'
NATIVE = ROOT / 'tests/native'


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


prep = load('lock_prepare', PORT / 'prepare_runtime.py')
adapter = load('runtime_lock_timing', PORT / 'runtime_lock_timing.py')
lifecycle = load('lock_lifecycle', PORT / 'lifecycle_adapters.py')


def before_lock(name):
    return lifecycle.adapt(name, prep.PATCHERS[name](
        (ROOT / 'vendor/skydream/runtime' / name).read_text(encoding='utf-8')))


def body(text, anchor):
    return adapter.function(text, anchor)[2]


class RuntimeLockTimingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.staged = prep.prepare(ROOT / 'vendor/skydream')

    def run_native(self, text, suffix):
        compiler = shutil.which(os.environ.get('CXX' if suffix == '.cpp' else 'CC',
                                               'c++' if suffix == '.cpp' else 'cc'))
        if os.name != 'posix' or not compiler:
            self.skipTest('Existing POSIX compiler required; run on WSL or Mac')
        with tempfile.TemporaryDirectory(prefix='runtime-lock-') as tmp:
            source, binary = Path(tmp) / ('fixture' + suffix), Path(tmp) / 'fixture'
            source.write_text(text, encoding='utf-8')
            sanitizers = os.environ.get('SKYRIM_TEST_SANITIZERS', '')
            flags = ['-fsanitize=' + ('thread' if sanitizers == 'thread' else 'address,undefined'),
                     '-fno-omit-frame-pointer'] if sanitizers else []
            compiled = subprocess.run([compiler, '-std=c++17' if suffix == '.cpp' else '-std=c11',
                '-D_POSIX_C_SOURCE=200809L', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-pthread',
                *flags, '-I', str(PORT), '-I', str(NATIVE), str(source), '-o', str(binary)],
                capture_output=True, text=True, timeout=60)
            self.assertEqual(compiled.returncode, 0, compiled.stderr[-10000:])
            executed = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            self.assertEqual(executed.returncode, 0, executed.stderr[-10000:])
            return executed.stdout

    def test_effective_source_coverage_and_only_observation_changes(self):
        for name in ('xaudio2.c', 'd3d9_bridge.cpp'):
            original = before_lock(name)
            patched = self.staged[name]
            self.assertEqual(patched, adapter.adapt(name, original))
            with self.assertRaises(ValueError):
                adapter.adapt(name, patched)
        # Decode functions, queue operations, callback delivery and guest Release
        # stay the actual bodies. Independent inverse removes observation only.
        original = before_lock('xaudio2.c')
        restored = self.staged['xaudio2.c']
        helpers = (PORT / 'runtime_lock_event.h.in').read_text() + '\n' + (PORT / 'runtime_audio_lock.c.in').read_text()
        restored = restored.replace('static pthread_mutex_t g_lock;\n' + helpers, 'static pthread_mutex_t g_lock;', 1)
        restored = re.sub(r'ios_audio_lock\(SKYRIM_LOCK_OP_AUDIO_[A-Z_]+\);', 'pthread_mutex_lock(&g_lock);', restored)
        restored = restored.replace('ios_audio_unlock();', 'pthread_mutex_unlock(&g_lock);')
        restored = restored.replace('        const uint64_t iosAdpcmStart = ios_runtime_lock_now();\n', '')
        restored = restored.replace('        ios_runtime_lock_report(SKYRIM_LOCK_AUDIO, SKYRIM_LOCK_WORK,\n'
            '            SKYRIM_LOCK_OP_ADPCM_DECODE, iosAdpcmStart, ios_runtime_lock_now(), 0);\n', '')
        restored = restored.replace('    {\n        const uint64_t iosResetStart = ios_runtime_lock_now();\n'
            '        xwma_reset(v->wma);\n'
            '        ios_runtime_lock_report(SKYRIM_LOCK_AUDIO, SKYRIM_LOCK_WORK,\n'
            '            SKYRIM_LOCK_OP_AUDIO_RESET, iosResetStart, ios_runtime_lock_now(), 0);\n    }',
            '    xwma_reset(v->wma);')
        self.assertEqual(restored, original)
        for function in ('void com_AddRef(CPU *c) {', 'void com_Release(CPU *c) {', 'void shadow_forget(void *obj) {'):
            self.assertEqual(body(self.staged['d3d9_bridge.cpp'], function), body(before_lock('d3d9_bridge.cpp'), function))
        self.assertEqual(body(self.staged['xaudio2.c'], 'static void *engine_main(void *arg) {'),
                         body(original, 'static void *engine_main(void *arg) {'))

    def test_actual_audio_submit_flush_errors_recursion_and_contention(self):
        text = self.staged['xaudio2.c']
        types = text[text.index('#define PASS_FRAMES_MS'):text.index('static HComClass')]
        start = text.index('/* Fixed-size observations only.')
        end = text.index('\nstatic Voice *g_voices', start)
        helper = text[start:end]
        start = text.index('static int ios_decode_xwma(')
        observer = text[start:text.index('SHIM(xs_SubmitSourceBuffer)', start)]
        adpcm = text[text.index('static int16_t adpcm_clamp('):text.index('static void set_format(')]
        fixture = (NATIVE / 'runtime_lock_audio.c').read_text()
        for marker, code in [('TYPES', types), ('AUDIO_HELPER', helper), ('ADPCM', adpcm), ('XWMA_OBSERVER', observer),
                ('SUBMIT', body(text, 'SHIM(xs_SubmitSourceBuffer) {')),
                ('FLUSH', body(text, 'SHIM(xs_FlushSourceBuffers) {')),
                ('GET_STATE', body(text, 'SHIM(xs_GetState) {'))]:
            fixture = fixture.replace('/* ACTUAL_' + marker + ' */', code)
        self.assertIn('get_state_wait=PASS', self.run_native(fixture, '.c'))

    def test_actual_com_fast_lookup_release_cleanup_and_recursive_scope(self):
        text = self.staged['d3d9_bridge.cpp']
        registry = text[text.index('// ---------------------------------------------------------------- registry'):text.index('void com_register_iface(')]
        # Hook the underlying blocking call only to synchronize the contender
        # test. All mutex operations delegate to the real recursive mutex.
        registry = registry.replace('std::recursive_mutex g_lock;', 'FixtureMutex g_lock;', 1)
        functions = '\n'.join(body(text, signature) for signature in (
            'void *com_host(uint32_t g) {', 'uint32_t com_wrap(void *host, int iface) {',
            'static void com_forget(uint32_t g) {', 'void com_AddRef(CPU *c) {', 'void com_Release(CPU *c) {'))
        fixture = (NATIVE / 'runtime_lock_com.cpp').read_text().replace('/* ACTUAL_REGISTRY */', registry)
        fixture = fixture.replace('/* ACTUAL_EVENT_HELPER */', (PORT / 'runtime_lock_event.h.in').read_text())
        fixture = fixture.replace('/* ACTUAL_COM_FUNCTIONS */', functions)
        self.assertIn('hot_clocks=0', self.run_native(fixture, '.cpp'))

    def test_actual_mixer_pcm_callbacks_and_retirement_remain_identical(self):
        mixer = load('lock_audio_submix_test', ROOT / 'tests/test_audio_submix.py')
        outputs = []
        for text in (before_lock('xaudio2.c'), self.staged['xaudio2.c']):
            fixture = mixer.fixture(text)
            if 'ios_audio_lock(' in fixture:
                start = text.index('/* Fixed-size observations only.')
                end = text.index('\nstatic Voice *g_voices', start)
                fixture = fixture.replace('static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;',
                    '#include "runtime_lock_metrics_fixture.h"\n'
                    'static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;\n' + text[start:end])
                # These fixture utilities are only needed by dedicated contention tests.
                fixture = fixture.replace('int main(void) {', 'int main(void) {\n'
                    '    fixture_clear(); (void)fixture_count; (void)fixture_wait_flag;')
            outputs.append(self.run_native(fixture, '.c'))
        self.assertEqual(outputs[0], outputs[1])
        self.assertIn('pcm_graph_cases=132 callback_lock=PASS', outputs[1])


if __name__ == '__main__':
    unittest.main()
