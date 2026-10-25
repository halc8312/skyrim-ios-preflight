#include "../../native/SampledFramePacket.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>
static void check(bool value,const char *message){if(!value){std::fprintf(stderr,"sampled packet check failed: %s\n",message);std::exit(1);}}
static void put(std::vector<uint8_t>&b,size_t at,uint32_t value){check(at+4<=b.size(),"test offset");for(size_t j=0;j<4;++j)b[at+j]=uint8_t(value>>(j*8));}
static bool valid(const std::vector<uint8_t>&b){return ohsample::Packet{{{b.data(),b.size()}}}.validate();}
int main(int argc,char **argv){
    check(argc==2,"authored fixture directory");size_t refusals=0,mutations=0,accepted=0;
    for(unsigned id=1;id<=15;++id){
        auto path=std::filesystem::path(argv[1])/("sampled-"+std::to_string(id)+".bin");
        std::ifstream input(path,std::ios::binary);check(bool(input),"fixture exists");
        std::vector<uint8_t> good((std::istreambuf_iterator<char>(input)),{});check(valid(good),"Rust packet accepted");
        const ohsample::Packet p{{{good.data(),good.size()}}};
        auto reject=[&](size_t at,uint32_t value){auto bad=good;put(bad,at,value);check(!valid(bad),"malformed field refused");++refusals;};
        for(size_t at=0;at<80;at+=4)reject(at,UINT32_MAX);
        for(size_t n=0;n<std::min(good.size(),size_t(2048));++n){check(!ohsample::Packet{{{good.data(),n}}}.validate(),"truncated packet refused");++refusals;}
        reject(24,0);reject(28,0);reject(32,0);
        for(uint32_t i=0;i<p.word(6);++i){auto t=p.texture(i);
            reject(t+4,2);reject(t+8,0);reject(t+16,0);reject(t+20,1025);reject(t+24,UINT32_MAX);reject(t+28,0);reject(t+32,1);
        }
        for(uint32_t i=0;i<p.word(7);++i){auto m=p.mip(i);
            reject(m,UINT32_MAX);reject(m+4,UINT32_MAX);reject(m+8,0);reject(m+8,3);reject(m+12,UINT32_MAX);reject(m+16,UINT32_MAX);reject(m+20,1);
            const auto end=p.wire.u32(m+12)+p.wire.u32(m+16);if(end%8)reject(end,1);
        }
        for(uint32_t i=0;i<p.word(8);++i){auto s=p.sampler(i);
            reject(s+4,1);reject(s+8,0);reject(s+16,3);reject(s+28,2);reject(s+40,0x7fc00000);reject(s+44,0x7f800000);reject(s+44,0xbf800000);reject(s+48,3);reject(s+52,1);
            auto bad=good;put(bad,s+40,0x40000000);put(bad,s+44,0x3f800000);check(!valid(bad),"reversed LOD refused");++refusals;
        }
        for(uint32_t i=0;i<p.word(9);++i){auto a=p.binding(i);reject(a,UINT32_MAX);reject(a+4,128);reject(a+8,0);reject(a+8,3);reject(a+12,UINT32_MAX);}
        if(p.word(6)>1){auto a=p.texture(0),b=p.texture(1);auto bad=good;put(bad,b,p.wire.u32(a));put(bad,b+8,p.wire.u32(a+8));put(bad,b+12,p.wire.u32(a+12));check(!valid(bad),"duplicate texture generation refused");++refusals;}
        if(p.word(8)>1){auto a=p.sampler(0),b=p.sampler(1);auto bad=good;put(bad,b,p.wire.u32(a));put(bad,b+8,p.wire.u32(a+8));put(bad,b+12,p.wire.u32(a+12));check(!valid(bad),"duplicate sampler generation refused");++refusals;}
        if(p.word(9)>=2){auto bad=good;put(bad,p.binding(1)+4,p.wire.u32(p.binding(0)+4));check(!valid(bad),"cross-kind binding alias refused");++refusals;}
        uint32_t random=0x8dc04b13u+id;
        for(unsigned n=0;n<1024;++n){auto bad=good;random=random*1664525u+1013904223u;const size_t at=random%bad.size();
            bad[at]^=uint8_t(1u<<(random%8));accepted+=valid(bad);++mutations;
        }
    }
    std::printf("sampled cases=15 explicit_refusals=%zu structured_mutations=%zu accepted_legal_changes=%zu\n",refusals,mutations,accepted);
}
