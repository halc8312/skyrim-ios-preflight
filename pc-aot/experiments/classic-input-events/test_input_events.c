/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "skse_compat.h"

enum { registry = 0x2000, resolve_policy = 0x2100, event_policy = 0x2200,
       event_vtable = 0x2300, create_address = 0x01101932, recipient_count = 24,
       raw_input = 0x8000, alt_name = raw_input + 0x28, input_pointer = 0x9100 };
static _Alignas(16) unsigned char memory[32 * 1024 * 1024];
typedef struct { uint32_t r[8]; } CPU;
#define M32(a) (*(uint32_t *)(memory + (a)))
#define MF32(a) (*(float *)(memory + (a)))
#define GPTR(a) ((void *)(memory + (a)))
#define GSTR(a) ((a) ? (char *)(memory + (a)) : NULL)
#define ARG(n) M32(c->r[4]+4+4*(n))
#define RET(val,pop) do { assert((pop)==2); c->r[0]=(val); return; } while(0)

static uint32_t heap = 0x10000, live_allocations, fixed_refs, control_reads;
static uint32_t gmalloc(uint32_t bytes) {
    uint32_t step = (bytes + 15) & ~7u;
    uint32_t header = __atomic_fetch_add(&heap, step, __ATOMIC_RELAXED);
    assert(header + step < 0x01400000);
    M32(header) = 0xadad1234;
    __atomic_add_fetch(&live_allocations, 1, __ATOMIC_RELAXED);
    return header + 8;
}
static void gfree(uint32_t ptr) {
    assert(__atomic_exchange_n((uint32_t *)GPTR(ptr - 8), 0, __ATOMIC_RELAXED) == 0xadad1234);
    assert(__atomic_fetch_sub(&live_allocations, 1, __ATOMIC_RELAXED));
}
static void rt_fatal(const char *message) { fprintf(stderr, "%s\n", message); abort(); }
static void SDL_Delay(unsigned ms) {
    if (ms) { struct timespec delay={0,1000000}; nanosleep(&delay,NULL); }
    else sched_yield();
}

static const char *names[] = {"", "Jump", "Activate", "Left Attack/Block", "Right Attack/Block", "Sneak", "Ready Weapon"};
static uint32_t fixed_name(unsigned i) {
    assert(i < sizeof names / sizeof names[0]);
    return i ? 0x1000 + i * 64 : 0;
}
static const char *str(uint32_t ptr) { return ptr ? GSTR(ptr) : ""; }
static unsigned name_index(const char *s) {
    for (unsigned i=0; i<sizeof names/sizeof names[0]; ++i) if (!strcmp(s,names[i])) return i;
    abort();
}
static unsigned fixed_index(uint32_t p) { return p ? name_index(str(p)) : 0; }
static void vm_clear(CPU *cpu, uint32_t value) {
    (void)cpu;
    if (M32(value)==2 && M32(value+4)) assert(__atomic_fetch_sub(&fixed_refs,1,__ATOMIC_RELAXED));
    M32(value)=M32(value+4)=0;
}
static void vm_string(CPU *cpu, uint32_t value, const char *s) {
    vm_clear(cpu,value);
    M32(value)=2;
    M32(value+4)=fixed_name(name_index(s));
    if (M32(value+4)) __atomic_add_fetch(&fixed_refs,1,__ATOMIC_RELAXED);
}
static uint32_t resize_return, resize_count, resize_buffer, vm_copies;
static uint32_t skse_member(CPU *cpu, uint32_t fn, uint32_t self, int argc, const uint32_t *a) {
    (void)cpu;
    if (fn==0x8c2a70) {
        assert(argc==1 && a);
        M32(self+4)=resize_buffer; M32(self+0xc)=resize_count;
        return resize_return;
    } else if (fn==0xa51210) {
        assert(argc==1 && a && *a && !M32(self));
        M32(self)=fixed_name(name_index(str(*a)));
        if (M32(self)) __atomic_add_fetch(&fixed_refs,1,__ATOMIC_RELAXED);
    } else {
        assert(fn==0xa511b0 && argc==0 && !a);
        if (M32(self)) assert(__atomic_fetch_sub(&fixed_refs,1,__ATOMIC_RELAXED));
        M32(self)=0;
    }
    return 0;
}
static void vm_copy(CPU *c,uint32_t dst,uint32_t src) {
    ++vm_copies;
    if (M32(src)==2 && M32(src+4)) vm_string(c,dst,str(M32(src+4)));
    else { vm_clear(c,dst); memcpy(GPTR(dst),GPTR(src),8); }
}

