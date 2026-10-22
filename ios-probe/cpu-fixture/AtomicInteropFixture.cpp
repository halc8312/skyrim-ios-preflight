#include "ProbeCpuFixture.h"
#include <sched.h>
#include <time.h>

static_assert(sizeof(int32_t) == 4 && alignof(int32_t) == 4);
static_assert(sizeof(uint8_t) == 1 && alignof(uint8_t) == 1);
static_assert(__atomic_always_lock_free(4, nullptr));
static_assert(__atomic_always_lock_free(1, nullptr));

namespace {
// No ordinary/volatile access ever aliases the borrowed Rust Atomic elements.
// This worker has no Rust/Swift frames below a capture and cannot throw/unwind.
uint32_t exchange_elements(int32_t *ea, uint8_t *dirty) noexcept {
    const auto wait_for = [dirty](uint8_t phase) noexcept {
        struct timespec until{};
        if (clock_gettime(CLOCK_MONOTONIC, &until) != 0) return false;
        until.tv_sec += 5;
        for (uint32_t spins = 0;; ++spins) {
            const uint8_t current = __atomic_load_n(dirty, __ATOMIC_ACQUIRE);
            if (current == phase) return true;
            if (current == 255) return false;
            if ((spins & 255) == 0) {
                struct timespec now{};
                if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 ||
                    now.tv_sec > until.tv_sec ||
                    (now.tv_sec == until.tv_sec && now.tv_nsec >= until.tv_nsec)) return false;
                sched_yield();
            }
        }
    };
    const auto abort = [dirty]() noexcept {
        __atomic_store_n(dirty, uint8_t(255), __ATOMIC_RELEASE);
    };
    uint32_t flags = 0;
    if (!wait_for(1) || __atomic_load_n(ea, __ATOMIC_ACQUIRE) != -0x1234567) {
        abort(); return flags;
    }
    flags |= 1;
    int32_t expected = -0x1234567;
    if (!__atomic_compare_exchange_n(ea, &expected, int32_t(0x23456789), false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        abort(); return flags;
    }
    flags |= 2;
    __atomic_store_n(dirty, uint8_t(2), __ATOMIC_RELEASE);
    if (!wait_for(3) || __atomic_load_n(ea, __ATOMIC_ACQUIRE) != -0x3456789) {
        abort(); return flags;
    }
    flags |= 4;
    if (__atomic_exchange_n(ea, int32_t(0x456789AB), __ATOMIC_ACQ_REL) != -0x3456789) {
        abort(); return flags;
    }
    flags |= 8;
    __atomic_store_n(dirty, uint8_t(4), __ATOMIC_RELEASE);
    return flags;
}
}

int32_t probe_cpp_atomic_selftest(ProbeRuntimeResult *out, size_t capacity) {
    return probe_runtime_atomic_roundtrip(&exchange_elements, out, capacity);
}
