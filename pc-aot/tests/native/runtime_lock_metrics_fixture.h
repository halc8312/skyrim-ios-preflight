/* A clock/recorder observer for the real runtime bodies. The production
 * bounded recorder has its own tests; no Apple audio/driver claim is made. */
#include "lock_metrics.h"
#include "performance_metrics.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#ifdef __cplusplus
#define FIXTURE_TLS thread_local
#else
#define FIXTURE_TLS _Thread_local
#endif

static SkyrimLockEvent fixture_events[8192];
static unsigned fixture_event_count, fixture_clock_count;
static uint64_t fixture_next_thread;
static FIXTURE_TLS uint64_t fixture_thread;

uint64_t skyrim_perf_thread_ordinal(void) {
    if (!fixture_thread) fixture_thread = __atomic_add_fetch(&fixture_next_thread, 1, __ATOMIC_RELAXED);
    return fixture_thread;
}
uint64_t skyrim_perf_now_ns(void) {
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    __atomic_add_fetch(&fixture_clock_count, 1, __ATOMIC_RELAXED);
    errno = EOVERFLOW; /* The wrapper must not leak observation side effects. */
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}
void skyrim_lock_record(const SkyrimLockEvent *event) {
    unsigned at = __atomic_fetch_add(&fixture_event_count, 1, __ATOMIC_RELAXED);
    assert(at < sizeof fixture_events / sizeof fixture_events[0]);
    assert(event->instance == 1 && event->thread != 0 && event->end_ns >= event->start_ns);
    fixture_events[at] = *event; /* Readers run only after all emitting threads join. */
    errno = ERANGE;
}
void skyrim_perf_end(unsigned kind, uint64_t start, int failed) {
    (void)kind; (void)start; (void)failed;
}
static void fixture_clear(void) {
    fixture_event_count = fixture_clock_count = 0;
    memset(fixture_events, 0, sizeof fixture_events);
}
static unsigned fixture_count(unsigned category, unsigned phase, unsigned operation) {
    unsigned count = 0;
    for (unsigned i = 0; i < fixture_event_count; ++i)
        count += fixture_events[i].lock == category && fixture_events[i].phase == phase &&
                 fixture_events[i].operation == operation;
    return count;
}
static void fixture_wait_flag(const int *flag) {
    for (unsigned i = 0; i < 50000; ++i) {
        if (__atomic_load_n(flag, __ATOMIC_ACQUIRE)) return;
        const struct timespec tick = {0, 100000};
        nanosleep(&tick, NULL);
    }
    assert(!"fixture synchronization timed out");
}
