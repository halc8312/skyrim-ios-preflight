/* Authored data; the real pinned shim bodies are inserted by Python. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include "save_write_probe.h"
typedef struct { uint32_t args[7], result, pops, error; } CPU;
/* GFILE_TYPE */
typedef struct { int unused; } Waitable;
struct HObj { int type; void *obj; };
enum { H_FREE, H_FILE, H_EVENT, ERROR_INVALID_HANDLE=6 };
static struct HObj slot;
static pthread_mutex_t g_hlock = PTHREAD_MUTEX_INITIALIZER;
static _Alignas(8) uint8_t memory[1024];
static uint8_t *MEM = memory;
static ssize_t transfer_result;
static unsigned writes, pwrites, reads, closes, frees, callbacks;
static off_t observed_offset, file_position;
static uint32_t callback_args[3];
static void (*during_write)(void);
#define STD_OUT 0x14u
#define STD_ERR 0x18u
#define M32(a) (*(uint32_t *)(MEM + (a)))
#define ARG(n) (c->args[n])
#define SHIM(n) static void shim_##n(CPU *c)
#define RET(v,n) do { c->result=(uint32_t)(v); c->pops=(n); return; } while(0)
static void *h_get(uint32_t h, int type) { return h==7 && slot.type==type ? slot.obj : NULL; }
static struct HObj *h_slot(uint32_t h) { return h==7 && slot.type!=H_FREE ? &slot : NULL; }
static void rt_set_last_error(CPU *c,uint32_t err) { c->error=err; }
static void w_signal(Waitable *w) { (void)w; abort(); }
static void rt_fatal(const char *msg) { (void)msg; abort(); }
static uint32_t rt_invoke(CPU *c,uint32_t fn,unsigned n,uint32_t *args) {
    (void)c; assert(fn==91 && n==3); ++callbacks; memcpy(callback_args,args,sizeof callback_args); return 0;
}
static ssize_t fake_write(int fd,const void *buffer,size_t n) {
    assert((fd==7 || fd==2) && buffer==MEM+128 && n==12 && MEM[128]==0x5b);
    ++writes; if(transfer_result>0)file_position+=transfer_result;
    if(during_write) during_write();
    errno=ENOSPC; return transfer_result;
}
static ssize_t fake_pwrite(int fd,const void *buffer,size_t n,off_t off) {
    assert(fd==7 && buffer==MEM+128 && n==12 && MEM[128]==0x5b);
    ++pwrites; observed_offset=off;
    if(during_write) during_write();
    errno=ENOSPC; return transfer_result;
}
static ssize_t fake_read(int fd,void *buffer,size_t n) {
    assert(fd==7 && buffer==MEM+128 && n==12); ++reads; errno=EINTR; return 0;
}
static ssize_t fake_pread(int fd,void *buffer,size_t n,off_t off) {
    observed_offset=off; return fake_read(fd,buffer,n);
}
static int fake_close(int fd) { assert(fd==7); ++closes; errno=EIO; return -1; }
static void fake_free(void *p) { ++frees; free(p); }
#define write fake_write
#define pwrite fake_pwrite
#define read fake_read
#define pread fake_pread
#define close fake_close
#define free fake_free
/* ACTUAL_SHIMS */

static void release_during_syscall(void) {
    CPU close_cpu={0};close_cpu.args[0]=7;
    shim_CloseHandle(&close_cpu);
    assert(close_cpu.result==1);
}

