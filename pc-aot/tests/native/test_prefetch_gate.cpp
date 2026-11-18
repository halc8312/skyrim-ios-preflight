/* SPDX-License-Identifier: GPL-3.0-only */
#include "prefetch_gate.h"
#include <cassert>
#include <future>
#include <thread>
#include <stdexcept>
#include <string>
#include <vector>

using Gate = skyrim::PrefetchGate;
static uint64_t fakeNow, clocks, sleeps, snapshots;
static bool hot;
static Gate* stopOnSleep;
static uint64_t now() { ++clocks; return fakeNow; }
static bool thermal() { ++snapshots; return hot; }
static void sleep(uint64_t ns) {
    assert(ns && ns <= Gate::sliceNs);
    ++sleeps;
    fakeNow = fakeNow > UINT64_MAX - ns ? UINT64_MAX : fakeNow + ns;
    if (stopOnSleep) stopOnSleep->stop();
}
static void frozenSleep(uint64_t ns) { assert(ns && ns<=Gate::sliceNs); ++sleeps; }
static Gate::Hooks hooks{now, sleep, thermal};
static void reset() { fakeNow=100; clocks=sleeps=snapshots=0; hot=false; stopOnSleep=nullptr; }

static void noWorkWhenOff() {
    reset(); Gate gate(false,hooks);
    {
        Gate::RequiredScope required(gate);
        Gate::Attempt attempt(gate);
        attempt.wait(); assert(attempt.mayStart()); gate.stop(); attempt.wait();
        assert(gate.stats().required==0);
    }
    assert(!clocks && !sleeps && !snapshots && !gate.stats().yielded);
}

static void requiredHasNoHooksAndUnwinds() {
    reset(); Gate gate(true,hooks);
    try { Gate::RequiredScope r(gate); assert(gate.stats().required==1); throw std::runtime_error("authored"); }
    catch (const std::runtime_error&) {}
    assert(gate.stats().required==0 && !clocks && !sleeps && !snapshots);
}

static void oneDeadlineAndOneExpiration() {
    reset(); Gate gate(true,hooks);
    Gate::RequiredScope required(gate);
    Gate::Attempt attempt(gate);
    const uint64_t start=fakeNow;
    assert(!attempt.mayStart());
    for (unsigned n=0;n<3;++n) {
        fakeNow += Gate::budgetNs/4;
        assert(!attempt.mayStart()); // mutex rechecks do not reset the budget
    }
    attempt.wait();
    assert(fakeNow==start+Gate::budgetNs);
    assert(attempt.mayStart()); attempt.wait(); assert(attempt.mayStart());
    assert(gate.stats().yielded==1 && gate.stats().expired==1);
    for(unsigned n=0;n<2;++n){ Gate::Attempt next(gate); next.wait(); }
    assert(gate.stats().expired==3 && gate.stats().disabled);
    const uint64_t slept=sleeps;
    Gate::Attempt bypass(gate); bypass.wait(); assert(bypass.mayStart());
    assert(sleeps==slept);
}

static void thermalAndUrgentShareBudget() {
    reset(); hot=true; Gate gate(true,hooks);
    Gate::Attempt attempt(gate);
    const uint64_t start=fakeNow;
    attempt.wait();
    assert(fakeNow==start+Gate::thermalPaceNs && sleeps==4 && snapshots==1);
    assert(gate.stats().thermal==1 && gate.stats().expired==0);
    Gate::RequiredScope required(gate);
    fakeNow=start+Gate::budgetNs-1;
    assert(!attempt.mayStart());
    ++fakeNow; assert(attempt.mayStart());
    assert(gate.stats().yielded==1 && gate.stats().expired==1 && snapshots==1);
}

static void stopAndClockFailure() {
    reset(); Gate stopped(true,hooks);
    Gate::RequiredScope required(stopped);
    Gate::Attempt attempt(stopped); stopOnSleep=&stopped; attempt.wait();
    assert(sleeps==1 && attempt.mayStart() && !stopped.stats().expired);
    reset(); Gate failed(true,hooks); fakeNow=0;
    Gate::RequiredScope r(failed);
    Gate::Attempt a(failed); a.wait(); a.wait();
    assert(failed.stats().clockFailures==1 && !sleeps);
    reset(); Gate backward(true,hooks);
    Gate::RequiredScope r2(backward); Gate::Attempt a2(backward);
    assert(!a2.mayStart()); --fakeNow; assert(a2.mayStart());
    assert(backward.stats().clockFailures==1);
    reset(); Gate overflow(true,hooks); fakeNow=UINT64_MAX-4;
    Gate::RequiredScope r3(overflow); Gate::Attempt a3(overflow); a3.wait();
    assert(overflow.stats().expired==1 && fakeNow==UINT64_MAX);
    reset(); Gate frozen(true,{now,frozenSleep,thermal});
    Gate::RequiredScope r4(frozen); Gate::Attempt a4(frozen); a4.wait();
    assert(frozen.stats().expired==1 && sleeps==Gate::budgetNs/Gate::sliceNs);
}

