"""Default-OFF module-cold adapter: actual pinned methods, authored Metal boundary."""
import hashlib
import importlib.util
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("module_cold", ROOT / "ios/dependencies/patch_mvk_module_cold.py")
adapter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(adapter)
VECTOR_PINS = {
    "MVKSmallVector.h": "5a5885e5d0fb6dbe0c0dbab86c500248920dee1d3a7d31e92712c3cda621aee4",
    "MVKSmallVectorAllocator.h": "17df316ff83973d0ea76dd34758e831a2c8ff78add14f3bd4ffe3251fed49889",
}


def region(raw, first, following):
    begin = raw.index(first)
    return raw[begin:raw.index(following, begin)]


def fixture_header(mixin, shader_header):
    raw = (ROOT / "tests/native/mvk_hot_cache_fixture.h").read_text()
    raw = '#include "performance_metrics.h"\n' + raw
    raw = raw.replace('#include <vector>', '#include <vector>\n#include <string>\n#include <cstdlib>\n#include <cstring>\n#include <cstdio>')
    raw = raw.replace('  int key=0;\n  bool matches', '  int key=0;\n  struct {int entryPointStage=0;} options{};\n  bool matches', 1)
    raw = raw.replace('struct SPIRVToMSLConversionResult {int key=0;};', 'struct SPIRVToMSLConversionResult {int key=0;std::string msl,resultLog;};')
    raw = raw.replace('class MVKPipelineCache;', '''using VkResult=int;
constexpr VkResult VK_SUCCESS=0, VK_ERROR_INITIALIZATION_FAILED=-3;
constexpr int VK_OBJECT_TYPE_PIPELINE_CACHE=17;
''' + mixin + '''
class MVKVulkanAPIDeviceObject : public MVKConfigurableMixin {
public: virtual int getVkObjectType(){return 0;} virtual ~MVKVulkanAPIDeviceObject()=default;
};
class MVKPipelineCache;''', 1)
    constructor = region(raw, '  MVKShaderLibrary(MVKPipelineCache*', '  explicit MVKShaderLibrary(int merged)')
    raw = raw.replace(constructor, '  bool valid=true;\n  MVKShaderLibrary(MVKPipelineCache*,const SPIRVToMSLConversionResult&);\n', 1)
    origin = region(shader_header, '\t// Runtime-only producer observations;', '\t/** Returns the Vulkan API opaque object')
    field = '\tSkyrimPerfShaderOrigin _iosShaderOrigin{};'
    assert shader_header.count(field) == 1
    raw = raw.replace('  bool valid=true;', '  bool valid=true;\n'
        '  MVKVulkanAPIDeviceObject* _owner=nullptr;\n  void* _mtlLibrary=nullptr;\n' + field + '\n' + origin, 1)
    module = region(raw, 'struct MVKShaderModule {', 'struct MVKPipeline {')
    raw = raw.replace(module, 'struct MVKShaderModule;\n', 1)
    raw = raw.replace('  FixturePairs _shaderLibraries;', '  std::mutex _moduleColdLock;\n  FixturePairs _shaderLibraries;', 1)
    raw = raw.replace('class MVKPipelineCache {\npublic:', 'class MVKPipelineCache : public MVKVulkanAPIDeviceObject {\npublic:\n'
        '  int getVkObjectType() override{return VK_OBJECT_TYPE_PIPELINE_CACHE;}\n' + adapter.ERROR_METHOD.decode(), 1)
    return raw


class ModuleColdExperimentTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = os.environ.get("SKYRIM_MVK_GPU_SOURCE")
        cls.cxx = shutil.which(os.environ.get("CXX", "c++"))
        if not(source and cls.cxx):
            raise unittest.SkipTest("Pinned MoltenVK source and C++ compiler required")
        cls.source = Path(source)
        cls.originals = {name: (cls.source / name).read_bytes() for name in adapter.NAMES}
        cls.base = adapter.baseline(cls.originals)
        cls.candidate = adapter.patched(cls.originals)

    def test_patch_scope_and_idempotence(self):
        with self.assertRaises(ValueError):
            adapter.apply(self.source, self.source)
        for name in adapter.NAMES:
            broken = dict(self.originals)
            broken[name] += b"\n"
            with self.assertRaises(ValueError):
                adapter.patched(broken)
        # Preserve complete cache read/write/merge/destruction and compiler
        # async/lifetime code, rather than merely looking for selected strings.
        for source in (self.base, self.candidate):
            self.assertEqual(region(source["MVKPipeline.mm"], b"MVKShaderLibrary* MVKPipelineCache::getShaderLibraryImpl(", b"#pragma mark Streaming pipeline"),
                             region(self.originals["MVKPipeline.mm"], b"MVKShaderLibrary* MVKPipelineCache::getShaderLibraryImpl(", b"#pragma mark Streaming pipeline"))
            self.assertEqual(source["MVKPipeline.mm"].split(b"#pragma mark Streaming pipeline", 1)[1],
                             self.originals["MVKPipeline.mm"].split(b"#pragma mark Streaming pipeline", 1)[1])
        # The ready/refusal path before cold-lock acquisition gains neither a
        # clock nor a provenance lookup. The serialized suffix above is exact.
        self.assertEqual(region(self.base["MVKPipeline.mm"], b"MVKShaderLibrary* MVKPipelineCache::getShaderLibrary(", b"    unique_lock<mutex> compileLock("),
                         region(self.candidate["MVKPipeline.mm"], b"MVKShaderLibrary* MVKPipelineCache::getShaderLibrary(", b"    unique_lock<mutex> compileLock("))
        self.assertEqual(region(self.candidate["MVKSync.mm"], b"void MVKMetalCompiler::compile(", b"void MVKMetalCompiler::handleError()"),
                         region(self.base["MVKSync.mm"], b"void MVKMetalCompiler::compile(", b"void MVKMetalCompiler::handleError()"))
        self.assertEqual(self.candidate["MVKSync.mm"].split(b"// Returns whether the compilation came in late",1)[1],
                         self.base["MVKSync.mm"].split(b"// Returns whether the compilation came in late",1)[1])
        with tempfile.TemporaryDirectory(prefix="mvk-module-stage-") as tmp:
            target = Path(tmp)
            for name, data in self.base.items():
                (target/name).write_bytes(data)
            adapter.apply(target, self.source)
            adapter.apply(target, self.source)
            self.assertEqual({name: (target/name).read_bytes() for name in adapter.NAMES}, self.candidate)
            # A later invalid input must reject before any earlier file is edited.
            for name, data in self.base.items():
                (target/name).write_bytes(data)
            (target/"MVKSync.mm").write_bytes(b"unreviewed")
            with self.assertRaises(ValueError):
                adapter.apply(target, self.source)
            self.assertEqual((target/"MVKPipeline.mm").read_bytes(), self.base["MVKPipeline.mm"])

    def test_final_composed_stage_rejects_drift_in_every_file(self):
        with tempfile.TemporaryDirectory(prefix="mvk-composed-verify-") as tmp:
            target = Path(tmp)
            for name, data in self.originals.items():
                (target/name).write_bytes(data)
            adapter.apply(target, self.source)
            adapter.verify(target, self.source)
            for name in (*adapter.NAMES, "skyrim_performance_metrics.h"):
                data = (target/name).read_bytes()
                (target/name).write_bytes(data + b"\n")
                with self.assertRaisesRegex(ValueError, "stage differs"):
                    adapter.verify(target, self.source)
                (target/name).write_bytes(data)
            adapter.apply(target, self.source)
            adapter.verify(target, self.source)
        staged = os.environ.get("SKYRIM_MVK_PATCHED_GPU_SOURCE")
        if staged:
            adapter.verify(Path(staged), self.source)

    def test_actual_methods_concurrency_failure_lifetime(self):
        utility = self.source.parent / "Utility"
        # Extract the actual non-atomic first-error mixin: do not substitute an
        # atomic mock which could hide the shared error-state race.
        base_object = (utility/"MVKBaseObject.h").read_bytes()
        self.assertEqual(hashlib.sha256(base_object).hexdigest(), "da0fabfafc78719f24de512c542bbe13605174bdd0f9115c924c8d117feeeb15")
        mixin = region(base_object.decode(), "class MVKConfigurableMixin {", "#pragma mark -")
        with tempfile.TemporaryDirectory(prefix="mvk-module-cold-") as tmp:
            temp = Path(tmp)
            vendor = temp / "vendor"
            vendor.mkdir()
            for name, digest in VECTOR_PINS.items():
                data=(utility/name).read_bytes()
                self.assertEqual(hashlib.sha256(data).hexdigest(), digest)
                (vendor/name).write_bytes(data)
            (vendor/"MVKFoundation.h").write_text("#pragma once\ntemplate<class T> class MVKArrayRef;\n")
            (temp/"mvk_module_fixture.h").write_text(fixture_header(mixin, self.candidate["MVKShaderModule.h"].decode()))
            flags=["-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unknown-pragmas", "-pthread"]
            sanitizer=os.environ.get("SKYRIM_TEST_SANITIZERS")
            if sanitizer in ("1", "thread"):
                flags += ["-fsanitize=" + ("thread" if sanitizer=="thread" else "address,undefined"), "-fno-omit-frame-pointer"]
            san=[flag for flag in flags if flag.startswith("-fsanitize=") or flag=="-fno-omit-frame-pointer"]
            obj=temp/"metrics.o"
            subprocess.run([os.environ.get("CC","cc"), "-std=c11", "-D_POSIX_C_SOURCE=200809L", *san, "-c",
                str(ROOT/"ios/runtime-port/performance_metrics.c"), "-o", str(obj)], check=True,timeout=45)
            for mode, source in enumerate((self.base,self.candidate)):
                pipeline=source["MVKPipeline.mm"].decode()
                shader=source["MVKShaderModule.mm"].decode()
                sync=source["MVKSync.mm"].decode()
                unit=temp/f"method-{mode}.cpp"
                unit.write_text('#include "performance_metrics.h"\n#include "mvk_module_fixture.h"\n#include "mvk_module_cold_support.h"\n'
                    + (region(pipeline, 'static bool mvkIOSModuleColdEnabled()', 'MVKShaderLibrary* MVKPipelineCache::getShaderLibrary(') if mode else '')
                    + region(pipeline, 'MVKShaderLibrary* MVKPipelineCache::getShaderLibrary(', '#pragma mark Streaming pipeline')
                    + region(pipeline, 'void MVKPipelineCache::markDirty()', 'VkResult MVKPipelineCache::mergePipelineCaches(')
                    + region(shader, 'MVKShaderLibrary* MVKShaderLibraryCache::getShaderLibrary(', '// Adds and returns a new shader library configured from contents')
                    + region(shader, 'bool MVKShaderModule::convert(', 'void MVKShaderModule::setWorkgroupSize(')
                    + region(sync, 'void MVKMetalCompiler::handleError()', '// Returns whether the compilation came in late'))
                binary=temp/f"test-{mode}"
                subprocess.run([self.cxx, '-std=c++17', *flags, f'-DCANDIDATE={mode}',str(unit),
                    str(ROOT/'tests/native/test_mvk_module_cold.cpp'),str(obj),'-I',str(ROOT/'ios/runtime-port'),
                    '-I',str(ROOT/'tests/native'),'-I',str(temp),'-isystem',str(vendor),'-o',str(binary)], check=True,timeout=60)
                command=[str(binary)]
                if sanitizer=='thread' and os.environ.get('SKYRIM_TSAN_DISABLE_ASLR')=='1':
                    setarch=shutil.which('setarch')
                    self.assertIsNotNone(setarch, 'Explicit process-local TSan layout option requires setarch')
                    command=[setarch,platform.machine(),'-R',*command]
                # The candidate executable is identical for OFF and ON runs.
                for value in ((None, "0", "1", "invalid") if mode else (None,)):
                    enabled = int(mode and value == "1")
                    env = dict(os.environ)
                    env.pop("SKYRIM_MVK_MODULE_COLD", None)
                    if value is not None:
                        env["SKYRIM_MVK_MODULE_COLD"] = value
                    completed=subprocess.run([*command, str(enabled)],env=env,text=True,capture_output=True,timeout=30)
                    self.assertEqual(completed.returncode,0,completed.stdout+'\n'+completed.stderr)
                    print(completed.stdout.strip())
                    self.assertIn(f'other_module_progress={enabled}', completed.stdout)
                    if mode:
                        self.assertIn('compile_error_reports=16384',completed.stdout)
                        self.assertEqual(completed.stderr.count("IOS_SHADER_POLICY "), 1)
                        self.assertIn(f"module_cold={enabled} cold_scope={enabled}",completed.stderr)


if __name__ == '__main__':
    unittest.main()
