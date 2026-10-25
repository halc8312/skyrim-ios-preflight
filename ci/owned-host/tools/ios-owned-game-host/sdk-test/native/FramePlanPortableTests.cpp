// Public packet refusal tests use only frame_probe_fixtures' authored packets.
#include "../../native/FramePlanPacket.h"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>
#define CHECK(x) do {if(!(x)){std::cerr<<"check failed line "<<__LINE__<<"\n";std::abort();}}while(0)
using Bytes=std::vector<uint8_t>;
void word(Bytes &b,size_t at,uint32_t value){for(size_t i=0;i<4;++i)b[at+i]=uint8_t(value>>(8*i));}
bool valid(const Bytes &b){return ohframe::Packet{{b.data(),b.size()}}.validate();}
int main(int argc,char **argv){
    CHECK(argc==2);size_t refused=0,accepted=0;
    for(int id=1;id<=9;++id){
        std::ifstream f(std::string(argv[1])+"/frame-"+std::to_string(id)+".bin",std::ios::binary);CHECK(f.good());
        Bytes original{std::istreambuf_iterator<char>(f),{}};CHECK(original.size()<4u<<20);CHECK(valid(original));
        const ohframe::Packet p{{original.data(),original.size()}};
        auto bad=[&](size_t at,uint32_t value){Bytes b=original;word(b,at,value);CHECK(!valid(b));++refused;};
        for(size_t n=0;n<64;++n){Bytes b(original.begin(),original.begin()+n);CHECK(!valid(b));++refused;}
        for(size_t n:{size_t(1),size_t(4),size_t(8)}){Bytes b(original.begin(),original.end()-n);CHECK(!valid(b));++refused;}
        bad(0,0);bad(4,0);bad(8,2);bad(12,0);bad(12,1025);bad(16,0);bad(20,65);bad(24,0);bad(24,17);bad(28,129);bad(32,1281);
        for(size_t i=9;i<=14;++i)bad(i*4,p.word(i)+1);bad(60,1);
        auto r=p.resource(0);bad(r+4,1);bad(r+8,0);bad(r+16,p.u32(r+16)+8);bad(r+20,0);bad(r+20,0xffffffffu);
        {Bytes b=original;word(b,p.resource(1),p.u32(r));word(b,p.resource(1)+8,p.u32(r+8));word(b,p.resource(1)+12,p.u32(r+12));CHECK(!valid(b));++refused;}
        auto pass=p.pass(0);bad(pass+40,p.word(7)+1);bad(pass+44,p.word(7)+1);bad(pass+48,0);bad(pass+48,4);bad(pass+52,1);
        bad(pass+4,0x7ff80000);bad(pass+36,0x40000000);
        auto d=p.draw(0);bad(d+8,0x7fc00000);bad(d+8,0xbf800000);bad(d+24,0xffffffffu);bad(d+32,0);bad(d+36,0);bad(d+76,1);
        bad(d+44,0x40000000);bad(d+80,0);bad(d+80,3);bad(d+84,0);bad(d+84,65537);bad(d+88,1);bad(d+92,p.word(8)+1);bad(d+96,4);bad(d+100,65);bad(d+104,3);bad(d+112,1);bad(d+116,1);
        if(p.u32(d+100)){bad(d+108,2);bad(d+112,0);bad(d+112,0xffffffff);}
        auto a=p.binding(0);bad(a,2);bad(a+4,5);bad(a+8,p.word(5));bad(a+12,1);bad(a+16,4);bad(a+16,16);bad(a+16,128);bad(a+16,0xffffffff);bad(a+24,0);bad(a+24,3);bad(a+28,1);
        {Bytes b=original;word(b,p.binding(1),p.u32(a));word(b,p.binding(1)+4,p.u32(a+4));CHECK(!valid(b));++refused;}
        // Structured fuzz is a safety check, not a requirement that arbitrary
        // legal state/data changes be rejected. No original packet is modified.
        uint32_t random=0x81340227u;
        for(size_t i=0;i<2048;++i){Bytes b=original;random=random*1664525+1013904223;
            size_t at=(random%(b.size()/4))*4;random=random*1664525+1013904223;word(b,at,random);
            if(valid(b))++accepted;
        }
        CHECK(valid(original));
    }
    std::cout<<"authored cases=9 explicit refusals="<<refused<<" structured fuzz=18432 accepted legal changes="<<accepted<<"\n";
}
