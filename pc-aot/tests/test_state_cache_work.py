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
    worker_name = 'src/dxvk/dxvk_state_cache.h'
    worker_header = (SOURCE/worker_name).read_text()
    assert hashlib.sha256(worker_header.encode()).hexdigest() == adapter.EXPECTED[worker_name]
    if patched:
        worker_header = adapter.adapt_worker_header(worker_header)
    worker_item = section(worker_header, '    struct WorkerItem {', '\n    };') + '\n    };'
    header = (SOURCE/'src/dxvk/dxvk_state_cache_types.h').read_text()
    key = section(header, '  struct DxvkStateCacheKey {', '\n  };') + '\n  };'
    methods = section(text, '  bool DxvkStateCacheKey::eq(', '  DxvkStateCache::DxvkStateCache(')
    methods += section(text, '  void DxvkStateCache::registerShader(', '  void DxvkStateCache::stopWorkerThreads(')
    methods += section(text, '  DxvkShaderKey DxvkStateCache::getShaderKey(', '  bool DxvkStateCache::readCacheFile(')
    methods += section(text, '  void DxvkStateCache::workerFunc(', '  void DxvkStateCache::writerFunc(')
    loading = section(text, '        size_t entryId = m_entries.size();', '\n      } else if (ifile)')
    authored = (ROOT/'tests/native/test_state_cache_work.cpp.in').read_text()
    for marker, content in [('/* ACTUAL_KEY */', key), ('/* ACTUAL_WORKER_ITEM */', worker_item),
                            ('/* ACTUAL_METHODS */', methods), ('/* ACTUAL_LOADING */', loading)]:
        assert authored.count(marker) == 1
        authored = authored.replace(marker, content)
    return authored


def compile_fixture(folder, patched):
    path = folder/'fixture.cpp'; path.write_text(fixture(patched))
    flags = ['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if os.getenv('SKYRIM_TEST_SANITIZERS') == '1' else []
    metrics = folder/'metrics.o'
    # Preserve real TLS and RAII behavior while the authored clock makes queue
    # delay deterministic; production metrics' own clock keeps a distinct name.
    subprocess.run(['cc','-std=c11','-O2','-D_POSIX_C_SOURCE=200809L',*flags,
                    '-Dskyrim_perf_now_ns=metrics_real_now_ns','-c',
                    str(ROOT/'ios/runtime-port/performance_metrics.c'),'-o',str(metrics)],
                   check=True,timeout=60)
    cmd = ['c++','-std=c++17','-O2','-g','-pthread','-DDXVK_NATIVE',f'-DPATCHED={int(patched)}',
           '-Wall','-Wextra','-Werror','-Wno-deprecated-declarations','-Wno-unused-parameter',*flags,
           '-I',str(SOURCE/'src'),'-I',str(SOURCE/'include'),'-I',str(SOURCE/'include/native'),
           '-I',str(ROOT/'ios/runtime-port'),
           '-I',str(SOURCE/'include/native/windows'),'-I',str(SOURCE/'include/native/directx'),
           '-x','c++',str(path),str(SOURCE/'src/dxvk/dxvk_shader_key.cpp'),
           str(SOURCE/'src/util/sha1/sha1_util.cpp'),str(SOURCE/'src/util/sha1/sha1.c'),
           '-x','none',str(metrics),'-o',str(folder/'fixture')]
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
                           ('  void DxvkStateCache::stopWorkerThreads(', '  bool DxvkStateCache::readCacheFile(')]:
            self.assertEqual(section(old,first,last),section(new,first,last))
        constructor = section(new,'  DxvkStateCache::DxvkStateCache(', '  DxvkStateCache::~DxvkStateCache(')
        self.assertLess(constructor.index('readCacheFile()'),constructor.index('m_workerThreads.emplace_back'))

    def test_prefetch_eligibility_queue_and_worker_scope(self):
        with tempfile.TemporaryDirectory(prefix='state-cache-prefetch-') as tmp:
            binary = compile_fixture(Path(tmp), True)
            r = subprocess.run([str(binary), 'prefetch'], text=True, capture_output=True, timeout=25)
            self.assertEqual(r.returncode, 0, r.stderr[-7000:])
            self.assertIn('prefetch_metadata_actual_worker=PASS cases=7', r.stdout)

    def test_complete_state_cache_translation_unit(self):
        # The same platform include/environment contract as the production
        # graphics/manager check, with the entire state-cache implementation.
        spec = importlib.util.spec_from_file_location('state_cache_syntax_support',
                    ROOT/'tests/test_graphics_state_flights.py')
        support = importlib.util.module_from_spec(spec); spec.loader.exec_module(support)
        with tempfile.TemporaryDirectory(prefix='state-cache-syntax-') as tmp:
            stage = Path(tmp)/'dxvk'
            adapter.stage(SOURCE, stage)
            cmd = [os.getenv('CXX','c++'), '-std=c++17', *support.macos_flags(),
                   '-fsyntax-only','-include','cstdint','-include','cstring',
                   '-Wno-deprecated-declarations','-DDXVK_NATIVE','-DNOMINMAX','-D_WIN32_WINNT=0xa00',
                   *support.includes(stage), str(stage/'src/dxvk/dxvk_state_cache.cpp')]
            r = subprocess.run(cmd, env=support.native_environment(), capture_output=True, text=True, timeout=90)
            self.assertEqual(r.returncode, 0, r.stdout+r.stderr)


if __name__ == '__main__': unittest.main()
