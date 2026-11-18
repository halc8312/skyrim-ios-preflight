/* Authored driver/adapter only. dxvk_memory.{h,cpp}, Rc and math are real sources. */
#pragma once
#include <vulkan/vulkan.h>
#include <array>
#include <cassert>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>
#include "util/rc/util_rc.h"
#include "util/rc/util_rc_ptr.h"
#include "util/util_math.h"

struct AuthoredBlock { VkDeviceSize size; std::vector<unsigned char> mapped; };
inline std::map<VkDeviceMemory, AuthoredBlock*> live;
inline unsigned allocations, frees, map_failures, allocate_failures;
inline VkDeviceSize driver_max = UINT64_MAX;
inline VkDeviceSize driver_budget = UINT64_C(4)<<30, reported_usage;
inline uint64_t fixture_now=100000000000ull, headroom=UINT64_MAX;
inline bool headroom_known=false, dedicated_seen=false;

namespace dxvk {
using mutex = std::mutex;
namespace vk {
class DeviceFn : public RcObject {
public:
    VkDevice device() const { return VK_NULL_HANDLE; }
    VkResult vkAllocateMemory(VkDevice, const VkMemoryAllocateInfo *info,
        const VkAllocationCallbacks*, VkDeviceMemory *out) {
        ++allocations;
        if (allocate_failures) { --allocate_failures; return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
        if (info->allocationSize > driver_max) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        if (info->pNext) {
            auto base=static_cast<const VkBaseInStructure*>(info->pNext);
            if (base->sType==VK_STRUCTURE_TYPE_MEMORY_PRIORITY_ALLOCATE_INFO_EXT) base=base->pNext;
            dedicated_seen=base && base->sType==VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        } else dedicated_seen=false;
        auto *block=new AuthoredBlock{info->allocationSize,{}};
        *out=reinterpret_cast<VkDeviceMemory>(block); live[*out]=block;
        return VK_SUCCESS;
    }
    VkResult vkMapMemory(VkDevice, VkDeviceMemory memory, VkDeviceSize offset,
        VkDeviceSize size, VkMemoryMapFlags, void **out) {
        assert(live.count(memory) && !offset && size==VK_WHOLE_SIZE);
        if (map_failures) { --map_failures; return VK_ERROR_MEMORY_MAP_FAILED; }
        auto *block=live.at(memory);
        block->mapped.resize(block->size);
        *out=block->mapped.data(); return VK_SUCCESS;
    }
    void vkFreeMemory(VkDevice, VkDeviceMemory memory, const VkAllocationCallbacks*) {
        assert(live.count(memory)==1); ++frees;
        delete live.at(memory); live.erase(memory);
    }
};
}
enum class DxvkGpuVendor : uint16_t { Nvidia=0x10de };
inline void applyTristate(bool&, int) { }
namespace env { inline bool is32BitHostPlatform() { return false; } }
namespace str {
template<class... T> std::string format(const T&... args) { std::ostringstream out; (out<<...<<args); return out.str(); }
}
struct Logger { static void err(const std::string&) { } };
class DxvkError : public std::runtime_error { public: using std::runtime_error::runtime_error; };
struct DxvkAdapterMemoryInfo {
    struct Heap { VkDeviceSize memoryBudget=driver_budget, memoryAllocated=reported_usage; };
    std::array<Heap, VK_MAX_MEMORY_HEAPS> heaps;
};
class DxvkAdapter {
public:
    VkDeviceSize notified=0;
    VkPhysicalDeviceProperties deviceProperties() const {
        VkPhysicalDeviceProperties props{}; props.limits.bufferImageGranularity=256; return props;
    }
    VkPhysicalDeviceMemoryProperties memoryProperties() const {
        VkPhysicalDeviceMemoryProperties props{};
        props.memoryHeapCount=1; props.memoryTypeCount=1;
        props.memoryHeaps[0]={UINT64_C(4)<<30, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT};
        props.memoryTypes[0]={VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT|VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,0};
        return props;
    }
    bool matchesDriver(DxvkGpuVendor, VkDriverId, uint32_t, uint32_t) const { return false; }
    DxvkAdapterMemoryInfo getMemoryHeapInfo() const { return {}; }
    void notifyHeapMemoryAlloc(uint32_t heap, VkDeviceSize bytes) { assert(heap==0); notified+=bytes; }
    void notifyHeapMemoryFree(uint32_t heap, VkDeviceSize bytes) { assert(heap==0 && notified>=bytes); notified-=bytes; }
};
class DxvkDevice {
    Rc<vk::DeviceFn> functions=new vk::DeviceFn();
    mutable DxvkAdapter adapter_;
public:
    struct Extensions { bool extMemoryBudget=false; } ext;
    struct Features { struct { bool memoryPriority=true; } extMemoryPriority; };
    struct Config { int halveNvidiaHVVHeap=0; };
    struct Properties { struct { VkPhysicalDeviceProperties properties{}; } core; };
    const Rc<vk::DeviceFn>& vkd() const { return functions; }
    DxvkAdapter *adapter() const { return &adapter_; }
    bool isUnifiedMemoryArchitecture() const { return true; }
    Extensions extensions() const { return ext; }
    Features features() const { return {}; }
    Config config() const { return {}; }
    Properties properties() const { return {}; }
};
}
