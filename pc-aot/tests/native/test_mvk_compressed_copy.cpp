// Actual pinned MVKCompressor and constructor/cache bodies are supplied by the
// driver. The codec and Metal compiler boundary below are authored test doubles.
#include "fixture_generated.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <chrono>

static thread_local bool count_allocations=false;
static thread_local size_t payload_size=0, payload_allocations=0;
static thread_local bool probe_allocation=false;
static thread_local size_t probe_allocations=0;
void* operator new(size_t size) {
    if(probe_allocation){payload_size=size;++probe_allocations;}
    if(count_allocations && size==payload_size) ++payload_allocations;
    if(void* value=malloc(size ? size : 1))return value;
    throw std::bad_alloc();
}
void operator delete(void* ptr) noexcept {free(ptr);}
void operator delete(void* ptr,size_t) noexcept {free(ptr);}

static MVKCompressor<std::string> source(size_t bytes,unsigned seed) {
    MVKCompressor<std::string> value;
    value._compressed.resize(bytes);value._uncompressedSize=bytes;
    for(size_t i=0;i<bytes;++i)value._compressed[i]=(uint8_t)(seed+i*17);
    return value;
}
static void check(const MVKShaderLibrary& lib,const MVKCompressor<std::string>& original) {
    assert(lib._compressedMSL._compressed==original._compressed);
    assert(lib._compressedMSL._uncompressedSize==original._uncompressedSize);
    assert(lib._compressedMSL._algorithm==original._algorithm);
    assert(lib.observed.size()==original._compressed.size());
    if(!lib.observed.empty())assert(!memcmp(lib.observed.data(),original._compressed.data(),lib.observed.size()));
    assert(lib._shaderConversionResultInfo.tag==19);
}
int main(int argc,char**argv) {
    MVKVulkanAPIDeviceObject owner;
    SPIRVToMSLConversionResultInfo info{19};
    SPIRVToMSLConversionConfiguration config{37};
    std::vector<std::pair<uint32_t,MVKShaderMacroValue>> macros{{3,{42}}};
#ifdef FIXTURE_BENCH
    size_t bytes=argc>1?strtoull(argv[1],nullptr,10):65536;
    auto data=source(bytes,4);uint64_t digest=0;
    auto begin=std::chrono::steady_clock::now();
    for(unsigned n=0;n<3000;++n){
        MVKShaderLibraryCache cache(&owner);
        auto* lib=cache.addShaderLibrary(&config,info,data);
        digest+=lib->observed.size()+lib->observed[bytes/2];
    }
    auto ns=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-begin).count()/3000;
    printf("bytes=%zu ns=%.3f digest=%llu\n",bytes,ns,(unsigned long long)digest);
#else
    (void)argc;(void)argv;
    unsigned cases=0;
    for(size_t size: {size_t(0),size_t(1),size_t(31),size_t(4096),size_t(65536),size_t(1048576)}) {
        auto data=source(size,7),original=data;
        for(bool specialized: {false,true}) {
            MVKShaderLibrary direct(&owner,info,data,specialized?&macros:nullptr);
            check(direct,original);
            assert(direct._maySpecializeWithMacro==!specialized);
            assert(direct.macroCount==(specialized?1:0));
            assert(data._compressed==original._compressed);++cases;
        }
        MVKShaderLibraryCache cache(&owner);
        auto* lib=cache.addShaderLibrary(&config,info,data);check(*lib,original);
        assert(cache._shaderLibraries.at(0).first.tag==37);
        data._compressed.assign(size,0);data._uncompressedSize=0;
        check(*lib,original); // Source may change or disappear immediately.
        auto* temporary=cache.addShaderLibrary(&config,info,source(size,7));
        check(*temporary,original);cases+=2;
    }
    // Calibrate the actual vector allocation request. Some standard libraries
    // add alignment space for large arrays. String buffers have a separate
    // terminator; cache nodes / objects have different sizes. This probe does
    // not alter the extracted compressor or constructor implementation.
    auto data=source(65536,9);
    probe_allocation=true;
    {auto probe=data._compressed;assert(probe==data._compressed);}
    probe_allocation=false;assert(probe_allocations==1);
    assert(payload_size>=data._compressed.size());
    payload_allocations=0;count_allocations=true;
    {MVKShaderLibraryCache cache(&owner);check(*cache.addShaderLibrary(&config,info,data),data);}
    count_allocations=false;size_t cacheCopies=payload_allocations;
    assert(cacheCopies==(FIXTURE_NEW?1:3));
    payload_allocations=0;count_allocations=true;
    {MVKShaderLibrary lib(&owner,info,data,&macros);check(lib,data);}
    count_allocations=false;size_t directCopies=payload_allocations;
    assert(directCopies==(FIXTURE_NEW?1:2));
    printf("actual_constructor=PASS cases=%u cache_payload_copies=%zu direct_payload_copies=%zu owned_lifetime=PASS\n",cases,cacheCopies,directCopies);
#endif
}
