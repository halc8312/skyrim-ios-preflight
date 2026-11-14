/* Authored handles and pthread waiters around the actual public runtime code. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { H_FREE, H_FILE, H_EVENT, H_MUTEX, H_SEM, H_THREAD };
#define WAIT_TIMEOUT 0x102u
#define LOG(...) ((void)0)
static _Noreturn void rt_fatal(const char *s) { (void)s; abort(); }
static __thread uint32_t test_tid = 1;
static uint32_t cur_tid(void) { return test_tid; }
static uint64_t comparisons, signals;
static int observed_signal(pthread_cond_t *cv) {
    ++signals;
    return pthread_cond_signal(cv);
}
#define pthread_cond_signal observed_signal
/* ACTUAL_HANDLES_AND_WAITS */
#undef pthread_cond_signal

static uint64_t ns(void) {
    struct timespec t; assert(!clock_gettime(CLOCK_MONOTONIC, &t));
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
static void sleep_ms(unsigned n) {
    struct timespec t = {n / 1000, (long)(n % 1000) * 1000000};
    while (nanosleep(&t, &t) && errno == EINTR) {}
}
static uint32_t make(int kind, int manual, int value) {
    Waitable *w = w_new(kind); assert(w);
    w->manual = manual; w->signaled = value; w->count = value; w->max = 64;
    return h_new(kind, w);
}
static Waitable *get(uint32_t h) { return h_get(h, -1); }
static void member_check_locked(void) {
#if FIXTURE_NEW
    for (unsigned h = 1; h < MAX_HANDLES; ++h) {
        if (!g_handles[h].type) continue;
        Waitable *w = g_handles[h].obj;
        uint32_t count = 0;
        for (Waiter *t = g_waiters; t; t = t->next)
            for (uint32_t i = 0; i < t->n; ++i) count += t->objs[i] == w;
        assert(w->waiting_entries == count || w->waiting_entries == UINT32_MAX);
    }
#endif
}
static void await_waiters(unsigned expected) {
    uint64_t deadline = ns() + 2000000000u;
    for (;;) {
        pthread_mutex_lock(&g_wlock);
        unsigned n = 0; for (Waiter *t = g_waiters; t; t = t->next) ++n;
        member_check_locked();
        pthread_mutex_unlock(&g_wlock);
        if (n == expected) return;
        assert(ns() < deadline); sleep_ms(1);
    }
}
static void release(uint32_t h, int count) {
    pthread_mutex_lock(&g_wlock);
    Waitable *w = get(h);
    if (w->kind == H_MUTEX) w->owner = 0;
    else if (w->kind == H_SEM) w->count += count;
    else w->signaled = 1;
    wake_waiters(w); member_check_locked();
    pthread_mutex_unlock(&g_wlock);
}
typedef struct { uint32_t handles[64], n, all, ms, result, tid; } Task;
static void cleanup_tls(void) {
    if (t_waiter) { assert(!pthread_cond_destroy(&t_waiter->cv)); free(t_waiter); t_waiter = NULL; }
}
static void *run_task(void *p) {
    Task *t = p; test_tid = t->tid;
    t->result = wait_objects(t->n, t->handles, t->all, t->ms);
    cleanup_tls(); return NULL;
}
static pthread_t start(Task *task, uint32_t h, uint32_t ms) {
    *task = (Task){.handles = {h}, .n = 1, .ms = ms, .tid = 42};
    pthread_t p; assert(!pthread_create(&p, NULL, run_task, task)); return p;
}
static void join(pthread_t p, Task *t, uint32_t result) {
    assert(!pthread_join(p, NULL)); assert(t->result == result);
    pthread_mutex_lock(&g_wlock); member_check_locked(); pthread_mutex_unlock(&g_wlock);
}
static void basic(void) {
    uint32_t a = make(H_EVENT, 0, 1), b = make(H_EVENT, 1, 1);
    assert(wait_objects(1, &a, 0, 0) == 0);
    assert(wait_objects(1, &a, 0, 0) == WAIT_TIMEOUT);
    for (unsigned i = 0; i < 8; ++i) assert(wait_objects(1, &b, 0, 0) == 0);
    uint32_t sem = make(H_SEM, 0, 2), mutex = make(H_MUTEX, 0, 0);
    assert(wait_objects(1, &sem, 0, 0) == 0); assert(wait_objects(1, &sem, 0, 0) == 0);
    assert(wait_objects(1, &sem, 0, 0) == WAIT_TIMEOUT);
    assert(wait_objects(1, &mutex, 0, 0) == 0); assert(wait_objects(1, &mutex, 0, 0) == 0);
    assert(get(mutex)->recursion == 2 && get(mutex)->owner == 1);
    uint32_t invalid = 7; assert(wait_objects(1, &invalid, 0, 0) == UINT32_MAX);
    Task t; pthread_t p = start(&t, a, 4); join(p, &t, WAIT_TIMEOUT);
    p = start(&t, a, UINT32_MAX); await_waiters(1);
    pthread_mutex_lock(&g_wlock);
    assert(!pthread_cond_signal(&g_waiters->cv)); /* spurious return must reregister */
    pthread_mutex_unlock(&g_wlock);
    sleep_ms(3); await_waiters(1); w_signal(get(a)); join(p, &t, 0);
    assert(!g_waiters);
    for (int kind = H_EVENT; kind <= H_THREAD; ++kind) {
        uint32_t h = make(kind, 1, 0); if (kind == H_MUTEX) get(h)->owner = 1;
        p = start(&t, h, 2000); await_waiters(1); release(h, 1); join(p, &t, 0);
    }
    /* WaitAny keeps input order; WaitAll must not consume an early semaphore. */
    uint32_t h0 = make(H_SEM, 0, 0), h1 = make(H_EVENT, 1, 0);
    t = (Task){.handles = {h0,h1}, .n = 2, .ms = 2000, .tid = 42};
    assert(!pthread_create(&p, NULL, run_task, &t)); await_waiters(1);
    w_signal(get(h1)); join(p, &t, 1); get(h1)->signaled = 0;
    t = (Task){.handles = {h0,h1}, .n = 2, .all = 1, .ms = 2000, .tid = 42};
    assert(!pthread_create(&p, NULL, run_task, &t)); await_waiters(1);
    release(h0, 1); sleep_ms(3); await_waiters(1);
    pthread_mutex_lock(&g_wlock); assert(get(h0)->count == 1); pthread_mutex_unlock(&g_wlock);
    w_signal(get(h1)); join(p, &t, 0); assert(get(h0)->count == 0);
    /* Duplicate entries are counted and removed symmetrically. */
    get(h1)->signaled = 0;
    t = (Task){.handles = {h1,h1}, .n = 2, .all = 1, .ms = 2000, .tid = 42};
    assert(!pthread_create(&p, NULL, run_task, &t)); await_waiters(1);
    w_signal(get(h1)); join(p, &t, 0);
    get(h1)->signaled = 0;
    t = (Task){.handles = {h1,h1}, .n = 2, .all = 1, .ms = 4, .tid = 42};
    assert(!pthread_create(&p, NULL, run_task, &t)); join(p, &t, WAIT_TIMEOUT);
    get(h0)->count = 1;
    t = (Task){.handles = {h0,h1}, .n = 2, .all = 1, .ms = 4, .tid = 42};
    assert(!pthread_create(&p, NULL, run_task, &t)); join(p, &t, WAIT_TIMEOUT);
    assert(get(h0)->count == 1);
    /* One manual event and one thread object each release every matching waiter. */
    for (int kind = H_EVENT; kind <= H_THREAD; kind += H_THREAD-H_EVENT) {
        uint32_t h = make(kind, 1, 0); Task tasks[8]; pthread_t threads[8];
        for (int i=0;i<8;++i) threads[i] = start(&tasks[i], h, 2000);
        await_waiters(8); release(h, 1);
        for (int i=0;i<8;++i) join(threads[i], &tasks[i], 0);
    }
#if FIXTURE_NEW
    /* Sticky saturation cannot wrap to false zero, even after unregister. */
    uint32_t sat = make(H_EVENT, 0, 0); get(sat)->waiting_entries = UINT32_MAX - 1;
    p = start(&t, sat, 2000); /* synthetic preexisting membership; don't use consistency checker */
    for (;;) {
        pthread_mutex_lock(&g_wlock); int ready = g_waiters != NULL; pthread_mutex_unlock(&g_wlock);
        if (ready) break;
        sleep_ms(1);
    }
    w_signal(get(sat)); join(p, &t, 0); assert(get(sat)->waiting_entries == UINT32_MAX);
#endif
}
typedef struct { uint32_t request, answer; } Pair;
static void *ping_pong(void *p) {
    Pair *pair = p; test_tid = 77;
    for (unsigned i = 0; i < 200; ++i) {
        assert(wait_objects(1, &pair->request, 0, 2000) == 0);
        w_signal(get(pair->answer));
    }
    cleanup_tls(); return NULL;
}
static void stress(void) {
    Pair pairs[8]; pthread_t threads[8];
    for (unsigned i = 0; i < 8; ++i) {
        pairs[i] = (Pair){make(H_EVENT,0,0), make(H_EVENT,0,0)};
        assert(!pthread_create(&threads[i], NULL, ping_pong, &pairs[i]));
    }
    for (unsigned round = 0; round < 200; ++round) {
        for (unsigned i=0;i<8;++i) w_signal(get(pairs[i].request));
        for (unsigned i=0;i<8;++i) assert(wait_objects(1,&pairs[i].answer,0,2000)==0);
    }
    for (unsigned i=0;i<8;++i) assert(!pthread_join(threads[i],NULL));
    await_waiters(0);
}
static void scans(unsigned n, unsigned width, int match) {
    Waitable objects[65] = {{0}}; Waiter waiters[64]; Waitable *lists[64][64];
    memset(waiters,0,sizeof waiters);
    for (unsigned i=0;i<n;++i) {
        assert(!pthread_cond_init(&waiters[i].cv,NULL)); waiters[i].n=width; waiters[i].objs=lists[i];
        waiters[i].next=i+1<n?&waiters[i+1]:NULL;
        for (unsigned j=0;j<width;++j) {
            lists[i][j]=&objects[match && j+1==width?64:j];
#if FIXTURE_NEW
            ++lists[i][j]->waiting_entries;
#endif
        }
    }
    pthread_mutex_lock(&g_wlock); assert(!g_waiters); g_waiters=n?waiters:NULL;
    comparisons=signals=0;
    for(unsigned i=0;i<1000;++i)wake_waiters(&objects[64]);
    uint64_t expected=(uint64_t)n*width*1000;
    assert(comparisons == (FIXTURE_NEW && !match ? 0 : expected));
    assert(signals == (match ? (uint64_t)n*1000 : 0));
    printf("SCAN waiters=%u width=%u match=%d comparisons=%llu signals=%llu\n",n,width,match,
           (unsigned long long)comparisons,(unsigned long long)signals);
    g_waiters=NULL; pthread_mutex_unlock(&g_wlock);
    for (unsigned i=0;i<n;++i) assert(!pthread_cond_destroy(&waiters[i].cv));
}
int main(void) {
    basic(); stress();
    scans(0,1,0); scans(8,1,0); scans(32,4,0); scans(64,64,0); scans(32,4,1);
    cleanup_tls();
    for(unsigned i=1;i<MAX_HANDLES;++i) free(g_handles[i].obj);
    (void)g_wcond;
    puts("WAIT_MEMBERSHIP event_mutex_semaphore_thread_any_all_timeout_spurious_duplicate=PASS handshakes=1600");
    return 0;
}
