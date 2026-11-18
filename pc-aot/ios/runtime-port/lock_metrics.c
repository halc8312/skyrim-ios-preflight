/* SPDX-License-Identifier: GPL-3.0-only */
#include "lock_metrics.h"
#include "performance_metrics.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

_Static_assert(ATOMIC_LONG_LOCK_FREE == 2 && ATOMIC_LLONG_LOCK_FREE == 2,
    "Lock observations require lock-free 64-bit atomics");

static atomic_flag samples_lock = ATOMIC_FLAG_INIT;
static SkyrimLockEvent samples[SKYRIM_LOCK_CAPACITY];
static unsigned sample_count;
static _Atomic uint64_t dropped_busy, dropped_full;
static _Atomic uint64_t observed_count[3], observed_ns[3];

void skyrim_lock_record(const SkyrimLockEvent *input) {
    if (!input || input->lock < SKYRIM_LOCK_D3D9 || input->lock > SKYRIM_LOCK_BC ||
            input->phase < SKYRIM_LOCK_WAIT || input->phase > SKYRIM_LOCK_WORK ||
            input->operation > SKYRIM_LOCK_OP_AUDIO_RESET ||
            input->owner_operation > SKYRIM_LOCK_OP_AUDIO_RESET ||
            input->cpu_valid > 1 || !input->start_ns || input->end_ns < input->start_ns ||
            !skyrim_perf_interval_active(input->start_ns)) return;
    const uint64_t elapsed = input->end_ns - input->start_ns;
    const unsigned phase = input->phase - 1;
    atomic_fetch_add_explicit(&observed_count[phase], 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&observed_ns[phase], elapsed, memory_order_relaxed);
    if (elapsed < SKYRIM_LOCK_THRESHOLD_NS) return;
    if (atomic_flag_test_and_set_explicit(&samples_lock, memory_order_acquire)) {
        atomic_fetch_add_explicit(&dropped_busy, 1, memory_order_relaxed);
        return;
    }
    if (skyrim_perf_interval_active(input->start_ns)) {
        unsigned slot = SKYRIM_LOCK_CAPACITY;
        if (sample_count < SKYRIM_LOCK_CAPACITY) {
            slot = sample_count++;
        } else {
            /* Bounded selection: ordinary Present holds must not permanently
             * exclude a much longer wait completing later in the same window.
             * This is a sample of completed long intervals, not a full trace.
             * Strict comparison keeps older samples when durations tie. */
            unsigned shortest = 0;
            uint64_t shortest_ns = samples[0].end_ns - samples[0].start_ns;
            for (unsigned i = 1; i < SKYRIM_LOCK_CAPACITY; ++i) {
                const uint64_t duration = samples[i].end_ns - samples[i].start_ns;
                if (duration < shortest_ns) { shortest = i; shortest_ns = duration; }
            }
            if (elapsed > shortest_ns) slot = shortest;
            atomic_fetch_add_explicit(&dropped_full, 1, memory_order_relaxed);
        }
        if (slot < SKYRIM_LOCK_CAPACITY) {
            SkyrimLockEvent event = *input;
            if (!event.owner_thread) event.owner_operation = SKYRIM_LOCK_OP_UNKNOWN;
            if (!event.cpu_valid || event.cpu_ns > elapsed) {
                event.cpu_valid = 0;
                event.cpu_ns = 0;
            }
            samples[slot] = event;
        }
    }
    atomic_flag_clear_explicit(&samples_lock, memory_order_release);
}

int skyrim_lock_take(SkyrimLockWindow *out) {
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (atomic_flag_test_and_set_explicit(&samples_lock, memory_order_acquire)) return 0;
    out->count = sample_count;
    memcpy(out->events, samples, sample_count * sizeof(samples[0]));
    sample_count = 0;
    out->dropped_busy = atomic_exchange_explicit(&dropped_busy, 0, memory_order_relaxed);
    out->dropped_full = atomic_exchange_explicit(&dropped_full, 0, memory_order_relaxed);
    for (unsigned i = 0; i < 3; ++i) {
        out->observed_count[i] = atomic_exchange_explicit(&observed_count[i], 0, memory_order_relaxed);
        out->observed_ns[i] = atomic_exchange_explicit(&observed_ns[i], 0, memory_order_relaxed);
    }
    atomic_flag_clear_explicit(&samples_lock, memory_order_release);
    return 1;
}

uint64_t skyrim_lock_cpu_now_ns(void) {
#ifdef CLOCK_THREAD_CPUTIME_ID
    struct timespec now;
    const int saved_errno = errno;
    const int result = clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
    errno = saved_errno;
    if (result || now.tv_sec < 0 || now.tv_nsec < 0 || now.tv_nsec >= 1000000000L)
        return 0;
    const uint64_t seconds = (uint64_t)now.tv_sec;
    if (seconds > (UINT64_MAX - (uint64_t)now.tv_nsec) / UINT64_C(1000000000)) return 0;
    return seconds * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
#else
    return 0;
#endif
}

static void log_window(void) {
    static uint64_t previous_ns; /* Existing UI reporter is the sole reader. */
    const uint64_t now = skyrim_perf_now_ns();
    if (!now || now < previous_ns) return;
    SkyrimLockWindow window;
    if (!skyrim_lock_take(&window)) return;
    const uint64_t before = previous_ns;
    previous_ns = now;
    /* Eleven bounded integer fields * 16 events plus header fit below 8191.
     * No partial/truncated line is exported if this bound ever changes. */
    char line[8192];
    int used = snprintf(line, sizeof(line),
        "IOS_LOCK schema=1 threshold_ns=%llu count=%u dropped_busy=%llu dropped_full=%llu "
        "window_start_ns=%llu window_end_ns=%llu totals=%llu,%llu,%llu,%llu,%llu,%llu "
        "fields=lock,phase,operation,instance,thread,owner_thread,owner_operation,start_ns,end_ns,cpu_ns,cpu_valid spans=",
        (unsigned long long)SKYRIM_LOCK_THRESHOLD_NS, window.count,
        (unsigned long long)window.dropped_busy, (unsigned long long)window.dropped_full,
        (unsigned long long)before, (unsigned long long)now,
        (unsigned long long)window.observed_count[0], (unsigned long long)window.observed_ns[0],
        (unsigned long long)window.observed_count[1], (unsigned long long)window.observed_ns[1],
        (unsigned long long)window.observed_count[2], (unsigned long long)window.observed_ns[2]);
    if (used < 0 || (size_t)used >= sizeof(line)) return;
    for (unsigned i = 0; i < window.count; ++i) {
        const SkyrimLockEvent *event = &window.events[i];
        const int wrote = snprintf(line + used, sizeof(line) - (size_t)used,
            "%s%u,%u,%u,%llu,%llu,%llu,%u,%llu,%llu,%llu,%u",
            i ? ";" : "", event->lock, event->phase, event->operation,
            (unsigned long long)event->instance, (unsigned long long)event->thread,
            (unsigned long long)event->owner_thread, event->owner_operation,
            (unsigned long long)event->start_ns, (unsigned long long)event->end_ns,
            (unsigned long long)event->cpu_ns, event->cpu_valid);
        if (wrote < 0 || (size_t)wrote >= sizeof(line) - (size_t)used) return;
        used += wrote;
    }
    if ((size_t)used + 1 >= sizeof(line)) return;
    line[used++] = '\n';
    line[used] = '\0';
    fputs(line, stderr);
}

void skyrim_lock_log_window(void) {
    const int saved_errno = errno;
    log_window();
    errno = saved_errno;
}
