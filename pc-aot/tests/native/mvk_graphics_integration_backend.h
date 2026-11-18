// Authored Metal boundary for production DXVK + MoltenVK coordination tests.
// No Vulkan device, shader translation or GPU performance is simulated.
#pragma once
#include <cstdint>
#include <memory>

namespace MvkIntegration {
struct Library {
    uintptr_t identity;
    bool valid;
};
struct CacheState;
class Cache {
    std::unique_ptr<CacheState> state;
public:
    Cache();
    ~Cache();
    Library request(unsigned request, unsigned module, unsigned config);
    void block(unsigned module, unsigned config);
    void releaseAll();
    bool waitForMetal(unsigned module, unsigned config);
    bool waitForCold(unsigned request);
    unsigned conversions(unsigned module, unsigned config);
    unsigned libraries(unsigned module, unsigned config);
    unsigned coldWaits(unsigned request);
    unsigned published();
};
// Observer-only hooks inserted at authored compiler entry and after a failed
// actual cache try_lock. They never substitute a lock decision or result.
void boundary(int key, bool library);
void coldWait();
bool allLibrariesDestroyed();
}
