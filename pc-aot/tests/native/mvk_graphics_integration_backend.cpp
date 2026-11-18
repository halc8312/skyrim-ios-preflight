// CPU bridge to the exact patched MoltenVK cache/converter entry points.
// Standard/vendor headers are included outside the fixture namespace so its
// authored Vulkan interfaces cannot collide with the real DXVK Vulkan types.
#include <atomic>
#include <cassert>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>
#include "MVKSmallVector.h"
#include "performance_metrics.h"
#include "mvk_graphics_integration_backend.h"

namespace FixtureMvk {
#include "mvk_integration_fixture.h"
#include "mvk_module_cold_support.h"
// ACTUAL_MVK_METHODS
}

namespace MvkIntegration {
using Key = std::pair<unsigned, unsigned>;
struct CacheState {
    FixtureMvk::MVKPipelineCache cache;
    std::mutex mutex;
    std::condition_variable changed;
    std::map<Key, unsigned> conversionCounts, libraryCounts;
    std::map<unsigned, unsigned> waitCounts;
    std::set<Key> blocked;
    bool released = false;
};
struct Context {
    CacheState* state;
    unsigned request, module, config;
};
static thread_local Context* current = nullptr;
struct ContextScope {
    Context* previous;
    explicit ContextScope(Context& context) : previous(current) { current = &context; }
    ~ContextScope() { current = previous; }
};

void boundary(int key, bool library) {
    assert(current && key > 0 && unsigned(key) == current->config);
    auto& s = *current->state;
    const Key identity{current->module, current->config};
    std::unique_lock<std::mutex> lock(s.mutex);
    ++(library ? s.libraryCounts : s.conversionCounts)[identity];
    s.changed.notify_all();
    if (library && s.blocked.count(identity)) {
        // This deadline detects a broken test/deadlock; elapsed time is never
        // interpreted as a compiler speed or as proof that work cannot proceed.
        assert(s.changed.wait_for(lock, std::chrono::seconds(20), [&] { return s.released; }));
    }
}

void coldWait() {
    assert(current);
    auto& s = *current->state;
    std::lock_guard<std::mutex> lock(s.mutex);
    ++s.waitCounts[current->request];
    s.changed.notify_all();
}

Cache::Cache() : state(std::make_unique<CacheState>()) {}
Cache::~Cache() = default;

Library Cache::request(unsigned request, unsigned module, unsigned config) {
    assert(module > 0 && config > 0);
    Context context{state.get(), request, module, config};
    ContextScope scope(context);
    // Match DXVK's per-pipeline VkShaderModule lifetime. Equal contents use the
    // same value key even though the converter object is newly constructed.
    FixtureMvk::MVKShaderModule shader{int(module)};
    FixtureMvk::SPIRVToMSLConversionConfiguration conversion;
    conversion.key = int(config);
    FixtureMvk::MVKPipeline pipeline;
    auto* library = state->cache.getShaderLibrary(&conversion, &shader, &pipeline);
    if (!library) return {0, false};
    assert(library->key == int(config));
    return {reinterpret_cast<uintptr_t>(library), library->valid};
}

void Cache::block(unsigned module, unsigned config) {
    std::lock_guard<std::mutex> lock(state->mutex);
    assert(!state->released);
    state->blocked.emplace(module, config);
}
void Cache::releaseAll() {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->released = true;
    state->changed.notify_all();
}
bool Cache::waitForMetal(unsigned module, unsigned config) {
    std::unique_lock<std::mutex> lock(state->mutex);
    return state->changed.wait_for(lock, std::chrono::seconds(5), [&] {
        return state->libraryCounts[{module, config}] != 0;
    });
}
bool Cache::waitForCold(unsigned request) {
    std::unique_lock<std::mutex> lock(state->mutex);
    return state->changed.wait_for(lock, std::chrono::seconds(5), [&] {
        return state->waitCounts[request] != 0;
    });
}
unsigned Cache::conversions(unsigned module, unsigned config) {
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->conversionCounts[{module, config}];
}
unsigned Cache::libraries(unsigned module, unsigned config) {
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->libraryCounts[{module, config}];
}
unsigned Cache::coldWaits(unsigned request) {
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->waitCounts[request];
}
unsigned Cache::published() { return unsigned(state->cache.snapshot()); }
bool allLibrariesDestroyed() {
    return FixtureMvk::fixture.live.load() == 0 && FixtureMvk::fixture.active.load() == 0;
}
}
