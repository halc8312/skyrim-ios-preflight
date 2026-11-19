/* SPDX-License-Identifier: GPL-3.0-only */
#include "skse_compat.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

enum { REGISTRY=4, FIRST=16, SECOND=24, FILL=32, SLOTS=40, STRING_START=0x40000, STRING_STRIDE=128 };
typedef struct {
    uint32_t requests, issued, released;
    int alloc_fail, array_fail, short_array;
} CPU;
static CPU cpu;
static uint32_t memory[0x60000/4], refs[128], nstrings;
static uint32_t array_next = 0x1000;
static int scratch_in_use;
static uint32_t *word(uint32_t at) {
    assert(at && !(at % 4) && at <= sizeof memory - 4);
    return &memory[at/4];
}
#define M32(a) (*word(a))
#define GPTR(a) ((unsigned char *)memory + (a))
static uint32_t float_bits(float f) { uint32_t bits; memcpy(&bits,&f,4); return bits; }
static float as_float(uint32_t bits) { float f; memcpy(&f,&bits,4); return f; }
static const char *str(uint32_t a) { assert(a < sizeof memory); return (const char *)GPTR(a); }
static uint32_t intern(const char *s) {
    if (!s) return 0;
    assert(strlen(s) < STRING_STRIDE);
    for (unsigned i=0;i<nstrings;++i) {
        uint32_t at=STRING_START+i*STRING_STRIDE;
        if (!strcasecmp(str(at),s)) return at;
    }
    assert(nstrings < sizeof refs/sizeof refs[0]);
    uint32_t at=STRING_START+nstrings++*STRING_STRIDE;
    strcpy((char *)GPTR(at),s);
    return at;
}
static uint32_t *string_refs(uint32_t at) {
    assert(at >= STRING_START && (at-STRING_START)%STRING_STRIDE==0);
    return refs+(at-STRING_START)/STRING_STRIDE;
}
static void vm_clear(CPU *c,uint32_t at) {
    assert(c==&cpu);
    unsigned type=M32(at); uint32_t previous=M32(at+4);
    M32(at)=0;M32(at+4)=0;
    if(type==2 && previous) {assert(*string_refs(previous)>0); --*string_refs(previous);}
    if(type>=12 && type<=15 && previous) {
        assert(M32(previous)>0);
        if (!--M32(previous)) {
            for(uint32_t i=0;i<M32(previous+8);++i) vm_clear(c,previous+20+i*8);
            ++cpu.released;
        }
    }
}
static void vm_string(CPU *c,uint32_t at,const char *s) {
    uint32_t id=intern(s);vm_clear(c,at);
    M32(at)=2;M32(at+4)=id;
    if(id)++*string_refs(id);
}
static void vm_copy(CPU *c,uint32_t target,uint32_t source) {
    unsigned type=M32(source);uint32_t value=M32(source+4);
    assert(type==2);
    if(value)++*string_refs(value);
    vm_clear(c,target); M32(target)=type; M32(target+4)=value;
}
static uint32_t gmalloc(unsigned n) {
    assert(n==20 && !scratch_in_use);
    if(cpu.alloc_fail)return 0;
    scratch_in_use=1;return 0x80;
}
static void gfree(uint32_t at) {
    assert(at==0x80 && scratch_in_use);
    scratch_in_use=0;
}
static uint32_t skse_virtual(CPU *c,uint32_t registry,unsigned slot,int n,const uint32_t *args) {
    assert(c==&cpu && registry==REGISTRY && slot==21 && n==3);
    assert(args[0] && !M32(args[0]) && !M32(args[0]+4) && !M32(args[2]));
    ++cpu.requests;
    if(cpu.array_fail)return 0;
    unsigned count=args[1]-(cpu.short_array ? 1 : 0);
    uint32_t data=array_next;
    assert(count<2048 && data+20+8*count < STRING_START);
    array_next+=20+8*count;
    M32(data)=1; M32(data+8)=count;
    M32(args[2])=data; ++cpu.issued;
    return 1;
}
#include "classic_skse_arrays.h"