static void strictStartupSwitch() {
    const char* old=std::getenv("SKYRIM_PREFETCH_GATE");
    const bool present=old; const std::string saved=old?old:"";
    for(const char* value : {"0", "1", "true", "01", "1x", ""}) {
        assert(!setenv("SKYRIM_PREFETCH_GATE", value, 1));
        Gate gate; Gate::RequiredScope r(gate);
        assert(gate.stats().required==unsigned(std::string(value)=="1"));
    }
    if(present) assert(!setenv("SKYRIM_PREFETCH_GATE",saved.c_str(),1));
    else assert(!unsetenv("SKYRIM_PREFETCH_GATE"));
}

static void stopRealWorker() {
    Gate gate(true); Gate::RequiredScope required(gate);
    std::atomic<bool> entered{false};
    auto worker=std::async(std::launch::async,[&] {
        Gate::Attempt attempt(gate); entered.store(true); attempt.wait();
    });
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(!entered.load() && std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
    assert(entered.load()); gate.stop();
    assert(worker.wait_for(std::chrono::milliseconds(500))==std::future_status::ready);
    worker.get(); assert(gate.stats().expired==0);
}

static void boundedLogging() {
    reset(); hot=true; Gate gate(true,hooks);
    for(unsigned i=0;i<6;++i) {
        { Gate::Attempt attempt(gate); attempt.wait(); errno=E2BIG; }
        assert(errno==E2BIG);
    }
    fakeNow+=UINT64_C(5000000000);
    { Gate::Attempt attempt(gate); attempt.wait(); }
    Gate::RequiredScope required(gate);
    { Gate::Attempt attempt(gate); attempt.wait(); errno=E2BIG; }
    assert(errno==E2BIG);
}

static std::atomic<uint64_t> concurrentNow{100};
static std::atomic<unsigned> prepared{0}, clockEntered{0};
static std::atomic<bool> clockBarrier{false}, releaseClock{false};
static uint64_t concurrentClock() {
    if(clockBarrier.load()) {
        clockEntered.fetch_add(1);
        while(!releaseClock.load()) std::this_thread::yield();
    }
    return concurrentNow.load();
}
static void concurrentExpirationLogging() {
    Gate gate(true,{concurrentClock,[](uint64_t){assert(false);},[](){return false;}});
    Gate::RequiredScope required(gate);
    std::vector<std::thread> workers;
    for(unsigned i=0;i<8;++i) workers.emplace_back([&] {
        Gate::Attempt attempt(gate);
        assert(!attempt.mayStart()); prepared.fetch_add(1);
        while(!clockBarrier.load()) std::this_thread::yield();
        assert(attempt.mayStart());
    });
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(prepared.load()!=8 && std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
    assert(prepared.load()==8);
    concurrentNow.store(100+Gate::budgetNs); clockBarrier.store(true);
    // All eight have observed disabled=false before any can expire. The
    // counter may legitimately exceed three; only the first three may log.
    while(clockEntered.load()!=8 && std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
    assert(clockEntered.load()==8); releaseClock.store(true);
    for(auto& worker:workers) worker.join();
    assert(gate.stats().expired==8 && gate.stats().disabled);
    puts("concurrent_expirations=8 report_bound=3 PASS");
}

int main(int argc, char** argv) {
    if(argc==2 && std::string(argv[1])=="off") { noWorkWhenOff(); return 0; }
    if(argc==2 && std::string(argv[1])=="logging") { boundedLogging(); return 0; }
    if(argc==2 && std::string(argv[1])=="concurrent-expiry") { concurrentExpirationLogging(); return 0; }
    noWorkWhenOff(); requiredHasNoHooksAndUnwinds(); oneDeadlineAndOneExpiration();
    thermalAndUrgentShareBudget(); stopAndClockFailure(); strictStartupSwitch(); stopRealWorker();
    puts("prefetch_admission_off_priority_deadline_thermal_stop=PASS");
}
