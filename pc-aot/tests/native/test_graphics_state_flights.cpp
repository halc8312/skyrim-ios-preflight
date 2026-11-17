// Authored driver controls around the pinned DXVK state and pipeline methods.
#include <cassert>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>
#include "dxvk/dxvk_bind_mask.h"
#include "dxvk/dxvk_graphics_state.h"
#include "util/sync/sync_spinlock.h"
#include "performance_metrics.h"
#include "prefetch_gate.h"
#include "graphics_flights.h"

using namespace std::chrono_literals;
using State = dxvk::DxvkGraphicsPipelineStateInfo;

namespace dxvk {
// Identical formats deliberately do not imply identical render-pass identity.
class DxvkRenderPass {
public:
    unsigned format() const { return 73; }
};
}
using Pass = dxvk::DxvkRenderPass;

State state(unsigned id) {
    State result;
    result.sc.specConstants[0] = id;
    return result;
}

class Signal {
    std::mutex mutex;
    std::condition_variable changed;
    unsigned count = 0;
public:
    void notify() { std::lock_guard<std::mutex> lock(mutex); ++count; changed.notify_all(); }
    bool wait(unsigned target = 1) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, 5s, [&] { return count >= target; });
    }
    unsigned value() { std::lock_guard<std::mutex> lock(mutex); return count; }
};

struct DriverGate {
    Signal entered, released;
    void hold() { entered.notify(); assert(released.wait()); }
    void open() { released.notify(); }
};

template<typename T> bool finished(std::future<T>& future) {
    return future.wait_for(5s) == std::future_status::ready;
}

// Only the real instance vector uses this allocator. Unrelated thread, fixture,
// state-cache and driver allocations cannot consume the injected failure.
thread_local bool fail_publication = false;
template<typename T> struct PublicationAllocator {
    using value_type = T;
    PublicationAllocator() = default;
    template<typename U> PublicationAllocator(const PublicationAllocator<U>&) noexcept {}
    T* allocate(size_t count) {
        if (fail_publication) { fail_publication = false; throw std::bad_alloc(); }
        return std::allocator<T>{}.allocate(count);
    }
    void deallocate(T* pointer, size_t count) noexcept { std::allocator<T>{}.deallocate(pointer, count); }
};
template<typename T, typename U> bool operator==(const PublicationAllocator<T>&, const PublicationAllocator<U>&) { return true; }
template<typename T, typename U> bool operator!=(const PublicationAllocator<T>&, const PublicationAllocator<U>&) { return false; }

struct Driver {
    struct Record {
        State state;
        const Pass* pass;
        unsigned calls = 0, waits = 0;
        VkPipeline latest = VK_NULL_HANDLE;
    };
    struct Block { State state; const Pass* pass; DriverGate* gate; };
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<Record> records;
    std::vector<Block> blocks;
    std::vector<VkPipeline> destroyed;
    std::map<unsigned, unsigned> validations, writes, throws, allocation_failures;
    uintptr_t next_handle = 1;
    std::function<void(const State&)> on_write;
    std::function<void(VkPipeline)> on_destroy;

