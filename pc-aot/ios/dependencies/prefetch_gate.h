/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "device_policy.h"
#include "performance_metrics.h"
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <time.h>

namespace skyrim {

/* Admission only: an admitted compilation, the compile mutex, and Metal are
 * never interrupted. Counters describe this gate, not lock owners or GPU time.
 * The manager owns this object until all worker and pipeline use has ended. */
class PrefetchGate {
public:
    static constexpr uint64_t budgetNs = UINT64_C(1000000000);
    static constexpr uint64_t sliceNs = UINT64_C(2000000);
    static constexpr uint64_t thermalPaceNs = UINT64_C(8000000);
    static constexpr uint64_t disableAfterExpirations = 3;
    struct Hooks {
        uint64_t (*now)();
        void (*sleep)(uint64_t);
        bool (*thermal)();
    };
    struct Stats {
        uint64_t yielded, sleeps, waitedNs, expired, thermal, clockFailures;
        uint32_t required;
        bool disabled, stopping;
    };

    PrefetchGate() noexcept : PrefetchGate(environmentEnabled(), productionHooks()) {}
    explicit PrefetchGate(bool enabled, Hooks hooks = productionHooks()) noexcept
        : enabled_(enabled), hooks_(hooks) {}
    ~PrefetchGate() {
        if (enabled_) report(2);
    }
    PrefetchGate(const PrefetchGate&) = delete;
    PrefetchGate& operator=(const PrefetchGate&) = delete;
    void stop() noexcept { stopping_.store(true); }
    Stats stats() const noexcept {
        return {yielded_.load(), sleeps_.load(), waitedNs_.load(), expired_.load(),
            thermal_.load(), clockFailures_.load(), required_.load(),
            disabled_.load(), stopping_.load()};
    }

    /* Construct after a required ready-hit has failed and before taking its
     * compile mutex. End before cache callbacks that may reenter prefetch. */
    class RequiredScope {
    public:
        explicit RequiredScope(PrefetchGate& gate) noexcept
            : gate_(gate), engaged_(gate.enabled_ && !gate.disabled_.load() && !gate.stopping_.load()) {
            if (engaged_) gate_.required_.fetch_add(1);
        }
        ~RequiredScope() { if (engaged_) gate_.required_.fetch_sub(1); }
        RequiredScope(const RequiredScope&) = delete;
        RequiredScope& operator=(const RequiredScope&) = delete;
    private:
        PrefetchGate& gate_;
        bool engaged_;
    };

