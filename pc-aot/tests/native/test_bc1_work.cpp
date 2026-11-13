#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
using D3DFORMAT=uint32_t;
constexpr D3DFORMAT D3DFMT_DXT1=0x31545844,D3DFMT_DXT2=0x32545844,D3DFMT_DXT3=0x33545844,D3DFMT_DXT4=0x34545844,D3DFMT_DXT5=0x35545844;
/* ACTUAL_BODY */
static uint64_t digest=14695981039346656037ull,cases;
static uint32_t seed=7939;
static uint32_t random_word(){seed^=seed<<13;seed^=seed>>17;return seed^=seed<<5;}
static void hash(const void*p,size_t n){for(size_t k=0;k<n;++k)digest=(digest^((const uint8_t*)p)[k])*1099511628211ull;}
int main(int argc,char**argv){
    uint8_t blocks[4096][16],out[4096][160];for(auto&b:blocks)for(auto&v:b)v=(uint8_t)random_word();
    if(argc>1){unsigned type=argc>2?(unsigned)strtoul(argv[2],nullptr,10):1;auto start=std::chrono::steady_clock::now();
        for(unsigned r=0;r<1000;++r)for(unsigned i=0;i<4096;++i)decode_block(D3DFMT_DXT1+(type-1)*0x01000000,blocks[i],out[i],20,4,4);
        double ns=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count()/(1000*4096.);
        hash(out,sizeof out);printf("{\"ns\":%.6f,\"digest\":%llu}\n",ns,(unsigned long long)digest);return 0;}
    for(unsigned type=0;type<5;++type)for(int h=-1;h<=5;++h)for(int w=-1;w<=5;++w)
        for(unsigned i=0;i<4096;++i){memset(out[0],0xa7,160);decode_block(D3DFMT_DXT1+type*0x01000000,blocks[i],out[0]+1,23,w,h);hash(out[0],160);++cases;}
    // Independent BC1 transparent selector and primary endpoints.
    uint8_t b[8]={0,0,0xff,0xff,0xff,0xff,0xff,0xff};memset(out[0],0xa7,160);
    decode_block(D3DFMT_DXT1,b,out[0],16,4,4);for(unsigned k=0;k<64;++k)assert(out[0][k]==0);
    b[0]=0;b[1]=0xf8;b[2]=b[3]=b[4]=b[5]=b[6]=b[7]=0;
    decode_block(D3DFMT_DXT1,b,out[0],16,4,4);for(unsigned k=0;k<16;++k){assert(out[0][k*4]==0&&out[0][k*4+1]==0&&out[0][k*4+2]==255&&out[0][k*4+3]==255);}
    printf("{\"cases\":%llu,\"digest\":%llu}\n",(unsigned long long)cases,(unsigned long long)digest);
}
