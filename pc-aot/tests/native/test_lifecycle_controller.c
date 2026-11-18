/* Authored scheduling model. No Vulkan/Metal or iPhone success is implied. */
#include "lifecycle_controller.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>

static SkyrimLifecycleController state;
static atomic_int entered, release_producer, produced, consumer_complete, stop;
static void nap(void) { struct timespec t = {0, 1000000}; nanosleep(&t, NULL); }
static void wait_flag(atomic_int *flag) {
    for (unsigned i = 0; i < 3000 && !atomic_load(flag); ++i) nap();
    assert(atomic_load(flag));
}
static void *admitted_producer(void *unused) {
    (void)unused;
    assert(skyrim_lifecycle_controller_enter(&state) == 1);
    atomic_store(&entered, 1);
    wait_flag(&release_producer);
    /* Existing work bypasses frontend admission. Closing a native commit gate
     * here would strand the previously enqueued consumer, the old failure. */
    atomic_store(&produced, 1);
    assert(skyrim_lifecycle_controller_leave(&state) == 1);
    return NULL;
}
static void *consumer(void *unused) {
    (void)unused;
    wait_flag(&produced);
    atomic_store(&consumer_complete, 1);
    return NULL;
}
static void boundary_and_resume_races(void) {
    skyrim_lifecycle_controller_init(&state);
    pthread_t p, c;
    assert(!pthread_create(&p, NULL, admitted_producer, NULL));
    assert(!pthread_create(&c, NULL, consumer, NULL));
    wait_flag(&entered);
    assert(skyrim_lifecycle_controller_request(&state, 0));
    assert(skyrim_lifecycle_controller_enter(&state) == 0);
    assert(skyrim_lifecycle_controller_claim(&state) == 0);
    SkyrimLifecycleSnapshot s = skyrim_lifecycle_controller_snapshot(&state);
    /* This exact state at didEnterBackground must be reported as deadline
     * missed. Async work merely started is not a completed safe suspension. */
    assert(s.phase == SKYRIM_LIFECYCLE_CLOSING && s.active_calls == 1);
    atomic_store(&release_producer, 1);
    assert(!pthread_join(p, NULL)); assert(!pthread_join(c, NULL));
    assert(atomic_load(&consumer_complete));
    uint32_t first = skyrim_lifecycle_controller_claim(&state); assert(first == 1);
    assert(skyrim_lifecycle_controller_request(&state, 1));
    assert(skyrim_lifecycle_controller_enter(&state) == 0); /* drain still owns state */
    assert(skyrim_lifecycle_controller_snapshot(&state).phase == SKYRIM_LIFECYCLE_DRAINING);
    assert(skyrim_lifecycle_controller_complete(&state, first, 0));
    assert(skyrim_lifecycle_controller_enter(&state) == 1);
    assert(skyrim_lifecycle_controller_leave(&state) == 0);
    /* Resume before claim cancels the pause without launching a needless GPU
     * wait; a delayed worker notification cannot close admission again. */
    assert(skyrim_lifecycle_controller_request(&state, 0));
    assert(skyrim_lifecycle_controller_request(&state, 1));
    assert(!skyrim_lifecycle_controller_claim(&state));
    assert(skyrim_lifecycle_controller_request(&state, 0));
    uint32_t second = skyrim_lifecycle_controller_claim(&state); assert(second == 2);
    assert(!skyrim_lifecycle_controller_complete(&state, first, 0));
    assert(skyrim_lifecycle_controller_request(&state, 1));
    assert(skyrim_lifecycle_controller_request(&state, 0));
    assert(skyrim_lifecycle_controller_complete(&state, second, 0));
    assert(skyrim_lifecycle_controller_snapshot(&state).phase == SKYRIM_LIFECYCLE_PAUSED);
    assert(skyrim_lifecycle_controller_enter(&state) == 0);
    assert(!skyrim_lifecycle_controller_claim(&state));
    assert(skyrim_lifecycle_controller_request(&state, 1));
    assert(skyrim_lifecycle_controller_request(&state, 0));
    uint32_t third = skyrim_lifecycle_controller_claim(&state); assert(third == 3);
    assert(skyrim_lifecycle_controller_complete(&state, third, -4));
    skyrim_lifecycle_controller_request(&state, 1);
    assert(skyrim_lifecycle_controller_snapshot(&state).phase == SKYRIM_LIFECYCLE_FAILED);
    assert(skyrim_lifecycle_controller_enter(&state) == -1);
    puts("lifecycle producer_dependency=PASS rapid_resume=PASS stale_completion=PASS explicit_failure=PASS");
}
static atomic_uint completed_calls, drains;
static void *caller(void *unused) {
    (void)unused;
    while (!atomic_load(&stop)) {
        int admitted = skyrim_lifecycle_controller_enter(&state);
        assert(admitted >= 0);
        if (!admitted) { nap(); continue; }
        /* The count must remain visible across every admitted operation. */
        assert(skyrim_lifecycle_controller_snapshot(&state).active_calls > 0);
        atomic_fetch_add(&completed_calls, 1);
        assert(skyrim_lifecycle_controller_leave(&state) >= 0);
    }
    return NULL;
}
static void *drainer(void *unused) {
    (void)unused;
    while (!atomic_load(&stop)) {
        uint32_t generation = skyrim_lifecycle_controller_claim(&state);
        if (!generation) { nap(); continue; }
        assert(skyrim_lifecycle_controller_snapshot(&state).active_calls == 0);
        nap();
        assert(skyrim_lifecycle_controller_snapshot(&state).active_calls == 0);
        assert(skyrim_lifecycle_controller_complete(&state, generation, 0));
        atomic_fetch_add(&drains, 1);
    }
    return NULL;
}
static void concurrency(void) {
    skyrim_lifecycle_controller_init(&state);
    pthread_t threads[9];
    for (unsigned i = 0; i < 8; ++i) assert(!pthread_create(&threads[i], NULL, caller, NULL));
    assert(!pthread_create(&threads[8], NULL, drainer, NULL));
    for (unsigned i = 0; i < 100; ++i) {
        skyrim_lifecycle_controller_request(&state, 0);
        nap(); nap();
        skyrim_lifecycle_controller_request(&state, 1);
        nap();
    }
    skyrim_lifecycle_controller_request(&state, 1);
    atomic_store(&stop, 1);
    for (unsigned i = 0; i < 9; ++i) assert(!pthread_join(threads[i], NULL));
    assert(atomic_load(&completed_calls) > 100 && atomic_load(&drains) > 0);
    assert(skyrim_lifecycle_controller_snapshot(&state).active_calls == 0);
    puts("lifecycle concurrent8=PASS admission_drain_exclusive=PASS iphone_verified=0");
}
int main(void) { boundary_and_resume_races(); concurrency(); }
