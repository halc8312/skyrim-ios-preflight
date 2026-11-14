/* Authored scheduling boundary; uses the real gate, never private inputs. */
#include "gpu_gate.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
#include <sched.h>

typedef struct Buffer { atomic_int refs, commits, scheduled, waits; int id; } Buffer;
static SkyrimGpuGate gate = SKYRIM_GPU_GATE_INIT;
static atomic_int blocked, entered, unblock, pause_done, committed_total;
static int nested;
static Buffer nested_buffer;
static const SkyrimGpuGateOps ops;
static void hold(void *p) { assert(atomic_fetch_add(&((Buffer *)p)->refs,1)>0); }
static void drop(void *p) { assert(atomic_fetch_sub(&((Buffer *)p)->refs,1)>1); }
static int scheduled(void *p) { return atomic_load(&((Buffer *)p)->scheduled); }
static void commit(void *p) {
    Buffer *b=p;
    assert(atomic_load(&b->refs)>0);
    assert(atomic_fetch_add(&b->commits,1)==0);
    atomic_fetch_add(&committed_total,1);
    if (b->id==1 && atomic_load(&blocked)) {
        atomic_store(&entered,1);
        while(!atomic_load(&unblock)) sched_yield();
    }
    if(b->id==2 && nested) skyrim_gpu_gate_commit(&gate,&ops,&nested_buffer,NULL);
}
static void wait_scheduled(void *p) {
    Buffer *b=p;
    assert(atomic_load(&b->refs)>0 && atomic_load(&b->commits)==1);
    atomic_fetch_add(&b->waits,1);
    atomic_store(&b->scheduled,1); /* GPU has not completed, only scheduled. */
}
static const SkyrimGpuGateOps ops={hold,drop,scheduled,commit,wait_scheduled};
static Buffer make(int id) { Buffer b={.refs=1,.id=id};return b; }
static void join(pthread_t t) { assert(!pthread_join(t,NULL)); }
static void *submit(void *p) { skyrim_gpu_gate_commit(&gate,&ops,p,NULL);return NULL; }
static void *pause_gate(void *p) { (void)p;skyrim_gpu_gate_pause(&gate,&ops);atomic_store(&pause_done,1);return NULL; }
static void until(atomic_int *p) {
    struct timespec start,now;clock_gettime(CLOCK_MONOTONIC,&start);
    while(!atomic_load(p)) {
        sched_yield();clock_gettime(CLOCK_MONOTONIC,&now);
        assert(now.tv_sec-start.tv_sec<5);
    }
}
static void until_closed(void) {
    for(;;) {pthread_mutex_lock(&gate.mutex);int active=gate.active;pthread_mutex_unlock(&gate.mutex);if(!active)return;sched_yield();}
}
static void resume_pump(void) {
    /* Verifies the main-thread pump runs outside the mutex. */
    assert(!pthread_mutex_trylock(&gate.mutex));pthread_mutex_unlock(&gate.mutex);
    skyrim_gpu_gate_resume(&gate);
}
static void *enqueue_scope(void *p) {
    skyrim_gpu_gate_enter(&gate,NULL);
    atomic_store(&entered,1); /* represents enqueue before pause */
    while(!atomic_load(&unblock)) sched_yield();
    skyrim_gpu_gate_commit(&gate,&ops,p,NULL);
    skyrim_gpu_gate_leave(&gate);return NULL;
}
/* A CPU-side dependency must not occupy admission while waiting for a
 * producer which may start only after resume. Unlike the original order,
 * pre-admission wait lets pause drain without requiring an external signal. */
