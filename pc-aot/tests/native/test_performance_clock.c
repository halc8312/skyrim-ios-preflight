/* Exercise the real recorder with successful and failed authored clock calls. */
#include <assert.h>
#include <errno.h>
#include <time.h>
static int fail_clock;
static int fixture_clock_gettime(clockid_t clock, struct timespec *now) {
    assert(clock==CLOCK_MONOTONIC);
    errno=EIO;
    if(fail_clock) return -1;
    now->tv_sec=7;now->tv_nsec=123;return 0;
}
#define clock_gettime fixture_clock_gettime
#include "performance_metrics.c"
#undef clock_gettime
int main(void) {
    errno=EINTR;assert(skyrim_perf_now_ns()==7000000123 && errno==EINTR);
    fail_clock=1;assert(skyrim_perf_now_ns()==0 && errno==EINTR);
    skyrim_perf_end(SKYRIM_PERF_READ_FILE,1,1);
    SkyrimPerfWindow w;skyrim_perf_take(SKYRIM_PERF_READ_FILE,&w);
    assert(w.total_ns==0 && w.max_ns==0 && w.failures==0 && errno==EINTR);
    for(unsigned k=SKYRIM_PERF_READ_FILE;k<=SKYRIM_PERF_TEXTURE_LOAD;k++)
        skyrim_perf_record(k,100000001,1,0);
    skyrim_perf_log_window();
    return 0;
}
