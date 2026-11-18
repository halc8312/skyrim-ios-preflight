/* Authored, unlinked prototype. Borrowed DDS payload, never a game ABI. */
#pragma once
#include "d3dx_image.h"
#include <limits.h>
#include <string.h>

typedef struct {
  unsigned char header[128];
  NativeD3DXImage image;
  const unsigned char *payload;
  size_t payload_bytes;
} SkyrimDdsView;

static void view_put32(unsigned char *p, uint32_t v) {
  for (unsigned n=0;n<4;++n) p[n]=(unsigned char)(v>>(8*n));
}

/* Owner must keep immutable source alive through synchronous consumption.
 * out must not alias source. No upload, allocation, callback, or fallback here.
 * Refusal leaves out untouched. Unsupported shapes use the existing loader. */
static int skyrim_dds_view(const void *source, size_t bytes, unsigned skip,
                           SkyrimDdsView *out) {
  NativeD3DXImage original;
  if (!out || !native_d3dx_image_info(source,bytes,&original) ||
      original.file_format!=4 || original.type!=3 || original.depth!=1 ||
      !original.block_bytes || (original.width&(original.width-1)) ||
      (original.height&(original.height-1)) || skip>=original.mips) return 0;
  unsigned max_mips=1;
  for (uint32_t n=original.width>original.height?original.width:original.height;
       n>1;n>>=1) ++max_mips;
  if (original.mips>max_mips) return 0;
  size_t begin=0,at; uint32_t w,h,d,p,r;
  /* Check every declared level, including removed levels, before publication. */
  for (unsigned l=0;l<original.mips;++l) {
    if (!native_d3dx_dds_level(&original,bytes,0,l,&at,&w,&h,&d,&p,&r)) return 0;
    if (l==skip) begin=at;
  }
  SkyrimDdsView result={0};
  memcpy(result.header,source,128);
  w=original.width>>skip; h=original.height>>skip;
  if (!w) w=1;
  if (!h) h=1;
  view_put32(result.header+12,h); view_put32(result.header+16,w);
  view_put32(result.header+28,original.mips-skip);
  if (!native_d3dx_image_info(result.header,128,&result.image)) return 0;
  result.payload=(const unsigned char *)source+begin;
  result.payload_bytes=bytes-begin; /* Preserve trailing bytes, too. */
  *out=result;
  return 1;
}

/* view must be a successful, unmodified result of skyrim_dds_view.
 * Only gives an immutable CPU span. An asynchronous consumer must copy to
 * its own staging before the owner releases source. Output unchanged on error. */
static int skyrim_dds_view_level(const SkyrimDdsView *view, unsigned level,
                                const unsigned char **data, size_t *bytes,
                                uint32_t *pitch, uint32_t *rows) {
  if (!view || !data || !bytes || !pitch || !rows ||
      view->payload_bytes>INT_MAX-128u) return 0;
  size_t offset; uint32_t w,h,d,p,r;
  if (!native_d3dx_dds_level(&view->image,128+view->payload_bytes,0,level,
                            &offset,&w,&h,&d,&p,&r)) return 0;
  *data=view->payload+offset-128; *bytes=(size_t)p*r*d;
  *pitch=p; *rows=r;
  return 1;
}