typedef struct { uint64_t handle; uint32_t registry; } Registration;
typedef struct {
    uint32_t base, identifier, object, type, concrete;
    uint64_t vm_handle, registration;
    unsigned refs, invalid, unresolvable;
} Recipient;
static Recipient recipients[recipient_count];
static int no_event_policy;
static Recipient *find_handle(uint64_t h, int published) {
    for (unsigned i=0;i<recipient_count;++i)
        if ((published?recipients[i].registration:recipients[i].vm_handle)==h) return &recipients[i];
    return NULL;
}
static uint32_t skse_virtual(CPU *c, uint32_t self, unsigned slot, int argc, const uint32_t *a) {
    (void)c;
    if (self==registry) { assert(slot==0x2c && !argc && !a); return no_event_policy?0:event_policy; }
    if (self==resolve_policy) {
        assert(argc==(slot==2?2:3));
        unsigned i=(slot!=2);
        Recipient *r=find_handle(((uint64_t)a[i+1]<<32)|a[i],0);
        if (slot==1) return 0xdeadbe00u | !!(r && a[0]==r->type);
        if (slot==2) return 0xfabcde00u | !!(r && !r->invalid);
        assert(slot==8 && r && !r->invalid && r->type==a[0]);
        return r->unresolvable?0:r->object;
    }
    if (self==event_policy) {
        assert(argc==2 && (slot==9 || slot==10));
        Recipient *r=find_handle(((uint64_t)a[1]<<32)|a[0],1);
        assert(r);
        if (slot==9) __atomic_add_fetch(&r->refs,1,__ATOMIC_RELAXED);
        else assert(__atomic_fetch_sub(&r->refs,1,__ATOMIC_RELAXED));
        return 0;
    }
    if (self>=raw_input && self<raw_input+0x1000) {
        assert(slot==2 && !a && !argc);
        __atomic_add_fetch(&control_reads,1,__ATOMIC_RELAXED);
        return M32(self+0x24);
    }
    for (unsigned i=0;i<recipient_count;i++) {
        Recipient *r=&recipients[i];
        if (r->object==self) { assert(!r->type && slot==0x15 && !argc && !a); return r->concrete; }
    }
    abort();
}
static uint32_t rt_invoke(CPU *cpu,uint32_t address,int n,const uint32_t *a) {
    assert(cpu->r[1]==event_policy && address==create_address && n==2);
    for (unsigned i=0;i<recipient_count;i++) {
        Recipient *r=&recipients[i];
        if (r->object!=a[1]) continue;
        assert(r->concrete==a[0]);
        /* A genuine 64-bit canonical registration; clobber caller-saved regs. */
        cpu->r[0]=(uint32_t)r->registration;
        cpu->r[2]=(uint32_t)(r->registration>>32);
        cpu->r[3]=0xabababab;
        return cpu->r[0];
    }
    abort();
}

static void send_event(CPU *,const Registration *,const char *,unsigned,uint32_t);
#include "classic_input_events.h"
#include "fixture_dispatch.inc"

static const char *classes[]={"Form","Alias","ActiveMagicEffect"};
static const char *ops[]={"RegisterForKey","UnregisterForKey","UnregisterForAllKeys",
                          "RegisterForControl","UnregisterForControl","UnregisterForAllControls"};
