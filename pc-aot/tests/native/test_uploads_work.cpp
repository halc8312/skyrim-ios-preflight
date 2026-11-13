#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <new>
#include <pthread.h>
#include <thread>
#include <vector>
static thread_local bool measuring;
static thread_local uint64_t allocations;
void *operator new(size_t n){if(measuring)++allocations;void*p=malloc(n?n:1);if(!p)throw std::bad_alloc();return p;}
void operator delete(void*p)noexcept{free(p);}void operator delete(void*p,size_t)noexcept{free(p);}
using DWORD=uint32_t;
#define D3DLOCK_DISCARD 0x2000
#define D3DLOCK_READONLY 0x10
static uint8_t memory[1<<20],*MEM=memory;static uint32_t next_address=16;
static uint32_t gmalloc(uint32_t n){uint32_t p=next_address;next_address+=(n+15)&~15u;assert(next_address<sizeof memory);return p;}
static void gfree(uint32_t){}
static void rt_fatal(const char*,...){abort();}
/* ACTUAL_BODY */
static uint64_t digest=14695981039346656037ull,cases,delivered;
static void hash(uint64_t n){digest=(digest^n)*1099511628211ull;}
struct CopyProbe {
    inline static int live=0,copies=0,throw_at=0;
    CopyProbe(){++live;}
    CopyProbe(const CopyProbe&){if(++copies==throw_at)throw 17;++live;}
    ~CopyProbe(){--live;}
    void operator()(const uint8_t*)const {++delivered;}
};
int main(int argc,char**argv){
    uint64_t objects[8]={};uint8_t host[16]={};
    if(argc>1){unsigned count=(unsigned)strtoul(argv[1],nullptr,10);for(unsigned i=0;i<count;++i){auto*p=&delivered;
            shadow_begin(objects,i,host,16,0,[p](const uint8_t*b){*p+=b[0]+1;});shadow_end(objects,i);}
        unsigned rounds=100000;auto start=std::chrono::steady_clock::now();allocations=0;measuring=true;
        for(unsigned i=0;i<rounds;++i)shadow_flush(objects);
        measuring=false;
        printf("{\"ns\":%.6f,\"allocations\":%llu,\"delivered\":%llu}\n",std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count()/rounds,(unsigned long long)allocations,(unsigned long long)delivered);return 0;}
    uint64_t counted=0;
    for(unsigned count:{0u,1u,2u,4u,8u,9u,16u,33u}) {
        g_persist.clear();g_shadows.clear();next_address=16;std::vector<unsigned> order;order.reserve(count);
        for(unsigned i=0;i<count;++i){
            shadow_begin(objects,i,host,16,0,[&,i](const uint8_t*){std::lock_guard<std::mutex> lock(g_shadow_lock);order.push_back(i);});shadow_end(objects,i);
        }
        allocations=0;measuring=true;shadow_flush(objects);measuring=false;counted+=allocations;
        assert(order.size()==count);for(unsigned i=0;i<count;++i){assert(order[i]==i);hash(i);}++cases;
        if(count){shadow_begin(objects,count/2,host,16,0,[](const uint8_t*){});order.clear();shadow_flush(objects);assert(order.size()==count-1);shadow_end(objects,count/2);++cases;}
    }
    // A callback can recursively flush another resource. Local queues cannot be shared.
    g_persist.clear();g_shadows.clear();next_address=16;
    shadow_begin(objects+1,0,host,16,0,[](const uint8_t*){hash(17);});shadow_end(objects+1,0);
    shadow_begin(objects,0,host,16,0,[&](const uint8_t*){hash(11);shadow_flush(objects+1);hash(13);});shadow_end(objects,0);
    shadow_flush(objects);++cases;
    // Snapshot callbacks must remain alive if a preceding callback erases the registry.
    g_persist.clear();g_shadows.clear();
    shadow_begin(objects,0,host,16,0,[&](const uint8_t*){shadow_forget(objects);hash(23);});shadow_end(objects,0);
    for(unsigned i=1;i<12;++i){shadow_begin(objects,i,host,16,0,[i](const uint8_t*){hash(i+100);});shadow_end(objects,i);}
    shadow_flush(objects);assert(g_persist.empty());++cases;
    // Failed snapshot copies unwind all already-constructed callbacks, without delivery.
    for(unsigned i=0;i<12;++i){shadow_begin(objects,i,host,16,0,CopyProbe{});shadow_end(objects,i);}
    const int live=CopyProbe::live;
    for(int throw_at:{1,5,9,12}){
        CopyProbe::copies=0;CopyProbe::throw_at=throw_at;delivered=0;
        try {shadow_flush(objects);assert(false);}catch(int e){assert(e==17);}
        assert(CopyProbe::live==live&&delivered==0);assert(g_shadow_lock.try_lock());g_shadow_lock.unlock();++cases;
    }
    CopyProbe::throw_at=0;g_persist.clear();assert(CopyProbe::live==0);
    printf("{\"cases\":%llu,\"digest\":%llu,\"allocations\":%llu}\n",(unsigned long long)cases,(unsigned long long)digest,(unsigned long long)counted);
}