static const SkseNative *native(const char *class_name,const char *name) {
    unsigned count;
    const SkseNative *table=skse_native_table(&count),*result=NULL;
    for(unsigned i=0;i<count;++i)
        if(!strcmp(table[i].class_name,class_name) && !strcmp(table[i].name,name)) {
            assert(!result); result=&table[i];
        }
    assert(result);
    return result;
}
static uint32_t run(const SkseNative *fn,uint32_t a,uint32_t b,uint32_t c,uint32_t out) {
    uint32_t args[]={a,b,c,0};
    skse_classic_array_run(&cpu,fn,args,REGISTRY,out);
    assert(!scratch_in_use);
    assert(M32(out)==fn->result || (!M32(out) && !M32(out+4)));
    return M32(out+4);
}
static void item(uint32_t arr,unsigned ix,unsigned type,uint32_t value) {
    assert(arr && ix<M32(arr+8));
    uint32_t slot=arr+20+ix*8;
    assert(M32(slot)==type && M32(slot+4)==value);
}
int main(void) {
    const SkseNative *makeI=native("Utility","CreateIntArray"), *resizeI=native("Utility","ResizeIntArray");
    const SkseNative *makeF=native("Utility","CreateFloatArray"), *resizeF=native("Utility","ResizeFloatArray");
    const SkseNative *makeS=native("Utility","CreateStringArray"), *resizeS=native("Utility","ResizeStringArray");
    const SkseNative *split=native("StringUtil","Split");
    assert(makeI->result==13 && makeI->argc==2 && makeI->args[0]==3 && makeI->args[1]==3);
    assert(resizeF->result==14 && resizeF->argc==3 && resizeF->args[0]==14 && resizeF->args[1]==3 && resizeF->args[2]==4);
    assert(makeS->result==12 && makeS->args[1]==2 && split->result==12 && split->args[0]==2 && split->args[1]==2);
    unsigned before=cpu.requests;
    assert(!run(makeI,0,0,0,FIRST) && !run(makeI,UINT32_MAX,0,0,FIRST) && cpu.requests==before);
    uint32_t a=run(makeI,129,0x80000000,0,FIRST);
    assert(M32(a+8)==129);item(a,0,3,0x80000000);item(a,128,3,0x80000000);
    M32(a+20+4)=7;
    uint32_t b=run(resizeI,a,132,UINT32_MAX,SECOND);
    assert(b!=a);item(b,0,3,7);item(b,128,3,0x80000000);item(b,131,3,UINT32_MAX);
    M32(a+20+4)=10;
    item(b,0,3,7);
    uint32_t resized=run(resizeI,a,1,3,FIRST); /* source and result alias */
    assert(M32(a)==0 && resized!=a);item(resized,0,3,10);
    assert(!run(resizeI,resized,0,0,FIRST) && !M32(resized));
    a=run(resizeF,0,3,float_bits(-0.0f),FIRST);
    for(unsigned i=0;i<3;++i)item(a,i,4,0x80000000);
    M32(a+20)=3;M32(a+24)=(uint32_t)-11;
    M32(a+28)=5;M32(a+32)=1;
    b=run(resizeF,a,3,0,SECOND);
    item(b,0,4,float_bits(-11.0f));item(b,1,4,float_bits(1));item(b,2,4,0x80000000);
    b=run(makeF,1,0x7fc12345,0,SECOND);item(b,0,4,0x7fc12345);

    vm_string(&cpu,FILL,"shared");uint32_t shared=M32(FILL+4);
    a=run(makeS,3,shared,0,FIRST);assert(*string_refs(shared)==4);
    b=run(resizeS,a,5,0,SECOND);
    assert(*string_refs(shared)==7);item(b,0,2,shared);item(b,4,2,0);
    b=run(resizeS,b,1,0,SECOND);item(b,0,2,shared);assert(*string_refs(shared)==5);
    vm_clear(&cpu,FILL);vm_clear(&cpu,FIRST);vm_clear(&cpu,SECOND);
    assert(!*string_refs(shared));

    uint32_t input=intern("::First;Second::;Third;"),separators=intern(":;");
    a=run(split,input,separators,0,FIRST);assert(M32(a+8)==3);
    item(a,0,2,intern("First"));item(a,1,2,intern("Second"));item(a,2,2,intern("Third"));
    a=run(split,intern("aBQuERyAB結果"),intern("ab"),0,FIRST);assert(M32(a+8)==2);
    item(a,0,2,intern("QuERy"));item(a,1,2,intern("結果"));
    assert(!run(split,intern("abAB"),intern("ba"),0,FIRST));
    a=run(split,intern("Whole text"),intern(""),0,FIRST);assert(M32(a+8)==1);item(a,0,2,intern("Whole text"));
    assert(!run(split,intern(""),intern(","),0,FIRST) && !run(split,0,intern(","),0,FIRST));
    before=cpu.requests;cpu.alloc_fail=1;
    assert(!run(makeI,2,0,0,FIRST) && cpu.requests==before);
    cpu.alloc_fail=0;cpu.array_fail=1;
    assert(!run(makeI,2,0,0,FIRST));
    cpu.array_fail=0;cpu.short_array=1;
    assert(!run(makeI,2,0,0,FIRST));
    cpu.short_array=0;
    uint32_t bad[]= {1,0,0};
    skse_classic_array_run(&cpu,makeI,bad,0,SLOTS);assert(!M32(SLOTS));
    assert(cpu.released==cpu.issued);
    for(unsigned i=0;i<nstrings;++i)assert(!refs[i]);
    return 0;
}
