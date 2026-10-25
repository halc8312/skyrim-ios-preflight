#pragma once
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <set>
#include <span>
#include <utility>

namespace ohframe {
// Canonical OHFP0001 packet, little endian. No caller-native structs or pointers.
// Accessors are used only after validate() succeeds and while bytes stay immutable.
struct Packet {
    std::span<const uint8_t> bytes;
    uint32_t u32(size_t at) const {
        return uint32_t(bytes[at]) | uint32_t(bytes[at+1])<<8 | uint32_t(bytes[at+2])<<16 | uint32_t(bytes[at+3])<<24;
    }
    uint64_t u64(size_t at) const { return u32(at) | uint64_t(u32(at+4))<<32; }
    float f32(size_t at) const { return std::bit_cast<float>(u32(at)); }
    double f64(size_t at) const { return std::bit_cast<double>(u64(at)); }
    uint32_t word(size_t i) const { return u32(i*4); }
    size_t resource(size_t i) const { return word(9)+i*24; }
    size_t pass(size_t i) const { return word(10)+i*64; }
    size_t draw(size_t i) const { return word(11)+i*128; }
    size_t binding(size_t i) const { return word(12)+i*32; }
    bool zero(size_t at,size_t count) const { for(size_t i=0;i<count;++i)if(bytes[at+i])return false;return true; }
    bool unit(double x) const { return std::isfinite(x)&&x>=0&&x<=1; }

    bool validate() const {
        if(bytes.size()<64||bytes.size()>(4u<<20)||word(0)!=0x5046484f||word(1)!=0x31303030||word(2)!=1
            ||word(3)<1||word(3)>1024||word(4)<1||word(4)>1024||word(5)>64||word(6)<1||word(6)>16
            ||word(7)>128||word(8)>1280||word(9)!=64||word(14)!=bytes.size()||word(15)!=0)return false;
        const uint64_t po=64+uint64_t(word(5))*24, d=po+uint64_t(word(6))*64,
            b=d+uint64_t(word(7))*128,data=b+uint64_t(word(8))*32;
        if(word(10)!=po||word(11)!=d||word(12)!=b||word(13)!=data||data>bytes.size())return false;
        std::set<std::pair<uint32_t,uint64_t>> versions;
        uint64_t cursor=data;
        for(uint32_t i=0;i<word(5);++i){
            auto r=resource(i);const uint64_t start=u32(r+16),length=u32(r+20);
            if(u32(r+4)||!u64(r+8)||!versions.insert({u32(r),u64(r+8)}).second||!length||length%4
                ||start!=cursor||start+length>bytes.size())return false;
            cursor=(start+length+7)&~uint64_t(7);
            if(cursor>bytes.size()||!zero(size_t(start+length),size_t(cursor-start-length)))return false;
        }
        if(cursor!=bytes.size())return false;
        uint32_t draw_cursor=0,binding_cursor=0;
        for(uint32_t i=0;i<word(6);++i){
            auto p=pass(i);auto count=u32(p+44),flags=u32(p+48);
            if((flags&~3u)||(!i&&flags!=3)||u32(p+40)!=draw_cursor||count>word(7)-draw_cursor||!zero(p+52,12))return false;
            for(size_t j=0;j<4;++j)if(!unit(f64(p+j*8)))return false;
            if(!unit(f64(p+32)))return false;
            if(!(flags&1)&&!zero(p,32))return false;
            if(!(flags&2)&&!zero(p+32,8))return false;
            draw_cursor+=count;
        }
        if(draw_cursor!=word(7))return false;
        for(uint32_t i=0;i<word(7);++i){
            auto q=draw(i);double x=f32(q),y=f32(q+4),w=f32(q+8),h=f32(q+12),z0=f32(q+16),z1=f32(q+20);
            if(!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(w)||!std::isfinite(h)||x<0||y<0||w<=0||h<=0
                ||x+w>word(3)||y+h>word(4)||!unit(z0)||!unit(z1)||z0>z1)return false;
            if(!u32(q+32)||!u32(q+36)||uint64_t(u32(q+24))+u32(q+32)>word(3)
                ||uint64_t(u32(q+28))+u32(q+36)>word(4)||u32(q+76)||!zero(q+116,12))return false;
            for(size_t j=0;j<4;++j)if(!unit(f64(q+40+j*8)))return false;
            auto topology=u32(q+80),count=u32(q+84),first=u32(q+88),bindings=u32(q+92),flags=u32(q+96),index=u32(q+100),stride=u32(q+104),off=u32(q+108);
            if((topology!=1&&topology!=2)||!count||count>65536||(flags&~3u)||first!=binding_cursor||bindings>word(8)-binding_cursor)return false;
            const uint64_t index_bytes=u32(q+112);
            if(!index){if(stride||off||index_bytes)return false;}
            else if(index>word(5)||(stride!=2&&stride!=4)||off%4||uint64_t(count)*stride>index_bytes
                ||uint64_t(off)+index_bytes>u32(resource(index-1)+20))return false;
            std::set<std::pair<uint32_t,uint32_t>> slots;
            for(uint32_t j=0;j<bindings;++j){
                auto a=binding(first+j);auto group=u32(a),slot=u32(a+4),ri=u32(a+8);
                auto offset=u64(a+16),length=u64(a+24);
                if(group>1||slot>4||ri>=word(5)||u32(a+12)||!slots.insert({group,slot}).second||!length
                    ||offset%256||length%4||offset>u32(resource(ri)+20)||length>u32(resource(ri)+20)-offset)return false;
            }
            binding_cursor+=bindings;
        }
        return binding_cursor==word(8);
    }
};
}
