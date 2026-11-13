// Authored compiler gate. Production lookup/list methods are inserted below.
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <map>
#include <vector>
#include <atomic>
#include <future>
#include <functional>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <stdexcept>
#include "util/sync/sync_spinlock.h"
#include "performance_metrics.h"

using namespace std::chrono_literals;
struct Driver {
    std::mutex mu;
    std::condition_variable cv;
    bool entered=false, release=false, throw_once=true;
    std::map<uint64_t, unsigned> calls, writes;
    std::function<void(unsigned)> on_write;
    static uint64_t key(unsigned id, unsigned rp) { return uint64_t(id)*2+rp; }
    static uint64_t handle(unsigned id, unsigned rp=0) { return id==13 ? 0 : key(id,rp)+1; }
    uint64_t create(unsigned id, unsigned rp) {
        std::unique_lock<std::mutex> lock(mu);
        ++calls[key(id,rp)];
        if (id==2 && !release) {
            entered=true; cv.notify_all();
            cv.wait(lock,[&]{return release;});
        }
        if (id==17 && throw_once) {throw_once=false; throw std::runtime_error("authored compiler failure");}
        return handle(id,rp);
    }
    void write(unsigned id,unsigned rp) {
        {std::lock_guard<std::mutex> lock(mu); ++writes[key(id,rp)];}
        if(on_write) on_write(id);
    }
    unsigned count(unsigned id,unsigned rp=0) {std::lock_guard<std::mutex> lock(mu);return calls[key(id,rp)];}
    unsigned written(unsigned id,unsigned rp=0) {std::lock_guard<std::mutex> lock(mu);return writes[key(id,rp)];}
};

// ACTUAL_DXVK_METHODS

int main() {
    Driver driver;
    dxvk::DxvkPipelineManager manager;
    TestPipeline pipeline(driver,manager);
    assert(pipeline.get(1)==Driver::handle(1));
    std::thread worker([&]{pipeline.build(2);});
    {std::unique_lock<std::mutex> lock(driver.mu); assert(driver.cv.wait_for(lock,3s,[&]{return driver.entered;}));}
    std::promise<void> reader_started;
    auto ready=std::async(std::launch::async,[&]{reader_started.set_value();return pipeline.get(1);});
    reader_started.get_future().wait();
    bool bypass=ready.wait_for(300ms)==std::future_status::ready;
    auto cold=std::async(std::launch::async,[&]{return pipeline.get(2);});
    auto duplicate=std::async(std::launch::async,[&]{pipeline.build(2);});
    bool cold_waited=cold.wait_for(50ms)==std::future_status::timeout;
    // Always release the gate before checking outcomes, including a regression.
    {std::lock_guard<std::mutex> lock(driver.mu);driver.release=true;driver.cv.notify_all();}
    worker.join(); duplicate.get();
    assert(ready.get()==Driver::handle(1));assert(cold.get()==Driver::handle(2));
    assert(bypass==bool(PATCHED));assert(cold_waited);
    assert(driver.count(1)==1 && driver.count(2)==1);
    assert(driver.written(1)==1 && driver.written(2)==0);
#if GRAPHICS
    assert(pipeline.get(1,1)==Driver::handle(1,1));
    assert(driver.count(1,1)==1);
    assert(!pipeline.get(9999));pipeline.build(9999);
    assert(driver.count(9999)==0 && driver.written(9999)==0);
#endif
    // API failure remains a cached null result, exactly as the pinned version.
    assert(!pipeline.get(13));assert(!pipeline.get(13));pipeline.build(13);
    assert(driver.count(13)==1 && driver.written(13)==1);
    bool threw=false;try {pipeline.get(17);}catch(const std::runtime_error&) {threw=true;}
    assert(threw && driver.written(17)==0);
    assert(pipeline.get(17)==Driver::handle(17));assert(driver.count(17)==2);
#if PATCHED
    // Cache callbacks run after both locks are released. Reentrant publication
    // grows the actual vector; a saved instance pointer would become dangling.
    driver.on_write=[&](unsigned id){if(id==7) for(unsigned j=1000;j<1257;j++) pipeline.build(j);};
    assert(pipeline.get(7)==Driver::handle(7));driver.on_write={};
    std::vector<std::thread> stress;
    for(unsigned t=0;t<8;t++) stress.emplace_back([&,t]{
        for(unsigned j=0;j<2000;j++) {
            unsigned id=100+(j*17+t)%128;
            if(j%3==0) pipeline.build(id);
            assert(pipeline.get(id)==Driver::handle(id));
        }
    });
    for(auto& t:stress)t.join();
    for(unsigned id=100;id<228;id++) assert(driver.count(id)==1);
    // Only contended cold requests report compiler-lock wait. A hot lookup has
    // no extra clock read or histogram update in the production method.
    SkyrimPerfWindow window{};
    constexpr unsigned wait_kind=GRAPHICS?SKYRIM_PERF_GRAPHICS_COMPILE_WAIT:SKYRIM_PERF_COMPUTE_COMPILE_WAIT;
    skyrim_perf_take(wait_kind,&window);
    assert(window.failures==0 && window.sleep_calls==0);
    assert(pipeline.get(1)==Driver::handle(1));
    skyrim_perf_take(wait_kind,&window);
    for(auto n:window.buckets) assert(n==0);
#endif
    std::printf("ready_while_compiler_blocked=%d cold_wait_preserved=1 semantics=PASS\n",int(bypass));
}