    Record& record(const State& input, const Pass* pass) {
        for (auto& value : records)
            if (value.pass == pass && value.state == input) return value;
        records.push_back({input, pass});
        return records.back();
    }
    void block(const State& input, const Pass* pass, DriverGate& gate) {
        std::lock_guard<std::mutex> lock(mutex);
        blocks.push_back({input, pass, &gate});
    }
    bool validate(const State& input) {
        std::lock_guard<std::mutex> lock(mutex);
        ++validations[input.sc.specConstants[0]];
        return input.sc.specConstants[0] != 9999;
    }
    VkPipeline create(const State& input, const Pass* pass) {
        DriverGate* gate = nullptr;
        bool throw_now, allocation_failure;
        VkPipeline handle;
        {
            std::lock_guard<std::mutex> lock(mutex);
            const unsigned id = input.sc.specConstants[0];
            auto& value = record(input, pass);
            ++value.calls;
            handle = id == 13 ? VK_NULL_HANDLE : reinterpret_cast<VkPipeline>(next_handle++);
            value.latest = handle;
            throw_now = throws[id] != 0;
            if (throw_now) --throws[id];
            allocation_failure = allocation_failures[id] != 0;
            if (allocation_failure) --allocation_failures[id];
            for (const auto& block : blocks)
                if (block.pass == pass && block.state == input) gate = block.gate;
            changed.notify_all();
        }
        const auto context = skyrim_perf_context_get();
        assert(context.role == SKYRIM_PERF_ROLE_DRAW || context.role == SKYRIM_PERF_ROLE_PREFETCH);
        if (gate) gate->hold();
        if (throw_now) throw std::runtime_error("authored driver exception");
        if (allocation_failure) fail_publication = true;
        return handle;
    }
    void destroy(VkPipeline handle) {
        { std::lock_guard<std::mutex> lock(mutex); destroyed.push_back(handle); }
        if (on_destroy) on_destroy(handle);
    }
    void write(const State& input, unsigned format) {
        assert(format == 73);
        { std::lock_guard<std::mutex> lock(mutex); ++writes[input.sc.specConstants[0]]; }
        if (on_write) on_write(input);
    }
    void note_wait(const State& input, const Pass* pass) {
        std::lock_guard<std::mutex> lock(mutex);
        ++record(input, pass).waits;
        changed.notify_all();
    }
    bool wait_for_waiters(const State& input, const Pass* pass, unsigned target) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, 5s, [&] { return record(input, pass).waits >= target; });
    }
    unsigned count(const State& input, const Pass* pass) {
        std::lock_guard<std::mutex> lock(mutex); return record(input, pass).calls;
    }
    unsigned total_calls() {
        std::lock_guard<std::mutex> lock(mutex);
        unsigned total = 0; for (const auto& value : records) total += value.calls;
        return total;
    }
    VkPipeline latest(const State& input, const Pass* pass) {
        std::lock_guard<std::mutex> lock(mutex); return record(input, pass).latest;
    }
    unsigned written(unsigned id) { std::lock_guard<std::mutex> lock(mutex); return writes[id]; }
    unsigned validated(unsigned id) { std::lock_guard<std::mutex> lock(mutex); return validations[id]; }
};

// ACTUAL_DXVK_METHODS

void compare_ready_and_cold_progress() {
    Driver driver, other_driver;
    Manager manager; // Admission disabled so only compilation coordination differs.
    TestPipeline pipeline(driver, manager), other(other_driver, manager);
    Pass pass, same_format_pass;
    const auto warm = state(1), cold = state(2), distinct = state(3);
    auto full_state_variant = cold;
    full_state_variant.ilBindings[dxvk::DxvkLimits::MaxNumVertexBindings - 1] =
        dxvk::DxvkIlBinding(1, 32, VK_VERTEX_INPUT_RATE_INSTANCE, 2);
    const auto warm_handle = pipeline.get(warm, &pass);
    DriverGate blocked;
    driver.block(cold, &pass, blocked);
    auto owner = std::async(std::launch::async, [&] { pipeline.build(cold, &pass); });
    const bool owner_entered = blocked.entered.wait();
    auto ready = std::async(std::launch::async, [&] { return pipeline.get(warm, &pass); });
    const bool ready_done = finished(ready);
    // The exact same state and render-pass pointer in a different pipeline owner.
    auto independent = std::async(std::launch::async, [&] { return other.get(cold, &pass); });
    const bool independent_done = finished(independent);
    auto different = std::async(std::launch::async, [&] { return pipeline.get(distinct, &pass); });
    auto packed_variant = std::async(std::launch::async, [&] { return pipeline.get(full_state_variant, &pass); });
    auto pass_variant = std::async(std::launch::async, [&] { return pipeline.get(cold, &same_format_pass); });
    bool progress, ordered;
#if PROTOTYPE
    progress = finished(different) && finished(packed_variant) && finished(pass_variant);
    ordered = progress;
#else
    // The failed try_lock notification proves each cold request has reached the
    // occupied compiler mutex. No elapsed-time assertion establishes blocking.
    ordered = driver.wait_for_waiters(distinct, &pass, 1)
        && driver.wait_for_waiters(full_state_variant, &pass, 1)
        && driver.wait_for_waiters(cold, &same_format_pass, 1);
    progress = driver.count(distinct, &pass) != 0
        || driver.count(full_state_variant, &pass) != 0
        || driver.count(cold, &same_format_pass) != 0;
#endif
    auto duplicate_draw = std::async(std::launch::async, [&] { return pipeline.get(cold, &pass); });
    auto duplicate_prefetch = std::async(std::launch::async, [&] { pipeline.build(cold, &pass); });
    const bool duplicates_waited = driver.wait_for_waiters(cold, &pass, 2);
    // Release the authored compiler before asserting all A/B outcomes, even if
    // the experiment regresses to serialization or a request fails to arrive.
    blocked.open();
    owner.get(); duplicate_prefetch.get();
    const auto duplicate_handle = duplicate_draw.get();
    const auto ready_handle = ready.get(), independent_handle = independent.get();
    const auto different_handle = different.get(), packed_handle = packed_variant.get(), pass_handle = pass_variant.get();
    assert(owner_entered && ready_done && independent_done && ordered && duplicates_waited);
    assert(progress == bool(PROTOTYPE));
    assert(ready_handle == warm_handle && independent_handle != VK_NULL_HANDLE);
    assert(different_handle && packed_handle && pass_handle && duplicate_handle);
    assert(driver.count(cold, &pass) == 1 && other_driver.count(cold, &pass) == 1);
    assert(driver.count(distinct, &pass) == 1 && driver.count(full_state_variant, &pass) == 1);
    assert(driver.count(cold, &same_format_pass) == 1 && driver.written(1) == 1);
    assert(driver.written(2) == 2); // Only the two distinct draw-created variants.
    assert(manager.gate.stats().required == 0);
    std::puts("ready_and_independent_owner=PASS");
    std::puts("same_identity_single_generation=PASS");
    std::printf("different_state_progress=%d\n", int(progress));
}