static atomic_int dependency_waiting, dependency_signal, dependency_done;
static void *dependency_consumer(void *p) {
    atomic_store(&dependency_waiting,1);
    while(!atomic_load(&dependency_signal)) sched_yield();
    skyrim_gpu_gate_enter(&gate,NULL);
    skyrim_gpu_gate_commit(&gate,&ops,p,NULL);
    skyrim_gpu_gate_leave(&gate);
    atomic_store(&dependency_done,1);return NULL;
}
static void *dependency_producer(void *p) {
    skyrim_gpu_gate_enter(&gate,NULL);
    skyrim_gpu_gate_commit(&gate,&ops,p,NULL);
    atomic_store(&dependency_signal,1);
    skyrim_gpu_gate_leave(&gate);return NULL;
}
static Buffer stress_storage[4][2000];
static void *stress(void *p) {
    int id=*(int*)p;
    for(unsigned i=0;i<2000;++i) {
        Buffer *b=&stress_storage[id][i];*b=make(0);
        skyrim_gpu_gate_commit(&gate,&ops,b,NULL);
        atomic_store(&b->scheduled,1);
    }
    return NULL;
}
int main(void) {
    skyrim_gpu_gate_commit(&gate,&ops,NULL,NULL);
    Buffer a=make(0);skyrim_gpu_gate_commit(&gate,&ops,&a,NULL);
    assert(a.commits==1 && a.waits==0 && a.refs==2); /* No per-submit scheduling wait. */
    skyrim_gpu_gate_pause(&gate,&ops);assert(a.waits==1 && a.refs==1);
    skyrim_gpu_gate_pause(&gate,&ops);assert(a.waits==1); /* Idempotent pause. */
    Buffer b=make(0);pthread_t t;assert(!pthread_create(&t,NULL,submit,&b));
    skyrim_gpu_gate_resume(&gate);join(t);assert(b.commits==1);
    skyrim_gpu_gate_pause(&gate,&ops);assert(b.refs==1);
    skyrim_gpu_gate_wait(&gate,resume_pump);

    /* Pause cannot return during an admitted commit; new work stays pending. */
    Buffer c=make(1),d=make(0);atomic_store(&blocked,1);atomic_store(&unblock,0);atomic_store(&entered,0);
    assert(!pthread_create(&t,NULL,submit,&c));until(&entered);
    pthread_t closer,newer;atomic_store(&pause_done,0);
    assert(!pthread_create(&closer,NULL,pause_gate,NULL));until_closed();
    assert(!pthread_create(&newer,NULL,submit,&d));assert(!atomic_load(&pause_done));
    atomic_store(&unblock,1);join(t);join(closer);assert(!d.commits && c.scheduled && c.refs==1);
    skyrim_gpu_gate_resume(&gate);join(newer);assert(d.commits==1);
    skyrim_gpu_gate_pause(&gate,&ops);skyrim_gpu_gate_resume(&gate);

    /* Explicit early enqueue must retain admission until its later commit. */
    Buffer e=make(0);atomic_store(&entered,0);atomic_store(&unblock,0);atomic_store(&pause_done,0);
    assert(!pthread_create(&t,NULL,enqueue_scope,&e));until(&entered);
    assert(!pthread_create(&closer,NULL,pause_gate,NULL));until_closed();
    atomic_store(&unblock,1);join(t);join(closer);assert(e.commits==1 && e.scheduled && e.refs==1);
    skyrim_gpu_gate_resume(&gate);

    Buffer full[SKYRIM_GPU_GATE_SLOTS+1];
    for(unsigned i=0;i<SKYRIM_GPU_GATE_SLOTS+1;++i) {
        full[i]=make(0);skyrim_gpu_gate_commit(&gate,&ops,&full[i],NULL);
    }
    assert(full[0].waits==1); /* Fixed capacity uses scheduling, not completion. */
    skyrim_gpu_gate_pause(&gate,&ops);
    for(unsigned i=0;i<SKYRIM_GPU_GATE_SLOTS+1;++i) assert(full[i].refs==1 && full[i].commits==1);
    skyrim_gpu_gate_resume(&gate);

    /* A transient burst must not leave completed upper slots retained when
     * subsequent traffic repeatedly reuses only slot zero. */
    Buffer burst[9];
    for(unsigned i=0;i<8;++i) {burst[i]=make(0);skyrim_gpu_gate_commit(&gate,&ops,&burst[i],NULL);}
    for(unsigned i=0;i<8;++i) atomic_store(&burst[i].scheduled,1);
    burst[8]=make(0);skyrim_gpu_gate_commit(&gate,&ops,&burst[8],NULL);
    for(unsigned i=0;i<8;++i) assert(burst[i].refs==1);
    skyrim_gpu_gate_pause(&gate,&ops);skyrim_gpu_gate_resume(&gate);

    nested=1;nested_buffer=make(0);Buffer parent=make(2);
    skyrim_gpu_gate_commit(&gate,&ops,&parent,NULL);
    skyrim_gpu_gate_pause(&gate,&ops);assert(parent.refs==1 && nested_buffer.refs==1);
    skyrim_gpu_gate_resume(&gate);
    /* Nested commit at capacity cannot wait on an earlier enqueued parent. */
    for(unsigned i=0;i<SKYRIM_GPU_GATE_SLOTS;++i) {full[i]=make(0);skyrim_gpu_gate_commit(&gate,&ops,&full[i],NULL);}
    nested_buffer=make(0);parent=make(2);skyrim_gpu_gate_enter(&gate,NULL);
    skyrim_gpu_gate_commit(&gate,&ops,&parent,NULL);skyrim_gpu_gate_leave(&gate);
    assert(parent.waits==1 && nested_buffer.waits==1);
    skyrim_gpu_gate_pause(&gate,&ops);assert(parent.refs==1 && nested_buffer.refs==1);
    skyrim_gpu_gate_resume(&gate);
    Buffer dependency_read=make(0),dependency_write=make(0);
    assert(!pthread_create(&t,NULL,dependency_consumer,&dependency_read));
    until(&dependency_waiting);
    atomic_store(&pause_done,0);
    assert(!pthread_create(&closer,NULL,pause_gate,NULL));until(&pause_done);join(closer);
    assert(!atomic_load(&dependency_done) && !atomic_load(&dependency_signal));
    assert(!pthread_create(&newer,NULL,dependency_producer,&dependency_write));
    skyrim_gpu_gate_resume(&gate);join(newer);join(t);
    skyrim_gpu_gate_pause(&gate,&ops);
    assert(dependency_read.commits==1 && dependency_write.commits==1);
    assert(dependency_read.refs==1 && dependency_write.refs==1);
    skyrim_gpu_gate_resume(&gate);
    pthread_t workers[4];int ids[4]={0,1,2,3};
    for(unsigned i=0;i<4;++i) assert(!pthread_create(&workers[i],NULL,stress,&ids[i]));
    for(unsigned i=0;i<100;++i) {skyrim_gpu_gate_pause(&gate,&ops);skyrim_gpu_gate_resume(&gate);}
    for(unsigned i=0;i<4;++i) join(workers[i]);
    skyrim_gpu_gate_pause(&gate,&ops);
    for(unsigned i=0;i<4;++i) for(unsigned j=0;j<2000;++j)
        assert(stress_storage[i][j].refs==1 && stress_storage[i][j].commits==1);
    puts("GPU_GATE pause_resume=PASS early_enqueue=PASS cpu_dependency_before_admission=PASS bounded_capacity=PASS nested_commit=PASS concurrent8000=PASS no_completion_wait=PASS");
}
