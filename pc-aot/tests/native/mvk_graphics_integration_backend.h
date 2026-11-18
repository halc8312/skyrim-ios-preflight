// Authored Metal boundary for production DXVK + MoltenVK coordination tests.
// No Vulkan device, shader translation or GPU performance is simulated.
#pragma once
#include <cstdint>
#include <memory>
#include <initializer_list>
#include "performance_metrics.h"

namespace MvkIntegration {
struct Library {
    uintptr_t identity;
    bool valid;
    SkyrimPerfShaderOrigin origin;
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
    void publishUnrelated(unsigned module, unsigned config);
    void clockScript(unsigned request, std::initializer_list<uint64_t> values);
    bool waitForClock(unsigned request, unsigned count);
    unsigned clockReads(unsigned request);
};
// Observer-only hooks inserted at authored compiler entry and after a failed
// actual cache try_lock. They never substitute a lock decision or result.
void boundary(int key, bool library);
void coldWait();
uint64_t clockNow();
void failLibraries(bool fail);
bool allLibrariesDestroyed();
}
