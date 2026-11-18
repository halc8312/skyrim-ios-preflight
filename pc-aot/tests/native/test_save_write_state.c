/* Candidate classification and concurrent recording; authored filenames only. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "save_write_probe.h"
static SkyrimSaveWriteProbe shared;
static void *writer(void *result) {
    uint64_t *latest = result;
    for(unsigned i=0;i<1000;++i) {
        errno=EINTR;
        uint64_t start=skyrim_save_write_begin(&shared);
        if (start > *latest) *latest = start;
        skyrim_save_write_end(shared.candidate,start,i==0,0);
        assert(errno==EINTR);
    }
    return NULL;
}
static uint64_t count(SkyrimPerfWindow *w) {
    uint64_t n=0;for(unsigned i=0;i<SKYRIM_PERF_BUCKET_COUNT;++i)n+=w->buckets[i];return n;
}
int main(void) {
    const char *paths[]={"A.ess","/Saves/QUICK.ESS.TMP","other.ess.bak", "shader.bin",
                        "a.ess.extra", "/a.ess/file.bin", "", "ess", "a.es"};
    for(unsigned i=0;i<sizeof paths/sizeof paths[0];++i) {
        skyrim_save_write_init(&shared,paths[i],strlen(paths[i])+1);
        assert(shared.candidate==(i<3));
        assert(skyrim_save_write_close_begin(&shared) == 0);
        skyrim_save_write_close(&shared, 0);
    }
    char bound[]={'.','e','s','s'};
    skyrim_save_write_init(&shared,bound,sizeof bound);assert(!shared.candidate);
    skyrim_save_write_init(&shared,NULL,0);assert(!shared.candidate);
    skyrim_save_write_init(&shared,bound,0);assert(!shared.candidate);
    skyrim_save_write_init(&shared,"authored.ess",13);
    pthread_t threads[4];
    uint64_t latest[4] = {0};
    for(unsigned i=0;i<4;++i)assert(!pthread_create(&threads[i],NULL,writer,&latest[i]));
    for(unsigned i=0;i<4;++i)assert(!pthread_join(threads[i],NULL));
    uint64_t last = 0;
    for(unsigned i=0;i<4;++i) if(latest[i]>last) last=latest[i];
    assert(last && last == atomic_load(&shared.last_write_start_ns));
    assert(skyrim_save_write_close_begin(&shared) >= last);
    SkyrimPerfWindow w;skyrim_perf_take(SKYRIM_PERF_SAVE_CANDIDATE_WRITE,&w);
    assert(count(&w)==4000 && w.failures==4);
    /* Force an authored long lifetime without sleeping or touching real files. */
    uint64_t now=skyrim_perf_now_ns();assert(now>400000000);
    atomic_store(&shared.first_observed_ns,now-400000000);
    atomic_store(&shared.last_write_start_ns,now-280000000);
    skyrim_save_write_close(&shared, now-180000000);
    skyrim_save_write_close(&shared, 0);
    skyrim_perf_take(SKYRIM_PERF_SAVE_CANDIDATE_WINDOW, &w);
    assert(count(&w)==1);
    const uint64_t whole = w.total_ns;
    skyrim_perf_take(SKYRIM_PERF_SAVE_FIRST_TO_LAST_WRITE_START, &w);
    assert(count(&w)==1 && w.total_ns==120000000);
    skyrim_perf_take(SKYRIM_PERF_SAVE_LAST_WRITE_START_TO_CLOSE, &w);
    assert(count(&w)==1 && w.total_ns==100000000);
    skyrim_perf_take(SKYRIM_PERF_SAVE_CLOSE_CALL, &w);
    assert(count(&w)==1 && w.total_ns>=180000000 && w.total_ns+220000000==whole);
    SkyrimPerfStalls spans;assert(skyrim_perf_stalls_take(&spans));
    unsigned windows=0, close_calls=0;
    for(unsigned i=0;i<spans.count;++i) {
        SkyrimPerfStall *s=&spans.spans[i];
        if(s->kind==SKYRIM_PERF_SAVE_CANDIDATE_WINDOW || s->kind==SKYRIM_PERF_SAVE_FIRST_TO_LAST_WRITE_START ||
           s->kind==SKYRIM_PERF_SAVE_LAST_WRITE_START_TO_CLOSE) {
            ++windows;assert(s->thread==0 && !s->estimated_end && !s->context.role);
        } else if(s->kind==SKYRIM_PERF_SAVE_CLOSE_CALL) {
            ++close_calls;assert(s->thread==skyrim_perf_thread_ordinal() && !s->estimated_end);
        }
    }
    assert(windows==3 && close_calls==1);
    /* Missing descriptor-close boundary (shared fd), then a write observed
     * after the boundary: keep the old window without inventing a partition. */
    for(unsigned i=0; i<2; ++i) {
        skyrim_save_write_init(&shared,"fresh.ess",10);
        const uint64_t start=skyrim_save_write_begin(&shared);
        skyrim_save_write_close(&shared, i ? start-1 : 0);
    }
    for(unsigned kind=SKYRIM_PERF_SAVE_FIRST_TO_LAST_WRITE_START;kind<=SKYRIM_PERF_SAVE_CLOSE_CALL;++kind) {
        skyrim_perf_take(kind,&w);assert(count(&w)==0);
    }
    for(unsigned kind=SKYRIM_PERF_WRITE_FILE;kind<SKYRIM_PERF_SAVE_CANDIDATE_WINDOW;++kind)
        skyrim_perf_record(kind,10,1,0);
    skyrim_perf_log_window();
    puts("save_candidate_state=PASS concurrent_writes=4000");
    return 0;
}
