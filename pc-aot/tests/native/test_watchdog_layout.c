/* Authored guest memory/registers. Production recorder inserted by Python. */
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
typedef struct { uint32_t r[8], fs_base; } CPU;
static uint32_t memory[65536];
#define M32(at) memory[(uint32_t)(at) / 4]
static __thread uint32_t own_tid;
static uint64_t rt_host_thread_id(void) { return own_tid + 700; }
/* ACTUAL_RECORDER */
static pthread_mutex_t start_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t start_cv = PTHREAD_COND_INITIALIZER;
static unsigned ready, workers = 8, iterations = 20000;
static int go;
static int slots[64];
static int own_thread_matches[64];
static const char label[] = "authored-import";
static void *worker(void *arg) {
    unsigned n = (unsigned)(uintptr_t)arg; own_tid = n;
    CPU cpu = {{0}, 16384 + n * 128};
    cpu.r[4] = 4096 + n * 128;
    wd_note(&cpu, label); slots[n] = t_wd;
    own_thread_matches[n] = pthread_equal(g_wd[t_wd].th, pthread_self());
    if (EXPECT_ALIGNED) {
        assert((uintptr_t)&g_wd[t_wd] % 128 == 0);
        assert(sizeof g_wd[t_wd] % 128 == 0);
    }
    pthread_mutex_lock(&start_lock);
    ++ready; pthread_cond_broadcast(&start_cv);
    while (!go) pthread_cond_wait(&start_cv, &start_lock);
    pthread_mutex_unlock(&start_lock);
    for (unsigned k = 0; k < iterations; ++k) {
        cpu.r[6] = k ^ n; cpu.r[3] = k + n;
        // Keep the actual per-call register stores visible to the optimizer,
        // as they are in rt_call across unrelated shim/callback execution.
        __asm__ __volatile__("" : : "r"(&cpu) : "memory");
        wd_note(&cpu, label);
    }
    return NULL;
}
int main(int argc, char **argv) {
    if (argc == 3) { workers = (unsigned)strtoul(argv[1], NULL, 10); iterations = (unsigned)strtoul(argv[2], NULL, 10); }
    assert(workers >= 1 && workers < WD_MAX && iterations > 0);
    pthread_t threads[64];
    for (unsigned n = 0; n < workers; ++n) {
        M32(4096 + n * 128) = 0x12340000u + n;
        M32(16384 + n * 128 + 0x24) = 900 + n;
        assert(pthread_create(&threads[n], NULL, worker, (void *)(uintptr_t)n) == 0);
    }
    pthread_mutex_lock(&start_lock);
    while (ready < workers) pthread_cond_wait(&start_cv, &start_lock);
    struct timespec before, after; clock_gettime(CLOCK_MONOTONIC, &before);
    go = 1; pthread_cond_broadcast(&start_cv); pthread_mutex_unlock(&start_lock);
    for (unsigned n = 0; n < workers; ++n) assert(pthread_join(threads[n], NULL) == 0);
    clock_gettime(CLOCK_MONOTONIC, &after);
    for (unsigned n = 0; n < workers; ++n) {
        int slot = slots[n]; assert(slot >= 0 && slot < (int)workers);
        assert(g_wd[slot].name == label && g_wd[slot].ret == 0x12340000u + n);
        assert(g_wd[slot].calls == iterations + 1);
        assert(g_wd[slot].tid == n + 700 && g_wd[slot].gtid == n + 900);
        assert(g_wd[slot].esi == ((iterations - 1) ^ n) && g_wd[slot].ebx == iterations - 1 + n);
        assert(g_wd[slot].esp == 4096 + n * 128 && own_thread_matches[n]);
        for (unsigned j = 0; j < n; ++j) assert(slot != slots[j]);
    }
    // The original shared overflow slot policy is not changed or tested as
    // concurrent-safe. Check its sequential selection and counter wrap only.
    t_wd = -1; g_wd_n = WD_MAX; own_tid = 99;
    CPU cpu = {{0}, 0}; g_wd[WD_MAX - 1].calls = UINT32_MAX;
    wd_note(&cpu, label);
    assert(t_wd == WD_MAX - 1 && g_wd[WD_MAX - 1].calls == 0);
    if (argc == 3)
        printf("%.3f\n", ((after.tv_sec - before.tv_sec) * 1e9 + after.tv_nsec - before.tv_nsec) / workers / iterations);
    else puts("thread_fields=PASS unsigned_wrap=PASS concurrent_writers=8 unchanged_recording=PASS");
}