void exact_state_and_pass_identity() {
    Driver driver; Manager manager; TestPipeline pipeline(driver, manager);
    Pass pass, same_format_pass;
    const auto input = state(40);
    const auto original = pipeline.get(input, &pass);
    State copy(input);
    assert(copy == input && pipeline.get(copy, &pass) == original);
    // The pinned equality compares the entire packed object, including its zeroed
    // padding. Exercise every byte, including all late bindings and constants.
    for (size_t byte = 0; byte < sizeof(State); ++byte) {
        State changed(input);
        reinterpret_cast<unsigned char*>(&changed)[byte] ^= 1;
        assert(changed != input);
        const auto handle = pipeline.get(changed, &pass);
        assert(handle != VK_NULL_HANDLE && handle != original);
        assert(pipeline.get(State(changed), &pass) == handle);
        assert(driver.count(changed, &pass) == 1);
    }
    assert(pass.format() == same_format_pass.format());
    const auto other_pass = pipeline.get(input, &same_format_pass);
    assert(other_pass != VK_NULL_HANDLE && other_pass != original);
    assert(pipeline.get(input, &pass) == original);
    assert(pipeline.size() == sizeof(State) + 2 && driver.total_calls() == sizeof(State) + 2);
    std::puts("exact_packed_state_and_pass_identity=PASS");
}

void failures_and_retry() {
    Driver driver; Manager manager(true); TestPipeline pipeline(driver, manager); Pass pass;
    const auto invalid = state(9999), null_state = state(13), retry = state(17);
    assert(pipeline.get(invalid, &pass) == VK_NULL_HANDLE);
    assert(pipeline.get(invalid, &pass) == VK_NULL_HANDLE);
    pipeline.build(invalid, &pass);
    assert(pipeline.size() == 0 && manager.m_numGraphicsPipelines == 0);
    assert(driver.count(invalid, &pass) == 0 && driver.validated(9999) == 3 && driver.written(9999) == 0);
    assert(pipeline.get(null_state, &pass) == VK_NULL_HANDLE);
    assert(pipeline.get(null_state, &pass) == VK_NULL_HANDLE);
    pipeline.build(null_state, &pass);
    assert(pipeline.size() == 1 && manager.m_numGraphicsPipelines == 1);
    assert(driver.count(null_state, &pass) == 1 && driver.validated(13) == 1 && driver.written(13) == 1);
    std::puts("cached_null_invalid_absent=PASS");
    DriverGate blocked;
    driver.block(retry, &pass, blocked);
    driver.throws[17] = 1;
    auto owner = std::async(std::launch::async, [&] {
        try { pipeline.get(retry, &pass); } catch (const std::runtime_error&) { return true; }
        return false;
    });
    const bool owner_entered = blocked.entered.wait();
    auto waiter = std::async(std::launch::async, [&] { return pipeline.get(retry, &pass); });
    const bool waiter_observed = driver.wait_for_waiters(retry, &pass, 1);
    blocked.open();
    const bool threw = owner.get();
    const auto handle = waiter.get();
    assert(owner_entered && waiter_observed && threw && handle);
    assert(driver.count(retry, &pass) == 2 && driver.written(17) == 1);
    assert(pipeline.get(retry, &pass) == handle && manager.gate.stats().required == 0);
    assert(skyrim_perf_context_get().role == 0);
    std::puts("exception_wakes_retry=PASS");
}

