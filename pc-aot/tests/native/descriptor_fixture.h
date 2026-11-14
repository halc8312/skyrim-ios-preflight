/* Authored Vulkan driver. Production pool, layout and rollover code stay intact. */
#pragma once
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <map>
#include <sstream>
#include <stdexcept>
#include <vector>
#include "util/rc/util_rc.h"
#include "util/rc/util_rc_ptr.h"
#include "util/util_flags.h"

struct FixtureLayout { uint32_t dynamic=0, images=0; };
struct FixtureSet { FixtureLayout shape; uint64_t payload=0; };
struct FixturePool {
    uint32_t maxSets=0, dynamicCapacity=0, imageCapacity=0;
    uint32_t dynamicUsed=0, imageUsed=0;
    std::vector<FixtureSet*> sets;
};
inline uint64_t poolCreates, poolDestroys, allocateCalls, resets, dynamicObjects, imageObjects;
inline unsigned allocateFailures, resetFailures;
inline std::map<VkDescriptorPool, FixturePool*> pools;

namespace dxvk {
using DxvkError=std::runtime_error;
namespace str {
template<class... T> std::string format(const T&... x) { std::ostringstream s; (s<<...<<x); return s.str(); }
}
namespace vk {
class DeviceFn : public RcObject {
public:
    VkDevice device() const { return VK_NULL_HANDLE; }
    VkResult vkCreateDescriptorPool(VkDevice, const VkDescriptorPoolCreateInfo *i,
            const VkAllocationCallbacks*, VkDescriptorPool *out) {
        auto *p=new FixturePool; p->maxSets=i->maxSets;
        for(uint32_t n=0;n<i->poolSizeCount;++n) {
            const auto& s=i->pPoolSizes[n];
            if(s.type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) p->dynamicCapacity=s.descriptorCount;
            if(s.type==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) p->imageCapacity=s.descriptorCount;
        }
        *out=reinterpret_cast<VkDescriptorPool>(p); pools[*out]=p; ++poolCreates;
        return VK_SUCCESS;
    }
    VkResult vkAllocateDescriptorSets(VkDevice, const VkDescriptorSetAllocateInfo *i, VkDescriptorSet *out) {
        ++allocateCalls; assert(i->descriptorSetCount==1 && pools.count(i->descriptorPool));
        if(allocateFailures) { --allocateFailures; return VK_ERROR_OUT_OF_POOL_MEMORY; }
        auto *p=pools.at(i->descriptorPool);
        if(p->sets.size()==p->maxSets) return VK_ERROR_OUT_OF_POOL_MEMORY;
        auto shape=*reinterpret_cast<FixtureLayout*>(i->pSetLayouts[0]);
        // MoltenVK permits per-type overflow: extra descriptors are heap objects,
        // but the allocation succeeds. Do not invent an OUT_OF_POOL_MEMORY here.
        const auto dynamicFree=p->dynamicCapacity-std::min(p->dynamicCapacity,p->dynamicUsed);
        const auto imageFree=p->imageCapacity-std::min(p->imageCapacity,p->imageUsed);
        dynamicObjects+=shape.dynamic>dynamicFree ? shape.dynamic-dynamicFree : 0;
        imageObjects+=shape.images>imageFree ? shape.images-imageFree : 0;
        p->dynamicUsed+=shape.dynamic; p->imageUsed+=shape.images;
        auto *s=new FixtureSet{shape,0}; p->sets.push_back(s);
        *out=reinterpret_cast<VkDescriptorSet>(s); return VK_SUCCESS;
    }
    VkResult vkResetDescriptorPool(VkDevice, VkDescriptorPool handle, VkDescriptorPoolResetFlags) {
        if(resetFailures) { --resetFailures; return VK_ERROR_DEVICE_LOST; }
        auto *p=pools.at(handle); for(auto *s:p->sets) delete s;
        p->sets.clear(); p->dynamicUsed=p->imageUsed=0; ++resets; return VK_SUCCESS;
    }
    void vkDestroyDescriptorPool(VkDevice, VkDescriptorPool handle, const VkAllocationCallbacks*) {
        auto *p=pools.at(handle); for(auto *s:p->sets) delete s;
        delete p; pools.erase(handle); ++poolDestroys;
    }
    VkResult vkCreateDescriptorSetLayout(VkDevice, const VkDescriptorSetLayoutCreateInfo *i,
            const VkAllocationCallbacks*, VkDescriptorSetLayout *out) {
        auto *p=new FixtureLayout;
        for(uint32_t n=0;n<i->bindingCount;++n) {
            if(i->pBindings[n].descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
                p->dynamic+=i->pBindings[n].descriptorCount;
            if(i->pBindings[n].descriptorType==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
                p->images+=i->pBindings[n].descriptorCount;
        }
        *out=reinterpret_cast<VkDescriptorSetLayout>(p); return VK_SUCCESS;
    }
    void vkDestroyDescriptorSetLayout(VkDevice, VkDescriptorSetLayout p, const VkAllocationCallbacks*) {
        delete reinterpret_cast<FixtureLayout*>(p);
    }
    VkResult vkCreatePipelineLayout(VkDevice, const VkPipelineLayoutCreateInfo*,
            const VkAllocationCallbacks*, VkPipelineLayout *out) {
        *out=reinterpret_cast<VkPipelineLayout>(new int(1)); return VK_SUCCESS;
    }
    void vkDestroyPipelineLayout(VkDevice, VkPipelineLayout p, const VkAllocationCallbacks*) {
        delete reinterpret_cast<int*>(p);
    }
    VkResult vkCreateDescriptorUpdateTemplate(VkDevice, const VkDescriptorUpdateTemplateCreateInfo*,
            const VkAllocationCallbacks*, VkDescriptorUpdateTemplate *out) {
        *out=reinterpret_cast<VkDescriptorUpdateTemplate>(new int(2)); return VK_SUCCESS;
    }
    void vkDestroyDescriptorUpdateTemplate(VkDevice, VkDescriptorUpdateTemplate p, const VkAllocationCallbacks*) {
        delete reinterpret_cast<int*>(p);
    }
};
}
}
