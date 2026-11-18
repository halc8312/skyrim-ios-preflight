// Actual module-set method/class, with authored bytecode analysis and compiler.
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>
#include "performance_metrics.h"
using namespace std::chrono_literals;
static std::atomic<unsigned> waits{0};
static thread_local unsigned allocation_fault;
static thread_local bool publication_fault;
template<class T> static auto flight_allocate() {
    if(allocation_fault==1) {allocation_fault=0;throw std::bad_alloc();}
    return std::make_shared<T>();
}
template<class M,class K,class V> static auto flight_insert(M& m,const K& k,const V& v) {
    if(allocation_fault==2) {allocation_fault=0;throw std::bad_alloc();}
    return m.emplace(k,v);
}
template<class M,class K,class V> static auto publish_module(M& m,const K& k,const V& v) {
    if(publication_fault) {publication_fault=false;throw std::bad_alloc();}
    return m.insert({k,v});
}
namespace dxvk {
using mutex=std::mutex;
class condition_variable: public std::condition_variable {
public:
    template<class P> void wait(std::unique_lock<std::mutex>& l,P pred) {
        ++waits;std::condition_variable::wait(l,pred);
    }
};
struct RcObject {};
using VkShaderStageFlagBits=unsigned;
struct DxvkError:std::runtime_error {using std::runtime_error::runtime_error;};
struct Code {unsigned id,stage=0,major=3;};
struct DxsoModuleInfo {struct {unsigned shaderModel=3;} options;};
struct DxsoAnalysisInfo {size_t bytecodeByteLength=sizeof(Code);};
struct DxsoReader {const Code* code; explicit DxsoReader(const char* p):code(reinterpret_cast<const Code*>(p)) {}};
struct DxsoModule {
    Code code;
    struct Info {Code code;unsigned majorVersion()const{return code.major;}unsigned shaderStage()const{return code.stage;}};
    explicit DxsoModule(DxsoReader r):code(*r.code) {}
    Info info()const{return {code};}
    DxsoAnalysisInfo analyze()const{return {};}
};
struct Sha1Hash {static unsigned compute(const void* p,size_t n) {assert(n==sizeof(Code));return static_cast<const Code*>(p)->id;}};
struct DxvkShaderKey {unsigned stage,id;DxvkShaderKey(unsigned s,unsigned i):stage(s),id(i){}};
struct DxvkHash {size_t operator()(const DxvkShaderKey& k)const{return k.id*2+k.stage;}};
struct DxvkEq {bool operator()(const DxvkShaderKey& a,const DxvkShaderKey& b)const{return a.stage==b.stage && a.id==b.id;}};
struct D3D9DeviceEx {
    std::mutex mu;std::condition_variable cv;
    unsigned blocked=0,failures=0;bool released=true;
    std::map<unsigned,unsigned> calls;
    std::function<void(unsigned,unsigned)> callback;
    unsigned create(unsigned id) {
        std::unique_lock<std::mutex> lock(mu);unsigned attempt=++calls[id];cv.notify_all();
        if(id==blocked)cv.wait(lock,[&]{return released;});
        bool fail=id==blocked && attempt<=failures;
        auto hook=callback;lock.unlock();
        if(fail)throw DxvkError("authored compiler failure");
        if(hook)hook(id,attempt);
        return id*10000+attempt;
    }
    unsigned count(unsigned id) {std::lock_guard<std::mutex> l(mu);return calls[id];}
    void block(unsigned id,unsigned fail=0) {std::lock_guard<std::mutex> l(mu);blocked=id;released=false;failures=fail;}
    void release() {std::lock_guard<std::mutex> l(mu);released=true;cv.notify_all();}
};
struct D3D9CommonShader {
    unsigned handle=0;
    D3D9CommonShader()=default;
    D3D9CommonShader(D3D9DeviceEx* d,unsigned stage,DxvkShaderKey key,const DxsoModuleInfo*,const void* code,DxsoAnalysisInfo,DxsoModule*) {
        assert(key.stage==stage && key.id==static_cast<const Code*>(code)->id);handle=d->create(key.id+stage*100000);
    }
};
#define private public
// ACTUAL_SHADER_SET
#undef private
}
using namespace dxvk;
template<class P> static bool await(P p) {
    auto end=std::chrono::steady_clock::now()+3s;
    while(!p()) {if(std::chrono::steady_clock::now()>=end)return false;std::this_thread::yield();}
    return true;
}
static unsigned get(D3D9ShaderModuleSet& set,D3D9DeviceEx& d,unsigned id,unsigned stage=0) {
    Code c{id,stage,3};DxsoModuleInfo options;D3D9CommonShader out;
    set.GetShaderModule(&d,&out,stage,&options,&c);return out.handle;
}
static unsigned batch(unsigned id,unsigned failures=0) {
    D3D9ShaderModuleSet set;D3D9DeviceEx d;d.block(id,failures);
    unsigned warm=get(set,d,1);waits=0;
    auto invoke=[&] {try{return get(set,d,id);}catch(const DxvkError&){return 0u;}};
    auto owner=std::async(std::launch::async,invoke);
    assert(await([&]{return d.count(id)==1;}));
    std::vector<std::future<unsigned>> followers;
    for(unsigned i=1;i<8;i++)followers.emplace_back(std::async(std::launch::async,invoke));
    bool all_waited=await([&]{return PATCHED?waits.load()>=7:d.count(id)==8;});
#if PATCHED
    { std::lock_guard<std::mutex> lock(set.m_mutex);set.m_iosCompiling.rehash(257); }
#endif
    // Different keys/hot entries must finish while the first key is blocked.
    auto unrelated=std::async(std::launch::async,[&]{
        assert(get(set,d,1)==warm);
        for(unsigned i=100;i<228;i++)assert(get(set,d,i)); // rehash pending/ready maps
        return get(set,d,id,1); // same source identity, different stage
    });
    bool hot=unrelated.wait_for(3s)==std::future_status::ready;
    d.release();
    unsigned failed=owner.get()==0,canonical=0;
    for(auto& f:followers) {unsigned h=f.get();if(!h)++failed;else {if(canonical)assert(canonical==h);canonical=h;}}
    assert(unrelated.get());assert(all_waited && hot);
    assert(failed==(failures>=8?8:failures));
    unsigned total=d.count(id);
    if(!failures)assert(total==unsigned(PATCHED?1:8));
    if(failures==1)assert(total==unsigned(PATCHED?2:8));
    if(failures>=8)assert(total==8); // each original caller gets its own failure
    SkyrimPerfWindow w{};skyrim_perf_take(SKYRIM_PERF_D3D_SHADER_WAIT,&w);
    uint64_t samples=0;for(auto n:w.buckets)samples+=n;
    assert(PATCHED?(failures?samples>=7:samples==7):samples==0);
    assert(!w.failures && !w.sleep_calls);
#if PATCHED
    assert(set.m_iosCompiling.empty());
#endif
    if(failures<8) {unsigned before=d.count(id);assert(get(set,d,id));assert(d.count(id)==before);}
    skyrim_perf_take(SKYRIM_PERF_D3D_SHADER_WAIT,&w);for(auto n:w.buckets)assert(!n);
    return total;
}
int main() {
    unsigned count=batch(2);batch(3,1);batch(4,8);
    D3D9ShaderModuleSet set;D3D9DeviceEx d;
    // Callback reentry can publish first; outer request returns the canonical
    // object without waiting for its own flight or leaking it on exceptions.
    unsigned inner=0;
    d.callback=[&](unsigned id,unsigned attempt) {if(id==30 && attempt==1)inner=get(set,d,id);};
    assert(get(set,d,30)==inner && d.count(30)==2);d.callback={};
    for(unsigned mode=1;mode<=2;mode++) {
        allocation_fault=mode;assert(get(set,d,40+mode));allocation_fault=0;
        assert(d.count(40+mode)==1);
    }
    publication_fault=true;bool publication_failed=false;
    try {get(set,d,50);}catch(const std::bad_alloc&) {publication_failed=true;}
    assert(publication_failed && d.count(50)==1);
#if PATCHED
    assert(set.m_iosCompiling.empty());
#endif
    assert(get(set,d,50) && d.count(50)==2);
    // Validation still happens before any lookup, even for a previously cached key.
    for(unsigned mode=0;mode<2;mode++) {
        Code c{30,mode?1u:0u,mode?3u:4u};DxsoModuleInfo options;D3D9CommonShader out;bool failed=false;
        try {set.GetShaderModule(&d,&out,0,&options,&c);}catch(const DxvkError&){failed=true;}
        assert(failed && !out.handle && d.count(30)==2);
    }
    // Independent module-set/device lifetimes never inherit a previous cache.
    {D3D9ShaderModuleSet other;assert(get(other,d,30));assert(d.count(30)==3);}
    std::vector<std::thread> stress;
    for(unsigned t=0;t<8;t++)stress.emplace_back([&,t]{for(unsigned j=0;j<1000;j++)assert(get(set,d,1000+(j*13+t)%64));});
    for(auto& thread:stress)thread.join();
    for(unsigned id=1000;id<1064;id++)assert(d.count(id)>=1 && (!PATCHED || d.count(id)==1));
#if PATCHED
    assert(set.m_iosCompiling.empty());
#endif
    SkyrimPerfWindow drained{};skyrim_perf_take(SKYRIM_PERF_D3D_SHADER_WAIT,&drained);
    skyrim_perf_record(SKYRIM_PERF_D3D_SHADER_WAIT,123,0,0);skyrim_perf_log_window();
    printf("same_key_compiles=%u different_keys_parallel=1 failure_retry=1 reentry=1 semantics=PASS\n",count);
}
