// Authored guest arguments and synchronous device sink, no game data or GPU.
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

static thread_local bool countAllocations = false;
static thread_local uint64_t allocations = 0;
void *operator new(size_t n) {
  if (countAllocations) ++allocations;
  if (void *p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, size_t) noexcept { std::free(p); }

using HRESULT = int32_t;
using BOOL = int32_t;
static constexpr HRESULT D3DERR_INVALIDCALL = int32_t(0x8876086cu);
struct CPU { uint32_t r[8] = {}; };
alignas(16) static uint8_t memory[1024*1024];
#define M32(p) (*reinterpret_cast<uint32_t *>(memory + (p)))
#define GPTR(p) (memory + (p))
#define GSTR(p) reinterpret_cast<const char *>(GPTR(p))
#define ARG(n) M32(c->r[4] + 4 + 4 * (n))
#define RET(v, nargs) do { c->r[0] = (uint32_t)(v); c->r[4] += 4 + 4 * (nargs); return; } while (0)
#define SHIM(name) void name(CPU *c)

static uint64_t digest = 1469598103934665603ull, deviceCalls = 0, cases = 0;
static bool collect = true;
static void hash_bytes(const void *p, size_t n) {
  if (!collect) return;
  for (size_t i=0; i<n; ++i) digest = (digest ^ static_cast<const uint8_t *>(p)[i]) * 1099511628211ull;
}
static void reenter();
struct IDirect3DDevice9 {
  HRESULT result = 0;
  bool nested = false;
  unsigned lastKind = 0, lastIndex = 0, lastCount = 0;
  uint32_t last[4096] = {};
  HRESULT accept(unsigned kind, unsigned index, const void *p, unsigned count) {
    assert(count <= 1024);
    const unsigned words = count * (kind % 3 == 0 ? 1 : 4);
    uint32_t before[4096];
    std::memcpy(before, p, words * 4);
    if (nested) {
      nested = false; reenter();
      assert(!std::memcmp(before, p, words * 4));
    }
    lastKind = kind; lastIndex = index; lastCount = count;
    std::memcpy(last, p, words * 4);
    ++deviceCalls;
    hash_bytes(&kind, sizeof kind); hash_bytes(&index, sizeof index);
    hash_bytes(&count, sizeof count); hash_bytes(p, words * 4);
    return result;
  }
  HRESULT SetVertexShaderConstantB(unsigned i, const BOOL *p, unsigned n) {return accept(0,i,p,n);}
  HRESULT SetVertexShaderConstantI(unsigned i, const int *p, unsigned n) {return accept(1,i,p,n);}
  HRESULT SetVertexShaderConstantF(unsigned i, const float *p, unsigned n) {return accept(2,i,p,n);}
  HRESULT SetPixelShaderConstantB(unsigned i, const BOOL *p, unsigned n) {return accept(3,i,p,n);}
  HRESULT SetPixelShaderConstantI(unsigned i, const int *p, unsigned n) {return accept(4,i,p,n);}
  HRESULT SetPixelShaderConstantF(unsigned i, const float *p, unsigned n) {return accept(5,i,p,n);}
} device;
static void *com_host(uint32_t id) {return id == 7 ? &device : nullptr;}
// ACTUAL_CONSTANT_PATH

static CPU request(uint32_t stack, unsigned count, uint32_t input=4096) {
  CPU c; c.r[4]=stack;
  M32(stack+4)=1; M32(stack+8)=7; M32(stack+12)=1;
  M32(stack+16)=input; M32(stack+20)=count;
  return c;
}
static uint32_t bits(float value) {uint32_t word;std::memcpy(&word,&value,4);return word;}
static void input(unsigned mode, bool pointers) {
  for (unsigned i=0;i<4096;++i) {
    float f = i%13 == 0 ? -0.0f : (int(i%127)-63)*0.25f;
    M32(4096+i*4) = mode==1 || mode==2 ? uint32_t(int(i%127)-63) : bits(f);
  }
  if (pointers) for (unsigned i=0;i<128;++i) M32(32768+i*4) = i%3 == 0 ? 0 : 4096+i*64;
}
static void exercise(unsigned mode, bool immediate, bool pointers, bool raw, unsigned n) {
  input(mode,pointers);
  CPU c=request(64,n,pointers?32768:4096);
  if(immediate) M32(80)= mode==1 || mode==2 ? uint32_t(-7) : bits(-1.25f);
  const unsigned nargs=immediate?4:5;
  set_value(&c,mode,nargs,immediate,pointers,raw);
  assert(c.r[4]==64+4+4*nargs);
  hash_bytes(c.r,sizeof c.r); ++cases;
}
static void reenter() {
  CPU inner=request(128,1,8192);M32(140)=2;
  // The active table's second handle always has a distinct, small vector.
  set_value(&inner,3,4);
  assert(inner.r[4]==148);
}
static void correctness() {
  Table table{}; table.roots=2;
  table.constants.push_back(Constant{});table.constants.push_back(Constant{});
  tables.emplace(1,std::move(table));
  for(unsigned stage: {0xfffeu,0xffffu})
    for(unsigned set=0;set<3;++set)
      for(unsigned cls=0;cls<4;++cls)
        for(unsigned rows=1;rows<=4;++rows)
          for(unsigned cols=1;cols<=4;++cols)
            for(unsigned count: {0u,1u,3u,4u,5u,16u,256u}) {
              auto &t=tables.at(1);t.version=stage<<16;
              t.constants[0]=Constant{65536,set,11,count,cls,3,rows,cols,7,0,0,0};
              for(unsigned mode=0;mode<6;++mode) {
                exercise(mode,false,false,false,5);
                if(mode<3)exercise(mode,true,false,false,1);
                if(mode>=4)exercise(mode,false,true,false,5);
              }
              for(unsigned type=1;type<=3;++type) {
                t.constants[0].type=type;
                exercise(type==1?2:type==2?1:0,false,false,true,67);
              }
            }
  auto &t=tables.at(1);t.version=0xfffe0000u;
  t.constants[0]=Constant{65536,2,11,4,2,3,4,4,1,0,64,0};
  t.constants[1]=Constant{65568,2,3,1,1,3,1,4,1,0,16,0};
  std::strcpy(reinterpret_cast<char *>(memory+65536),"matrix");
  std::strcpy(reinterpret_cast<char *>(memory+65568),"other");
  for(unsigned i=0;i<16;++i)M32(4096+i*4)=bits(float(i+1));
  CPU c=request(64,1);SetMatrix(&c);
  assert(device.lastKind==2&&device.lastCount==4&&device.lastIndex==11);
  for(unsigned i=0;i<16;++i)assert(device.last[i]==bits(float(i+1)));
  c=request(64,1);SetMatrixTranspose(&c);
  for(unsigned r=0;r<4;++r)for(unsigned col=0;col<4;++col)assert(device.last[r*4+col]==bits(float(col*4+r+1)));
  for(unsigned variant=0;variant<10;++variant) {
    c=request(64,1);
    Constant saved=t.constants[0];
    switch(variant) {
      case 0:M32(68)=999;break;
      case 1:M32(76)=0;break;
      case 2:M32(76)=3;break;
      case 3:M32(76)=65536;break; // name handle
      case 4:M32(80)=0;break;
      case 5:t.constants[0].members=1;break;
      case 6:t.constants[0].set=3;break;
      case 7:M32(72)=0;break;
      case 8:device.result=int32_t(0x80004005u);break;
      case 9:t.constants[0].count=0;break;
    }
    SetMatrix(&c);hash_bytes(c.r,sizeof c.r);++cases;
    t.constants[0]=saved;device.result=0;
  }
  for(unsigned i=0;i<4;++i)M32(8192+i*4)=bits(float(-12-int(i)));
  device.nested=true;c=request(64,1);SetMatrix(&c);assert(!device.nested);
  // Both native threads use the actual table mutex and independent stack frames.
  collect=false;
  auto concurrent=[](uint32_t stack){for(unsigned i=0;i<256;++i){CPU c=request(stack,1);SetMatrix(&c);assert(c.r[0]==0);}};
  std::thread a(concurrent,256),b(concurrent,384);a.join();b.join();
  collect=true;
}
static uint64_t count_for(unsigned registers) {
  tables.at(1).constants[0].count=registers;
  CPU c=request(64,1);allocations=0;countAllocations=true;
  SetMatrix(&c);countAllocations=false;return allocations;
}
static void benchmark(unsigned registers,unsigned mode) {
  Table table{};table.version=0xfffe0000u;table.roots=1;
  table.constants.push_back(Constant{0,2,0,registers,2,3,4,4,1,0,64,0});
  tables.emplace(1,std::move(table));input(mode,false);collect=false;
  const unsigned rounds=300000;allocations=0;countAllocations=true;
  auto start=std::chrono::steady_clock::now();
  for(unsigned i=0;i<rounds;++i){CPU c=request(64,1);set_value(&c,mode,4);}
  const double ns=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count()/rounds;
  countAllocations=false;
  std::printf("{\"registers\":%u,\"mode\":%u,\"ns\":%.6f,\"allocations\":%llu,\"calls\":%u}\n",registers,mode,ns,(unsigned long long)allocations,rounds);
}
int main(int argc,char **argv) {
  if(argc==3){benchmark(unsigned(std::strtoul(argv[1],nullptr,10)),unsigned(std::strtoul(argv[2],nullptr,10)));return 0;}
  correctness();
  const uint64_t small=count_for(4),large=count_for(5);
  std::printf("{\"cases\":%llu,\"digest\":%llu,\"device_calls\":%llu,\"reentrant\":true,\"concurrent\":512,\"small_allocations\":%llu,\"large_allocations\":%llu}\n",
    (unsigned long long)cases,(unsigned long long)digest,(unsigned long long)deviceCalls,(unsigned long long)small,(unsigned long long)large);
}
