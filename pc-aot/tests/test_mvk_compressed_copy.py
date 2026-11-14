"""Run actual compressed-data constructors with an authored compiler boundary."""
import hashlib
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('compressed_patch',ROOT/'ios/dependencies/patch_mvk_compressed_copy.py')
adapter=importlib.util.module_from_spec(spec);spec.loader.exec_module(adapter)
CODEC_SHA='dfe81dba1bb352d81b78eaa38abeb567793ecfb79c891a551d955f6f65bb4a98'

def fixture(source,codec,new):
    shader=(source/'MVKShaderModule.mm').read_bytes()
    header=(source/'MVKShaderModule.h').read_bytes()
    assert hashlib.sha256(shader).hexdigest()==adapter.PINS['MVKShaderModule.mm']
    assert hashlib.sha256(header).hexdigest()==adapter.PINS['MVKShaderModule.h']
    codec=codec.read_bytes();assert hashlib.sha256(codec).hexdigest()==CODEC_SHA
    text=codec.decode();a=text.index('template <class C>');b=text.index('#pragma mark -',a)
    compressor=text[a:b]
    if new:shader=adapter.patched('MVKShaderModule.mm',shader);header=adapter.patched('MVKShaderModule.h',header)
    text=shader.decode()
    a=text.index('MVKShaderLibrary::MVKShaderLibrary(MVKVulkanAPIDeviceObject* owner,',text.index('void MVKShaderLibrary::compressMSL'))
    a=text.index('MVKShaderLibrary::MVKShaderLibrary(MVKVulkanAPIDeviceObject* owner,',a+1)
    b=text.index('void MVKShaderLibrary::compileLibrary(',a)
    constructor=text[a:b]
    a=text.index('// Adds and returns a new shader library configured from contents')
    a=text.index('MVKShaderLibrary* MVKShaderLibraryCache::addShaderLibrary(',a)
    b=text.index('// Merge another shader library cache',a)
    cache=text[a:b]
    declaration='const MVKCompressor<std::string>'+('&' if new else '')+' compressedMSL'
    # Header and implementation must select the same two internal signatures.
    assert header.count(declaration.encode())==2
    scaffold='''#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <utility>
#include <algorithm>
using namespace std;
using MVKConfigCompressionAlgorithm=int;
const int MVK_CONFIG_COMPRESSION_ALGORITHM_NONE=0;
static size_t mvkCompress(const uint8_t* a,size_t n,uint8_t* b,size_t z,int){if(n&&z)memcpy(b,a,min(n,z));return min(n,z);}
static size_t mvkDecompress(const uint8_t* a,size_t n,uint8_t* b,size_t z,int){if(n&&z)memcpy(b,a,min(n,z));return min(n,z);}
'''+compressor+'''
struct MVKVulkanAPIDeviceObject{void* getDevice(){return this;}};
struct MVKBaseDeviceObject{explicit MVKBaseDeviceObject(void*){}};
struct SPIRVToMSLConversionResultInfo{int tag;};
struct SPIRVToMSLConversionConfiguration{int tag;};
struct MVKShaderMacroValue{uint64_t value;};
class MVKShaderLibrary:public MVKBaseDeviceObject{
public:
 MVKVulkanAPIDeviceObject* _owner;
 bool _maySpecializeWithMacro;
 SPIRVToMSLConversionResultInfo _shaderConversionResultInfo;
 MVKCompressor<string> _compressedMSL;
 string observed;size_t macroCount=0;
 MVKShaderLibrary(MVKVulkanAPIDeviceObject* owner,const SPIRVToMSLConversionResultInfo& resultInfo,ARG,const vector<pair<uint32_t,MVKShaderMacroValue>>* specializationMacroDef=nullptr);
 void decompressMSL(string& output){_compressedMSL.decompress(output);}
 void compileLibrary(const string& text,const vector<pair<uint32_t,MVKShaderMacroValue>>* macros){observed=text;macroCount=macros?macros->size():0;}
};
class MVKShaderLibraryCache{
public:
 MVKVulkanAPIDeviceObject* _owner;
 vector<pair<SPIRVToMSLConversionConfiguration,MVKShaderLibrary*>> _shaderLibraries;
 explicit MVKShaderLibraryCache(MVKVulkanAPIDeviceObject* owner):_owner(owner){}
 ~MVKShaderLibraryCache(){for(auto& entry:_shaderLibraries)delete entry.second;}
 MVKShaderLibrary* addShaderLibrary(const SPIRVToMSLConversionConfiguration* pShaderConfig,const SPIRVToMSLConversionResultInfo& resultInfo,ARG);
};
'''.replace('ARG',declaration)
    return scaffold,constructor+cache

