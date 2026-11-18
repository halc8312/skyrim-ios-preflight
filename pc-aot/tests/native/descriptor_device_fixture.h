#pragma once
#include "descriptor_fixture.h"
#include "dxvk_descriptor.h"
namespace dxvk {
class DxvkDevice {
public:
    Rc<vk::DeviceFn> functions=new vk::DeviceFn();
    std::vector<Rc<DxvkDescriptorPool>> recycled;
    Rc<DxvkDescriptorPool> createDescriptorPool() {
        if(recycled.empty()) return new DxvkDescriptorPool(functions);
        auto p=recycled.back(); recycled.pop_back(); return p;
    }
    void recycleDescriptorPool(Rc<DxvkDescriptorPool> p) { recycled.push_back(std::move(p)); }
};
class DxvkContext {
public:
    DxvkDevice *m_device;
    DxvkDescriptorPoolTracker tracker;
    DxvkDescriptorPoolTracker *m_cmd=&tracker;
    Rc<DxvkDescriptorPool> m_descPool;
    explicit DxvkContext(DxvkDevice *d):m_device(d),tracker(d) { }
    VkDescriptorSet allocateDescriptorSet(VkDescriptorSetLayout layout
#if FIXTURE_NEW
        ,uint32_t uniformDynamic=0,uint32_t combinedImages=2
#endif
    );
    void retire() {
        if(m_descPool!=nullptr) tracker.trackDescriptorPool(std::move(m_descPool));
        tracker.reset();
    }
};
}