    class Attempt {
    public:
        /* No DXVK/MoltenVK lock may be held here: the existing device snapshot
         * takes a short mutex. Thermal state is sampled once per variant. */
        explicit Attempt(PrefetchGate& gate) noexcept : gate_(gate) {
            if (inactive()) return;
            if (gate_.hooks_.thermal()) {
                const uint64_t now = checkedNow();
                if (now) {
                    thermalUntil_ = add(now, thermalPaceNs);
                    gate_.thermal_.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        /* Declare this before compileLock so destruction/logging occurs only
         * after that lock is released, including early return/exception. */
        ~Attempt() noexcept {
            if (expired_) { if (reportExpiration_) gate_.reportAfterYield(true); }
            else if (requestedSleepNs_) gate_.reportAfterYield(false);
        }
        /* Must be called outside every DXVK/MoltenVK lock. One deadline is
         * shared by thermal pacing and all rechecks for this variant. Timer
         * delivery/scheduling and compile-mutex waits are not bounded by it. */
        void wait() noexcept {
            if (inactive()) return;
            uint64_t started = 0;
            while (!mayStart()) {
                const uint64_t now = lastNow_;
                if (!started) started = now;
                uint64_t remaining = deadline_ > now ? deadline_ - now : 0;
                const uint64_t sleepBudget = budgetNs - requestedSleepNs_;
                if (remaining > sleepBudget) remaining = sleepBudget;
                if (!remaining) continue;
                uint64_t delay = remaining < sliceNs ? remaining : sliceNs;
                requestedSleepNs_ += delay;
                gate_.sleeps_.fetch_add(1, std::memory_order_relaxed);
                gate_.hooks_.sleep(delay);
            }
            if (started && lastNow_ >= started)
                gate_.waitedNs_.fetch_add(lastNow_ - started, std::memory_order_relaxed);
        }
        /* Recheck immediately after acquiring compile mutex. This path reads
         * only atomics/the clock; it never sleeps or snapshots device state. */
        bool mayStart() noexcept {
            if (inactive()) return true;
            const uint64_t now = checkedNow();
            if (!now) return true; // invalid clock -> baseline behavior
            if (!gate_.required_.load() && now >= thermalUntil_) return true;
            if ((deadline_ && now >= deadline_) || requestedSleepNs_ >= budgetNs) {
                expired_ = true; // exactly one expiration per variant
                const uint64_t ordinal = gate_.expired_.fetch_add(1, std::memory_order_relaxed) + 1;
                reportExpiration_ = ordinal <= disableAfterExpirations;
                if (ordinal >= disableAfterExpirations)
                    gate_.disabled_.store(true);
                return true;
            }
            if (!deadline_) {
                deadline_ = add(now, budgetNs);
                gate_.yielded_.fetch_add(1, std::memory_order_relaxed);
            }
            return false;
        }
        Attempt(const Attempt&) = delete;
        Attempt& operator=(const Attempt&) = delete;
    private:
        PrefetchGate& gate_;
        uint64_t deadline_ = 0, thermalUntil_ = 0, lastNow_ = 0, requestedSleepNs_ = 0;
        bool expired_ = false, badClock_ = false, reportExpiration_ = false;
        bool inactive() const noexcept {
            return !gate_.enabled_ || expired_ || badClock_ || gate_.stopping_.load() || gate_.disabled_.load();
        }
        uint64_t checkedNow() noexcept {
            const uint64_t now = gate_.hooks_.now();
            if (!now || (lastNow_ && now < lastNow_)) {
                badClock_ = true;
                gate_.clockFailures_.fetch_add(1, std::memory_order_relaxed);
                return 0;
            }
            lastNow_ = now;
            return now;
        }
        static uint64_t add(uint64_t a, uint64_t b) noexcept {
            return a > UINT64_MAX - b ? UINT64_MAX : a + b;
        }
    };
private:
    const bool enabled_;
    const Hooks hooks_;
    std::atomic<uint32_t> required_{0};
    std::atomic<bool> stopping_{false}, disabled_{false};
    std::atomic<uint64_t> yielded_{0}, sleeps_{0}, waitedNs_{0}, expired_{0}, thermal_{0}, clockFailures_{0};
    std::atomic<uint64_t> nextReportNs_{0};
    struct PreserveErrno {
        int saved = errno;
        ~PreserveErrno() { errno = saved; }
    };
    void report(unsigned event) const noexcept {
        PreserveErrno keep;
        const Stats s = stats();
        // event: 0 bounded progress, 1 expiration, 2 destruction after workers.
        std::fprintf(stderr, "IOS_PREFETCH_GATE event=%u enabled=1 yielded=%llu sleeps=%llu waited_ns=%llu expired=%llu thermal=%llu clock_failures=%llu required=%u disabled=%u stopping=%u\n",
            event, (unsigned long long)s.yielded, (unsigned long long)s.sleeps,
            (unsigned long long)s.waitedNs, (unsigned long long)s.expired,
            (unsigned long long)s.thermal, (unsigned long long)s.clockFailures,
            s.required, unsigned(s.disabled), unsigned(s.stopping));
    }
    void reportAfterYield(bool expired) noexcept {
        PreserveErrno keep;
        if (expired) { report(1); return; } // at most three per manager
        const uint64_t now = hooks_.now();
        constexpr uint64_t interval = UINT64_C(5000000000);
        if (!now || now > UINT64_MAX - interval) return;
        uint64_t next = nextReportNs_.load(std::memory_order_relaxed);
        while (now >= next) {
            if (nextReportNs_.compare_exchange_weak(next, now + interval, std::memory_order_relaxed)) {
                report(0); return;
            }
        }
    }
    static bool environmentEnabled() noexcept {
        const char* p = std::getenv("SKYRIM_PREFETCH_GATE");
        return p && p[0] == '1' && !p[1];
    }
    static Hooks productionHooks() noexcept {
        return {skyrim_perf_now_ns,
            [](uint64_t ns) { struct timespec delay{0, static_cast<long>(ns)}; nanosleep(&delay, nullptr); },
            []() { SkyrimDeviceState state{}; skyrim_device_snapshot(nullptr, &state); return state.low_power || state.thermal >= 2; }};
    }
};
}