void reentrant_callback() {
    Driver driver; Manager manager(true); TestPipeline pipeline(driver, manager); Pass pass;
    size_t old_capacity = 0, new_capacity = 0;
    driver.on_write = [&](const State& input) {
        if (input.sc.specConstants[0] != 7) return;
        assert(manager.gate.stats().required == 0);
        assert(pipeline.list_unlocked() && pipeline.flight_idle(input, &pass));
        old_capacity = pipeline.capacity();
        // Ready reentry must be safe, and nested prefetch grows the real vector.
        assert(pipeline.get(input, &pass) == driver.latest(input, &pass));
        for (unsigned id = 1000; id < 1257; ++id) pipeline.build(state(id), &pass);
        new_capacity = pipeline.capacity();
    };
    const auto handle = pipeline.get(state(7), &pass);
    assert(handle == driver.latest(state(7), &pass) && new_capacity > old_capacity);
    assert(driver.written(7) == 1 && manager.gate.stats().required == 0);
    {
        SkyrimPerfContextScope outer(SKYRIM_PERF_ROLE_PREFETCH, SKYRIM_PERF_SOURCE_MACRO);
        assert(pipeline.get(state(777), &pass));
        const auto context = skyrim_perf_context_get();
        assert(context.role == SKYRIM_PERF_ROLE_PREFETCH && context.source == SKYRIM_PERF_SOURCE_MACRO);
    }
    assert(skyrim_perf_context_get().role == 0 && skyrim_perf_context_get().source == 0);
    std::puts("reentrant_callback_growth=PASS");
}

void concurrent_eight_threads() {
    Driver driver; Manager manager; TestPipeline pipeline(driver, manager); Pass passes[2];
    Signal arrived, start;
    std::vector<std::thread> threads;
    for (unsigned thread = 0; thread < 8; ++thread) threads.emplace_back([&, thread] {
        arrived.notify(); assert(start.wait());
        for (unsigned index = 0; index < 512; ++index) {
            const auto input = state(100 + (index * 17 + thread) % 64);
            const auto* pass = &passes[(index / 64) % 2];
            if ((index + thread) % 3 == 0) pipeline.build(input, pass);
            assert(pipeline.get(input, pass) != VK_NULL_HANDLE);
        }
    });
    const bool all_arrived = arrived.wait(8);
    start.notify();
    for (auto& thread : threads) thread.join();
    assert(all_arrived && pipeline.size() == 128 && driver.total_calls() == 128);
    for (unsigned id = 100; id < 164; ++id)
        for (const auto& pass : passes) assert(driver.count(state(id), &pass) == 1);
    std::puts("eight_threads=PASS");
}

#if PROTOTYPE
void publication_failure_cleanup() {
    Driver driver; Manager manager(true); TestPipeline pipeline(driver, manager); Pass pass;
    const auto input = state(31);
    driver.allocation_failures[31] = 1;
    DriverGate destroying;
    driver.on_destroy = [&](VkPipeline handle) {
        assert(handle != VK_NULL_HANDLE && pipeline.list_unlocked());
        destroying.hold();
    };
    auto owner = std::async(std::launch::async, [&] {
        try { pipeline.get(input, &pass); } catch (const std::bad_alloc&) { return true; }
        return false;
    });
    const bool cleanup_entered = destroying.entered.wait();
    auto waiter = std::async(std::launch::async, [&] { return pipeline.get(input, &pass); });
    const bool waited_during_cleanup = driver.wait_for_waiters(input, &pass, 1);
    const unsigned calls_during_cleanup = driver.count(input, &pass);
    const unsigned published_during_cleanup = manager.m_numGraphicsPipelines;
    destroying.open();
    const bool threw = owner.get();
    const auto handle = waiter.get();
    assert(cleanup_entered && waited_during_cleanup && calls_during_cleanup == 1 && published_during_cleanup == 0);
    assert(threw && handle && driver.destroyed.size() == 1 && driver.destroyed[0] != handle);
    assert(driver.count(input, &pass) == 2 && driver.written(31) == 1 && pipeline.size() == 1);
    assert(manager.m_numGraphicsPipelines == 1 && manager.gate.stats().required == 0);
    // A null driver result is not a destroyable handle, including when its cache
    // publication fails. Its retry still becomes a cached null once published.
    Driver null_driver; Manager null_manager; TestPipeline null_pipeline(null_driver, null_manager);
    null_driver.allocation_failures[13] = 1;
    bool null_threw = false;
    try { null_pipeline.get(state(13), &pass); } catch (const std::bad_alloc&) { null_threw = true; }
    assert(null_threw && null_driver.destroyed.empty() && null_pipeline.size() == 0);
    assert(null_manager.m_numGraphicsPipelines == 0);
    assert(null_pipeline.get(state(13), &pass) == VK_NULL_HANDLE);
    assert(null_pipeline.get(state(13), &pass) == VK_NULL_HANDLE && null_driver.count(state(13), &pass) == 2);
    std::puts("publication_failure_cleanup_before_release=PASS");
}

