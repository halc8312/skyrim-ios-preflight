/* Authored inputs; compare against a separately constructed contiguous DDS. */
#include "dds_view.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static unsigned cases;
static size_t level_bytes(unsigned w,unsigned h,unsigned block) {
  return (size_t)((w+3)/4)*((h+3)/4)*block;
}
static size_t run(unsigned w,unsigned h,unsigned format,unsigned skip,unsigned tail) {
  unsigned block=format==1?8:16, mips=1;
  for(unsigned n=w>h?w:h;n>1;n>>=1)++mips;
  size_t length=128+tail,cut=128;unsigned x=w,y=h;
  for(unsigned l=0;l<mips;++l) {
    size_t n=level_bytes(x,y,block);length+=n;if(l<skip)cut+=n;
    if(x>1)x/=2;
    if(y>1)y/=2;
  }
  unsigned char *src=calloc(1,length);assert(src);
  memcpy(src,"DDS ",4);view_put32(src+4,124);view_put32(src+76,32);
  view_put32(src+80,4);view_put32(src+84,0x30545844u+(format<<24));
  view_put32(src+12,h);view_put32(src+16,w);view_put32(src+28,mips);
  for(size_t n=128;n<length;++n)src[n]=(unsigned char)(n*13+(n>>9));
  size_t flat_bytes=128+length-cut;
  unsigned char *flat=malloc(flat_bytes);assert(flat);
  memcpy(flat,src,128);memcpy(flat+128,src+cut,length-cut);
  x=w;y=h;for(unsigned n=0;n<skip;++n){if(x>1)x/=2;if(y>1)y/=2;}
  view_put32(flat+12,y);view_put32(flat+16,x);view_put32(flat+28,mips-skip);
  SkyrimDdsView view;assert(skyrim_dds_view(src,length,skip,&view));
  assert(!memcmp(flat,view.header,128) && view.payload==src+cut);
  assert(view.payload_bytes==length-cut && !memcmp(flat+128,view.payload,length-cut));
  NativeD3DXImage fi;assert(native_d3dx_image_info(flat,flat_bytes,&fi));
  assert(fi.width==view.image.width && fi.height==view.image.height &&
         fi.depth==view.image.depth && fi.mips==view.image.mips &&
         fi.format==view.image.format && fi.type==view.image.type &&
         fi.file_format==view.image.file_format && fi.data_offset==view.image.data_offset &&
         fi.block_bytes==view.image.block_bytes && fi.pixel_bytes==view.image.pixel_bytes &&
         fi.base_pitch==view.image.base_pitch);
  unsigned char *staging=malloc(flat_bytes);assert(staging);memset(staging,0xcc,flat_bytes);
  size_t consumed=0;
  for(unsigned l=0;l<mips-skip;++l) {
    size_t at,n;uint32_t a,b,d,p,r,vp,vr;const unsigned char *data;
    assert(native_d3dx_dds_level(&fi,flat_bytes,0,l,&at,&a,&b,&d,&p,&r));
    assert(skyrim_dds_view_level(&view,l,&data,&n,&vp,&vr));
    assert(n==(size_t)p*r*d && vp==p && vr==r && !memcmp(data,flat+at,n));
    /* Independently owned upload staging remains valid after source release. */
    memcpy(staging+at,data,n);consumed+=n;
  }
  assert(!memcmp(staging+128,flat+128,consumed));
  SkyrimDdsView sentinel,unchanged;memset(&sentinel,0xa7,sizeof sentinel);memcpy(&unchanged,&sentinel,sizeof sentinel);
  assert(!skyrim_dds_view(src,length,mips,&sentinel)&&!memcmp(&sentinel,&unchanged,sizeof sentinel));
  assert(!skyrim_dds_view(src,length-tail-1,skip,&sentinel)&&!memcmp(&sentinel,&unchanged,sizeof sentinel));
  assert(!skyrim_dds_view(src,127,skip,&sentinel));
  view_put32(src+112,0x200);assert(!skyrim_dds_view(src,length,skip,&sentinel));
  view_put32(src+112,0x200000);assert(!skyrim_dds_view(src,length,skip,&sentinel));
  view_put32(src+112,0);view_put32(src+16,7);assert(!skyrim_dds_view(src,length,skip,&sentinel));
  assert(!memcmp(&sentinel,&unchanged,sizeof sentinel));
  free(src);assert(!memcmp(staging+128,flat+128,consumed));free(staging);free(flat);
  ++cases;return length-cut;
}
int main(void) {
  for(unsigned f=1;f<=5;++f)for(unsigned w=1;w<=64;w*=2)for(unsigned h=1;h<=32;h*=2) {
    unsigned m=1;for(unsigned n=w>h?w:h;n>1;n>>=1)++m;
    for(unsigned s=0;s<m;++s){run(w,h,f,s,0);run(w,h,f,s,19);}
  }
  size_t saved=run(4096,4096,1,1,0);
  printf("DDS_VIEW cases=%u payload_copy_bytes=%zu->0 header_copy_bytes=128 borrowed_owner=required production_enabled=false\n",cases,saved);
}
