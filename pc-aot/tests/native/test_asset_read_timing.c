/* Authored file/APC fixture; actual upstream shim bodies are appended by Python. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include "performance_metrics.h"

typedef struct CPU { uint32_t args[7], result, pops, error; } CPU;
typedef struct GFile { int fd, shared; off_t pos; } GFile;
typedef struct Waitable { int unused; } Waitable;
static _Alignas(8) uint8_t memory[1024];
static uint8_t *MEM = memory;
static GFile file = {7, 0, 0};
static Waitable event;
static ssize_t transfer_result;
static unsigned reads, preads, writes, signals, callbacks;
static off_t observed_offset;
static uint32_t last_callback, callback_args[3];
enum { H_FILE, H_EVENT, ERROR_INVALID_HANDLE = 6 };
#define M32(a) (*(uint32_t *)(MEM + (a)))
#define ARG(n) (c->args[n])
#define SHIM(n) static void shim_##n(CPU *c)
#define RET(value, pop) do { c->result=(uint32_t)(value); c->pops=(pop); return; } while (0)
static void *h_get(uint32_t h, int type) {
    return type==H_FILE && h==7 ? (void *)&file : type==H_EVENT && h==8 ? &event : NULL;
}
static void rt_set_last_error(CPU *c, uint32_t error) { c->error=error; }
static void w_signal(Waitable *w) { assert(w==&event); signals++; }
static void rt_fatal(const char *s) { (void)s; abort(); }
static uint32_t rt_invoke(CPU *c, uint32_t fn, unsigned n, uint32_t *args) {
    (void)c; assert(n==3); callbacks++; last_callback=fn;
    memcpy(callback_args,args,sizeof callback_args); return 0;
}
static ssize_t fixture_transfer(void *buffer, size_t n) {
    if (transfer_result>0) { assert((size_t)transfer_result<=n); memset(buffer,0x5b,(size_t)transfer_result); }
    errno=EINTR; return transfer_result;
}
static ssize_t fixture_read(int fd, void *buffer, size_t n) {
    assert(fd==7); reads++; return fixture_transfer(buffer,n);
}
static ssize_t fixture_pread(int fd, void *buffer, size_t n, off_t offset) {
    assert(fd==7); preads++; observed_offset=offset; return fixture_transfer(buffer,n);
}
static ssize_t fixture_pwrite(int fd, const void *buffer, size_t n, off_t offset) {
    assert(fd==7 && buffer==MEM+128 && n==12); writes++; observed_offset=offset;
    errno=EINTR; return transfer_result;
}
#define read fixture_read
#define pread fixture_pread
#define pwrite fixture_pwrite
/* ACTUAL_UPSTREAM_SHIMS */

static void check_metric(unsigned kind, unsigned samples, unsigned failures) {
    SkyrimPerfWindow w; skyrim_perf_take(kind,&w); uint64_t count=0;
    for(unsigned i=0;i<SKYRIM_PERF_BUCKET_COUNT;i++) count+=w.buckets[i];
    assert(count==samples*EXPECT_METRICS && w.failures==failures*EXPECT_METRICS);
}
int main(void) {
    CPU c={0}; c.args[0]=7; c.args[1]=128; c.args[2]=12; c.args[3]=64;
    uint64_t checksum=0;
    for(unsigned mode=0;mode<3;mode++) for(int outcome=-1;outcome<3;outcome++) {
        memset(memory,0,sizeof memory); file.pos=33; file.shared=(mode==1);
        c.args[4]=mode==2 ? 256 : 0; transfer_result=outcome<0 ? -1 : outcome*4;
        M32(264)=19;M32(268)=1;M32(272)=8;
        unsigned old_reads=reads,old_preads=preads,old_signals=signals;
        shim_ReadFile(&c);
        assert(c.result==1 && c.pops==5 && errno==EINTR);
        assert(reads-old_reads==(mode==0));assert(preads-old_preads==(mode!=0));
        assert(signals-old_signals==(mode==2));
        assert(M32(64)==(outcome<0 ? 0u : (unsigned)outcome*4));
        assert(file.pos==(mode==1 && outcome>0 ? 33+outcome*4 : 33));
        if(mode==2) assert(observed_offset==((off_t)1<<32)+19 && M32(256)==0 && M32(260)==M32(64));
        if(mode==1) assert(observed_offset==33);
        if(outcome>0) assert(MEM[128]==0x5b && MEM[128+outcome*4-1]==0x5b);
        check_metric(SKYRIM_PERF_READ_FILE,1,outcome<0);
        checksum+=M32(64)+file.pos+signals;
    }
    c.args[0]=99;M32(64)=123;shim_ReadFile(&c);
    assert(c.result==0 && c.error==6 && M32(64)==0);check_metric(SKYRIM_PERF_READ_FILE,0,0);
    c.args[0]=7;c.args[3]=256;c.args[4]=77;
    for(int writing=0;writing<2;writing++) for(int outcome=-1;outcome<3;outcome++) {
        transfer_result=outcome<0 ? -1 : outcome*4; M32(264)=31;M32(268)=1;
        unsigned before=callbacks,before_reads=preads,before_writes=writes;
        if(writing) shim_WriteFileEx(&c); else shim_ReadFileEx(&c);
        assert(c.result==1 && c.pops==5 && errno==EINTR);
        assert(callbacks==before && t_napc==1);
        assert(preads-before_reads==(unsigned)!writing && writes-before_writes==(unsigned)writing);
        assert(observed_offset==((off_t)1<<32)+31);
        uint32_t error=outcome<0 ? 30 : !writing && outcome==0 ? 38 : 0;
        assert(M32(256)==(error ? 0xc0000011u : 0) && M32(260)==(outcome<0 ? 0u : (unsigned)outcome*4));
        assert(run_apcs(&c)==1 && t_napc==0 && callbacks==before+1 && last_callback==77);
        assert(callback_args[0]==error && callback_args[1]==M32(260) && callback_args[2]==256);
        check_metric(SKYRIM_PERF_READ_FILE_EX,!writing,!writing && outcome<0);
        checksum+=M32(260)+callback_args[0];
    }
    c.args[3]=0;shim_ReadFileEx(&c);assert(c.result==0 && c.error==6 && t_napc==0);
    check_metric(SKYRIM_PERF_READ_FILE_EX,0,0);
    assert(run_apcs(&c)==0);
    printf("read_apc_state=%llu callbacks=%u reads=%u preads=%u writes=%u\n",
        (unsigned long long)checksum,callbacks,reads,preads,writes);
}