// Gate sleeps are explicit barriers. Fake time does not advance while parked,
// so a timeout/thermal fallback cannot hide a registered-flight deadlock.
struct GateHooks {
    DriverGate sleeping;
    static GateHooks* active;
    static uint64_t now() { return 1000; }
    static bool thermal() { return false; }
    static void sleep(uint64_t) { active->sleeping.hold(); }
    skyrim::PrefetchGate::Hooks hooks() { active = this; return {now, sleep, thermal}; }
};
GateHooks* GateHooks::active = nullptr;

void parked_prefetch_owns_no_flight() {
    GateHooks hooks; Driver driver; Manager manager(hooks.hooks()); TestPipeline pipeline(driver, manager); Pass pass;
    const auto input = state(20);
    auto required = std::make_unique<skyrim::PrefetchGate::RequiredScope>(manager.gate);
    auto prefetch = std::async(std::launch::async, [&] { pipeline.build(input, &pass); });
    const bool sleeping = hooks.sleeping.entered.wait();
    auto draw = std::async(std::launch::async, [&] { return pipeline.get(input, &pass); });
    const bool draw_completed_while_prefetch_parked = finished(draw);
    // Release every authored barrier before checking the outcome.
    required.reset(); hooks.sleeping.open();
    const auto handle = draw.get(); prefetch.get();
    assert(sleeping && draw_completed_while_prefetch_parked && handle);
    assert(driver.count(input, &pass) == 1 && manager.gate.stats().required == 0 && manager.gate.stats().expired == 0);
    std::puts("parked_prefetch_owns_no_flight=PASS");
}

void admitted_work_not_preempted() {
    GateHooks hooks; Driver driver; Manager manager(hooks.hooks()); TestPipeline pipeline(driver, manager); Pass pass;
    const auto input = state(21);
    DriverGate compiling; driver.block(input, &pass, compiling);
    auto prefetch = std::async(std::launch::async, [&] { pipeline.build(input, &pass); });
    const bool admitted = compiling.entered.wait();
    auto required = std::make_unique<skyrim::PrefetchGate::RequiredScope>(manager.gate);
    compiling.open();
    const bool completed_with_required_active = finished(prefetch);
    const unsigned sleeps = hooks.sleeping.entered.value();
    required.reset(); hooks.sleeping.open(); prefetch.get();
    assert(admitted && completed_with_required_active && sleeps == 0);
    assert(pipeline.get(input, &pass) && driver.count(input, &pass) == 1 && manager.gate.stats().required == 0);
    std::puts("admitted_work_not_preempted=PASS");
}

void shutdown_releases_admission() {
    GateHooks hooks; Driver driver; Manager manager(hooks.hooks()); TestPipeline pipeline(driver, manager); Pass pass;
    const auto input = state(22);
    auto required = std::make_unique<skyrim::PrefetchGate::RequiredScope>(manager.gate);
    auto prefetch = std::async(std::launch::async, [&] { pipeline.build(input, &pass); });
    const bool parked = hooks.sleeping.entered.wait();
    manager.gate.stop(); hooks.sleeping.open();
    const bool completed = finished(prefetch);
    required.reset(); prefetch.get();
    assert(parked && completed && driver.count(input, &pass) == 1);
    assert(manager.gate.stats().stopping && manager.gate.stats().required == 0 && manager.gate.stats().expired == 0);
    std::puts("shutdown_releases_admission=PASS");
}
#endif

int main() {
    compare_ready_and_cold_progress();
    exact_state_and_pass_identity();
    failures_and_retry();
    reentrant_callback();
    concurrent_eight_threads();
#if PROTOTYPE
    publication_failure_cleanup();
    parked_prefetch_owns_no_flight();
    admitted_work_not_preempted();
    shutdown_releases_admission();
#endif
    assert(skyrim_perf_context_get().role == 0);
    std::puts("semantics=PASS");
}
