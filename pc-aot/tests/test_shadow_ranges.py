"""Differential test and optional benchmark of the actual texture shadow code.

No private game data. The actual shadow block uses authored host objects and
callbacks. Compare uploaded bytes, callback order and freed addresses.
"""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location("runtime_stage",ROOT/"ios/runtime-port/prepare_runtime.py")
stage=importlib.util.module_from_spec(spec); spec.loader.exec_module(stage)

SHADOW_PREFIX=r'''
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <vector>
#include <map>
#include <mutex>
#include <functional>
#include <thread>
#include <algorithm>
#include <chrono>
#include <pthread.h>
typedef uint32_t DWORD;
#define D3DLOCK_DISCARD 0x2000
#define D3DLOCK_READONLY 0x10
static uint8_t memory[32*1024*1024]; static uint8_t *MEM=memory;
static uint32_t next_address=16; static std::vector<uint32_t> freed;
uint32_t gmalloc(uint32_t size){uint32_t p=next_address; next_address+=(size+15)&~15u; assert(next_address<sizeof memory);return p;}
void gfree(uint32_t p){freed.push_back(p);}
void rt_fatal(const char*,...){std::abort();}
'''

SHADOW_DRIVER=r'''
int main(int argc,char **argv) {
    const unsigned count=argc>1?(unsigned)strtoul(argv[1],0,10):257;
    std::vector<uint64_t> objects(count+2);
    std::vector<unsigned> order;
    unsigned calls=0; uint64_t checksum=0;
    for(unsigned i=0;i<count;i++) for(unsigned sub:{0u,1u,7u,UINT32_MAX}) {
        uint8_t host[16]; memset(host,(int)i,sizeof host);
        auto upload=[&,i,sub](const uint8_t *p) {
            // Callback must be outside g_shadow_lock. Reentrant lookup would deadlock otherwise.
            std::lock_guard<std::mutex> lock(g_shadow_lock);
            assert(p[0]==(uint8_t)(i^sub));
            calls++; checksum+=p[0]; if(argc==1) order.push_back(sub);
        };
        uint32_t g=shadow_begin(&objects[i],sub,host,16,0,upload);
        MEM[g]=(uint8_t)(i^sub);
        assert(shadow_end(&objects[i],sub));
        assert(host[0]==(uint8_t)(i^sub));
    }
    unsigned target=count/2;
    uint8_t host[16]={};
    uint32_t locked=shadow_begin(&objects[target],1,host,16,D3DLOCK_READONLY,
        [](const uint8_t*) { std::abort(); }); // Existing persist callback stays authoritative.
    assert(locked); calls=0; shadow_flush(&objects[target]);
    assert(calls==3);
    if(argc==1) {assert((order==std::vector<unsigned>{0,7,UINT32_MAX})); order.clear();}
    assert(shadow_end(&objects[target],1));
    assert(!shadow_end(&objects[target],1));
    calls=0; shadow_flush(&objects[target]); assert(calls==4);
    if(argc==1) assert((order==std::vector<unsigned>{0,1,7,UINT32_MAX}));
    unsigned before=calls; shadow_flush(&objects[count]); shadow_flush(nullptr); assert(before==calls);
    if(argc>1) {
        double ns=0; const unsigned iterations=10000;
        auto start=std::chrono::steady_clock::now();
        for(unsigned k=0;k<iterations;k++) shadow_flush(&objects[k%count]);
        ns=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count()/iterations;
        printf("shadow_objects=%u flush_ns=%.3f checksum=%llu\n",count,ns,(unsigned long long)checksum);
    } else {
        // Nonpersistent locks and persistent buffer mirrors keep old copy/free semantics.
        uint8_t dest[32]={}; uint32_t p=shadow_begin(&objects[count],0,dest,32,0);
        MEM[p+3]=0x71; assert(shadow_end(&objects[count],0)); assert(dest[3]==0x71);
        uint32_t b=buffer_shadow(&objects[target],32,0,dest,32,0);
        MEM[b+4]=0x99; assert(shadow_end(&objects[target],0)); assert(dest[4]==0x99);
        std::vector<uint32_t> expected{b};
        for(const auto &x:g_persist) if(x.first.first==&objects[target]) expected.push_back(x.second.guest);
        size_t f=freed.size(), total=g_persist.size(); shadow_forget(&objects[target]);
        assert(g_persist.size()==total-4);
        assert(std::vector<uint32_t>(freed.begin()+f,freed.end())==expected);
        shadow_forget(&objects[target]); assert(freed.size()==f+5);
        for(unsigned i=0;i<count;i++) if(i!=target) {calls=0;shadow_flush(&objects[i]);assert(calls==4);}
        // Independent objects are accessed concurrently, through the actual mutex.
        std::thread a([&]{for(unsigned k=0;k<200;k++) shadow_flush(&objects[0]);});
        std::thread bthread([&]{for(unsigned k=0;k<200;k++) shadow_flush(&objects[count-1]);});
        a.join(); bthread.join();
        printf("shadow_lifecycle=ok remaining=%zu frees=%zu\n",g_persist.size(),freed.size());
    }
    return 0;
}
'''

@unittest.skipUnless(os.name=="posix" and shutil.which("c++"), "Existing POSIX C++ compiler required")
class ShadowRangeTests(unittest.TestCase):
    def test_actual_shadow_lock_upload_order_release_and_unrelated_objects(self):
        with tempfile.TemporaryDirectory(prefix="shadow-ranges-") as tmp:
            work=Path(tmp); sources=stage.prepare(ROOT/"vendor/skydream")
            outputs=[]
            for label in ("baseline","patched"):
                folder=work/label;folder.mkdir()
                bridge=(ROOT/"vendor/skydream/runtime/d3d9_bridge.cpp").read_text() if label=="baseline" else sources["d3d9_bridge.cpp"]
                section=bridge.split('// Guest code cannot address host memory,',1)[1].split('\nnamespace {\nbool is_block_format',1)[0]
                (folder/"shadow.cpp").write_text(SHADOW_PREFIX+'\n// Guest code cannot address host memory,'+section+SHADOW_DRIVER)
                instrument=["-fsanitize=address,undefined","-fno-omit-frame-pointer"] if os.environ.get("RUNTIME_PERF_SANITIZE")=="1" else []
                subprocess.run(["c++","-std=c++17","-O2","-g","-fno-strict-aliasing","-pthread",*instrument,"shadow.cpp","-o","shadow"],
                    cwd=folder,capture_output=True,text=True,check=True,timeout=90)
                result=subprocess.run([str(folder/"shadow")],capture_output=True,text=True,timeout=30)
                self.assertEqual(result.returncode,0,result.stderr);outputs.append(result.stdout)
            self.assertEqual(outputs,["shadow_lifecycle=ok remaining=1024 frees=6\n"]*2)
            # The rejected heap experiment must not change production.
            self.assertEqual(sources["heap.c"],(ROOT/"vendor/skydream/runtime/heap.c").read_text())


if __name__=="__main__":unittest.main()