static const SkseNative *method(int cls,int op) {
    unsigned n;
    const SkseNative *table=skse_native_table(&n);
    for (unsigned i=0;i<n;++i) if (!strcmp(table[i].class_name,classes[cls]) && !strcmp(table[i].name,ops[op])) {
        assert((table[i].operation&S_METHOD) && table[i].result==0);
        assert(table[i].argc==(op%3==2?0:1));
        if (table[i].argc) assert(table[i].args[0]==(op<3?3:2));
        return &table[i];
    }
    abort();
}
static int call_as(CPU *cpu, int cls, unsigned i, int op, uint32_t key) {
    assert(i<recipient_count);
    uint32_t result=gmalloc(8), args[]={op<3?key:fixed_name(key),0,0,0};
    vm_string(cpu,result,"Sneak");
    CPU before=*cpu;
    int ok=skse_system_run(cpu,method(cls,op),recipients[i].base,args,registry,result);
    assert(ok==0 || ok==1);
    assert(!memcmp(cpu,&before,sizeof before));
    if (ok) assert(!M32(result) && !M32(result+4));
    else assert(M32(result)==2 && M32(result+4)==fixed_name(5));
    vm_clear(cpu,result); gfree(result);
    return ok;
}
static void subscription(CPU *cpu,unsigned i,int op,uint32_t key) { assert(call_as(cpu,i%3,i,op,key)); }

typedef struct { unsigned who; char what[20]; uint32_t value,timer; } Delivery;
static Delivery observed[8192];
static unsigned used, gfx_calls;
static int trace, quiet, modify_inside_event;
static void send_event(CPU *c,const Registration *r,const char *s,unsigned n,uint32_t args) {
    Recipient *who=find_handle(r->handle,1);
    assert(who && r->registry==registry && __atomic_load_n(&who->refs,__ATOMIC_RELAXED));
    int named=strstr(s,"Control")!=NULL, up=strstr(s,"Up")!=NULL;
    assert(n==(unsigned)(up?2:1) && M32(args)==(named?2u:3u));
    uint32_t v=named?fixed_index(M32(args+4)):M32(args+4), time=0;
    if (up) { assert(M32(args+8)==4); time=M32(args+12); }
    unsigned i=(unsigned)(who-recipients);
    if (trace) printf("%u %s %u %08x\n",i,s,v,time);
    else if (!quiet) {
        assert(used<sizeof observed/sizeof observed[0]);
        observed[used]=(Delivery){.who=i,.value=v,.timer=time};
        snprintf(observed[used++].what,sizeof observed[0].what,"%s",s);
    }
    if (modify_inside_event) {
        modify_inside_event=0;
        subscription(c,0,5,0);   /* Key subscribers precede the control-registration lookup. */
        subscription(c,9,3,1);
        subscription(c,6,1,30);  /* The key snapshot still owns this pending recipient. */
    }
}
static void skse_gfx_input_event(CPU *cpu,unsigned k,const char *s,int down) {
    (void)cpu;(void)k;(void)s;(void)down; ++gfx_calls;
}
#include "fixture_observer.inc"
#undef RET
#define RET(val,pop) do { assert((pop)==1); c->r[0]=(val); return; } while(0)
#include "fixture_copy.inc"

static void check_arguments_copy(CPU *c) {
    uint32_t source=0xa000,output=0xa100,data=0xa200;
    M32(source+4)=2; M32(source+8)=3; M32(source+12)=280;
    vm_string(c,source+16,"Jump");
    c->r[1]=source; M32(c->r[4]+4)=output;
    resize_return=0x77770100; resize_count=2; resize_buffer=data;
    event_copy(c); assert(c->r[0]==0 && !vm_copies);
    resize_return=0xabcd1201; resize_count=1;
    event_copy(c); assert(c->r[0]==0 && !vm_copies);
    resize_count=2; resize_buffer=0;
    event_copy(c); assert(c->r[0]==0 && !vm_copies);
    resize_buffer=0xfffffff8;
    event_copy(c); assert(c->r[0]==0 && !vm_copies);
    resize_buffer=data;
    event_copy(c); assert(c->r[0]==1 && vm_copies==2);
    assert(M32(data)==3 && M32(data+4)==280 && M32(data+8)==2 && M32(data+12)==fixed_name(1));
    M32(source+4)=0; resize_buffer=0; resize_count=0;
    event_copy(c); assert(c->r[0]==1 && vm_copies==2);
    vm_clear(c,data+8); vm_clear(c,source+16);
    M32(c->r[4]+4)=input_pointer;
}

