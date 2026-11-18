"""Compatibility rejects unreviewed inputs; no driver or game data required."""
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('cache_namespace', ROOT/'ios/dependencies/pipeline_cache_namespace.py')
namespace = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(namespace)


class NamespaceTests(unittest.TestCase):
    def prepare(self, root):
        for name in (*namespace.INPUTS, namespace.CONTRACT, 'ios/dependencies/pipeline_cache_namespace.py'):
            path = root/name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes((ROOT/name).read_bytes())

    def identity(self, root, compiler='clang-test', sdk='/test/iphoneos.sdk', target='18.0'):
        return namespace.identities(root, compiler, sdk, target)

    def test_unknown_observation_falls_back_until_explicit_review(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); self.prepare(root)
            before = self.identity(root)
            self.assertEqual(before['reviewed'], 1)
            name = 'ios/runtime-port/performance_metrics.h'
            path = root/name
            path.write_bytes(path.read_bytes()+b'\n/* Authored documentation-only change. */\n')
            unknown = self.identity(root)
            self.assertEqual(unknown['reviewed'], 0)
            self.assertEqual(unknown['strict'], unknown['reuse'])
            self.assertNotEqual(unknown['reuse'], before['reuse'])
            contract = json.loads((root/namespace.CONTRACT).read_text())
            contract['observations'][name].append(namespace.source_digest(path.read_bytes()))
            (root/namespace.CONTRACT).write_text(json.dumps(contract))
            reviewed = self.identity(root)
            self.assertEqual(reviewed['reviewed'], 1)
            self.assertEqual(reviewed['reuse'], before['reuse'])
            self.assertNotEqual(reviewed['strict'], before['strict'])

    def test_shader_and_toolchain_changes_always_invalidate(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); self.prepare(root)
            before = self.identity(root)
            for kwargs in ({'compiler': 'new-clang'}, {'sdk': '/new/iphoneos.sdk'}, {'target': '26.0'}):
                self.assertNotEqual(self.identity(root, **kwargs)['reuse'], before['reuse'])
            path = root/'ios/dependencies/patch_moltenvk.py'
            path.write_bytes(path.read_bytes()+b'\n# Authored change requiring a new cache.\n')
            self.assertNotEqual(self.identity(root)['reuse'], before['reuse'])

    def test_graphics_coordination_sources_are_part_of_both_identities(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); self.prepare(root)
            for name in ('graphics_flights.h', 'graphics_policy.h', 'graphics_state_methods.cpp.in'):
                path = root/'ios/dependencies'/name
                before = self.identity(root)
                path.write_bytes(path.read_bytes() + b'\n/* Authored coordination change. */\n')
                after = self.identity(root)
                self.assertNotEqual(before['strict'], after['strict'])
                self.assertNotEqual(before['reuse'], after['reuse'])

    def test_malformed_contract_and_missing_sources_cannot_reuse(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); self.prepare(root)
            for content in ('{}', '[]', 'null', '{"schema":2}', 'bad json'):
                (root/namespace.CONTRACT).write_text(content)
                result = self.identity(root)
                self.assertEqual(result['reviewed'], 0)
                self.assertEqual(result['reuse'], result['strict'])
            contract = json.loads((ROOT/namespace.CONTRACT).read_text())
            for schema in (True, 1.0):
                contract['schema'] = schema
                (root/namespace.CONTRACT).write_text(json.dumps(contract))
                self.assertEqual(self.identity(root)['reviewed'], 0)
            (root/'source-lock.json').unlink()
            with self.assertRaises(FileNotFoundError):
                self.identity(root)

    def test_newline_conversion_does_not_change_identity(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); self.prepare(root)
            before = self.identity(root)
            for name in namespace.INPUTS:
                path = root/name
                path.write_bytes(path.read_bytes().replace(b'\r\n', b'\n').replace(b'\n', b'\r\n'))
            self.assertEqual(self.identity(root), before)

    @unittest.skipUnless(shutil.which('cmake'), 'CMake is required for configure-only identity wiring')
    def test_actual_cmake_identity_block_exports_both_namespaces(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)/'identity.cmake'
            source = (ROOT/'ios/dependencies/CMakeLists.txt').read_text()
            begin = source.index('execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/pipeline_cache_namespace.py"')
            end = source.index('\nenable_language(OBJC)', begin)
            import sys
            path.write_text('\n'.join((
                f'set(_root "{ROOT.as_posix()}")',
                f'set(Python3_EXECUTABLE "{Path(sys.executable).as_posix()}")',
                'set(CMAKE_CXX_COMPILER_VERSION "test-clang")',
                'set(_cache_compiler_build "test compiler build")',
                'set(_cache_sdk_build "test SDK build")',
                'set(CMAKE_OSX_SYSROOT "test sdk with spaces")',
                'set(CMAKE_OSX_DEPLOYMENT_TARGET "18.0")',
                'function(target_compile_definitions target)',
                '  if(NOT target STREQUAL "dxvk_d3d9")',
                '    message(FATAL_ERROR "wrong target")',
                '  endif()',
                '  message("namespace_definitions=${ARGN}")',
                'endfunction()',
                'function(set_property)',
                '  message("configure_dependency=${ARGV4}")',
                'endfunction()',
                source[begin:end].replace('${CMAKE_CURRENT_LIST_DIR}', (ROOT/'ios/dependencies').as_posix()),
            )))
            result = subprocess.run(['cmake', '-P', str(path)], capture_output=True, text=True, check=True, timeout=15)
            identities = self.identity(ROOT, compiler='test-clang;test compiler build', sdk='test sdk with spaces;test SDK build')
            for value in (identities['strict'], identities['reuse'], 'SKYRIM_PIPELINE_CACHE_REUSE_REVIEWED=1'):
                self.assertIn(value, result.stdout+result.stderr)
            for name in identities['inputs']:
                self.assertIn('configure_dependency='+ROOT.as_posix()+'/'+name, result.stdout+result.stderr)


if __name__ == '__main__':
    unittest.main()
