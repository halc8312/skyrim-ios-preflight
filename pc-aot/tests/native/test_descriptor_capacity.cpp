#include "descriptor_device_fixture.h"
#include "dxvk_pipelayout.h"
#include <cstdio>
using namespace dxvk;

static Rc<DxvkPipelineLayout> layout(DxvkDevice& d,unsigned dynamic,unsigned images,bool convert=true) {
    DxvkDescriptorSlotMapping mapping;
    for(unsigned n=0;n<dynamic+images;++n) {
        DxvkResourceSlot slot{n,n<dynamic ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_IMAGE_VIEW_TYPE_2D,VK_ACCESS_SHADER_READ_BIT};
        mapping.defineSlot(VK_SHADER_STAGE_VERTEX_BIT,slot);
        mapping.defineSlot(VK_SHADER_STAGE_FRAGMENT_BIT,slot); // duplicate stage is not another descriptor
    }
    mapping.makeDescriptorsDynamic(convert ? dynamic : 0,0);
    auto p=Rc<DxvkPipelineLayout>(new DxvkPipelineLayout(d.functions,mapping,VK_PIPELINE_BIND_POINT_GRAPHICS));
#if FIXTURE_NEW
    assert(p->iosDynamicDescriptors()==(convert ? dynamic : 0));
    assert(p->iosCombinedImages()==images);
#endif
    if(dynamic+images) {
        auto actual=reinterpret_cast<FixtureLayout*>(p->descriptorSetLayout());
        assert(actual->dynamic==(convert ? dynamic : 0) && actual->images==images);
    }
    return p;
}
static VkDescriptorSet allocate(DxvkContext& c,const Rc<DxvkPipelineLayout>& l) {
    return c.allocateDescriptorSet(l->descriptorSetLayout()
#if FIXTURE_NEW
        ,l->iosDynamicDescriptors(),l->iosCombinedImages()
#endif
    );
}
int main() {
    uint64_t digest=0;
    {
        DxvkDevice d;
        auto empty=layout(d,0,0); auto statics=layout(d,8,4,false);
        assert(statics->hasStaticBufferBindings() && !statics->dynamicBindingCount());
        auto a=layout(d,8,4),b=layout(d,4,8),meta=layout(d,0,1),exact=layout(d,128,0);
        DxvkContext c(&d), independent(&d);
        uint64_t firstWarm=0;
        for(unsigned frame=0;frame<5;++frame) {
            std::vector<VkDescriptorSet> captured;
            for(unsigned n=0;n<8192;++n) {
                VkDescriptorSet s;
                if(n%5==0) s=c.allocateDescriptorSet(meta->descriptorSetLayout());
                else s=allocate(c,n%2 ? a:b);
                assert(s); auto *data=reinterpret_cast<FixtureSet*>(s); data->payload=n+1;
                captured.push_back(s);
            }
            // No descriptor is reset/recycled before command completion. Another
            // context may allocate/retire while these handles remain in flight.
            for(unsigned n=0;n<80;++n) assert(allocate(independent,b));
            independent.retire();
            for(unsigned n=0;n<captured.size();++n) {
                auto *s=reinterpret_cast<FixtureSet*>(captured[n]);
                assert(s->payload==n+1);
                digest=digest*131+s->payload+s->shape.dynamic*17+s->shape.images*31;
            }
            c.retire();
            if(!frame) firstWarm=poolCreates;
            else assert(poolCreates==firstWarm);
        }
        auto p=d.createDescriptorPool();
        // Exact fit uses all 6144 dynamic slots; allocation failure does not
        // consume accounting. Check actual production pool with driver failures.
        allocateFailures=1;
#if FIXTURE_NEW
        assert(!p->alloc(exact->descriptorSetLayout(),128,0));
        for(unsigned n=0;n<48;++n) assert(p->alloc(exact->descriptorSetLayout(),128,0));
        const auto before=allocateCalls;
        assert(!p->alloc(exact->descriptorSetLayout(),128,0));
        assert(!p->alloc(exact->descriptorSetLayout(),UINT32_MAX,0));
        assert(allocateCalls==before);
        resetFailures=1; bool refused=false;
        try {p->reset();} catch(const DxvkError&) {refused=true;} assert(refused);
        assert(!p->alloc(exact->descriptorSetLayout(),128,0) && allocateCalls==before);
        p->reset(); assert(p->alloc(exact->descriptorSetLayout(),128,0));
#else
        assert(!p->alloc(exact->descriptorSetLayout()));
        for(unsigned n=0;n<48;++n) assert(p->alloc(exact->descriptorSetLayout()));
        assert(p->alloc(exact->descriptorSetLayout()));
        p->reset(); assert(p->alloc(exact->descriptorSetLayout()));
#endif
        p->reset(); d.recycleDescriptorPool(p); p=nullptr;
        // Rollover preserves the original single retry and its failure result.
        allocateFailures=2; assert(!allocate(c,a)); assert(allocateFailures==0);
        assert(allocate(c,a)); c.retire();
        assert(resets>0);
    }
    assert(pools.empty() && poolCreates==poolDestroys);
#if FIXTURE_NEW
    assert(dynamicObjects==0 && imageObjects==0);
#else
    assert(dynamicObjects>0 && imageObjects>0);
#endif
    printf("DESCRIPTOR_CAPACITY actual_pool_layout_rollover=PASS digest=%llu pools=%llu dynamic_objects=%llu image_objects=%llu\n",
        (unsigned long long)digest,(unsigned long long)poolCreates,
        (unsigned long long)dynamicObjects,(unsigned long long)imageObjects);
}