static void event_at(uint32_t at,unsigned type,unsigned device,unsigned code,unsigned flags,uint32_t timer,unsigned name) {
    memset(GPTR(at),0,0x40);
    M32(at+4)=device; M32(at+8)=type; M32(at+0x10)=fixed_name(name==1?2:1);
    M32(at+0x14)=code; M32(at+0x18)=flags; M32(at+0x1c)=timer;
    /* Custom ButtonEvent resolves its control virtually instead of at +0x10. */
    M32(at+0x24)=at+0x28; M32(at+0x28)=fixed_name(name);
}
static void send_button(CPU *cpu,unsigned type,unsigned device,unsigned code,unsigned flags,uint32_t timer,unsigned name) {
    event_at(raw_input,type,device,code,flags,timer,name);
    M32(input_pointer)=raw_input; sink_event(cpu);
}
static void expect(unsigned index,unsigned who,const char *name,unsigned value,uint32_t timer) {
    assert(index<used && observed[index].who==who && !strcmp(observed[index].what,name));
    assert(observed[index].value==value && observed[index].timer==timer);
}
static void cleanup(CPU *cpu) {
    for (unsigned i=0;i<recipient_count;i++) {
        subscription(cpu,i,2,0); subscription(cpu,i,5,0);
        assert(!recipients[i].refs);
    }
    assert(!live_allocations && !fixed_refs && !classic_input_count[0] && !classic_input_count[1]);
}

static void setup(CPU *cpu) {
    cpu->r[4]=0x9200; M32(cpu->r[4]+4)=input_pointer;
    M32(0x01ba7008)=resolve_policy; M32(event_policy)=event_vtable;
    M32(event_vtable+16)=create_address;
    for (unsigned i=1;i<sizeof names/sizeof names[0];++i) strcpy(GPTR(fixed_name(i)),names[i]);
    for (unsigned i=0;i<recipient_count;i++) {
        Recipient *r=&recipients[i];
        r->base=0x4000+i*0xb0; r->identifier=r->base+0x10; r->object=r->base+0x50;
        r->type=(uint32_t[]){0,136,139}[i%3]; r->concrete=r->type?r->type:0x29+i;
        r->vm_handle=UINT64_C(0x0000020000000180)+i;
        r->registration=UINT64_C(0x8000750000000012)+i;
        M32(r->base)=i%2?0x400:1; M32(r->base+4)=r->identifier;
        M32(r->identifier+0x10)=(uint32_t)r->vm_handle;
        M32(r->identifier+0x14)=(uint32_t)(r->vm_handle>>32);
        M32(r->identifier+0x18)=7;
    }
}
static void *registration_race(void *p) {
    (void)p; CPU thread={{0}};
    for (unsigned i=0;i<1800;i++) {
        subscription(&thread,17,0,30); subscription(&thread,17,3,1);
        subscription(&thread,17,4,1); subscription(&thread,17,1,30);
    }
    return NULL;
}
static void *unlock_receiver(void *p) {
    Recipient *r=p; SDL_Delay(3);
    __atomic_store_n((uint32_t *)GPTR(r->identifier+0x18),7,__ATOMIC_RELEASE);
    return NULL;
}

