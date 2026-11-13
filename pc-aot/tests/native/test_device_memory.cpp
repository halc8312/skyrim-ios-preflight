/* Actual patched allocator with authored Vulkan memory driver. */
#include "dxvk_memory.h"
#include "device_policy.h"
#include <iostream>
#include <thread>

extern "C" uint64_t skyrim_perf_now_ns(void) { return fixture_now; }
extern "C" int skyrim_device_platform_headroom(uint64_t *bytes) { *bytes=headroom; return headroom_known; }
static constexpr uint64_t MIB=1048576;
static dxvk::DxvkMemory allocate(dxvk::DxvkMemoryAllocator& a, VkDeviceSize size,
    VkDeviceSize alignment=256, float priority=0.5f, bool mapped=false, bool dedicated=false) {
    VkMemoryRequirements req{size,alignment,1};
    VkMemoryDedicatedRequirements need{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,nullptr,dedicated,dedicated};
    VkMemoryDedicatedAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,nullptr,VK_NULL_HANDLE,VK_NULL_HANDLE};
    return a.alloc(&req,need,info,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT|
        (mapped ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT : 0),priority);
}
static void reset() {
    assert(live.empty()); allocations=frees=0; allocate_failures=map_failures=0;
    driver_max=headroom=UINT64_MAX; headroom_known=false; reported_usage=0;
    driver_budget=4*1024*MIB; fixture_now+=20000000000ull;
}
int main() {
    reset();
    {
        dxvk::DxvkDevice device;
        dxvk::DxvkMemoryAllocator a(&device);
        auto first=allocate(a,80*MIB); auto pending=allocate(a,80*MIB); auto third=allocate(a,80*MIB);
        auto held=pending.memory();
        assert(live.size()==3 && a.getMemoryStats(0).memoryUsed==240*MIB);
        first={}; third={};
        assert(live.size()==2 && live.count(held)); /* one warm + live pending slice */
        unsigned before=allocations;
        auto warm=allocate(a,80*MIB); assert(allocations==before);
        auto moved=std::move(pending); assert(!pending && moved.memory()==held);
        skyrim_device_memory_warning(fixture_now);
        warm={}; assert(live.size()==1 && live.count(held));
        auto tail=allocate(a,4096); assert(live.count(held));
        moved={}; assert(live.count(held)); /* the tail still owns bytes in that pool */
        tail={}; assert(live.empty() && device.adapter()->notified==0);
    }
    reset();
    {
        dxvk::DxvkDevice device; dxvk::DxvkMemoryAllocator a(&device);
        headroom_known=true; headroom=20*MIB;
        auto small=allocate(a,4096); assert(live.at(small.memory())->size==16*MIB);
        fixture_now+=3000000000ull; small={}; /* expired pressure grace keeps one warm */
        headroom_known=false;
        auto big=allocate(a,80*MIB); assert(live.size()==2);
        big={}; assert(live.size()==1 && live.begin()->second->size==128*MIB);
        unsigned before=allocations; auto reuse=allocate(a,80*MIB); assert(allocations==before);
        auto priority=allocate(a,4096,256,0.9f); reuse={}; priority={};
        assert(live.size()==2); /* distinct immutable priority buckets */
        skyrim_device_memory_warning(fixture_now);
        auto required=allocate(a,4096);
        assert(live.size()==1 && live.at(required.memory())->size==4*MIB);
        required={}; assert(live.empty());
    }
    reset();
    {
        dxvk::DxvkDevice device; device.ext.extMemoryBudget=true; dxvk::DxvkMemoryAllocator a(&device);
        reported_usage=driver_budget-10*MIB; /* advisory pressure, not allocation denial */
        headroom_known=true; headroom=0;
        auto required=allocate(a,6*MIB); assert(live.at(required.memory())->size==6*MIB);
        assert(a.getMemoryStats(0).memoryUsed==6*MIB); required={}; assert(live.empty());
        auto dedicated=allocate(a,7000,256,0.8f,false,true);
        assert(dedicated_seen && live.at(dedicated.memory())->size==7000);
        dedicated={}; assert(live.empty());
        auto aligned=allocate(a,4096,256*MIB); /* offset zero satisfies larger-than-pool alignment */
        assert(aligned.offset()==0 && live.at(aligned.memory())->size==4096);
    }
    reset();
    {
        dxvk::DxvkDevice device; dxvk::DxvkMemoryAllocator a(&device);
        headroom_known=true; headroom=8*MIB;
        map_failures=1; /* failed map must free its exact allocation before retry */
        auto one=allocate(a,3000,4096,0.5f,true);
        assert(frees==1 && one.offset()%4096==0 && one.length()==4096);
        auto two=allocate(a,5000,8192,0.5f,true);
        assert(two.offset()%8192==0 && two.length()==8192);
        std::memset(one.mapPtr(0),0x35,3000); std::memset(two.mapPtr(0),0xa7,5000);
        for (unsigned i=0;i<3000;i++) assert(static_cast<unsigned char*>(one.mapPtr(0))[i]==0x35);
        one={}; assert(live.count(two.memory()));
        for (unsigned i=0;i<5000;i++) assert(static_cast<unsigned char*>(two.mapPtr(0))[i]==0xa7);
        two={}; assert(live.empty() && a.getMemoryStats(0).memoryAllocated==0);
        driver_max=0;
        try { auto refused=allocate(a,4096); (void)refused; assert(false); }
        catch (const dxvk::DxvkError&) { }
        assert(live.empty() && device.adapter()->notified==0);
    }
    reset();
    {
        dxvk::DxvkDevice device; dxvk::DxvkMemoryAllocator a(&device);
        headroom_known=true; headroom=8*MIB;
        auto private_pool=allocate(a,4096);
        auto mapped_pool=allocate(a,4096,256,0.5f,true);
        assert(private_pool.memory()!=mapped_pool.memory());
        fixture_now+=3000000000ull;
        private_pool={}; mapped_pool={};
        assert(live.size()==2); /* memory flags cannot merge otherwise equal buckets */
        unsigned before=allocations;
        auto again=allocate(a,4096,256,0.5f,true); assert(allocations==before);
    }
    reset();
    {
        dxvk::DxvkDevice device; dxvk::DxvkMemoryAllocator a(&device);
        /* Constant authored provider state; production mutex protects driver calls. */
        std::thread workers[4];
        for (auto& worker:workers) worker=std::thread([&] {
            for (unsigned i=0;i<4000;++i) {
                auto x=allocate(a,1024+(i%32)*256); auto y=allocate(a,3333,4096);
                assert(x.offset()%256==0 && y.offset()%4096==0);
            }
        });
        for (auto& worker:workers) worker.join();
        assert(a.getMemoryStats(0).memoryUsed==0 && live.size()==1);
    }
    assert(live.empty());
    uint64_t reclaimed,reduced; skyrim_device_take_counters(&reclaimed,&reduced);
    assert(reclaimed>0 && reduced>0);
    std::cout << "actual_allocator=PASS live_retention=PASS mapping_alignment=PASS pressure=PASS concurrent=PASS\n";
}
