/* Authored inputs exercise production recording. No game assets or handles. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

static _Atomic uint64_t fixture_now = 1000000000, fixture_cpu = 100000;
static _Atomic unsigned clock_calls;
static int fixture_clock_gettime(clockid_t clock, struct timespec *out) {
    atomic_fetch_add(&clock_calls, 1);
    uint64_t now;
    if (clock == CLOCK_MONOTONIC) now = atomic_load(&fixture_now);
    else { assert(clock == CLOCK_THREAD_CPUTIME_ID); now = atomic_load(&fixture_cpu); }
    errno = EIO;
    if (!now) return -1;
    out->tv_sec = (time_t)(now / 1000000000);
    out->tv_nsec = (long)(now % 1000000000);
    return 0;
}
#define clock_gettime fixture_clock_gettime
#include "performance_metrics.c"
#include "lock_metrics.c"
#undef clock_gettime

static SkyrimLockEvent event(unsigned phase, uint64_t duration) {
    return (SkyrimLockEvent){.start_ns=100, .end_ns=100+duration, .instance=1,
        .thread=1, .owner_thread=2, .cpu_ns=100,
        .lock=SKYRIM_LOCK_AUDIO, .phase=phase,
        .operation=SKYRIM_LOCK_OP_AUDIO_SUBMIT,
        .owner_operation=SKYRIM_LOCK_OP_AUDIO_MIX, .cpu_valid=1};
}

static _Atomic unsigned finished;
#define ITERATIONS 10000
#define THREADS 4
static void *produce(void *arg) {
    SkyrimLockEvent e = event(SKYRIM_LOCK_WAIT, 100000000);
    e.thread = (uintptr_t)arg + 1;
    for (unsigned i=0; i<ITERATIONS; ++i) skyrim_lock_record(&e);
    atomic_fetch_add(&finished, 1);
    return NULL;
}

int main(void) {
    SkyrimLockWindow w;
    SkyrimLockEvent e = event(SKYRIM_LOCK_WAIT, SKYRIM_LOCK_THRESHOLD_NS-1);
    errno = EINTR;
    skyrim_lock_record(&e);
    assert(clock_calls == 0 && errno == EINTR);
    assert(skyrim_lock_take(&w) && w.count == 0 && w.observed_count[0] == 1);
    assert(w.observed_ns[0] == SKYRIM_LOCK_THRESHOLD_NS-1);
    e.end_ns++;
    skyrim_lock_record(&e);
    assert(skyrim_lock_take(&w) && w.count == 1 && w.events[0].cpu_valid);
    assert(w.events[0].owner_thread == 2 && w.events[0].owner_operation == SKYRIM_LOCK_OP_AUDIO_MIX);
    e.owner_thread=0; e.cpu_ns=UINT64_MAX;
    skyrim_lock_record(&e);
    assert(skyrim_lock_take(&w) && w.count == 1);
    assert(!w.events[0].cpu_valid && !w.events[0].cpu_ns && !w.events[0].owner_operation);
    e = event(SKYRIM_LOCK_HOLD, 100000000);
    e.cpu_valid=0;
    skyrim_lock_record(&e);
    assert(skyrim_lock_take(&w) && w.count == 1 && !w.events[0].cpu_ns);
    skyrim_lock_record(NULL);
    e.start_ns=0; skyrim_lock_record(&e);
    e.start_ns=e.end_ns+1; skyrim_lock_record(&e);
    e=event(0, 100000000); skyrim_lock_record(&e);
    e.phase=4; skyrim_lock_record(&e);
    e.phase=1; e.lock=0; skyrim_lock_record(&e);
    e.lock=6; skyrim_lock_record(&e);
    e.lock=1; e.operation=16; skyrim_lock_record(&e);
    e.operation=0; e.owner_operation=16; skyrim_lock_record(&e);
    e.owner_operation=0; e.cpu_valid=2; skyrim_lock_record(&e);
    assert(skyrim_lock_take(&w) && !w.count && !w.observed_count[0] && !w.observed_count[1]);
    assert(!skyrim_lock_take(NULL));
    assert(clock_calls == 0 && errno == EINTR);

    e=event(SKYRIM_LOCK_WAIT, 100000000);
    assert(!atomic_flag_test_and_set(&samples_lock));
    skyrim_lock_record(&e);
    assert(!skyrim_lock_take(&w) && !w.count);
    atomic_flag_clear(&samples_lock);
    assert(skyrim_lock_take(&w) && !w.count && w.dropped_busy == 1 && w.observed_count[0] == 1);
    for (unsigned i=0; i<SKYRIM_LOCK_CAPACITY+3; ++i) skyrim_lock_record(&e);
    assert(skyrim_lock_take(&w) && w.count == SKYRIM_LOCK_CAPACITY && w.dropped_full == 3);
    assert(w.observed_count[0] == SKYRIM_LOCK_CAPACITY+3);

    /* Early 30FPS-like holds must not crowd out a later 600ms audio wait.
     * Replacement and tied rejection each discard exactly one old/new event. */
    e=event(SKYRIM_LOCK_HOLD, 33333334);
    e.lock=SKYRIM_LOCK_D3D9; e.operation=SKYRIM_LOCK_OP_D3D9_PRESENT;
    for (unsigned i=0; i<SKYRIM_LOCK_CAPACITY; ++i) {
        e.instance=i+1; skyrim_lock_record(&e);
    }
    e.instance=777; skyrim_lock_record(&e); /* Tie must preserve earlier sample. */
    e=event(SKYRIM_LOCK_WAIT, 600000000);
    e.instance=888; skyrim_lock_record(&e);
    assert(skyrim_lock_take(&w) && w.count==SKYRIM_LOCK_CAPACITY && w.dropped_full==2);
    assert(w.observed_count[0]==1 && w.observed_count[1]==SKYRIM_LOCK_CAPACITY+1);
    unsigned short_holds=0, long_waits=0;
    for (unsigned i=0; i<w.count; ++i) {
        assert(w.events[i].instance!=777);
        if (w.events[i].instance==888) {
            assert(w.events[i].phase==SKYRIM_LOCK_WAIT && w.events[i].lock==SKYRIM_LOCK_AUDIO);
            assert(w.events[i].end_ns-w.events[i].start_ns==600000000);
            ++long_waits;
        } else {
            assert(w.events[i].phase==SKYRIM_LOCK_HOLD && w.events[i].instance>=1 &&
                   w.events[i].instance<=SKYRIM_LOCK_CAPACITY);
            ++short_holds;
        }
    }
    assert(long_waits==1 && short_holds==SKYRIM_LOCK_CAPACITY-1);

    skyrim_perf_activity(0);
    skyrim_lock_record(&e);
    assert(skyrim_lock_take(&w) && !w.count && !w.observed_count[0]);
    atomic_store(&fixture_now, 2000000000);
    skyrim_perf_activity(1);
    skyrim_lock_record(&e); /* Old interval crossing inactivity remains excluded. */
    e.start_ns=2000000001; e.end_ns=e.start_ns+100000000;
    skyrim_lock_record(&e);
    assert(skyrim_lock_take(&w) && w.count==1 && w.observed_count[0]==1);
    atomic_store(&fixture_now, 1);
    skyrim_perf_activity(1);

    errno=EINTR;
    assert(skyrim_lock_cpu_now_ns()==100000 && errno==EINTR);
    atomic_store(&fixture_cpu, 0);
    assert(skyrim_lock_cpu_now_ns()==0 && errno==EINTR);
    unsigned clocks_before=clock_calls;
    pthread_t threads[THREADS];
    for (uintptr_t i=0; i<THREADS; ++i) assert(!pthread_create(&threads[i], NULL, produce, (void *)i));
    uint64_t recorded=0, dropped=0, total=0, duration=0;
    while (atomic_load(&finished)<THREADS) {
        if (skyrim_lock_take(&w)) {
            recorded+=w.count; dropped+=w.dropped_busy+w.dropped_full;
            total+=w.observed_count[0]; duration+=w.observed_ns[0];
            for (unsigned i=0; i<w.count; ++i) {
                assert(w.events[i].phase==SKYRIM_LOCK_WAIT && w.events[i].thread>=1 && w.events[i].thread<=THREADS);
                assert(w.events[i].end_ns-w.events[i].start_ns==100000000);
            }
        }
    }
    for (unsigned i=0; i<THREADS; ++i) assert(!pthread_join(threads[i], NULL));
    assert(skyrim_lock_take(&w));
    recorded+=w.count; dropped+=w.dropped_busy+w.dropped_full;
    total+=w.observed_count[0]; duration+=w.observed_ns[0];
    assert(recorded+dropped==THREADS*ITERATIONS && total==THREADS*ITERATIONS);
    assert(duration==UINT64_C(100000000)*THREADS*ITERATIONS);
    assert(clock_calls==clocks_before);
    puts("concurrent_accounting=PASS");

    e=event(SKYRIM_LOCK_WAIT, 100000000);
    skyrim_lock_record(&e);
    atomic_store(&fixture_now, 0);
    errno=EINTR;
    skyrim_lock_log_window(); /* Failed clock must neither emit nor drain. */
    assert(errno==EINTR);
    assert(skyrim_lock_take(&w) && w.count==1);
    atomic_store(&fixture_now, 4000000000);
    errno=EINTR;
    skyrim_lock_log_window(); /* Empty rows expose coverage without claiming waits. */
    assert(errno==EINTR);
    for (unsigned i=0; i<SKYRIM_LOCK_CAPACITY; ++i) {
        e=event(SKYRIM_LOCK_WORK, 100000000);
        e.instance=UINT64_MAX; e.thread=UINT64_MAX; e.owner_thread=UINT64_MAX;
        e.start_ns=UINT64_MAX-100000000; e.end_ns=UINT64_MAX;
        e.cpu_ns=100000000; e.cpu_valid=1;
        e.operation=SKYRIM_LOCK_OP_AUDIO_RESET; e.owner_operation=SKYRIM_LOCK_OP_AUDIO_RESET;
        skyrim_lock_record(&e);
    }
    atomic_store(&fixture_now, 3000000000);
    skyrim_lock_log_window(); /* Regressing clock also preserves pending data. */
    assert(errno==EINTR);
    atomic_store(&fixture_now, UINT64_MAX);
    skyrim_lock_log_window();
    assert(errno==EINTR);
    puts("bounds_lifecycle_errno=PASS");
    return 0;
}
