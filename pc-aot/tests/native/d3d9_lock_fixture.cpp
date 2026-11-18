// Authored scheduling gates around the actual generated D3D9 lock classes.
// Metric stubs count clock use and retain anonymous samples; they never change
// the production lock's spin, recursion, acquisition or release code.
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <time.h>
#include <vector>
#include "d3d9_multithread.h"

using namespace std::chrono_literals;
static std::atomic<unsigned> wallCalls { 0 }, cpuCalls { 0 };
static std::atomic<uint64_t> nextThread { 0 };
static std::mutex eventMutex;
static std::vector<SkyrimLockEvent> events;

extern "C" uint64_t skyrim_perf_thread_ordinal(void) {
    static thread_local uint64_t ordinal = nextThread.fetch_add(1) + 1;
    return ordinal;
}
extern "C" uint64_t skyrim_perf_now_ns(void) {
    ++wallCalls;
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
extern "C" uint64_t skyrim_lock_cpu_now_ns(void) {
    ++cpuCalls;
    struct timespec ts {};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts)) return 0;
    return uint64_t(ts.tv_sec) * 1000000000u + uint64_t(ts.tv_nsec);
}
extern "C" void skyrim_lock_record(const SkyrimLockEvent* event) {
    assert(event && event->end_ns >= event->start_ns);
    std::lock_guard<std::mutex> guard(eventMutex);
    events.push_back(*event);
}

template<class Predicate> static void until(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!predicate()) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}
static void resetMetrics() {
    std::lock_guard<std::mutex> guard(eventMutex);
    events.clear(); wallCalls = 0; cpuCalls = 0;
}

int main() {
    dxvk::D3D9Multithread device(TRUE), otherDevice(TRUE), unprotected(FALSE);
    // Single-thread API calls, recursion and disabled protection must not read
    // either clock. Recursive Present does not time a second outer hold.
    for (unsigned i = 0; i < 10000; ++i) {
        auto a = device.AcquireLock();
        auto b = device.AcquireLock();
        auto c = device.AcquireLock(SKYRIM_LOCK_OP_D3D9_PRESENT);
        auto d = unprotected.AcquireLock(SKYRIM_LOCK_OP_D3D9_PRESENT);
    }
    assert(wallCalls == 0 && cpuCalls == 0 && events.empty());
    std::puts("hot_recursion_unprotected_clocks_zero=PASS");

    // Move construction/assignment preserve the single outer Present scope.
    const uint64_t owner = skyrim_perf_thread_ordinal();
    std::atomic<bool> entered { false };
    auto first = device.AcquireLock(SKYRIM_LOCK_OP_D3D9_PRESENT);
    auto moved = std::move(first);
    dxvk::D3D9DeviceLock finalOwner;
    finalOwner = std::move(moved);
    std::thread waiter([&] { auto lock = device.AcquireLock(); entered = true; });
    until([] { return cpuCalls.load() != 0; });
    assert(!entered.load());
    {
        auto recursive = device.AcquireLock();
        auto recursivePresent = device.AcquireLock(SKYRIM_LOCK_OP_D3D9_PRESENT);
    }
    assert(!entered.load());
    finalOwner = dxvk::D3D9DeviceLock();
    waiter.join();
    assert(entered.load() && events.size() == 2);
    unsigned holds = 0, waits = 0;
    uint64_t instance = 0;
    for (const auto& event : events) {
        assert(event.lock == SKYRIM_LOCK_D3D9 && event.instance);
        if (event.phase == SKYRIM_LOCK_HOLD) {
            ++holds; instance = event.instance;
            assert(event.operation == SKYRIM_LOCK_OP_D3D9_PRESENT);
            assert(event.thread == owner && !event.owner_thread && !event.cpu_valid);
        } else {
            ++waits;
            assert(event.phase == SKYRIM_LOCK_WAIT);
            assert(event.operation == SKYRIM_LOCK_OP_D3D9_OTHER);
            assert(event.owner_operation == SKYRIM_LOCK_OP_D3D9_PRESENT);
            assert(event.owner_thread == owner && event.thread != owner);
            assert(event.cpu_valid && event.cpu_ns > 0);
        }
    }
    for (const auto& event : events) assert(event.instance == instance);
    assert(holds == 1 && waits == 1 && wallCalls == 4 && cpuCalls == 2);
    std::puts("contended_present_owner_recursion_moves=PASS");

    // Exception exit releases the real device lock and records one hold. A
    // distinct device has its own ordinal; there are no pointers in the sample.
    resetMetrics();
    try {
        auto present = otherDevice.AcquireLock(SKYRIM_LOCK_OP_D3D9_PRESENT);
        auto recursive = otherDevice.AcquireLock(SKYRIM_LOCK_OP_D3D9_PRESENT);
        throw std::runtime_error("authored Present failure");
    } catch (const std::runtime_error&) { }
    { auto unlocked = otherDevice.AcquireLock(); }
    assert(events.size() == 1 && events[0].phase == SKYRIM_LOCK_HOLD);
    assert(events[0].instance != instance && wallCalls == 2 && cpuCalls == 0);
    std::puts("exception_outer_hold_distinct_device=PASS");

    // Real threads share a plain value protected by the actual acquire/release
    // atomics. TSan checks ordering and the recursive counter's ownership.
    resetMetrics();
    unsigned value = 0;
    std::atomic<unsigned> inside { 0 };
    std::vector<std::thread> workers;
    for (unsigned n = 0; n < 8; ++n) workers.emplace_back([&] {
        for (unsigned i = 0; i < 2000; ++i) {
            auto lock = device.AcquireLock();
            assert(inside.fetch_add(1) == 0);
            { auto recursive = device.AcquireLock(); ++value; }
            assert(inside.fetch_sub(1) == 1);
        }
    });
    for (auto& worker : workers) worker.join();
    assert(value == 16000 && inside == 0);
    for (const auto& event : events) assert(event.phase == SKYRIM_LOCK_WAIT);
    std::puts("concurrent_recursive_ordering=PASS");
}
