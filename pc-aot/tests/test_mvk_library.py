"""Exercise pinned copy/assignment/destruction and macro lookup with fake Metal."""
import hashlib
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
stage_spec = importlib.util.spec_from_file_location("composed_mvk_stage", ROOT / "ios/dependencies/patch_mvk_module_cold.py")
composed_stage = importlib.util.module_from_spec(stage_spec)
stage_spec.loader.exec_module(composed_stage)
spec = importlib.util.spec_from_file_location('library_patch', ROOT/'ios/dependencies/patch_mvk_library.py')
adapter = importlib.util.module_from_spec(spec); spec.loader.exec_module(adapter)


def between(text, start, stop):
    a = text.index(start)
    return text[a:text.index(stop, a)]


def fixture(source, converter, codec):
    files = {name: (source/name).read_bytes() for name in adapter.PINS}
    text = adapter.patched('MVKShaderModule.mm', files['MVKShaderModule.mm']).decode()
    header = adapter.patched('MVKShaderModule.h', files['MVKShaderModule.h']).decode()
    converter = converter.read_bytes()
    assert hashlib.sha256(converter).hexdigest() == 'a36806cf4caf7e3d72ffc04885f58fa6a07af07a0f37416e7ceb1cc63a02b6be'
    codec = codec.read_bytes()
    assert hashlib.sha256(codec).hexdigest() == 'dfe81dba1bb352d81b78eaa38abeb567793ecfb79c891a551d955f6f65bb4a98'
    types = between(converter.decode(), 'typedef struct SPIRVWorkgroupSizeDimension', '/** The results of a SPIRV to MSL conversion. */')
    compressor = between(codec.decode(), 'template <class C>', '#pragma mark -')
    macro = between(header, 'typedef struct MVKShaderMacroValue', '/**\n * Wraps a single')
    copy = between(text, 'MVKShaderLibrary::MVKShaderLibrary(const MVKShaderLibrary&', '// If err object is nil')
    destroy = between(text, 'MVKShaderLibrary::~MVKShaderLibrary()', '#pragma mark -')
    choose = between(text, '\tif ( !_mtlLibrary )', '\t@synchronized (getMTLDevice())')
    choose = 'FixtureHandle* MVKShaderLibrary::choose(const VkSpecializationInfo* pSpecializationInfo) {\n'+choose+'return lib;\n}\n'
    decompress = between(text, 'void MVKShaderLibrary::decompressMSL(string& msl)', 'MVKShaderLibrary::MVKShaderLibrary(MVKVulkanAPIDeviceObject* owner,')
    body = copy+destroy+choose+decompress
    for old, new in [('id<MTLLibrary>', 'FixtureHandle*'), ('[other._mtlLibrary retain]', 'retain(other._mtlLibrary)'),
                     ('[_mtlLibrary release]', 'release(_mtlLibrary)'), ('MVKMTLFunctionNull', 'nullptr'), ('nil', 'nullptr')]:
        body = body.replace(old, new)
    # Keep the source constructor and owned compressor/result copies unchanged.
    a = text.index('MVKShaderLibrary::MVKShaderLibrary(MVKVulkanAPIDeviceObject* owner,', text.index('void MVKShaderLibrary::compressMSL'))
    a = text.index('MVKShaderLibrary::MVKShaderLibrary(MVKVulkanAPIDeviceObject* owner,', a+1)
    constructor = text[a:text.index('void MVKShaderLibrary::compileLibrary(', a)]
    template = (ROOT/'tests/native/mvk_library_fixture.cpp.in').read_text()
    for anchor in ('@TYPES@', '@CODEC@', '@MACRO@', '@METHODS@'):
        assert template.count(anchor) == 1
    return '#include "performance_metrics.h"\n' + template.replace('@TYPES@', types).replace('@CODEC@', compressor).replace('@MACRO@', macro).replace('@METHODS@', body+constructor)


def metrics_object(tmp, flags):
    obj = tmp/'metrics.o'
    cc = os.environ.get('CC','cc')
    sanitizers = [f for f in flags if f.startswith('-fsanitize=') or f=='-fno-omit-frame-pointer']
    subprocess.run([cc,'-std=c11','-D_POSIX_C_SOURCE=200809L',*sanitizers,'-c',
        str(ROOT/'ios/runtime-port/performance_metrics.c'),'-o',str(obj)],check=True,timeout=45)
    return obj


