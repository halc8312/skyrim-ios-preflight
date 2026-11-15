/* Callback extracted from the actual prepared DXVK source. Driver behavior is
 * authored and deliberately requires a buffered producer before GPU wait. */
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <iostream>
using VkResult = int32_t;
constexpr VkResult VK_SUCCESS = 0, VK_ERROR_UNKNOWN = -13;
struct DxvkError { std::string message() const { return "authored flush failure"; } };
struct Logger { static void err(const std::string&) {} };
static std::vector<unsigned> sequence;
struct Driver {
    bool produced = false, consumer = false;
    bool throws = false;
    int32_t result = 0;
    void* device() { return this; }
    int32_t vkDeviceWaitIdle(void* p) {
        assert(p == this && produced && consumer);
        if (throws) throw DxvkError();
        sequence.push_back(4); return result;
    }
};
struct Dxvk {
    std::shared_ptr<Driver> driver = std::make_shared<Driver>();
    bool locked = false;
    void lockSubmission() { assert(!locked); locked = true; sequence.push_back(3); }
    void unlockSubmission() { assert(locked); locked = false; sequence.push_back(5); }
    std::shared_ptr<Driver> vkd() { return driver; }
};
struct D3D9DeviceEx {
    std::shared_ptr<Dxvk> dxvk = std::make_shared<Dxvk>();
    bool fail = false;
    void Flush() { if (fail) throw DxvkError(); sequence.push_back(1); dxvk->driver->produced = true; }
    void SynchronizeCsThread() { sequence.push_back(2); dxvk->driver->consumer = true; }
    std::shared_ptr<Dxvk> GetDXVKDevice() { return dxvk; }
};
/* ACTUAL_CALLBACK */
int main() {
    D3D9DeviceEx device;
    for (unsigned phase = 0; phase < 3; ++phase) assert(iosLifecycleDrain(&device, phase) == VK_SUCCESS);
    assert((sequence == std::vector<unsigned>{1, 2, 3, 4, 5}));
    device.dxvk->driver->result = -4;
    assert(iosLifecycleDrain(&device, 2) == -4 && !device.dxvk->locked);
    device.dxvk->driver->throws = true;
    assert(iosLifecycleDrain(&device, 2) == VK_ERROR_UNKNOWN && !device.dxvk->locked);
    device.fail = true;
    assert(iosLifecycleDrain(&device, 0) == VK_ERROR_UNKNOWN);
    assert(iosLifecycleDrain(&device, 99) == VK_ERROR_UNKNOWN);
    std::cout << "lifecycle producer_flush_before_wait=PASS driver_error_preserved=PASS real_metal=0\n";
}
