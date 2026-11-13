#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <unordered_map>
#include <vector>
#include <chrono>
static uint64_t allocations;
static bool measuring;
void *operator new(size_t n) { if(measuring)++allocations;void *p=malloc(n?n:1);if(!p)throw std::bad_alloc();return p; }
void operator delete(void *p) noexcept {free(p);}
void operator delete(void *p,size_t) noexcept {free(p);}
static uint8_t memory[1<<20]; static uint32_t next_address=16, frees, table_vt=77;
#define M32(a) (*(uint32_t *)(memory+(a)))
static void gfree(uint32_t) {++frees;}
static uint32_t make_buffer(const void *p,size_t bytes,uint32_t vt) {
    uint32_t obj=next_address;next_address+=16;uint32_t data=next_address;next_address+=(bytes+15)&~15u;
    assert(next_address<sizeof memory);memcpy(memory+data,p,bytes);M32(obj)=vt;M32(obj+8)=data;return obj;
}
/* ACTUAL_BODY */
static uint64_t digest=14695981039346656037ull, cases;
static void hash(const void *p,size_t n){for(size_t k=0;k<n;++k)digest=(digest^((const uint8_t*)p)[k])*1099511628211ull;}
static void put(std::vector<uint32_t>& code,size_t at,uint32_t n){memcpy((uint8_t*)code.data()+at,&n,4);}
static std::vector<uint32_t> authored(unsigned n) {
    size_t size=32+n*20+16+8;
    std::vector<uint32_t> code((size+12)/4,0);code[0]=0xfffe0300;code[1]=((size/4+1)<<16)|0xfffe;code[2]=0x42415443;
    size_t b=12;put(code,b+8,0xfffe0300);put(code,b+12,n);put(code,b+16,32);
    size_t type=32+n*20;put(code,b+type,2u<<16);put(code,b+type+4,4u<<16|1);put(code,b+type+8,1);
    memcpy((uint8_t*)code.data()+b+type+16,"uniform",8);
    for(unsigned i=0;i<n;++i){size_t p=b+32+i*20;put(code,p,type+16);put(code,p+4,(i<<16)|2);put(code,p+8,1);put(code,p+12,type);}
    return code;
}
static uint32_t run(const std::vector<uint32_t>& code,size_t bytes) {
    tables.clear();next_address=16;frees=0;
    uint32_t obj=make_table(code.data(),bytes);hash(&obj,4);hash(&frees,4);
    if(obj){const auto&t=tables.at(obj);hash(&t.data,20);hash(t.constants.data(),t.constants.size()*sizeof(Constant));
        assert(t.constants.size()==t.roots);for(const auto&v:t.constants){assert(v.set==2&&v.rows==1&&v.cols==4&&v.bytes==16);}}
    ++cases;return obj;
}
int main(int argc,char**argv) {
    if(argc>1){unsigned n=(unsigned)strtoul(argv[1],nullptr,10);auto code=authored(n);const unsigned rounds=20000;
        auto start=std::chrono::steady_clock::now();measuring=true;
        for(unsigned r=0;r<rounds;++r){tables.clear();next_address=16;assert(make_table(code.data(),code.size()*4));}
        measuring=false;double ns=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count()/rounds;
        printf("{\"ns\":%.6f,\"allocations\":%llu}\n",ns,(unsigned long long)allocations);return 0;}
    for(unsigned n:{0u,1u,2u,3u,16u,127u,256u,1024u,4096u}) {
        auto code=authored(n);assert(run(code,code.size()*4));
        for(size_t bytes=0;bytes<std::min<size_t>(code.size()*4,64);++bytes)run(code,bytes);
        auto bad=code;put(bad,12+12,4097);assert(!run(bad,bad.size()*4));
        bad=code;put(bad,12+16,0xffffffff);assert(!run(bad,bad.size()*4));
        if(n){bad=code;put(bad,12+32+(n-1)*20+12,0xfffffff0);assert(!run(bad,bad.size()*4));}
    }
    auto code=authored(256);tables.clear();next_address=16;allocations=0;measuring=true;
    assert(make_table(code.data(),code.size()*4));measuring=false;
    printf("{\"cases\":%llu,\"digest\":%llu,\"allocations\":%llu}\n",(unsigned long long)cases,(unsigned long long)digest,(unsigned long long)allocations);
}
