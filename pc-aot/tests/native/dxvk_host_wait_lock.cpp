// Actual baseline/trial Spinlock with authored contention and portable API stubs.
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>
#include <time.h>
using namespace std::chrono_literals;
struct Signal {
    std::mutex mutex; std::condition_variable changed; bool done = false;
    void notify() { std::lock_guard<std::mutex> lock(mutex); done = true; changed.notify_all(); }
    bool wait() { std::unique_lock<std::mutex> lock(mutex); return changed.wait_for(lock, 5s, [&] { return done; }); }
};
static std::atomic<unsigned> clock_reads{0};
static thread_local Signal* clock_started = nullptr;
static thread_local bool fail_clock = false;
static thread_local unsigned authored_clock = 0, authored_index = 0;
static int counted_clock(clockid_t which, timespec* value) {
    ++clock_reads;
    if (clock_started) { clock_started->notify(); clock_started = nullptr; }
    if (fail_clock) { errno = EIO; return -1; }
    if (authored_clock) {
        const clockid_t order[] = {CLOCK_MONOTONIC, CLOCK_THREAD_CPUTIME_ID, CLOCK_THREAD_CPUTIME_ID, CLOCK_MONOTONIC};
        const long stamps[][4] = {
            {1000, 200, 500, 1100}, // CPU exceeds wall: unavailable.
            {1000, 200, 100, 1100}, // CPU goes backwards: unavailable.
            {1000, 200, 250,  900}, // Wall goes backwards: unavailable.
            {1000, 200, 250, 1100}, // Valid nested interval: wall=100, CPU=50.
        };
        assert(authored_index < 4 && which == order[authored_index]);
        value->tv_sec = 0; value->tv_nsec = stamps[authored_clock - 1][authored_index++];
        return 0;
    }
    return ::clock_gettime(which, value);
}
#define clock_gettime counted_clock
#include "host_lock_wait.h"
#if HOST_APPLE_STUB
#include "host_wait_apple_stubs.h"
#define __APPLE__ 1
#endif
#include "util/sync/sync_spinlock.h"
#if HOST_APPLE_STUB
#undef __APPLE__
#endif
#undef clock_gettime
using Lock = dxvk::sync::Spinlock;
using skyrim::host_wait::LockWaitCounters;
static_assert(!std::is_copy_constructible_v<Lock> && !std::is_copy_assignable_v<Lock>);
static_assert(!std::is_move_constructible_v<Lock> && !std::is_move_assignable_v<Lock>);

static LockWaitCounters contend(Lock& lock, bool clock_failure = false) {
    Signal entered;
    LockWaitCounters result;
    lock.lock();
    std::thread waiter([&] {
        const unsigned before_try = clock_reads.load();
        assert(!lock.try_lock()); // A different thread owns this stable instance.
        assert(clock_reads.load() == before_try);
        clock_started = &entered; fail_clock = clock_failure; errno = EAGAIN;
#if HOST_TRIAL
        lock.lock(); // Trial observer starts after its single failed initial try.
#else
        { // Baseline stays unchanged; observe only after a confirmed failed try.
            skyrim::host_wait::LockWaitSample sample;
            lock.lock();
        }
#endif
        const int restored_errno = errno;
        lock.unlock();
        result = skyrim::host_wait::takeLockWaitCounters();
        assert(restored_errno == EAGAIN);
    });
    const bool observed = entered.wait();
    // Deliberately hold the owner to expose a bounded contended CPU/wall sample.
    // The wait has already entered its observer; this is not a fast-path timing.
    std::this_thread::sleep_for(40ms);
    lock.unlock(); waiter.join(); assert(observed);
    return result;
}

int main() {
    Lock lock, independent;
    for (unsigned n = 0; n < 10000; ++n) {
        lock.lock(); lock.unlock(); assert(lock.try_lock()); lock.unlock();
    }
    assert(clock_reads == 0 && skyrim::host_wait::takeLockWaitCounters().waits == 0);
#if HOST_APPLE_STUB
    assert(host_stub::tries == 20000 && host_stub::blocks == 0 && host_stub::unlocks == 20000);
    host_stub::tries = 0; host_stub::blocks = 0; host_stub::unlocks = 0;
#endif
    lock.lock(); assert(independent.try_lock()); independent.unlock(); lock.unlock();
    try { std::lock_guard<Lock> guard(lock); throw std::runtime_error("authored"); }
    catch (const std::runtime_error&) {}
    std::thread after_exception([&] { assert(lock.try_lock()); lock.unlock(); }); after_exception.join();
    LockWaitCounters totals;
    for (unsigned n = 0; n < 3; ++n) {
        const auto sample = contend(lock);
        assert(sample.waits == 1 && sample.samples == 1 && sample.clock_failures == 0);
        assert(sample.wall_ns > 0);
        totals.waits += sample.waits; totals.samples += sample.samples;
        totals.wall_ns += sample.wall_ns; totals.cpu_ns += sample.cpu_ns;
    }
#if HOST_APPLE_STUB
    assert(host_stub::blocks == 3); // Exactly one blocking call per failed lock attempt.
#endif
    const auto bad_clock = contend(lock, true);
    assert(bad_clock.waits == 1 && bad_clock.samples == 0 && bad_clock.clock_failures == 1);
    for (authored_clock = 1; authored_clock <= 4; ++authored_clock) {
        authored_index = 0; errno = EAGAIN;
        { skyrim::host_wait::LockWaitSample sample; }
        const auto result = skyrim::host_wait::takeLockWaitCounters();
        assert(authored_index == 4 && errno == EAGAIN && result.waits == 1);
        if (authored_clock == 4)
            assert(result.samples == 1 && result.wall_ns == 100 && result.cpu_ns == 50 && result.clock_failures == 0);
        else
            assert(result.samples == 0 && result.wall_ns == 0 && result.cpu_ns == 0 && result.clock_failures == 1);
    }
    authored_clock = 0;
    uint64_t serial = 0, published = 0;
    std::vector<std::thread> workers;
    for (unsigned n = 0; n < 8; ++n) workers.emplace_back([&] {
        for (unsigned iteration = 0; iteration < 2000; ++iteration) {
            std::lock_guard<Lock> guard(lock);
            assert(published == serial * 17);
            ++serial; published = serial * 17;
        }
    });
    for (auto& worker : workers) worker.join();
    assert(serial == 16000 && published == serial * 17);
#if defined(__APPLE__) && !HOST_APPLE_STUB && HOST_TRIAL
    constexpr unsigned apple = 1;
#else
    constexpr unsigned apple = 0;
#endif
    std::printf("{\"schema\":1,\"trial\":%u,\"authored_api_stub\":%u,\"apple_backend_verified\":%u,"
        "\"iphone_verified\":0,\"performance_verified\":0,\"semantics\":1,\"hot_clock_reads\":0,"
        "\"waits\":%llu,\"samples\":%llu,\"wall_ns\":%llu,\"cpu_ns\":%llu,\"clock_failure_contract\":1}\n",
        unsigned(HOST_TRIAL), unsigned(HOST_APPLE_STUB), apple,
        (unsigned long long)totals.waits, (unsigned long long)totals.samples,
        (unsigned long long)totals.wall_ns, (unsigned long long)totals.cpu_ns);
}
