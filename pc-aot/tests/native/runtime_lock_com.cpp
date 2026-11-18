#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include "runtime_lock_metrics_fixture.h"

static int waiter_entered, cleanup_entered, release_cleanup;
static thread_local bool watch_wait, spurious;
class FixtureMutex : public std::recursive_mutex {
public:
    bool try_lock() { if (spurious) { spurious = false; return false; } return std::recursive_mutex::try_lock(); }
    void lock() {
        if (watch_wait) __atomic_store_n(&waiter_entered, 1, __ATOMIC_RELEASE);
        std::recursive_mutex::lock();
    }
};
static uint32_t memory[8192], next_guest = 16;
#define M32(at) memory[(at) / 4]
static std::vector<unsigned> cleanup_order;
static uint32_t gmalloc(uint32_t size) { uint32_t result = next_guest; next_guest += size; assert(next_guest < sizeof memory); return result; }
static void gfree(uint32_t) { cleanup_order.push_back(3); }
[[noreturn]] static void rt_fatal(const char*, ...) { std::abort(); }
struct CPU { uint32_t args[2], result; };
#define ARG(n) c->args[n]
#define RET(value, nargs) do { c->result = (uint32_t)(value); return; } while (0)
using ULONG = unsigned;
struct IUnknown {
    unsigned refs = 1;
    unsigned AddRef() { return ++refs; }
    unsigned Release();
};
static void shadow_forget(void*);
static void bc_forget(void*);
/* ACTUAL_EVENT_HELPER */
/* ACTUAL_REGISTRY */
/* ACTUAL_COM_FUNCTIONS */

static bool unlocked_on_other_thread(void) {
    bool unlocked = false;
    std::thread probe([&] { unlocked = g_lock.try_lock(); if (unlocked) g_lock.unlock(); });
    probe.join();
    return unlocked;
}
unsigned IUnknown::Release() {
    assert(unlocked_on_other_thread()); /* Release itself must remain outside the registry lock. */
    return --refs;
}
static void shadow_forget(void*) {
    assert(!unlocked_on_other_thread());
    cleanup_order.push_back(1);
    if (__atomic_load_n(&release_cleanup, __ATOMIC_ACQUIRE) == -1) {
        __atomic_store_n(&cleanup_entered, 1, __ATOMIC_RELEASE);
        while (__atomic_load_n(&release_cleanup, __ATOMIC_ACQUIRE) == -1) std::this_thread::yield();
    }
}
static void bc_forget(void*) {
    assert(!unlocked_on_other_thread());
    cleanup_order.push_back(2);
}

int main(void) {
    IUnknown keep, victim;
    g_ifaces[1].vtbl = 1234;
    uint32_t keep_guest = com_wrap(&keep, 1);
    uint32_t victim_guest = com_wrap(&victim, 1);
    assert(com_wrap(&keep, 1) == keep_guest && M32(keep_guest) == 1234);
    fixture_clear(); errno = EINTR;
    for (unsigned i = 0; i < 10000; ++i) assert(com_host(keep_guest) == &keep);
    assert(fixture_clock_count == 0 && fixture_event_count == 0 && errno == EINTR);
    assert(com_host(0) == nullptr);
    {
        IosComLock outer(SKYRIM_LOCK_OP_COM_WRAP, true);
        uint64_t started = ios_com_lock_state.start;
        {
            IosComLock nested(SKYRIM_LOCK_OP_COM_LOOKUP, false);
            assert(ios_com_lock_state.depth == 2 && ios_com_lock_state.start == started);
        }
        assert(fixture_event_count == 0 && ios_com_lock_state.depth == 1);
    }
    assert(fixture_count(SKYRIM_LOCK_COM, SKYRIM_LOCK_HOLD, SKYRIM_LOCK_OP_COM_WRAP) == 1);
    assert(ios_com_lock_state.depth == 0 && ios_com_lock_owner.load() == 0 && errno == EINTR);
    try { IosComLock scope(SKYRIM_LOCK_OP_COM_WRAP, true); throw 7; }
    catch (int value) { assert(value == 7); }
    assert(unlocked_on_other_thread() && ios_com_lock_state.depth == 0);

    fixture_clear(); spurious = true;
    assert(com_host(keep_guest) == &keep);
    assert(fixture_count(SKYRIM_LOCK_COM, SKYRIM_LOCK_WAIT, SKYRIM_LOCK_OP_COM_LOOKUP) == 1);
    assert(fixture_events[0].owner_thread == 0); /* Never invent a holder after try_lock failure. */

    fixture_clear();
    CPU cpu{{victim_guest, 0}, 0};
    com_AddRef(&cpu); assert(cpu.result == 2);
    com_Release(&cpu); assert(cpu.result == 1 && com_host(victim_guest) == &victim);
    __atomic_store_n(&release_cleanup, -1, __ATOMIC_RELEASE);
    uint64_t forget_thread = 0;
    std::thread forget([&] { forget_thread = skyrim_perf_thread_ordinal(); com_Release(&cpu); });
    fixture_wait_flag(&cleanup_entered);
    std::thread lookup([&] { watch_wait = true; assert(com_host(keep_guest) == &keep); });
    fixture_wait_flag(&waiter_entered);
    __atomic_store_n(&release_cleanup, 1, __ATOMIC_RELEASE);
    forget.join(); lookup.join();
    assert(cpu.result == 0 && M32(victim_guest) == 0xDEAD0000u);
    assert((cleanup_order == std::vector<unsigned>{1, 2, 3}));
    assert(fixture_count(SKYRIM_LOCK_COM, SKYRIM_LOCK_WAIT, SKYRIM_LOCK_OP_COM_LOOKUP) == 1);
    assert(fixture_count(SKYRIM_LOCK_COM, SKYRIM_LOCK_HOLD, SKYRIM_LOCK_OP_COM_FORGET) == 1);
    assert(fixture_count(SKYRIM_LOCK_SHADOW, SKYRIM_LOCK_WORK, SKYRIM_LOCK_OP_SHADOW_CLEANUP) == 1);
    assert(fixture_count(SKYRIM_LOCK_BC, SKYRIM_LOCK_WORK, SKYRIM_LOCK_OP_BC_CLEANUP) == 1);
    for (unsigned i = 0; i < fixture_event_count; ++i) if (fixture_events[i].phase == SKYRIM_LOCK_WAIT) {
        assert(fixture_events[i].owner_thread == forget_thread);
        assert(fixture_events[i].owner_operation == SKYRIM_LOCK_OP_COM_FORGET);
    }
    assert(com_host(keep_guest) == &keep && unlocked_on_other_thread());
    assert(ios_com_lock_owner.load() == 0);
    puts("com_lock hot_clocks=0 recursive=PASS release_unlocked=PASS cleanup_order=PASS lookup_wait=PASS spurious=PASS exception=PASS errno=PASS");
}
