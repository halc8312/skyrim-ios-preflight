"""Actual compiler control flow, with authored dispatch and Metal completions."""
import hashlib
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('metal_timing', ROOT/'ios/dependencies/patch_mvk_compile_timing.py')
adapter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(adapter)


class MetalCompileTimingTests(unittest.TestCase):
    def test_real_wait_timeout_and_late_callback_keep_original_control_flow(self):
        source = os.environ.get('SKYRIM_MVK_GPU_SOURCE')
        cxx = shutil.which(os.environ.get('CXX', 'c++'))
        if not source or not cxx:
            self.skipTest('Pinned MoltenVK sources and C++ compiler required')
        raw = (Path(source)/'MVKSync.mm').read_bytes()
        self.assertEqual(hashlib.sha256(raw).hexdigest(), adapter.PIN)
        changed = adapter.patched(raw)
        self.assertEqual(adapter.original(changed), raw)
        with self.assertRaises(ValueError):
            adapter.original(changed+b'\n')
        staged = os.environ.get('SKYRIM_MVK_PATCHED_GPU_SOURCE')
        if staged:
            self.assertEqual((Path(staged)/'MVKSync.mm').read_bytes(), changed)
            self.assertEqual((Path(staged)/'skyrim_performance_metrics.h').read_bytes(),
                             (ROOT/'ios/runtime-port/performance_metrics.h').read_bytes())
        fixture = (ROOT/'tests/native/mvk_compile_timing_fixture.cpp.in').read_text()
        with tempfile.TemporaryDirectory(prefix='metal-compile-timing-') as tmp:
            tmp = Path(tmp)
            for mode, data in enumerate((raw, changed)):
                text = data.decode()
                a = text.index('void MVKMetalCompiler::compile(')
                b = text.index('void MVKMetalCompiler::handleError()', a)
                actual = text[a:b]
                # Replace only ObjC syntax and the authored NSError boundary.
                # Scheduling, original lock/wait/predicate/timeout/return remain.
                a = actual.index('\t\t@autoreleasepool {\n\t\t\tNSString* errDesc')
                b = actual.index('\n\t\t}', a)+len('\n\t\t}')
                error_boundary = actual[a:b]
                self.assertIn('_compileError = [[NSError alloc]', error_boundary)
                actual = actual[:a]+'\t\t_compileError = fixture_timeout_error();'+actual[b:]
                actual = actual.replace('^{', '[=]{').replace('@autoreleasepool', '')
                unit = tmp/f'case-{mode}.cpp'
                unit.write_text(fixture.replace('/* ACTUAL_COMPILER_METHOD */', actual))
                binary = tmp/f'case-{mode}'
                cmd = [cxx, '-std=c++17', '-O2', '-pthread', '-Wall', '-Wextra', '-Werror',
                       '-Wno-unused-lambda-capture', f'-DOBSERVED={mode}',
                       '-I', str(ROOT/'ios/runtime-port'), str(unit), '-o', str(binary)]
                if os.environ.get('SKYRIM_TEST_SANITIZERS') == '1':
                    cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
                subprocess.run(cmd, check=True, timeout=60)
                result = subprocess.run([str(binary)], text=True, capture_output=True, timeout=20)
                self.assertEqual(result.returncode, 0, result.stdout+'\n'+result.stderr)
                self.assertIn('actual_compile_wait_and_late_callback=PASS', result.stdout)
                print(result.stdout.strip())
            destination = tmp/'stage'
            destination.mkdir()
            (destination/'MVKSync.mm').write_bytes(raw)
            header = ROOT/'ios/runtime-port/performance_metrics.h'
            adapter.apply(destination, header)
            adapter.apply(destination, header)
            self.assertEqual((destination/'MVKSync.mm').read_bytes(), changed)

    def test_numeric_categories_preserve_old_ids_and_parser_names(self):
        analysis_spec = importlib.util.spec_from_file_location('timing_analysis', ROOT/'scripts/analyze-frame-timing.py')
        analysis = importlib.util.module_from_spec(analysis_spec)
        analysis_spec.loader.exec_module(analysis)
        header = (ROOT/'ios/runtime-port/performance_metrics.h').read_text()
        enums = header.split('enum SkyrimPerfKind {', 1)[1].split('SKYRIM_PERF_KIND_COUNT', 1)[0]
        names = [line.strip().rstrip(',').removeprefix('SKYRIM_PERF_').lower()
                 for line in enums.splitlines() if 'SKYRIM_PERF_' in line]
        self.assertEqual(tuple(names), analysis.KINDS)
        self.assertEqual(names[20], 'submission_wait')
        self.assertEqual(names[21:27], ['mvk_msl_wait', 'mvk_specialize_wait', 'mvk_pipeline_wait',
                                     'mvk_other_wait', 'mvk_dispatch_wait', 'mvk_dispatch_block'])


if __name__ == '__main__':
    unittest.main()
