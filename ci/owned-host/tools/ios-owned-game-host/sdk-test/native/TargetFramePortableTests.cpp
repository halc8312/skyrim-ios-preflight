#include "../../native/TargetFramePacket.h"
#include "../../native/TargetFrameLifetime.h"
#include "../../native/MetalFrameProbe.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>
static void check(bool v,const char*m){if(!v){std::fprintf(stderr,"target packet check failed: %s\n",m);std::exit(1);}}
static void put(std::vector<uint8_t>&b,size_t a,uint32_t v){check(a+4<=b.size(),"test offset");for(size_t j=0;j<4;++j)b[a+j]=uint8_t(v>>(8*j));}
static bool valid(const std::vector<uint8_t>&b){return ohtarget::Packet{{{b.data(),b.size()}}}.validate();}
int main(int argc,char**argv){
    check(argc==2,"authored fixture directory");size_t refusals=0,mutations=0,accepted=0;
    for(unsigned id=1;id<=16;++id){
        std::ifstream stream(std::filesystem::path(argv[1])/("target-"+std::to_string(id)+".bin"),std::ios::binary);check(bool(stream),"fixture exists");
        std::vector<uint8_t> good((std::istreambuf_iterator<char>(stream)),{});check(valid(good),"Rust packet accepted");
        const ohtarget::Packet p{{{good.data(),good.size()}}};const auto input=p.inputs();
        std::ifstream expected(std::filesystem::path(argv[1])/("target-"+std::to_string(id)+".lifetimes"));
        std::string magic;expected>>magic;check(magic=="authored-target-lifetimes-v1","Rust lifetime fixture exists");
        auto equal=[&](uint64_t actual){uint64_t wanted=UINT64_MAX;expected>>wanted;check(bool(expected)&&actual==wanted,"C++ allocation matches Rust mapping and elision");};
        for(bool reuse:{false,true}){
            auto plan=ohtarget::Allocation::make(p,reuse);check(bool(plan),"validated plan accepted");
            equal(plan->slots.size());equal(plan->mapping.size());equal(plan->load_elided.size());equal(plan->dedicated_bytes);equal(plan->allocated_bytes());
            for(auto s:plan->slots){equal(s.kind);equal(s.width);equal(s.height);}
            for(auto slot:plan->mapping)equal(slot);
            for(size_t i=0;i<plan->load_elided.size();++i){equal(plan->load_elided[i][0]);equal(plan->load_elided[i][1]);equal(plan->copy_elided[i]);}
            check(plan->allocated_bytes()<=plan->dedicated_bytes,"reuse cannot increase storage");
        }
        expected>>std::ws;check(expected.eof(),"no extra lifetime expectations");
        auto reject=[&](size_t a,uint32_t v){auto bad=good;put(bad,a,v);check(!valid(bad),"malformed field refused");++refusals;};
        for(size_t a=0;a<80;a+=4)reject(a,UINT32_MAX);
        for(size_t n=0;n<std::min(good.size(),size_t(2048));++n){check(!ohtarget::Packet{{{good.data(),n}}}.validate(),"truncated packet refused");++refusals;}
        for(uint32_t i=0;i<p.word(6);++i){auto t=p.target(i);
            reject(t+4,0);reject(t+4,3);reject(t+8,0);reject(t+16,0);reject(t+16,1025);reject(t+20,0);reject(t+24,1);reject(t+28,1);
        }
        for(uint32_t i=0;i<p.word(7);++i){auto a=p.operation(i);
            reject(a,0);reject(a,3);for(size_t j=28;j<64;j+=4)reject(a+j,1);
            if(p.wire.u32(a)==1){
                reject(a+4,UINT32_MAX);reject(a+8,UINT32_MAX);reject(a+12,UINT32_MAX);reject(a+16,UINT32_MAX);reject(a+20,UINT32_MAX);reject(a+24,4);
                auto bad=good;put(bad,a+12,p.wire.u32(a+8));check(!valid(bad),"color cannot alias depth");++refusals;
                // Every pass needs a clear OR an initialized load, never both/neither.
                reject(a+16,p.wire.u32(a+16)?0:1);reject(a+20,p.wire.u32(a+20)?0:1);
            }else{
                reject(a+4,0);reject(a+4,3);reject(a+8,UINT32_MAX);reject(a+12,UINT32_MAX);reject(a+16,UINT32_MAX);
                reject(a+20,UINT32_MAX);reject(a+24,UINT32_MAX);
            }
        }
        for(uint32_t i=0;i<p.word(8);++i){auto a=p.binding(i);
            reject(a,UINT32_MAX);reject(a+4,128);reject(a+8,0);reject(a+8,4);reject(a+12,UINT32_MAX);
        }
        if(p.word(8)>=2){auto bad=good;put(bad,p.binding(1)+4,p.wire.u32(p.binding(0)+4));check(!valid(bad),"binding aliases refused");++refusals;}
        auto a=p.target(0),b=p.target(1);auto duplicate=good;
        put(duplicate,b,p.wire.u32(a));put(duplicate,b+8,p.wire.u32(a+8));put(duplicate,b+12,p.wire.u32(a+12));
        check(!valid(duplicate),"target generations are unique across kinds");++refusals;
        // New empty resource bundles remain illegal through the old standalone API.
        if(!input.word(6)||!input.word(8))check(!input.validate(),"standalone OHST contract preserved");
        if(id>=15){
            check(input.geometry().validate_geometry(),"contextual geometry structure accepted");
            check(!input.geometry().validate(),"standalone OHFP first-clear guard preserved");
            check(!input.validate(),"standalone OHST first-clear guard preserved");
            check(input.word(6)>0,"precopy fixture has CPU input");
            if(id==15)check(input.word(8)>0,"first-clear rejection independent of empty sampler table");
            auto late=good;const auto copy=p.operation(0),render=p.operation(1);
            for(size_t j=0;j<64;++j)std::swap(late[copy+j],late[render+j]);
            check(!valid(late),"first load cannot precede its copy");++refusals;
        }
        if(id==1){ // Force the second pass to read its own output instead of the first.
            auto bad=good;bool changed=false;
            for(uint32_t i=0;i<p.word(8);++i){auto x=p.binding(i);
                if(p.wire.u32(x+8)==3){put(bad,x+12,2);changed=true;break;}}
            check(changed&&!valid(bad),"same-pass feedback refused");++refusals;
            auto no_store=good;put(no_store,p.operation(0)+24,2);
            check(!valid(no_store),"reading a discarded source refused");++refusals;
        }
        uint32_t random=0xa18d349bu+id;
        for(unsigned n=0;n<2048;++n){auto bad=good;random=random*1664525u+1013904223u;const size_t at=random%bad.size();
            bad[at]^=uint8_t(1u<<(random%8));const bool okay=valid(bad);accepted+=okay;++mutations;
            if(okay){const ohtarget::Packet changed{{{bad.data(),bad.size()}}};
                for(bool reuse:{false,true}){auto plan=ohtarget::Allocation::make(changed,reuse);check(bool(plan),"accepted mutation allocation safe");
                    check(plan->mapping.size()==changed.word(6)&&plan->allocated_bytes()<=plan->dedicated_bytes,"accepted mutation allocation bounds");}}
        }
    }
    std::printf("target cases=16 explicit_refusals=%zu structured_mutations=%zu accepted_legal_changes=%zu\n",refusals,mutations,accepted);
}