class MoltenCompressedCopyTests(unittest.TestCase):
    def test_actual_metadata_constructor_keeps_owned_result_and_function_lifetime(self):
        source=os.environ.get('SKYRIM_MVK_GPU_SOURCE');converter=os.environ.get('SKYRIM_MVK_CONVERTER_SOURCE')
        cxx=shutil.which(os.environ.get('CXX','c++'))
        if not(source and converter and cxx):self.skipTest('Pinned shader/converter headers and C++ compiler required')
        source=Path(source)
        raw=(Path(converter)/'SPIRVToMSLConverter.h').read_bytes()
        self.assertEqual(hashlib.sha256(raw).hexdigest(),'a36806cf4caf7e3d72ffc04885f58fa6a07af07a0f37416e7ceb1cc63a02b6be')
        text=raw.decode();a=text.index('typedef struct SPIRVWorkgroupSizeDimension');b=text.index('/** The results of a SPIRV to MSL conversion. */',a)
        types=text[a:b]
        with tempfile.TemporaryDirectory(prefix='mvk-metadata-copy-') as tmp:
            tmp=Path(tmp)
            for mode in (0,1):
                raw=(source/'MVKShaderModule.mm').read_bytes()
                self.assertEqual(hashlib.sha256(raw).hexdigest(),adapter.PINS['MVKShaderModule.mm'])
                if mode:raw=adapter.patched('MVKShaderModule.mm',raw)
                text=raw.decode();a=text.index('MVKMTLFunction::MVKMTLFunction(id<MTLFunction>');b=text.index('MVKMTLFunction::MVKMTLFunction(const MVKMTLFunction&',a)
                body=text[a:b].replace('id<MTLFunction>','FixtureFunction*').replace('[mtlFunc retain]','retainFunction(mtlFunc)')
                header=(source/'MVKShaderModule.h').read_bytes()
                if mode:header=adapter.patched('MVKShaderModule.h',header)
                declaration=next(line.strip() for line in header.decode().splitlines() if 'MVKMTLFunction(id<MTLFunction>' in line).replace('id<MTLFunction>','FixtureFunction*')
                scaffold='''#pragma once
#include <string>
#include <map>
#include <cstdint>
#include <cstddef>
namespace mvk {'''+types+'''}
using namespace mvk;
struct FixtureFunction{unsigned references=1;};
inline FixtureFunction* retainFunction(FixtureFunction* f){if(f)++f->references;return f;}
struct MTLSize{size_t width,height,depth;};
struct MVKMTLFunction {
 SPIRVToMSLConversionResultInfo shaderConversionResults;
 MTLSize threadGroupSize;
 FixtureFunction* _mtlFunction=nullptr;
 '''+declaration+'''
 ~MVKMTLFunction(){if(_mtlFunction)--_mtlFunction->references;}
};
'''
                (tmp/'metadata.h').write_text(scaffold)
                (tmp/'body.cpp').write_text('#include "metadata.h"\n'+body)
                flags=['-O2','-Wall','-Wextra','-Werror']
                if os.environ.get('SKYRIM_TEST_SANITIZERS')=='1':flags+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
                output=tmp/f'metadata-{mode}'
                subprocess.run([cxx,'-std=c++17',*flags,f'-DFIXTURE_NEW={mode}','-I',str(tmp),str(tmp/'body.cpp'),str(ROOT/'tests/native/test_mvk_metadata_copy.cpp'),'-o',str(output)],check=True,timeout=60)
                result=subprocess.run([str(output)],text=True,capture_output=True,timeout=20)
                self.assertEqual(result.returncode,0,result.stdout+'\n'+result.stderr)
                print(result.stdout.strip());self.assertIn('actual_metadata=PASS cases=16',result.stdout)

    def test_actual_compressor_and_constructor_keep_ownership_without_argument_copies(self):
        source=os.environ.get('SKYRIM_MVK_GPU_SOURCE');codec=os.environ.get('SKYRIM_MVK_CODEC_SOURCE')
        cxx=shutil.which(os.environ.get('CXX','c++'))
        if not(source and codec and cxx):self.skipTest('Pinned MoltenVK source and C++ compiler required')
        source=Path(source);codec=Path(codec)
        with tempfile.TemporaryDirectory(prefix='mvk-compressed-copy-') as tmp:
            tmp=Path(tmp)
            for mode in (0,1):
                header,body=fixture(source,codec,mode)
                (tmp/'fixture_generated.h').write_text(header)
                (tmp/'body.cpp').write_text('#include "fixture_generated.h"\n'+body)
                flags=['-O2','-Wall','-Wextra','-Werror','-Wno-unused-function']
                if os.environ.get('SKYRIM_TEST_SANITIZERS')=='1':flags+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
                output=tmp/f'copy-{mode}'
                subprocess.run([cxx,'-std=c++17',*flags,f'-DFIXTURE_NEW={mode}','-I',str(tmp),str(tmp/'body.cpp'),str(ROOT/'tests/native/test_mvk_compressed_copy.cpp'),'-o',str(output)],check=True,timeout=60)
                result=subprocess.run([str(output)],text=True,capture_output=True,timeout=20)
                self.assertEqual(result.returncode,0,result.stdout+'\n'+result.stderr)
                print(result.stdout.strip());self.assertIn('actual_constructor=PASS cases=24',result.stdout)
        for name in adapter.PINS:
            raw=(source/name).read_bytes();patched=adapter.patched(name,raw);adapter.verify_patched(name,patched)
            with self.assertRaises(ValueError):adapter.verify_patched(name,patched+b'\n')
            staged=os.environ.get('SKYRIM_MVK_PATCHED_GPU_SOURCE')
            if staged:
                # The final stage composes copy reductions with variant ownership.
                spec=importlib.util.spec_from_file_location('library_stage',ROOT/'ios/dependencies/patch_mvk_library.py')
                library=importlib.util.module_from_spec(spec);spec.loader.exec_module(library)
                library.verify(name,(Path(staged)/name).read_bytes(),raw)

if __name__=='__main__':unittest.main()
