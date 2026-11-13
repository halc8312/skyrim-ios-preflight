#include "d3dx_image.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static uint64_t old_steps, new_steps;
/* ACTUAL_DDS */
typedef int (*LevelFn)(const NativeD3DXImage *,size_t,uint32_t,uint32_t,size_t *,uint32_t *,uint32_t *,uint32_t *,uint32_t *,uint32_t *);
typedef struct {size_t offset;uint32_t w,h,d,p,r;} Result;
static uint64_t cases;
static void compare(const NativeD3DXImage *image,size_t bytes,unsigned face,unsigned level) {
  Result a,b;memset(&a,0xA7,sizeof a);b=a;
  int before=old_level(image,bytes,face,level,&a.offset,&a.w,&a.h,&a.d,&a.p,&a.r);
  int after=new_level(image,bytes,face,level,&b.offset,&b.w,&b.h,&b.d,&b.p,&b.r);
  assert(before==after&&!memcmp(&a,&b,sizeof a));++cases;
}
static uint64_t ns(void) {struct timespec t;assert(!clock_gettime(CLOCK_MONOTONIC,&t));return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
static uint32_t random_word=0x52ac8231u;
static uint32_t next(void){random_word=random_word*1664525u+1013904223u;return random_word;}
int main(int argc,char **argv) {
  NativeD3DXImage base={.width=1024,.height=1024,.depth=1,.mips=11,.type=5,.file_format=4,.data_offset=128,.block_bytes=16};
  if(argc>1) {
    LevelFn fn=argv[1][0]=='o'?old_level:new_level;
    if(argc>2)base.type=(uint32_t)strtoul(argv[2],NULL,10);
    if(base.type!=5 && argv[1][0]!='o') fn=new_original_level;
    unsigned faces=base.type==5?6:1;Result r;uint64_t sum=0,start=ns();
    const unsigned repeats=15000;
    for(unsigned n=0;n<repeats;++n)for(unsigned f=0;f<faces;++f)for(unsigned l=0;l<base.mips;++l){
      assert(fn(&base,SIZE_MAX,f,l,&r.offset,&r.w,&r.h,&r.d,&r.p,&r.r));sum+=r.offset+r.w+r.p;
    }
    printf("{\"ns_per_texture\":%.6f,\"checksum\":%" PRIu64 "}\n",(double)(ns()-start)/repeats,sum);return 0;
  }
  compare(NULL,0,0,0);
  for(unsigned seed=0;seed<256;++seed) {
    NativeD3DXImage i=base;
    i.width=1+next()%16384;i.height=1+next()%16384;i.depth=seed%3?1:1+next()%2048;
    i.mips=1+next()%15;i.type=seed%3+3;i.block_bytes=seed%4==0?8:seed%4==1?16:0;
    i.pixel_bytes=i.block_bytes?0:1+next()%16;i.data_offset=seed%17==0?SIZE_MAX-10:128;
    i.base_pitch=seed%5==0?i.width*(i.pixel_bytes?i.pixel_bytes:4)+32:0;
    for(unsigned face=0;face<7;++face)for(unsigned l=0;l<=i.mips;++l) {
      Result r;int valid=old_level(&i,SIZE_MAX,face,l,&r.offset,&r.w,&r.h,&r.d,&r.p,&r.r);
      compare(&i,0,face,l);compare(&i,127,face,l);compare(&i,128,face,l);compare(&i,SIZE_MAX,face,l);
      if(valid) {
        size_t end=r.offset+(size_t)r.p*r.r*r.d;
        compare(&i,r.offset,face,l);compare(&i,end,face,l);
        if(end)compare(&i,end-1,face,l);
        if(end<SIZE_MAX)compare(&i,end+1,face,l);
      }
    }
  }
  // Independent authored offsets: 8x4 BC1 has 16+8+8+8 bytes per face.
  base.width=8;base.height=4;base.mips=4;base.block_bytes=8;
  const size_t offsets[]={128,144,152,160};Result r;
  for(unsigned f=0;f<6;++f)for(unsigned l=0;l<4;++l){
    assert(new_level(&base,368,f,l,&r.offset,&r.w,&r.h,&r.d,&r.p,&r.r));assert(r.offset==offsets[l]+f*40);
  }
  base.file_format=0;compare(&base,368,0,0);base.file_format=4;
  old_steps=new_steps=0;
  for(unsigned f=0;f<6;++f)for(unsigned l=0;l<4;++l)compare(&base,368,f,l);
  assert(old_steps==300&&new_steps==90);
  printf("DDS_LAYOUT passed cases=%" PRIu64 " cube_steps=%" PRIu64 "->%" PRIu64 "\n",cases,old_steps,new_steps);
}
