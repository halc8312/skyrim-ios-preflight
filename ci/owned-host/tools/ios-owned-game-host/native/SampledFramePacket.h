#pragma once
#include "FramePlanPacket.h"
#include <algorithm>

namespace ohsample {
// OHST0001 contains an unchanged OHFP0001 plus immutable sampled resources.
// All accessors require successful validation and caller-owned immutable bytes.
struct Packet {
    ohframe::Packet wire;
    uint32_t word(size_t i) const {return wire.word(i);}
    size_t texture(size_t i) const {return word(10)+i*40;}
    size_t mip(size_t i) const {return word(11)+i*24;}
    size_t sampler(size_t i) const {return word(12)+i*64;}
    size_t binding(size_t i) const {return word(13)+i*16;}
    ohframe::Packet geometry() const {return {wire.bytes.subspan(word(4),word(5))};}
    bool validate() const {
        const uint64_t bytes=wire.bytes.size();
        if(bytes<80||bytes>(4u<<20)||word(0)!=0x5453484f||word(1)!=0x31303030||word(2)!=1
            ||word(3)!=bytes||word(4)!=80||word(5)<64||word(5)%8||uint64_t(word(5))+80>bytes
            ||word(6)<1||word(6)>16||word(7)<word(6)||word(7)>176||word(8)<1||word(8)>16
            ||word(9)>4096||!wire.zero(60,20))return false;
        const uint64_t to=80+uint64_t(word(5)),mo=to+uint64_t(word(6))*40,
            so=mo+uint64_t(word(7))*24,bo=so+uint64_t(word(8))*64,data=bo+uint64_t(word(9))*16;
        if(word(10)!=to||word(11)!=mo||word(12)!=so||word(13)!=bo||word(14)!=data||data>bytes)return false;
        const auto base=geometry();if(!base.validate())return false;
        std::set<std::pair<uint32_t,uint64_t>> textures,samplers;
        uint64_t cursor=data;uint32_t mi=0;
        for(uint32_t i=0;i<word(6);++i){
            const auto t=texture(i);const auto w=wire.u32(t+16),h=wire.u32(t+20),count=wire.u32(t+28);
            if(wire.u32(t+4)!=1||!wire.u64(t+8)||!textures.insert({wire.u32(t),wire.u64(t+8)}).second
                ||!w||w>1024||!h||h>1024||!count||count>11||wire.u32(t+24)!=mi
                ||count>word(7)-mi||!wire.zero(t+32,8))return false;
            uint32_t levels=0;for(auto side=std::max(w,h);side;side>>=1)++levels;
            if(count>levels)return false;
            for(uint32_t level=0;level<count;++level,++mi){
                const auto m=mip(mi);const uint64_t pitch=wire.u32(m+8),start=wire.u32(m+12),n=wire.u32(m+16);
                if(wire.u32(m)!=i||wire.u32(m+4)!=level||wire.u32(m+20)||pitch%4
                    ||pitch<uint64_t(std::max(1u,w>>level))*4||n!=pitch*std::max(1u,h>>level)
                    ||start!=cursor||start+n>bytes)return false;
                cursor=(start+n+7)&~uint64_t(7);
                if(cursor>bytes||!wire.zero(size_t(start+n),size_t(cursor-start-n)))return false;
            }
        }
        if(mi!=word(7)||cursor!=bytes)return false;
        for(uint32_t i=0;i<word(8);++i){
            const auto s=sampler(i);const auto aniso=wire.u32(s+48);const float lo=wire.f32(s+40),hi=wire.f32(s+44);
            if(wire.u32(s+4)||!wire.u64(s+8)||!samplers.insert({wire.u32(s),wire.u64(s+8)}).second
                ||!wire.zero(s+52,12)||!std::isfinite(lo)||!std::isfinite(hi)||lo<0||lo>hi||hi>32
                ||(aniso!=1&&aniso!=2&&aniso!=4&&aniso!=8&&aniso!=16))return false;
            for(size_t j=0;j<3;++j)if(wire.u32(s+16+j*4)>2||wire.u32(s+28+j*4)>1
                ||(aniso>1&&wire.u32(s+28+j*4)!=1))return false;
        }
        uint32_t previous=0,count=0;std::set<uint32_t> slots;
        for(uint32_t i=0;i<word(9);++i){
            const auto a=binding(i);const auto draw=wire.u32(a),slot=wire.u32(a+4),kind=wire.u32(a+8),index=wire.u32(a+12);
            if(draw>=base.word(7)||draw<previous||slot>127||(kind!=1&&kind!=2)
                ||index>=(kind==1?word(6):word(8)))return false;
            if(draw!=previous){previous=draw;count=0;slots.clear();}
            if(++count>32||!slots.insert(slot).second)return false;
        }
        return true;
    }
    size_t find_binding(uint32_t draw,uint32_t slot) const {
        for(uint32_t i=0;i<word(9);++i){auto a=binding(i);if(wire.u32(a)==draw&&wire.u32(a+4)==slot)return a;}
        return SIZE_MAX;
    }
};
}
