#pragma once
#include "TargetFramePacket.h"
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace ohtarget {
// Frame-local only: one ordered command buffer with tracked Metal resources.
// A slot remains strongly owned through command-buffer completion. This plan
// cannot authorize recycling a resource from another frame or submission.
struct Slot {
    uint32_t kind, width, height;
    bool operator==(const Slot &) const = default;
};
struct Allocation {
    std::vector<Slot> slots;
    std::vector<uint32_t> mapping;
    std::vector<std::array<bool,2>> load_elided;
    std::vector<bool> copy_elided;
    uint64_t dedicated_bytes=0;
    uint64_t allocated_bytes() const {
        uint64_t result=0;
        for(auto s:slots)result+=uint64_t(s.width)*s.height*4;
        return result;
    }
    static std::optional<Allocation> make(const Packet &p,bool reuse) {
        if(!p.validate())return std::nullopt;
        const auto base=p.geometry();const auto &w=p.wire;
        const auto n=p.word(6),operations=p.word(7);
        auto descriptor=[&](uint32_t i){auto t=p.target(i);return Slot{w.u32(t+4),w.u32(t+16),w.u32(t+20)};};
        auto sampled=[&](uint32_t pass,uint32_t image){
            auto a=base.pass(pass);const auto first=base.u32(a+40),last=first+base.u32(a+44);
            for(uint32_t i=0;i<p.word(8);++i){auto b=p.binding(i);
                if(w.u32(b)>=first&&w.u32(b)<last&&w.u32(b+8)==3&&w.u32(b+12)==image)return true;}
            return false;
        };
        std::array<uint32_t,32> last{};
        for(uint32_t oi=0;oi<operations;++oi){auto a=p.operation(oi);
            if(w.u32(a)==1){
                last[w.u32(a+8)]=last[w.u32(a+12)]=oi;
                for(unsigned k=0;k<2;++k)if(auto source=w.u32(a+16+k*4))last[source-1]=oi;
                auto pass=base.pass(w.u32(a+4));const auto first=base.u32(pass+40),end=first+base.u32(pass+44);
                for(uint32_t bi=0;bi<p.word(8);++bi){auto b=p.binding(bi);
                    if(w.u32(b)>=first&&w.u32(b)<end&&w.u32(b+8)==3)last[w.u32(b+12)]=oi;}
            }else{
                last[w.u32(a+16)]=oi;
                if(w.u32(a+4)==2)last[w.u32(a+8)]=oi;
            }
        }
        last[p.word(9)]=operations;
        Allocation result;
        result.mapping.assign(n,std::numeric_limits<uint32_t>::max());
        result.load_elided.resize(operations);result.copy_elided.resize(operations);
        for(uint32_t i=0;i<n;++i){auto s=descriptor(i);result.dedicated_bytes+=uint64_t(s.width)*s.height*4;}
        std::vector<uint32_t> owners;
        for(uint32_t oi=0;oi<operations;++oi){auto a=p.operation(oi);
            auto allocate=[&](uint32_t destination,std::optional<uint32_t> source,bool transfer_allowed){
                const auto desc=descriptor(destination);
                uint32_t slot=std::numeric_limits<uint32_t>::max();bool transferred=false;
                if(reuse&&source&&transfer_allowed&&last[*source]==oi){slot=result.mapping[*source];transferred=true;}
                if(slot==std::numeric_limits<uint32_t>::max()&&reuse){
                    for(uint32_t i=0;i<result.slots.size();++i)
                        if(result.slots[i]==desc&&last[owners[i]]<oi){slot=i;break;}
                }
                if(slot==std::numeric_limits<uint32_t>::max()){
                    slot=uint32_t(result.slots.size());result.slots.push_back(desc);owners.push_back(destination);
                }
                result.mapping[destination]=slot;owners[slot]=destination;
                return transferred;
            };
            if(w.u32(a)==1){
                for(unsigned k=0;k<2;++k){auto load=w.u32(a+16+k*4);
                    auto source=load?std::optional<uint32_t>(load-1):std::nullopt;
                    const bool can_transfer=k||!source||!sampled(w.u32(a+4),*source);
                    result.load_elided[oi][k]=allocate(w.u32(a+8+k*4),source,can_transfer);
                }
            }else{
                std::optional<uint32_t> source;
                if(w.u32(a+4)==2&&w.u32(a+12)==0&&w.u32(a+20)==0&&w.u32(a+24)==0
                    &&descriptor(w.u32(a+8))==descriptor(w.u32(a+16)))source=w.u32(a+8);
                result.copy_elided[oi]=allocate(w.u32(a+16),source,true);
            }
        }
        if(result.slots.size()>n||result.mapping.size()!=n||result.allocated_bytes()>result.dedicated_bytes)return std::nullopt;
        for(uint32_t i=0;i<n;++i)
            if(result.mapping[i]>=result.slots.size()||!(result.slots[result.mapping[i]]==descriptor(i)))return std::nullopt;
        return result;
    }
};
}
