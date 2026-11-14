"""Actual cache methods with authored conversion/Metal-latency boundaries."""
import hashlib
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
PINS={"MVKPipeline.mm":"4d8d6c9bbe639d8fa78a71802d5357f91ef15568964dd8e0d570ce4ba05790d2",
      "MVKShaderModule.mm":"f455a0f4f364fad8071f237e54b810ba47af2f8c3a7da4acb31e10619e8722e1"}
VECTOR_PINS={"MVKSmallVector.h":"5a5885e5d0fb6dbe0c0dbab86c500248920dee1d3a7d31e92712c3cda621aee4",
             "MVKSmallVectorAllocator.h":"17df316ff83973d0ea76dd34758e831a2c8ff78add14f3bd4ffe3251fed49889"}
spec=importlib.util.spec_from_file_location('hot_patch',ROOT/'ios/dependencies/patch_mvk_hot_cache.py')
adapter=importlib.util.module_from_spec(spec);spec.loader.exec_module(adapter)

class MoltenHotCacheTests(unittest.TestCase):
    def test_actual_methods_keep_one_compiler_release_hits_and_publish_atomic_metadata(self):
        source=os.environ.get("SKYRIM_MVK_GPU_SOURCE")
        cxx=shutil.which(os.environ.get("CXX","c++"))
        if not(source and cxx):self.skipTest("Pinned MoltenVK sources and C++ compiler required")
        sources={}
        for name,digest in PINS.items():
            data=(Path(source)/name).read_bytes()
            self.assertEqual(hashlib.sha256(data).hexdigest(),digest)
            sources[name]=data.decode()
        pipeline=sources["MVKPipeline.mm"]
        start=pipeline.index('MVKShaderLibrary* MVKPipelineCache::getShaderLibrary(')
        impl=pipeline.index('MVKShaderLibrary* MVKPipelineCache::getShaderLibraryImpl(',start)
        end=pipeline.index('#pragma mark Streaming pipeline',impl)
        dirty=pipeline[pipeline.index('void MVKPipelineCache::markDirty()'):pipeline.index('VkResult MVKPipelineCache::mergePipelineCaches(')]
        shader=sources["MVKShaderModule.mm"]
        a=shader.index('MVKShaderLibrary* MVKShaderLibraryCache::getShaderLibrary(')
        b=shader.index('// Adds and returns a new shader library configured from contents',a)
        methods=shader[a:b]
        replacement=adapter.template().decode()
        for name in adapter.PINS:
            original=(Path(source)/name).read_bytes()
            changed=adapter.patched(name,original)
            self.assertEqual(hashlib.sha256(changed).hexdigest(),adapter.PATCHED_PINS[name])
            with self.assertRaises(ValueError):adapter.patched(name,original+b'\n')
            staged=os.environ.get('SKYRIM_MVK_PATCHED_GPU_SOURCE')
            if staged:self.assertEqual((Path(staged)/name).read_bytes(),changed)
        # Verify the final whole-file patch inserts exactly the function tested
        # below and keeps getShaderLibraryImpl / all serialization methods.
        patched=adapter.patched('MVKPipeline.mm',pipeline.encode()).decode()
        self.assertIn(replacement,patched)
        self.assertIn(pipeline[impl:end],patched)
        with tempfile.TemporaryDirectory(prefix='mvk-hot-cache-') as tmp:
            # Exercise the actual pinned cache container and its allocator too.
            # Only Foundation's unused dependent view type needs a declaration;
            # this fixture never calls contents() or represents the Apple APIs.
            utility=Path(os.environ.get('SKYRIM_MVK_UTILITY_SOURCE',str(Path(source).parent/'Utility')))
            for name,digest in VECTOR_PINS.items():
                data=(utility/name).read_bytes()
                self.assertEqual(hashlib.sha256(data).hexdigest(),digest)
                (Path(tmp)/name).write_bytes(data)
            (Path(tmp)/'MVKFoundation.h').write_text('#pragma once\ntemplate<class T> class MVKArrayRef;\n')
            for mode in (0,1):
                unit=Path(tmp)/f'method-{mode}.cpp'
                unit.write_text('#include "mvk_hot_cache_fixture.h"\n'+
                    (replacement if mode else pipeline[start:impl])+pipeline[impl:end]+dirty+methods)
                binary=Path(tmp)/f'test-{mode}'
                flags=['-O2','-Wall','-Wextra','-Werror','-Wno-unknown-pragmas']
                if os.name!='nt':flags+=['-pthread']
                if os.environ.get('SKYRIM_TEST_SANITIZERS')=='1':flags+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
                subprocess.run([cxx,'-std=c++17',*flags,f'-DFIXTURE_NEW={mode}',str(unit),
                    str(ROOT/'tests/native/test_mvk_hot_cache.cpp'),'-I',str(ROOT/'tests/native'),'-isystem',tmp,'-o',str(binary)],check=True,timeout=60)
                result=subprocess.run([str(binary)],text=True,capture_output=True,timeout=20)
                self.assertEqual(result.returncode,0,result.stdout+'\n'+result.stderr)
                print(result.stdout.strip())
                self.assertIn(f'actual_cache_method=PASS hot_during_cold={mode} snapshot_during_cold={mode} max_cold=1 duplicate_compiles=0',result.stdout)

if __name__=='__main__':unittest.main()
