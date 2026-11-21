/* Authored checks for the Classic VM generation. */
#define _POSIX_C_SOURCE 200809L
#include "classic_vm_epoch.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>

static uint64_t retired[64];
static unsigned retired_count;
static unsigned retired_second;
static bool commit_finished;  /* guarded by the epoch lock in both writers */
static bool retire_saw_commit_finished;

static void record(void *context, uint64_t old) {
    assert(context == &retired);
    assert(retired_count < 64);
    retired[retired_count++] = old;
    retire_saw_commit_finished = commit_finished;
}
static void second(void *context, uint64_t old) { (void)context; (void)old; ++retired_second; }
static void ignore(void *context, uint64_t old) { (void)context; (void)old; }

static void pause_ms(long ms) {
    struct timespec t = {0, ms * 1000000L};
    nanosleep(&t, NULL);
}

static atomic_int committer_inside;
static SkyrimVmScope held_scope;

static void *committer(void *unused) {
    (void)unused;
    assert(skyrim_vm_commit_begin(&held_scope));
    atomic_store(&committer_inside, 1);
    pause_ms(50);  /* the revert below must wait for this */
    commit_finished = true;
    skyrim_vm_commit_end();
    return NULL;
}

/* Stress: commits never observe a revert in progress or a stale generation. */
static uint64_t shared_generation;  /* written only by the retire callback, under the lock */
static atomic_int stop;
static atomic_ulong accepted, refused;

static void stress_retire(void *context, uint64_t old) {
    (void)context;
    assert(old == shared_generation);
    ++shared_generation;
}

static void *stress_committer(void *unused) {
    (void)unused;
    while (!atomic_load(&stop)) {
        SkyrimVmScope scope;
        if (!skyrim_vm_scope_current(&scope)) continue;
        if (skyrim_vm_commit_begin(&scope)) {
            assert(shared_generation == scope.epoch);
            atomic_fetch_add(&accepted, 1);
            skyrim_vm_commit_end();
        } else {
            atomic_fetch_add(&refused, 1);
        }
    }
    return NULL;
}

