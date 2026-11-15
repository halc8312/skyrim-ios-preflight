"""Codec-profile regression using authored audio only; no game assets."""
import ctypes
import importlib.util
import os
from pathlib import Path
import shlex
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


adapter = module('voice_profile_adapter', ROOT / 'ios/runtime-port/prepare_runtime.py')


def silent_variable_block_wave(explicit_options=False):
    # Authored WMAv2 packet: 3 silent frames, 4 blocks/frame at 512 samples.
    # Header: index=0, stored frame count=4, reservoir offset=0. First block
    # has previous/current/next length codes; later blocks only the next code.
    # This exercises the two-bit variable-length fields that generic 0x1f
    # misreads as three-bit fields. See FFmpeg n7.1.1 wmadec.c/wma.c.
    bits = '0000' + '0100' + '0' * 12 + '1010100' + '100' * 11
    packet = int(bits + '0' * (-len(bits) % 8), 2).to_bytes((len(bits) + 7) // 8, 'big')
    packet = packet.ljust(1280, b'\0')
    extra = struct.pack('<IH', 0, 0x17) if explicit_options else b''
    fmt = struct.pack('<HHIIHHH', 0x161, 1, 32000, 2500, 1280, 16, len(extra)) + extra
    body = b'WAVE' + b'fmt ' + struct.pack('<I', len(fmt)) + fmt
    body += b'data' + struct.pack('<I', len(packet)) + packet
    return b'RIFF' + struct.pack('<I', len(body)) + body


def load_decoder(path):
    lib = ctypes.CDLL(str(path))
    lib.xwma_create.argtypes = [ctypes.c_uint, *([ctypes.c_int] * 5), ctypes.c_void_p, ctypes.c_size_t]
    lib.xwma_create.restype = ctypes.c_void_p
    lib.xwma_decode.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint,
                               ctypes.c_int, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_uint32)]
    lib.xwma_destroy.argtypes = [ctypes.c_void_p]
    return lib


@unittest.skipUnless(os.name == 'posix' and shutil.which('cc'), 'Existing POSIX C compiler required')
class XwmaVoiceProfileTests(unittest.TestCase):
    def test_actual_setup_and_reset_limit_the_change_to_the_missing_profile(self):
        source = adapter.prepare(ROOT / 'vendor/skydream')['xwma.c']
        body = source[source.index('struct XWmaDecoder {'):source.index('static int receive_pcm(')]
        template = (ROOT / 'tests/native/test_xwma_voice_profile.c').read_text(encoding='utf-8')
        with tempfile.TemporaryDirectory(prefix='xwma-profile-') as tmp:
            src, binary = Path(tmp) / 'test.c', Path(tmp) / 'test'
            src.write_text(template.replace('/* ACTUAL_SETUP_RESET */', body), encoding='utf-8')
            flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if os.getenv('SKYRIM_TEST_SANITIZERS') == '1' else []
            built = subprocess.run([os.getenv('CC', 'cc'), '-std=c11', '-O2', '-g', '-Wall', '-Wextra',
                                    '-Werror', *flags, str(src), '-o', str(binary)], capture_output=True, text=True, timeout=60)
            self.assertEqual(built.returncode, 0, built.stderr[-8000:])
            ran = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            self.assertEqual(ran.returncode, 0, ran.stderr[-8000:])

    @unittest.skipUnless(os.getenv('SKYRIM_FFMPEG_TEST_CFLAGS') and shutil.which('ffmpeg'),
                         'Optional host FFmpeg development headers/libraries not configured')
    def test_authored_two_bit_blocks_decode_and_reuse_like_explicit_reference(self):
        upstream = module('voice_profile_upstream_test', ROOT / 'vendor/skydream/tools/test_xwma.py')
        old = (ROOT / 'vendor/skydream/runtime/xwma.c').read_text(encoding='utf-8')
        texts = (old, adapter.prepare(ROOT / 'vendor/skydream')['xwma.c'])
        with tempfile.TemporaryDirectory(prefix='xwma-profile-real-') as tmp:
            work = Path(tmp)
            libs = []
            for index, text in enumerate(texts):
                src, binary = work / f'decoder{index}.c', work / f'decoder{index}.so'
                src.write_text(text, encoding='utf-8')
                command = [os.getenv('CC', 'cc'), '-O2', '-shared', '-fPIC', '-DLB_HAVE_FFMPEG',
                           '-I', str(ROOT / 'vendor/skydream/runtime'),
                           *shlex.split(os.environ['SKYRIM_FFMPEG_TEST_CFLAGS']), str(src),
                           '-o', str(binary), *shlex.split(os.environ['SKYRIM_FFMPEG_TEST_LIBS'])]
                built = subprocess.run(command, capture_output=True, text=True, timeout=60)
                self.assertEqual(built.returncode, 0, built.stderr[-8000:])
                libs.append(load_decoder(binary))
            with self.assertRaisesRegex(AssertionError, 'decode failed'):
                upstream.decode_wave(libs[0], silent_variable_block_wave())
            reference = subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-i', 'pipe:0',
                                        '-f', 's16le', '-'], input=silent_variable_block_wave(True),
                                       capture_output=True, timeout=30)
            self.assertEqual(reference.returncode, 0, reference.stderr)
            self.assertEqual(reference.stderr, b'')
            self.assertEqual(reference.stdout, bytes(8192))
            for streaming in (False, True):
                self.assertEqual(upstream.decode_wave(libs[1], silent_variable_block_wave(), streaming), reference.stdout)
            # Explicit metadata and unchanged normal paths retain their own
            # interpretation. A nonzero authored tone also checks PCM quality.
            # FFmpeg's encoder does not implement the 20 kbps xWMA profile;
            # the authored variable-block packet above covers that profile.
            for channels, rate, bitrate in ((1, 44100, '32k'), (2, 44100, '48k')):
                with self.subTest(channels=channels, rate=rate, bitrate=bitrate):
                    wav = work / 'tone.wav'
                    subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y', '-f', 'lavfi',
                                    '-i', 'sine=frequency=617:duration=1.7', '-ar', str(rate), '-ac', str(channels),
                                    '-c:a', 'wmav2', '-b:a', bitrate, '-f', 'wav', str(wav)], check=True, capture_output=True, timeout=30)
                    pcm = subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-i', str(wav),
                                          '-f', 's16le', '-'], check=True, capture_output=True, timeout=30).stdout
                    self.assertTrue(any(pcm))
                    for lib in libs:
                        for streaming in (False, True):
                            self.assertEqual(upstream.decode_wave(lib, wav.read_bytes(), streaming), pcm)


if __name__ == '__main__':
    unittest.main()