static void cases(CPU *cpu) {
    check_arguments_copy(cpu);
    for (int cls=0;cls<3;cls++) for (int op=0;op<6;op++) (void)method(cls,op);
    send_button(cpu,0,0,30,1,0,1);
    assert(gfx_calls==1 && !control_reads && !live_allocations);
    for (unsigned i=0;i<3;i++) {
        Recipient *r=recipients+i;
        assert(!call_as(cpu,(i+1)%3,i,3,1));
        r->invalid=1; assert(!call_as(cpu,i,i,3,1)); r->invalid=0;
        r->unresolvable=1; assert(!call_as(cpu,i,i,3,1)); r->unresolvable=0;
        uint32_t tag=M32(r->base);
        M32(r->base)=0x401; assert(!call_as(cpu,i,i,3,1));
        M32(r->base)=2; assert(!call_as(cpu,i,i,3,1));
        M32(r->base)=tag;
    }
    no_event_policy=1; assert(!call_as(cpu,0,0,3,1)); no_event_policy=0;
    assert(!classic_input_count[0] && !classic_input_count[1]);

    pthread_t unlock;
    __atomic_store_n((uint32_t *)GPTR(recipients[1].identifier+0x18),0x80000007,__ATOMIC_RELEASE);
    assert(!pthread_create(&unlock,NULL,unlock_receiver,&recipients[1]));
    subscription(cpu,1,3,2);
    assert(!pthread_join(unlock,NULL) && M32(recipients[1].identifier+0x18)==7);
    subscription(cpu,1,5,0);

    subscription(cpu,6,0,282); /* Recognized for registration, never delivered. */
    subscription(cpu,6,0,30); subscription(cpu,3,0,30);
    subscription(cpu,0,3,1); subscription(cpu,2,3,1); subscription(cpu,1,3,2);
    subscription(cpu,0,3,1);
    assert(recipients[0].refs==1 && fixed_refs==3);
    event_at(raw_input,0,0,30,1,0,1);
    event_at(raw_input+0x40,0,0,30,0,0x3e800000,1);
    M32(raw_input+0xc)=raw_input+0x40; M32(input_pointer)=raw_input; sink_event(cpu);
    assert(used==8);
    expect(0,3,"OnKeyDown",30,0); expect(1,6,"OnKeyDown",30,0);
    expect(2,0,"OnControlDown",1,0); expect(3,2,"OnControlDown",1,0);
    expect(4,3,"OnKeyUp",30,0x3e800000); expect(7,2,"OnControlUp",1,0x3e800000);
    send_button(cpu,0,0,30,1,0x3f800000,1); send_button(cpu,0,0,30,0,0,1);
    send_button(cpu,1,0,30,1,0,1); send_button(cpu,0,2,0x7777,1,0,1);
    assert(used==8);
    modify_inside_event=1; used=0; send_button(cpu,0,0,30,1,0,1);
    assert(used==4); expect(1,6,"OnKeyDown",30,0); expect(2,2,"OnControlDown",1,0); expect(3,9,"OnControlDown",1,0);
    used=0; send_button(cpu,0,0,30,0,0x3f400000,1);
    assert(used==3); expect(1,2,"OnControlUp",1,0x3f400000); expect(2,9,"OnControlUp",1,0x3f400000);
    cleanup(cpu);

    for (unsigned i=0;i<recipient_count;i++) {
        subscription(cpu,recipient_count-1-i,0,280); subscription(cpu,i,3,0);
    }
    used=0; send_button(cpu,0,2,9,1,0,0);
    assert(used==recipient_count*2);
    for (unsigned i=0;i<recipient_count;i++) {
        expect(i,i,"OnKeyDown",280,0); expect(i+recipient_count,i,"OnControlDown",0,0);
    }
    cleanup(cpu);
    subscription(cpu,3,0,265); used=0; send_button(cpu,0,1,9,1,0,2);
    expect(0,3,"OnKeyDown",265,0);
    cleanup(cpu);
    unsigned gfx=gfx_calls;
    subscription(cpu,3,0,30); used=0; send_button(cpu,0,3,30,1,0,2);
    expect(0,3,"OnKeyDown",30,0); assert(gfx==gfx_calls);
    cleanup(cpu);

    quiet=1;
    pthread_t worker; assert(!pthread_create(&worker,NULL,registration_race,NULL));
    for (unsigned i=0;i<2400;i++) send_button(cpu,0,0,30,i%2,i%2?0:0x3eaaaaab,1);
    assert(!pthread_join(worker,NULL));
    cleanup(cpu);
}

int main(int argc,char **argv) {
    CPU cpu={{0}};
    setup(&cpu);
    if (argc==2 && !strcmp(argv[1],"--trace")) {
        trace=1;
        char command;
        while (scanf(" %c",&command)==1) {
            unsigned a,b,c,d,e,f;
            if (command=='R') {
                assert(scanf("%u %u %u",&a,&b,&c)==3);
                subscription(&cpu,a,b,c);
            } else {
                assert(command=='E' && scanf("%u %u %u %u %x %u",&a,&b,&c,&d,&e,&f)==6);
                send_button(&cpu,a,b,c,d,e,f);
            }
        }
        cleanup(&cpu);
    } else {
        assert(argc==1); cases(&cpu);
        puts("Classic key/control observer, receiver lifetime and concurrent subscription checks passed");
    }
    return 0;
}