static unsigned take(unsigned kind,unsigned failures) {
    SkyrimPerfWindow w; skyrim_perf_take(kind,&w); uint64_t count=0;
    for(unsigned i=0;i<SKYRIM_PERF_BUCKET_COUNT;++i)count+=w.buckets[i];
    assert(w.failures==failures*EXPECT_METRICS); return (unsigned)count;
}
int main(void) {
    unsigned checksum=0;
    const char *paths[]={"/test/Saves/example.ESS", "/test/CachedShaders/code.bin", "/test/unknown.tmp"};
    for(unsigned p=0;p<3;++p)for(int mode=0;mode<4;++mode) {
        GFile *f=calloc(1,sizeof *f); f->fd=7; snprintf(f->path,sizeof f->path,"%s",paths[p]);
#if EXPECT_METRICS
        skyrim_save_write_init(&f->ios_write,f->path,sizeof f->path);
#endif
        slot=(struct HObj){H_FILE,f};
        CPU c={0}; c.args[0]=7;c.args[1]=128;c.args[2]=12;c.args[3]=64;
        memset(memory,0,sizeof memory);MEM[128]=0x5b;
        transfer_result=mode==0 ? -1 : mode==1 ? 0 : mode==2 ? 5 : 12;
        file_position=10;errno=EINTR;c.error=77;
        shim_WriteFile(&c);
        assert(c.result==(mode!=0) && c.pops==5 && c.error==77 && errno==ENOSPC);
        assert(M32(64)==(mode==0?0:(unsigned)transfer_result));
        assert(file_position==10+(transfer_result>0?transfer_result:0));
        checksum+=M32(64);
        unsigned kind=p==0?SKYRIM_PERF_SAVE_CANDIDATE_WRITE:SKYRIM_PERF_WRITE_FILE;
        assert(take(kind,mode==0)==EXPECT_METRICS);
        c.args[3]=256;c.args[4]=91;M32(264)=19;M32(268)=1;
        off_t old_position=file_position;unsigned old_callbacks=callbacks;
        shim_WriteFileEx(&c);
        assert(c.result==1 && c.pops==5 && c.error==77 && errno==ENOSPC);
        assert(file_position==old_position && observed_offset==((off_t)1<<32)+19);
        assert(callbacks==old_callbacks && t_napc==1);
        assert(run_apcs(&c)==1 && t_napc==0 && callbacks==old_callbacks+1);
        assert(callback_args[0]==(mode==0?30u:0u));
        assert(callback_args[1]==(mode==0?0u:(unsigned)transfer_result) && callback_args[2]==256);
        kind=p==0?SKYRIM_PERF_SAVE_CANDIDATE_WRITE_EX:SKYRIM_PERF_WRITE_FILE_EX;
        assert(take(kind,mode==0)==EXPECT_METRICS);
        /* read_ex still follows the old behavior; it is not classified as a save write. */
        shim_ReadFileEx(&c);assert(run_apcs(&c)==1 && callback_args[0]==38);
        c.args[3]=64;c.args[4]=0;shim_ReadFile(&c);assert(c.result==1 && M32(64)==0);
        c.args[0]=99;M32(64)=0xfeed;unsigned old_writes=writes;
        shim_WriteFile(&c);assert(!c.result && writes==old_writes && M32(64)==0xfeed && c.error==77);
        shim_WriteFileEx(&c);assert(!c.result && c.error==6 && !t_napc);
        c.args[0]=STD_OUT;shim_WriteFile(&c);c.args[0]=STD_ERR;shim_WriteFile(&c);
        assert(!take(SKYRIM_PERF_WRITE_FILE,0) && !take(SKYRIM_PERF_SAVE_CANDIDATE_WRITE,0));
        c.args[0]=7;shim_CloseHandle(&c);
        assert(c.result==1 && c.pops==1 && slot.type==H_FREE && errno==EIO);
        assert(take(SKYRIM_PERF_SAVE_CANDIDATE_WINDOW,0)==(p==0?EXPECT_METRICS:0));
        for(unsigned phase=SKYRIM_PERF_SAVE_FIRST_TO_LAST_WRITE_START;phase<=SKYRIM_PERF_SAVE_CLOSE_CALL;++phase)
            assert(take(phase,0)==(p==0?EXPECT_METRICS:0));
        shim_CloseHandle(&c);assert(!c.result);
    }
    /* The original WriteFile stops dereferencing GFile after entering write().
     * A diagnostics-only wrapper must not introduce a post-I/O dereference. */
    for(unsigned ex=0;ex<2;++ex) {
    GFile *f=calloc(1,sizeof *f);f->fd=7;
#if EXPECT_METRICS
    skyrim_save_write_init(&f->ios_write,"example.ess",12);
#endif
    slot=(struct HObj){H_FILE,f};during_write=release_during_syscall;
    CPU c={0};c.args[0]=7;c.args[1]=128;c.args[2]=12;c.args[3]=64;
    transfer_result=12;MEM[128]=0x5b;
    if(ex) {
        c.args[3]=256;c.args[4]=91;
        shim_WriteFileEx(&c);assert(run_apcs(&c)==1 && callback_args[1]==12);
    } else {
        shim_WriteFile(&c);assert(M32(64)==12);
    }
    assert(c.result==1 && slot.type==H_FREE);
    assert(take(ex?SKYRIM_PERF_SAVE_CANDIDATE_WRITE_EX:SKYRIM_PERF_SAVE_CANDIDATE_WRITE,0)==EXPECT_METRICS);
    assert(take(SKYRIM_PERF_SAVE_CANDIDATE_WINDOW,0)==EXPECT_METRICS);
    for(unsigned phase=SKYRIM_PERF_SAVE_FIRST_TO_LAST_WRITE_START;phase<=SKYRIM_PERF_SAVE_CLOSE_CALL;++phase)
        assert(take(phase,0)==EXPECT_METRICS);
    }
    /* A shared descriptor can have a handle window with no close syscall. */
    GFile *shared = calloc(1, sizeof *shared);
    shared->fd = 7;
    shared->shared = 1;
#if EXPECT_METRICS
    skyrim_save_write_init(&shared->ios_write, "example.ess", 12);
#endif
    slot = (struct HObj){H_FILE, shared};
    during_write = NULL;
    CPU shared_cpu = {.args = {7, 128, 12, 64}};
    shim_WriteFile(&shared_cpu);
    assert(shared_cpu.result == 1);
    shim_CloseHandle(&shared_cpu);
    assert(shared_cpu.result == 1 && slot.type == H_FREE);
    assert(take(SKYRIM_PERF_SAVE_CANDIDATE_WINDOW, 0) == EXPECT_METRICS);
    for (unsigned phase = SKYRIM_PERF_SAVE_FIRST_TO_LAST_WRITE_START; phase <= SKYRIM_PERF_SAVE_CLOSE_CALL; ++phase)
        assert(take(phase, 0) == 0);
    assert(writes==38 && pwrites==13 && closes==14 && frees==15 && callbacks==25);
    printf("save_write_behavior=PASS checksum=%u writes=%u pwrites=%u reads=%u closes=%u frees=%u callbacks=%u\n",
           checksum,writes,pwrites,reads,closes,frees,callbacks);
    return 0;
}
