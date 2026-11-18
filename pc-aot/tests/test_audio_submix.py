"""Real public mixer differential: authored PCM/graphs, no game inputs."""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('audio_stage', ROOT / 'ios/runtime-port/prepare_runtime.py')
adapter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(adapter)


def sources():
    old = (ROOT / 'vendor/skydream/runtime/xaudio2.c').read_text(encoding='utf-8')
    new = adapter.patch_audio_submix(old)
    start = old.index('    /* submixes in processing-stage order')
    end = old.index('    if (g_master && g_master->acc)', start)
    changed_start = new.index('    /* Stable stage buckets:')
    changed_end = new.index('    if (g_master && g_master->acc)', changed_start)
    # Independently bound the change; all decoding, floating-point expressions,
    # callback delivery, cleanup and mutex operations must remain verbatim.
    assert new[:changed_start] == old[:start]
    assert new[changed_end:] == old[end:]
    return old, new


def fixture(text, instrument=True):
    template = (ROOT / 'tests/native/test_audio_submix.c').read_text(encoding='utf-8')
    types = text[text.index('#define PASS_FRAMES_MS'):text.index('static HComClass')]
    code = text[text.index('static void read_frame('):text.index('static uint64_t now_us(')]
    if instrument:
        code = code.replace('static void route(', 'static void actual_route(', 1)
        marker = 'static void mix_source('
        wrapper = '''static void route(Voice *v, const float *in, int frames) {
    assert(pthread_mutex_trylock(&g_lock) == EBUSY);
    hash_u64(v->guest);
    ++route_count;
    actual_route(v, in, frames);
}
'''
        code = code.replace(marker, wrapper + marker, 1)
    return template.replace('/* ACTUAL_TYPES */', types).replace('/* ACTUAL_MIXER */', code)


@unittest.skipUnless(os.name == 'posix' and shutil.which('cc'), 'Existing POSIX C compiler required')
class AudioSubmixTests(unittest.TestCase):
    def test_real_pcm_order_callbacks_and_voice_lifetimes_are_identical(self):
        with tempfile.TemporaryDirectory(prefix='audio-submix-') as tmp:
            work = Path(tmp)
            results = []
            for index, text in enumerate(sources()):
                source, binary = work / f'mixer{index}.c', work / f'mixer{index}'
                source.write_text(fixture(text), encoding='utf-8')
                flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if os.getenv('SKYRIM_TEST_SANITIZERS') == '1' else []
                command = [os.getenv('CC', 'cc'), '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-O2', '-g',
                           '-Wall', '-Wextra', '-Werror', '-pthread', *flags, str(source), '-o', str(binary)]
                compiled = subprocess.run(command, capture_output=True, text=True, timeout=60)
                self.assertEqual(compiled.returncode, 0, compiled.stderr[-8000:])
                result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stderr[-8000:])
                results.append(result.stdout)
            self.assertEqual(results[0], results[1])
            self.assertIn('pcm_graph_cases=132', results[1])
            self.assertIn('callback_lock=PASS', results[1])
        adapter.prepare(ROOT / 'vendor/skydream')


if __name__ == '__main__':
    unittest.main()