class MoltenLibraryTests(unittest.TestCase):
    def inputs(self):
        names = ('SKYRIM_MVK_GPU_SOURCE', 'SKYRIM_MVK_CONVERTER_SOURCE', 'SKYRIM_MVK_CODEC_SOURCE')
        values = [os.environ.get(name) for name in names]
        if not all(values): self.skipTest('Pinned GPU/converter/codec sources required')
        return [Path(value) for value in values]

    def test_composed_stage_is_exact_idempotent_and_refuses_drift_before_writing(self):
        source, _, _ = self.inputs()
        with tempfile.TemporaryDirectory(prefix='mvk-library-patch-') as tmp:
            tmp = Path(tmp)
            for name in adapter.PINS: (tmp/name).write_bytes((source/name).read_bytes())
            adapter.apply(tmp, source)
            for name in adapter.PINS:
                adapter.verify(name, (tmp/name).read_bytes(), (source/name).read_bytes())
            adapter.apply(tmp, source)
            # The formerly shipped compressed-only stage remains a valid input.
            for name in adapter.PINS: (tmp/name).write_bytes(adapter.compressed.patched(name, (source/name).read_bytes()))
            adapter.apply(tmp, source)
            header = (tmp/'MVKShaderModule.h').read_bytes()
            (tmp/'MVKShaderModule.mm').write_bytes((tmp/'MVKShaderModule.mm').read_bytes()+b'\n')
            with self.assertRaises(ValueError): adapter.apply(tmp, source)
            self.assertEqual((tmp/'MVKShaderModule.h').read_bytes(), header)
        staged = os.environ.get('SKYRIM_MVK_PATCHED_GPU_SOURCE')
        if staged:
            composed_stage.verify(Path(staged), source)

    def test_real_methods_preserve_independent_ownership_parallel_publication_and_errors(self):
        source, converter, codec = self.inputs()
        cxx = shutil.which(os.environ.get('CXX', 'c++'))
        if not cxx: self.skipTest('C++ compiler required')
        with tempfile.TemporaryDirectory(prefix='mvk-library-fixture-') as tmp:
            tmp = Path(tmp)
            cpp = tmp/'fixture.cpp'; cpp.write_text(fixture(source, converter/'SPIRVToMSLConverter.h', codec))
            flags = ['-std=c++17', '-O2', '-pthread', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function']
            if os.environ.get('SKYRIM_TEST_SANITIZERS') == '1': flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
            subprocess.run([cxx, *flags, str(cpp), str(metrics_object(tmp, flags)), '-I', str(ROOT/'ios/runtime-port'), '-o', str(tmp/'fixture')], check=True, timeout=60)
            result = subprocess.run([str(tmp/'fixture')], capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout+'\n'+result.stderr)
            self.assertIn('ownership=PASS parallel_publication=PASS failure_policy=PASS', result.stdout)
            print(result.stdout.strip())

    def test_variant_payload_reduction_against_previous_constructor(self):
        source, converter, codec = self.inputs()
        cxx = shutil.which(os.environ.get('CXX', 'c++'))
        if not cxx: self.skipTest('C++ compiler required')
        candidate = fixture(source, converter/'SPIRVToMSLConverter.h', codec)
        before = adapter.BORROWED_PAYLOAD.decode().replace('\tcompileLibrary(msl, specializationMacroDef);',
            '\tSkyrimPerfContextScope iosSource(SKYRIM_PERF_CONTEXT_KEEP, specializationMacroDef ? SKYRIM_PERF_SOURCE_MACRO : SKYRIM_PERF_SOURCE_RESTORE);\n\tcompileLibrary(msl, specializationMacroDef);')
        self.assertEqual(candidate.count(before), 1)
        baseline_body = adapter.CONSTRUCTOR_PAYLOAD.decode().replace('\tcompileLibrary(msl, specializationMacroDef);',
            '\tSkyrimPerfContextScope iosSource(SKYRIM_PERF_CONTEXT_KEEP, specializationMacroDef ? SKYRIM_PERF_SOURCE_MACRO : SKYRIM_PERF_SOURCE_RESTORE);\n\tcompileLibrary(msl, specializationMacroDef);')
        baseline = candidate.replace(before, baseline_body)
        with tempfile.TemporaryDirectory(prefix='mvk-payload-comparison-') as tmp:
            tmp = Path(tmp)
            for name, code, retained in [('baseline', baseline, 1), ('candidate', candidate, 0)]:
                cpp = tmp/(name+'.cpp'); cpp.write_text(code)
                flags = ['-std=c++17', '-O2', '-pthread', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                         '-DEXPECT_VARIANT_PAYLOAD_RETAINED='+str(retained)]
                if os.environ.get('SKYRIM_TEST_SANITIZERS') == '1': flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
                subprocess.run([cxx, *flags, str(cpp), str(metrics_object(tmp, flags)), '-I', str(ROOT/'ios/runtime-port'), '-o', str(tmp/name)], check=True, timeout=60)
                result = subprocess.run([str(tmp/name)], capture_output=True, text=True, timeout=20)
                self.assertEqual(result.returncode, 0, result.stdout+'\n'+result.stderr)
                self.assertIn('largest_variant_payload_bytes='+str(retained*16*1024*1024), result.stdout)
                print(name+': '+result.stdout.strip())


if __name__ == '__main__': unittest.main()
