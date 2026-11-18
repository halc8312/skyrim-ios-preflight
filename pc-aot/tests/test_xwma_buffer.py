"""Public xWMA buffer differential; no game assets or private runtime inputs."""
import ctypes
import importlib.util
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


adapter = module('xwma_adapter', ROOT / 'ios/runtime-port/prepare_runtime.py')


def sources():
    old = (ROOT / 'vendor/skydream/runtime/xwma.c').read_text(encoding='utf-8')
    new = adapter.prepare(ROOT / 'vendor/skydream')['xwma.c']
    start = 'static int receive_pcm('
    end = '\n#else\nstruct XWmaDecoder { int unused; };'
    # The independently tested voice-profile adapter changes only codec setup
    # and reset; this differential isolates the PCM allocation optimization.
    setup = adapter.patch_xwma_voice_profile(old)
    assert setup[:setup.index(start)] == new[:new.index(start)].replace('#include <errno.h>\n', '', 1)
    assert old[old.index(end):] == new[new.index(end):]
    return old, new


@unittest.skipUnless(os.name == 'posix' and shutil.which('cc'), 'Existing POSIX C compiler required')
class XwmaBufferTests(unittest.TestCase):
    def test_actual_decode_bodies_preserve_samples_order_and_failure_cleanup(self):
        template = (ROOT / 'tests/native/test_xwma_buffer.c').read_text(encoding='utf-8')
        results = []
        with tempfile.TemporaryDirectory(prefix='xwma-buffer-') as tmp:
            work = Path(tmp)
            for index, text in enumerate(sources()):
                body = text[text.index('static int receive_pcm('):text.index('\n#else\nstruct XWmaDecoder { int unused; };')]
                source, binary = work / f'decoder{index}.c', work / f'decoder{index}'
                overflow = ('int rc = receive_pcm(&d, &pcm, &frames);' if index == 0 else
                            'size_t capacity = 0, extent = 0; int rc = receive_pcm(&d, &pcm, &frames, &capacity, &extent, 65536);')
                source.write_text(template.replace('/* ACTUAL_DECODE */', body).replace('/* OVERFLOW_RECEIVE */', overflow), encoding='utf-8')
                flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if os.getenv('SKYRIM_TEST_SANITIZERS') == '1' else []
                compiled = subprocess.run([os.getenv('CC', 'cc'), '-std=c11', '-O2', '-g', '-Wall', '-Wextra',
                                           '-Werror', *flags, str(source), '-o', str(binary)],
                                          capture_output=True, text=True, timeout=60)
                self.assertEqual(compiled.returncode, 0, compiled.stderr[-8000:])
                result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stderr[-8000:])
                results.append(json.loads(result.stdout))
        self.assertEqual(results[0]['cases'], results[1]['cases'])
        self.assertEqual(results[0]['digest'], results[1]['digest'])
        self.assertEqual(results[0]['streaming_calls'], results[1]['streaming_calls'])
        self.assertGreater(results[1]['growth_failures'], results[0]['growth_failures'])
        self.assertGreater(results[1]['shrink_failures'], 0)
        self.assertLess(results[1]['alloc_calls'], results[0]['alloc_calls'])
        self.assertLess(results[1]['copied_bytes'], results[0]['copied_bytes'] // 2)
        print('XWMA_BUFFER_DIFFERENTIAL ' + json.dumps(results, sort_keys=True))

    @unittest.skipUnless(os.getenv('SKYRIM_FFMPEG_TEST_CFLAGS') and shutil.which('ffmpeg'),
                         'Optional host FFmpeg development headers/libraries not configured')
    def test_real_decoder_pcm_matches_old_and_independent_ffmpeg(self):
        upstream = module('xwma_public_test', ROOT / 'vendor/skydream/tools/test_xwma.py')
        with tempfile.TemporaryDirectory(prefix='xwma-real-') as tmp:
            work = Path(tmp)
            libraries = []
            for index, text in enumerate(sources()):
                source, binary = work / f'decoder{index}.c', work / f'decoder{index}.so'
                source.write_text(text, encoding='utf-8')
                command = [os.getenv('CC', 'cc'), '-O2', '-shared', '-fPIC', '-DLB_HAVE_FFMPEG',
                           '-I', str(ROOT / 'vendor/skydream/runtime'),
                           *shlex.split(os.environ['SKYRIM_FFMPEG_TEST_CFLAGS']), str(source),
                           '-o', str(binary), *shlex.split(os.environ['SKYRIM_FFMPEG_TEST_LIBS'])]
                compiled = subprocess.run(command, capture_output=True, text=True, timeout=60)
                self.assertEqual(compiled.returncode, 0, compiled.stderr[-8000:])
                lib = ctypes.CDLL(str(binary))
                lib.xwma_create.argtypes = [ctypes.c_uint, *([ctypes.c_int] * 5), ctypes.c_void_p, ctypes.c_size_t]
                lib.xwma_create.restype = ctypes.c_void_p
                lib.xwma_decode.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint,
                                           ctypes.c_int, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_uint32)]
                lib.xwma_destroy.argtypes = [ctypes.c_void_p]
                libraries.append(lib)
            for channels, rate, duration in ((1, 22050, 0.3), (1, 44100, 3), (2, 48000, 12)):
                with self.subTest(channels=channels, rate=rate, duration=duration):
                    wav = work / 'authored-tone.wav'
                    subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y', '-f', 'lavfi',
                                    '-i', f'sine=frequency=440:duration={duration}', '-ar', str(rate),
                                    '-ac', str(channels), '-c:a', 'wmav2', '-b:a', '64k', '-f', 'wav', str(wav)],
                                   check=True, timeout=30, capture_output=True)
                    reference = subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-i', str(wav),
                                                '-f', 's16le', '-'], check=True, capture_output=True, timeout=30).stdout
                    self.assertTrue(any(reference))
                    for lib in libraries:
                        for streaming in (False, True):
                            self.assertEqual(upstream.decode_wave(lib, wav.read_bytes(), streaming), reference)


if __name__ == '__main__':
    unittest.main()
