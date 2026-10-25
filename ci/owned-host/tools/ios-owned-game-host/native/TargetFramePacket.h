#pragma once
#include "SampledFramePacket.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <set>

namespace ohtarget {
// OHRT0001 is an unchanged OHST input bundle plus single-producer targets and
// ordered operations. All accessors require validate() and immutable input bytes.
struct Packet {
    ohframe::Packet wire;
    uint32_t word(size_t i) const{return wire.word(i);}
    size_t target(size_t i)const{return word(10)+i*32;}
    size_t operation(size_t i)const{return word(11)+i*64;}
    size_t binding(size_t i)const{return word(12)+i*16;}
    ohsample::Packet inputs()const{return {{{wire.bytes.subspan(word(4),word(5))}}};}
    ohframe::Packet geometry()const{return inputs().geometry();}
    size_t find_binding(uint32_t draw,uint32_t slot)const{
        for(uint32_t i=0;i<word(8);++i){const auto a=binding(i);if(wire.u32(a)==draw&&wire.u32(a+4)==slot)return a;}
        return SIZE_MAX;
    }
    bool validate()const{
        const uint64_t bytes=wire.bytes.size();
        if(bytes<80||bytes>(4u<<20)||word(0)!=0x5452484f||word(1)!=0x31303030||word(2)!=1
            ||word(3)!=bytes||word(4)!=80||word(5)<80||word(5)%8||uint64_t(word(5))+80>bytes
            ||!word(6)||word(6)>32||!word(7)||word(7)>64||word(8)>4096||word(9)>=word(6)||!wire.zero(52,28))return false;
        const uint64_t to=80+uint64_t(word(5)),oo=to+uint64_t(word(6))*32,bo=oo+uint64_t(word(7))*64;
        if(word(10)!=to||word(11)!=oo||word(12)!=bo||bo+uint64_t(word(8))*16!=bytes)return false;
        const auto input=inputs();if(!input.validate_inputs())return false;const auto base=input.geometry();
        std::set<std::pair<uint32_t,uint64_t>> versions;
        for(uint32_t i=0;i<input.word(6);++i){auto a=input.texture(i);versions.insert({input.wire.u32(a),input.wire.u64(a+8)});}
        uint64_t gpu_bytes=0;
        for(uint32_t i=0;i<word(6);++i){auto a=target(i);const uint64_t w=wire.u32(a+16),h=wire.u32(a+20);auto kind=wire.u32(a+4);
            if((kind!=1&&kind!=2)||!wire.u64(a+8)||!versions.insert({wire.u32(a),wire.u64(a+8)}).second
                ||!w||w>1024||!h||h>1024||!wire.zero(a+24,8))return false;
            gpu_bytes+=w*h*4;
        }
        if(gpu_bytes>(64u<<20))return false;
        uint32_t previous=0,count=0;std::set<uint32_t> slots;
        for(uint32_t i=0;i<word(8);++i){const auto a=binding(i);auto draw=wire.u32(a),slot=wire.u32(a+4),kind=wire.u32(a+8),index=wire.u32(a+12);
            if(draw>=base.word(7)||draw<previous||slot>127||kind<1||kind>3)return false;
            if(index>=(kind==1?input.word(6):kind==2?input.word(8):word(6)))return false;
            if(kind==3&&wire.u32(target(index)+4)!=1)return false;
            if(draw!=previous){previous=draw;count=0;slots.clear();}
            if(++count>32||!slots.insert(slot).second)return false;
        }
        std::array<bool,32> produced{},available{};uint32_t pass=0;
        for(uint32_t oi=0;oi<word(7);++oi){const auto a=operation(oi);auto kind=wire.u32(a);
            if(!wire.zero(a+28,36))return false;
            if(kind==1){
                const auto color=wire.u32(a+8),depth=wire.u32(a+12),stores=wire.u32(a+24);
                if(wire.u32(a+4)!=pass||pass>=base.word(6)||color>=word(6)||depth>=word(6)||stores>3)return false;
                const auto p=base.pass(pass);const auto flags=base.u32(p+48);
                for(unsigned k=0;k<2;++k){auto index=k?depth:color,load=wire.u32(a+16+k*4);auto t=target(index);
                    if(wire.u32(t+4)!=k+1||wire.u32(t+16)!=base.word(3)||wire.u32(t+20)!=base.word(4)||produced[index]
                        ||bool(flags&(1u<<k))==bool(load)||load>word(6))return false;
                    if(load){auto source=target(load-1);
                        if(!available[load-1]||wire.u32(source+4)!=k+1||wire.u32(source+16)!=base.word(3)||wire.u32(source+20)!=base.word(4))return false;}
                }
                const auto first=base.u32(p+40),last=first+base.u32(p+44);
                for(uint32_t bi=0;bi<word(8);++bi){auto b=binding(bi);const auto di=wire.u32(b);
                    if(di>=first&&di<last&&wire.u32(b+8)==3&&!available[wire.u32(b+12)])return false;}
                produced[color]=produced[depth]=true;available[color]=(stores&1)!=0;available[depth]=(stores&2)!=0;++pass;
            }else if(kind==2){
                const auto source_kind=wire.u32(a+4),source=wire.u32(a+8),level=wire.u32(a+12),destination=wire.u32(a+16);
                if(destination>=word(6)||produced[destination]||(source_kind!=1&&source_kind!=2))return false;
                uint32_t source_format=1,w=0,h=0;
                if(source_kind==1){
                    if(source>=input.word(6))return false;auto t=input.texture(source);
                    if(level>=input.wire.u32(t+28))return false;
                    w=std::max(1u,input.wire.u32(t+16)>>level);h=std::max(1u,input.wire.u32(t+20)>>level);
                }else{
                    if(source>=word(6)||!available[source]||level)return false;auto t=target(source);
                    source_format=wire.u32(t+4);w=wire.u32(t+16);h=wire.u32(t+20);
                }
                const auto t=target(destination);const uint64_t x=wire.u32(a+20),y=wire.u32(a+24),dw=wire.u32(t+16),dh=wire.u32(t+20);
                if(source_format!=wire.u32(t+4)||x+dw>w||y+dh>h||(source_format==2&&(x||y||dw!=w||dh!=h)))return false;
                produced[destination]=available[destination]=true;
            }else return false;
        }
        if(pass!=base.word(6))return false;
        for(uint32_t i=0;i<word(6);++i)if(!produced[i])return false;
        return available[word(9)]&&wire.u32(target(word(9))+4)==1;
    }
};
}
