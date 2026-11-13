#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
typedef struct {int unused;} XWmaDecoder;
/* ACTUAL_BODY */
static uint64_t digest=UINT64_C(14695981039346656037),cases,nans,negative_nans;
// Compare all finite/Inf/signed-zero bits exactly. IEEE NaN sign/payload are
// not specified by arithmetic; record them separately, preserving NaN class.
static void hash(const void *p,size_t n){assert(n%4==0);for(size_t i=0;i<n;i+=4){
    uint32_t word;memcpy(&word,(const uint8_t*)p+i,4);
    if((word&0x7f800000u)==0x7f800000u&&(word&0x7fffffu)){++nans;negative_nans+=word>>31;word=0x7fc00000u;}
    for(unsigned k=0;k<4;++k)digest=(digest^((word>>(k*8))&255))*UINT64_C(1099511628211);
}}
static uint64_t now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
int main(int argc,char **argv){
    Voice *v=calloc(1,sizeof *v),*d=calloc(1,sizeof *d);assert(v&&d);
    float in[8*513],out[8*513];v->send[0]=d;v->nsends=1;d->acc=out;v->volume=.75f;
    for(int i=0;i<8;++i)v->chvol[i]=(float)(i+1)/8;
    if(argc>1){unsigned rounds=100000;v->in_ch=argc>2?atoi(argv[2]):2;d->in_ch=2;
        for(int i=0;i<8*513;++i){in[i]=(float)(i%13-6)/16;out[i]=0;}
        uint64_t start=now();for(unsigned r=0;r<rounds;++r)route(v,in,480);
        printf("{\"ns\":%.6f,\"sum\":%.9g}\n",(double)(now()-start)/rounds,(double)out[0]);free(v);free(d);return 0;}
    for(int sc=1;sc<=8;++sc)for(int dc=1;dc<=8;++dc)for(int explicit_matrix=0;explicit_matrix<2;++explicit_matrix)
    for(int shape=0;shape<5;++shape){
        int frames=(int[]){0,1,7,480,513}[shape];v->in_ch=sc;d->in_ch=dc;v->matrix_set[0]=explicit_matrix;
        for(int k=0;k<64;++k)v->matrix[0][k]=(float)(k%9-4)/16;
        for(int k=0;k<8*513;++k){in[k]=(float)(k%29-14)/32;out[k]=(k%3==0)?-0.0f:.125f;}
        if(shape==2){in[0]=-0.0f;in[1]=INFINITY;in[2]=-INFINITY;in[3]=NAN;}
        route(v,in,frames);hash(out,sizeof out);++cases;
        // No send uses the current master, and missing/dead destinations are skipped.
        v->nsends=0;g_master=d;route(v,in,frames);hash(out,sizeof out);++cases;v->nsends=1;g_master=NULL;
        d->dead=1;route(v,in,frames);hash(out,sizeof out);++cases;d->dead=0;
    }
    // Self-send and overlapping accumulation preserve the original write/read order.
    v->in_ch=d->in_ch=2;v->matrix_set[0]=1;
    for(int shift=0;shift<=2;++shift){
        for(int k=0;k<8*513;++k)in[k]=(float)(k%13-6)/16;
        d->acc=in+shift;route(v,in,480);hash(in,sizeof in);++cases;
    }
    free(v);free(d);printf("{\"cases\":%" PRIu64 ",\"digest\":%" PRIu64 ",\"nans\":%" PRIu64 ",\"negative_nans\":%" PRIu64 "}\n",cases,digest,nans,negative_nans);
}
