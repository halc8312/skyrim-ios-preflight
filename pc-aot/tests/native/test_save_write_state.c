/* Candidate classification and concurrent recording; authored filenames only. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "save_write_probe.h"
static SkyrimSaveWriteProbe shared;
static void *writer(void *unused) {
    (void)unused;
    for(unsigned i=0;i<1000;++i) {
        errno=EINTR;
        uint64_t start=skyrim_save_write_begin(&shared);
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
        skyrim_save_write_close(&shared);
    }
    char bound[]={'.','e','s','s'};
    skyrim_save_write_init(&shared,bound,sizeof bound);assert(!shared.candidate);
    skyrim_save_write_init(&shared,NULL,0);assert(!shared.candidate);
    skyrim_save_write_init(&shared,bound,0);assert(!shared.candidate);
    skyrim_save_write_init(&shared,"authored.ess",13);
    pthread_t threads[4];
    for(unsigned i=0;i<4;++i)assert(!pthread_create(&threads[i],NULL,writer,NULL));
    for(unsigned i=0;i<4;++i)assert(!pthread_join(threads[i],NULL));
    SkyrimPerfWindow w;skyrim_perf_take(SKYRIM_PERF_SAVE_CANDIDATE_WRITE,&w);
    assert(count(&w)==4000 && w.failures==4);
    /* Force an authored long lifetime without sleeping or touching real files. */
    uint64_t now=skyrim_perf_now_ns();assert(now>100000000);
    atomic_store(&shared.first_observed_ns,now-100000000);
    skyrim_save_write_close(&shared);skyrim_save_write_close(&shared);
    SkyrimPerfStalls spans;assert(skyrim_perf_stalls_take(&spans));
    unsigned windows=0;
    for(unsigned i=0;i<spans.count;++i)if(spans.spans[i].kind==SKYRIM_PERF_SAVE_CANDIDATE_WINDOW) {
        ++windows;assert(spans.spans[i].thread==0 && !spans.spans[i].estimated_end);
    }
    assert(windows==1);
    skyrim_save_write_init(&shared,"fresh.ess",10);skyrim_save_write_close(&shared);
    for(unsigned kind=SKYRIM_PERF_WRITE_FILE;kind<SKYRIM_PERF_SAVE_CANDIDATE_WINDOW;++kind)
        skyrim_perf_record(kind,10,1,0);
    skyrim_perf_log_window();
    puts("save_candidate_state=PASS concurrent_writes=4000");
    return 0;
}
