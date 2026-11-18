#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>
using HRESULT=int32_t;using LONG=int32_t;using D3DFORMAT=uint32_t;
constexpr HRESULT D3DERR_INVALIDCALL=-1,D3DERR_NOTAVAILABLE=-2;
#define FAILED(hr) ((hr)<0)
#define SUCCEEDED(hr) ((hr)>=0)
#define LOG(...) ((void)0)
constexpr unsigned D3DPOOL_SYSTEMMEM=2;
constexpr D3DFORMAT D3DFMT_R8G8B8=20,D3DFMT_A8R8G8B8=21,D3DFMT_X8R8G8B8=22,D3DFMT_R5G6B5=23,D3DFMT_X1R5G5B5=24,D3DFMT_A1R5G5B5=25,D3DFMT_A4R4G4B4=26,D3DFMT_A8=28,D3DFMT_A8B8G8R8=32,D3DFMT_X8B8G8R8=33,D3DFMT_L8=50,D3DFMT_A8L8=51;
constexpr D3DFORMAT D3DFMT_DXT1=0x31545844,D3DFMT_DXT2=0x32545844,D3DFMT_DXT3=0x33545844,D3DFMT_DXT4=0x34545844,D3DFMT_DXT5=0x35545844;
struct RECT{LONG left,top,right,bottom;};struct POINT{LONG x,y;};
struct D3DSURFACE_DESC{unsigned Width,Height;D3DFORMAT Format;};struct D3DLOCKED_RECT{int Pitch;void*pBits;};
struct IDirect3DDevice9;
static uint64_t events;static int failure;
struct IDirect3DSurface9{
    D3DSURFACE_DESC desc;std::vector<uint8_t> bytes;unsigned pitch;bool temporary;
    IDirect3DSurface9(unsigned w,unsigned h,D3DFORMAT f,bool temp=false):desc{w,h,f},bytes((w*4+32)*(h+4),0xa7),pitch(w*4+32),temporary(temp){}
    HRESULT GetDesc(D3DSURFACE_DESC*out){events=events*7+1;*out=desc;return failure==1?-5:0;}
    HRESULT LockRect(D3DLOCKED_RECT*out,const RECT*r,unsigned){events=events*7+2;
        if((failure>=2&&!temporary)||failure==4)return -6;
        out->Pitch=pitch;out->pBits=bytes.data()+r->top*pitch+r->left*4;return 0;}
    HRESULT GetDevice(IDirect3DDevice9**out);
    void UnlockRect(){events=events*7+3;}void Release(){events=events*7+4;delete this;}
};
struct IDirect3DDevice9{
    HRESULT CreateOffscreenPlainSurface(unsigned w,unsigned h,D3DFORMAT f,unsigned,IDirect3DSurface9**out,void*){events=events*7+5;if(failure==3)return -7;*out=new IDirect3DSurface9(w,h,f,true);return 0;}
    HRESULT UpdateSurface(IDirect3DSurface9*temp,const RECT*,IDirect3DSurface9*dst,const POINT*){events=events*7+6;if(failure==5)return -8;dst->bytes=temp->bytes;return 0;}
    void Release(){events=events*7+7;}
};
static IDirect3DDevice9 device;
HRESULT IDirect3DSurface9::GetDevice(IDirect3DDevice9**out){events=events*7+8;*out=&device;return 0;}
/* ACTUAL_BODY */
static uint64_t digest=14695981039346656037ull,cases;
static void hash(const void*p,size_t n){for(size_t k=0;k<n;++k)digest=(digest^((const uint8_t*)p)[k])*1099511628211ull;}
int main(int argc,char**argv){
    const unsigned dimension=argc>1?(unsigned)strtoul(argv[1],nullptr,10):36;
    std::vector<uint8_t> input((dimension+16)*(dimension+16)*4);for(size_t k=0;k<input.size();++k)input[k]=(uint8_t)(k*97+31);
    if(argc>1){IDirect3DSurface9 out(dimension,dimension,D3DFMT_A8B8G8R8);RECT sr{0,0,(LONG)dimension,(LONG)dimension};
        unsigned rounds=200;auto start=std::chrono::steady_clock::now();for(unsigned n=0;n<rounds;++n)assert(!native_d3dx_surface_memory(&out,nullptr,input.data(),D3DFMT_A8R8G8B8,dimension*4,&sr,2,0));
        hash(out.bytes.data(),out.bytes.size());printf("{\"ns\":%.6f,\"digest\":%llu}\n",std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count()/rounds,(unsigned long long)digest);return 0;}
    const D3DFORMAT formats[]={20,21,22,23,24,25,26,28,32,33,50,51,D3DFMT_DXT1,D3DFMT_DXT3,D3DFMT_DXT5};
    for(auto src:formats)for(auto dest:formats)for(int f=0;f<6;++f)for(unsigned filter:{1u,2u,3u,7u}){
        failure=f;IDirect3DSurface9 out(24,20,dest);RECT sr{0,0,17,13},dr{0,0,23,19};events=0;
        HRESULT hr=native_d3dx_surface_memory(&out,&dr,input.data(),src,144,&sr,filter,(f&1)?0x42e1801f:0);
        hash(&hr,sizeof hr);hash(&events,sizeof events);hash(out.bytes.data(),out.bytes.size());++cases;
        RECT identity{0,0,17,13};events=0;
        hr=native_d3dx_surface_memory(&out,&identity,input.data(),src,144,&sr,filter,(f&1)?0x42e1801f:0);
        hash(&hr,sizeof hr);hash(&events,sizeof events);hash(out.bytes.data(),out.bytes.size());++cases;
    }
    failure=0;IDirect3DSurface9 out(16,16,D3DFMT_A8R8G8B8);RECT sr{0,0,16,16};
    assert(native_d3dx_surface_memory(nullptr,nullptr,input.data(),21,64,&sr,2,0)==D3DERR_INVALIDCALL);
    assert(native_d3dx_surface_memory(&out,nullptr,nullptr,21,64,&sr,2,0)==D3DERR_INVALIDCALL);
    // Unchanged same-format copy bypasses the intermediate allocation entirely.
    assert(!native_d3dx_surface_memory(&out,nullptr,input.data(),21,64,&sr,2,0));hash(out.bytes.data(),out.bytes.size());++cases;
    for(int n=1;n<=8;++n){RECT cropped{4,4,4+n,4+n},dest{2,3,2+n,3+n};events=0;
        assert(!native_d3dx_surface_memory(&out,&dest,input.data(),D3DFMT_A8B8G8R8,144,&cropped,2,0));
        hash(out.bytes.data(),out.bytes.size());hash(&events,sizeof events);++cases;
    }
    // Compare the actual converter against its original division path at
    // odd ratios, thin dimensions, limits, and the deliberately retained
    // fallback just beyond the new output bound. Hash padding and API order.
    const unsigned shapes[][4]={{1,1,127,63},{127,63,1,1},{17,13,31,19},
        {31,19,17,13},{16384,1,8191,2},{8191,2,16384,1},{1,16384,2,8191},
        {2,8191,1,16384},{16384,1,16385,1},{1,16384,1,16385}};
    for(const auto &shape:shapes){
        unsigned sw=shape[0],sh=shape[1],w=shape[2],h=shape[3],pitch=sw*4+12;
        std::vector<uint8_t> pixels((size_t)pitch*sh);
        for(size_t k=0;k<pixels.size();++k)pixels[k]=(uint8_t)(k*131+37);
        for(unsigned filter:{0u,1u,2u,0x10002u,3u})for(unsigned colorKey:{0u,0xae2ba825u}){
            IDirect3DSurface9 scaled(w,h,D3DFMT_A8B8G8R8);RECT source{0,0,(LONG)sw,(LONG)sh};events=0;
            HRESULT hr=native_d3dx_surface_memory(&scaled,nullptr,pixels.data(),D3DFMT_A8R8G8B8,pitch,&source,filter,colorKey);
            assert(hr==0);hash(&hr,sizeof hr);hash(&events,sizeof events);hash(scaled.bytes.data(),scaled.bytes.size());++cases;
        }
    }
    IDirect3DSurface9 alias(17,13,D3DFMT_A8B8G8R8);RECT aliasSource{0,0,13,7};events=0;
    for(size_t k=0;k<alias.bytes.size();++k)alias.bytes[k]=(uint8_t)(k*23+9);
    assert(!native_d3dx_surface_memory(&alias,nullptr,alias.bytes.data(),D3DFMT_A8R8G8B8,alias.pitch,&aliasSource,2,0));
    hash(alias.bytes.data(),alias.bytes.size());hash(&events,sizeof events);++cases;
    printf("{\"cases\":%llu,\"digest\":%llu}\n",(unsigned long long)cases,(unsigned long long)digest);
}
