#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

typedef struct { uint32_t args[5], result, pops, error; } CPU;
typedef struct { int fd; } GFile;
static GFile file = {7};
_Alignas(8) static __thread uint8_t memory[4096];
#define MEM memory
#define M32(a) (*(uint32_t *)(memory+(a)))
#define ARG(n) (c->args[n])
#define SHIM(n) static void shim_##n(CPU *c)
#define RET(v,p) do { c->result=(v);c->pops=(p);return; } while(0)
enum { H_FILE, ERROR_INVALID_HANDLE=6 };
static __thread unsigned io_calls, delivered, expected_at, expected_end;
static __thread uint32_t expected[16384][4];
static __thread size_t shifted;
static __thread int nested, fatal_expected;
static __thread int benchmarking;
static __thread uint64_t checksum;
static __thread jmp_buf fatal_jump;
static void *h_get(uint32_t h,int type) { assert(type==H_FILE);return h==7?&file:NULL; }
static void rt_set_last_error(CPU *c,uint32_t e) { c->error=e; }
static void rt_fatal(const char *s) { assert(!strcmp(s,"APC queue full") && fatal_expected);longjmp(fatal_jump,1); }
static ssize_t fake_io(int fd,const void *buf,size_t n,off_t off) {
    assert(fd==7 && buf==MEM+128 && off==123);++io_calls;errno=EINTR;return n==3?-1:n==2?0:(ssize_t)n;
}
#define pread fake_io
#define pwrite fake_io
static void *count_move(void *d,const void *s,size_t n) { shifted+=n;return memmove(d,s,n); }
/* Apple SDK fortify may define memmove. Instrument only the extracted APC
 * calls; count_move above retains the SDK implementation and its checks. */
#pragma push_macro("memmove")
#undef memmove
#define memmove count_move
static uint32_t rt_invoke(CPU*,uint32_t,unsigned,uint32_t*);
/* ACTUAL_APC */
#pragma pop_macro("memmove")

static void enqueue(CPU *c,uint32_t id,int writing,unsigned bytes) {
    c->args[0]=7;c->args[1]=128;c->args[2]=bytes;c->args[3]=256;c->args[4]=id;
    M32(264)=123;M32(268)=0;
    if(writing)shim_WriteFileEx(c);else shim_ReadFileEx(c);
    assert(c->result==1 && c->pops==5 && errno==EINTR);
    assert(expected_end<16384);
    uint32_t err=bytes==3?30:!writing&&bytes==2?38:0;
    unsigned transferred=bytes==2||bytes==3?0:bytes;
    assert(M32(256)==(err?0xc0000011u:0) && M32(260)==transferred);
    expected[expected_end][0]=id;expected[expected_end][1]=err;
    expected[expected_end][2]=transferred;expected[expected_end++][3]=256;
}
static uint32_t rt_invoke(CPU *c,uint32_t fn,unsigned count,uint32_t *args) {
    if(benchmarking) { checksum+=fn+count+args[0]+args[1]+args[2];return 0; }
    assert(count==3 && expected_at<expected_end);
    assert(fn==expected[expected_at][0]);
    assert(!memcmp(args,&expected[expected_at][1],3*sizeof(uint32_t)));
    ++expected_at;++delivered;
    if(nested && fn==100) { enqueue(c,200,0,4);enqueue(c,201,1,3); assert(run_apcs(c)); }
    if(nested && fn==200) enqueue(c,202,0,0);
    return 0;
}
static void reset_reference(void) { assert(!t_napc);expected_at=expected_end=0; }
static void *check_thread(void *unused) {
    (void)unused;CPU c={0};assert(!t_napc);
    for(unsigned size=0;size<=64;size++) for(unsigned repeat=0;repeat<3;repeat++) {
        reset_reference();for(unsigned i=0;i<size;i++)enqueue(&c,i+1,i%2,i%5);
        assert(run_apcs(&c)==(size!=0));assert(expected_at==expected_end && !t_napc);
    }
    reset_reference();nested=1;enqueue(&c,100,0,5);enqueue(&c,101,0,5);enqueue(&c,102,0,5);
    assert(run_apcs(&c));assert(expected_at==6 && !t_napc);nested=0;
    reset_reference();enqueue(&c,0,0,1); // null routine is dequeued but never invoked
    assert(run_apcs(&c) && !t_napc);assert(!expected_at);
    reset_reference();for(unsigned i=0;i<64;i++)enqueue(&c,i+1,0,4);
    fatal_expected=1;unsigned before=io_calls;
    if(!setjmp(fatal_jump)) { enqueue(&c,999,0,4);abort(); }
    assert(io_calls==before+1 && t_napc==64);fatal_expected=0;
    assert(run_apcs(&c));assert(expected_at==64);
    reset_reference();c.args[0]=9;shim_ReadFileEx(&c);assert(!c.result && c.error==6 && !t_napc);
    return NULL;
}
static uint64_t now(void) {
    struct timespec t;int rc=clock_gettime(CLOCK_MONOTONIC,&t);assert(!rc);
    return (uint64_t)t.tv_sec*1000000000ull+(uint64_t)t.tv_nsec;
}
int main(int argc,char **argv) {
    (void)count_move; // baseline-only instrumentation
    if(argc==2) {
        unsigned size=(unsigned)strtoul(argv[1],NULL,10);assert(size && size<=64);
        CPU c={{7,128,4,256,77},0,0,0};M32(264)=123;benchmarking=1;
        unsigned loops=4000000/size;uint64_t start=now();
        for(unsigned n=0;n<loops;n++) {
            for(unsigned j=0;j<size;j++)shim_ReadFileEx(&c);
            int ran=run_apcs(&c);assert(ran && !t_napc);
        }
        printf("{\"batch\":%u,\"ns_per_callback\":%.3f,\"checksum\":%llu,\"shifted_bytes\":%zu}\n",
            size,(double)(now()-start)/(loops*size),(unsigned long long)checksum,shifted);
        return 0;
    }
    check_thread(NULL);
    pthread_t threads[4];for(unsigned i=0;i<4;i++)assert(!pthread_create(&threads[i],NULL,check_thread,NULL));
    for(unsigned i=0;i<4;i++)assert(!pthread_join(threads[i],NULL));
    CPU c={0};reset_reference();shifted=0;for(unsigned i=0;i<64;i++)enqueue(&c,i+1,0,4);
    assert(run_apcs(&c));assert(expected_at==64);
    printf("queue64 shifted_bytes=%zu callbacks=64 semantics=PASS\n",shifted);
}
