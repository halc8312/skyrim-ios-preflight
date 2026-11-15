"""Actual pinned state-cache indexing/registration/worker iteration.

Uses real shader/tuple keys and their equality/hash functions. Cache records and
GPU pipelines are authored boundaries; this is not an Apple/compiler benchmark.
"""
import hashlib
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'vendor/skydream/third_party/dxvk'
spec = importlib.util.spec_from_file_location('state_stage', ROOT/'ios/dependencies/prepare_dxvk.py')
adapter = importlib.util.module_from_spec(spec); spec.loader.exec_module(adapter)


def section(text, first, last):
    a = text.index(first)
    return text[a:text.index(last, a)]


def fixture(patched):
    name = 'src/dxvk/dxvk_state_cache.cpp'
    old = (SOURCE/name).read_text()
    assert hashlib.sha256(old.encode()).hexdigest() == adapter.EXPECTED[name]
    text = adapter.adapt_workers(old) if patched else old
    header = (SOURCE/'src/dxvk/dxvk_state_cache_types.h').read_text()
    key = section(header, '  struct DxvkStateCacheKey {', '\n  };') + '\n  };'
    methods = section(text, '  bool DxvkStateCacheKey::eq(', '  DxvkStateCache::DxvkStateCache(')
    methods += section(text, '  void DxvkStateCache::registerShader(', '  void DxvkStateCache::stopWorkerThreads(')
    methods += section(text, '  DxvkShaderKey DxvkStateCache::getShaderKey(', '  bool DxvkStateCache::readCacheFile(')
    loading = section(text, '        size_t entryId = m_entries.size();', '\n      } else if (ifile)')
    authored = (ROOT/'tests/native/test_state_cache_work.cpp.in').read_text()
    for marker, content in [('/* ACTUAL_KEY */', key), ('/* ACTUAL_METHODS */', methods), ('/* ACTUAL_LOADING */', loading)]:
        assert authored.count(marker) == 1
        authored = authored.replace(marker, content)
    return authored


def compile_fixture(folder, patched):
    path = folder/'fixture.cpp'; path.write_text(fixture(patched))
    flags = ['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if os.getenv('SKYRIM_TEST_SANITIZERS') == '1' else []
    cmd = ['c++','-std=c++17','-O2','-g','-pthread','-DDXVK_NATIVE',f'-DPATCHED={int(patched)}',
           '-Wall','-Wextra','-Werror','-Wno-deprecated-declarations','-Wno-unused-parameter',*flags,
           '-I',str(SOURCE/'src'),'-I',str(SOURCE/'include'),'-I',str(SOURCE/'include/native'),
           '-I',str(SOURCE/'include/native/windows'),'-I',str(SOURCE/'include/native/directx'),
           '-x','c++',str(path),str(SOURCE/'src/dxvk/dxvk_shader_key.cpp'),
           str(SOURCE/'src/util/sha1/sha1_util.cpp'),str(SOURCE/'src/util/sha1/sha1.c'),'-o',str(folder/'fixture')]
    r = subprocess.run(cmd,capture_output=True,text=True,timeout=60)
    if r.returncode: raise AssertionError(r.stderr[-7000:])
    return folder/'fixture'


@unittest.skipUnless(os.name == 'posix' and shutil.which('c++'),'POSIX C++ fixture compiler required')
class StateCacheWorkTests(unittest.TestCase):
    def test_all_states_survive_once_per_tuple_wakeup_and_concurrent_registration(self):
        outputs = []
        for patched in (False, True):
            with tempfile.TemporaryDirectory(prefix='state-cache-work-') as tmp:
                binary = compile_fixture(Path(tmp), patched)
                r = subprocess.run([str(binary)],text=True,capture_output=True,timeout=25)
                self.assertEqual(r.returncode,0,r.stderr[-7000:])
                self.assertIn('all_states_and_failure_identity=PASS',r.stdout)
                outputs.append(r.stdout)
        print('\n'.join(outputs))

    def test_binary_reader_writer_and_pipeline_compilers_remain_unchanged(self):
        old = (SOURCE/'src/dxvk/dxvk_state_cache.cpp').read_text()
        new = adapter.adapt_workers(old)
        stop = '    m_pipeManager->iosPrefetchGate().stop();\n'
        self.assertEqual(new.count(stop), 1)
        self.assertLess(new.index(stop), new.index('if (m_stopThreads.exchange(true))'))
        new = new.replace(stop, '')
        for first,last in [('  bool DxvkStateCache::readCacheHeader(', '  void DxvkStateCache::workerFunc('),
                           ('  void DxvkStateCache::registerShader(', '  bool DxvkStateCache::readCacheFile(')]:
            self.assertEqual(section(old,first,last),section(new,first,last))
        constructor = section(new,'  DxvkStateCache::DxvkStateCache(', '  DxvkStateCache::~DxvkStateCache(')
        self.assertLess(constructor.index('readCacheFile()'),constructor.index('m_workerThreads.emplace_back'))


if __name__ == '__main__': unittest.main()