int main(int argc, char **argv) {
    assert(argc == 2);
    SkyrimVmScope scope = {0}, old = {0};
    SkyrimVmEpochStats stats;
    if (argv[1][0] == 's') {
        /* Stress run, its own process. */
        assert(skyrim_vm_epoch_subscribe(stress_retire, NULL));
        skyrim_vm_epoch_registered(0x1000);
        shared_generation = 1;
        pthread_t threads[4];
        for (int n = 0; n < 4; ++n) assert(!pthread_create(&threads[n], NULL, stress_committer, NULL));
        for (int n = 0; n < 2000; ++n) {
            skyrim_vm_epoch_before_revert(n & 1 ? SKYRIM_VM_REVERT_LOAD : SKYRIM_VM_REVERT_SCRIPT_RELOAD);
            skyrim_vm_epoch_after_load(n % 7 != 0);
        }
        atomic_store(&stop, 1);
        for (int n = 0; n < 4; ++n) assert(!pthread_join(threads[n], NULL));
        skyrim_vm_epoch_stats(&stats);
        assert(stats.epoch == 2001 && stats.reverts == 2000);
        printf("accepted=%lu refused=%lu\n", (unsigned long)atomic_load(&accepted),
               (unsigned long)atomic_load(&refused));
        return 0;
    }

    /* Unregistered: nothing is current, nothing commits. */
    assert(!skyrim_vm_scope_current(&scope));
    assert(!skyrim_vm_scope_pending_load(&scope));
    assert(!skyrim_vm_commit_begin(&scope));
    skyrim_vm_commit_end();  /* without begin: no effect */
    assert(!skyrim_vm_epoch_subscribe(NULL, NULL));
    assert(skyrim_vm_epoch_subscribe(record, &retired));
    assert(skyrim_vm_epoch_subscribe(second, NULL));
    for (int n = 2; n < SKYRIM_VM_EPOCH_MAX_SUBSCRIBERS; ++n) assert(skyrim_vm_epoch_subscribe(ignore, NULL));
    assert(!skyrim_vm_epoch_subscribe(ignore, NULL));  /* table full */

    skyrim_vm_epoch_registered(0);
    assert(!skyrim_vm_scope_current(&scope));
    skyrim_vm_epoch_registered(0x1000);
    assert(skyrim_vm_scope_current(&scope) && scope.epoch == 1 && scope.registry == 0x1000);
    assert(!skyrim_vm_scope_pending_load(&scope));
    skyrim_vm_epoch_registered(0x1000);  /* the same VM again */
    assert(skyrim_vm_scope_current(&old) && old.epoch == 1 && retired_count == 0);

    /* A commit in the current generation. Nested begin on one thread is refused. */
    assert(skyrim_vm_commit_begin(&scope));
    assert(!skyrim_vm_commit_begin(&scope));
    skyrim_vm_commit_end();

    /* A revert in the middle of a native call: its result is refused, also after the load. */
    old = scope;
    skyrim_vm_epoch_before_revert(SKYRIM_VM_REVERT_LOAD);
    assert(retired_count == 1 && retired[0] == 1 && retired_second == 1);
    assert(!skyrim_vm_scope_current(&scope));
    SkyrimVmScope pending;
    assert(skyrim_vm_scope_pending_load(&pending) && pending.epoch == 2 && pending.registry == 0x1000);
    assert(!skyrim_vm_commit_begin(&old));
    skyrim_vm_epoch_after_load(1);
    assert(skyrim_vm_scope_current(&scope) && scope.epoch == 2);
    assert(skyrim_vm_commit_begin(&pending));
    skyrim_vm_commit_end();
    assert(!skyrim_vm_commit_begin(&old));

    /* A failed load stays closed until a later revert and successful load. */
    skyrim_vm_epoch_before_revert(SKYRIM_VM_REVERT_SCRIPT_RELOAD);
    skyrim_vm_epoch_after_load(0);
    assert(!skyrim_vm_scope_current(&scope));
    skyrim_vm_epoch_registered(0x1000);
    assert(!skyrim_vm_scope_current(&scope));
    skyrim_vm_epoch_after_load(0);
    assert(!skyrim_vm_scope_current(&scope));
    skyrim_vm_epoch_before_revert(SKYRIM_VM_REVERT_LOAD);
    skyrim_vm_epoch_after_load(1);
    assert(skyrim_vm_scope_current(&scope) && scope.epoch == 4);
    assert(retired_count == 3 && retired[1] == 2 && retired[2] == 3 && retired_second == 3);

    /* Another registry: earlier scopes are stale and the subscribers run once. */
    old = scope;
    skyrim_vm_epoch_registered(0x2000);
    assert(retired_count == 4 && retired[3] == 4);
    assert(!skyrim_vm_commit_begin(&old));
    assert(skyrim_vm_scope_current(&scope) && scope.registry == 0x2000 && scope.epoch == 5);
    SkyrimVmScope forged = {scope.epoch, 0x1000};
    assert(!skyrim_vm_commit_begin(&forged));

    /* Subscribing after start is refused. */
    assert(!skyrim_vm_epoch_subscribe(ignore, NULL));

    /* A revert waits for a commit in progress and then retires its generation. */
    held_scope = scope;
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, committer, NULL));
    while (!atomic_load(&committer_inside)) pause_ms(1);
    skyrim_vm_epoch_before_revert(SKYRIM_VM_REVERT_LOAD);
    assert(retire_saw_commit_finished);
    assert(!pthread_join(thread, NULL));
    assert(!skyrim_vm_commit_begin(&held_scope));
    skyrim_vm_epoch_after_load(1);

    /* Generations only increase. */
    for (unsigned n = 1; n < retired_count; ++n) assert(retired[n] > retired[n - 1]);
    skyrim_vm_epoch_stats(&stats);
    assert(stats.epoch == 6 && stats.open && stats.reverts == 4 && stats.failed_loads == 2);
    assert(stats.refused_commits == 6);
    return 0;
}
