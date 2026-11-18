// Actual generated DXVK entry points call the actual patched MoltenVK cache.
// Only SPIRV-Cross/Metal and final VkPipeline creation are authored boundaries.
// All assertions use event ordering or identity/counts, never speed thresholds.
// REUSED_DXVK_FIXTURE_TYPES
#include "mvk_graphics_integration_backend.h"

struct Driver {
    struct Binding { unsigned module, config; };
    struct Record {
        unsigned calls = 0, waits = 0, role = 0;
        uintptr_t library = 0;
        VkPipeline handle = VK_NULL_HANDLE;
    };
    MvkIntegration::Cache& cache;
    std::mutex mutex;
    std::condition_variable changed;
    std::map<unsigned, Binding> bindings;
    std::map<unsigned, Record> records;
    uintptr_t nextHandle = 1;
    explicit Driver(MvkIntegration::Cache& c) : cache(c) {}
    void bind(unsigned id, unsigned module, unsigned config) {
        bindings.emplace(id, Binding{module, config});
    }
    bool validate(const State& input) { return bindings.count(input.sc.specConstants[0]) != 0; }
    VkPipeline create(const State& input, const Pass*) {
        const auto id = input.sc.specConstants[0];
        Binding binding{};
        VkPipeline handle;
        {
            std::lock_guard<std::mutex> lock(mutex);
            binding = bindings.at(id);
            auto& entry = records[id];
            ++entry.calls;
            entry.role = skyrim_perf_context_get().role;
            assert(entry.role == SKYRIM_PERF_ROLE_DRAW || entry.role == SKYRIM_PERF_ROLE_PREFETCH);
            handle = reinterpret_cast<VkPipeline>(nextHandle++);
        }
        const auto library = cache.request(id, binding.module, binding.config);
        assert(library.valid && library.identity);
        {
            std::lock_guard<std::mutex> lock(mutex);
            records[id].library = library.identity;
            records[id].handle = handle;
        }
        return handle;
    }
    void destroy(VkPipeline) {}
    void write(const State&, unsigned format) { assert(format == 73); }
    void note_wait(const State& input, const Pass*) {
        std::lock_guard<std::mutex> lock(mutex);
        ++records[input.sc.specConstants[0]].waits;
        changed.notify_all();
    }
    bool waitForFlight(unsigned id) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, 5s, [&] { return records[id].waits != 0; });
    }
    Record record(unsigned id) { std::lock_guard<std::mutex> lock(mutex); return records[id]; }
};

// ACTUAL_DXVK_METHODS

struct ReleaseOnExit {
    MvkIntegration::Cache& cache;
    ~ReleaseOnExit() { cache.releaseAll(); }
};

static void distinct_modules(bool moduleCold) {
    MvkIntegration::Cache cache;
    Driver driver(cache);
    Manager manager; // #2 admission OFF; #6 exact graphics state stays ON.
    assert(manager.iosExactGraphicsState());
    TestPipeline pipeline(driver, manager);
    Pass pass;
    driver.bind(1, 1, 10);
    driver.bind(2, 1, 10); // New DXVK state, existing MoltenVK library.
    for (unsigned i = 1; i <= 4; ++i) driver.bind(100 + i, i, 77);
    const auto warm = pipeline.get(state(1), &pass);
    assert(warm && cache.published() == 1);
    for (unsigned i = 1; i <= 3; ++i) cache.block(i, 77);
    std::vector<std::future<void>> workers;
    std::future<VkPipeline> draw, ready, libraryReady;
    ReleaseOnExit release{cache}; // Destructs before futures even on exceptions.

    workers.push_back(std::async(std::launch::async, [&] { pipeline.build(state(101), &pass); }));
    assert(cache.waitForMetal(1, 77));
    for (unsigned i = 2; i <= 3; ++i)
        workers.push_back(std::async(std::launch::async, [&, i] { pipeline.build(state(100 + i), &pass); }));
    for (unsigned i = 2; i <= 3; ++i) {
        if (moduleCold) assert(cache.waitForMetal(i, 77));
        else assert(cache.waitForCold(100 + i));
    }
    // Each worker has demonstrably reached its actual cache lock or authored
    // Metal boundary. A sleeping scheduler is not treated as a blocking result.
    const unsigned before = cache.libraries(1, 77) + cache.libraries(2, 77) + cache.libraries(3, 77);
    assert(before == (moduleCold ? 3u : 1u));
    draw = std::async(std::launch::async, [&] { return pipeline.get(state(104), &pass); });
    if (moduleCold) {
        assert(finished(draw));
        assert(cache.libraries(4, 77) == 1);
        assert(cache.coldWaits(104) == 0);
    } else {
        assert(cache.waitForCold(104));
        assert(cache.libraries(4, 77) == 0);
    }
    ready = std::async(std::launch::async, [&] { return pipeline.get(state(1), &pass); });
    libraryReady = std::async(std::launch::async, [&] { return pipeline.get(state(2), &pass); });
    assert(finished(ready) && finished(libraryReady));
    assert(ready.get() == warm);
    assert(libraryReady.get() != warm);
    assert(driver.record(1).calls == 1 && driver.record(2).calls == 1);
    assert(driver.record(1).library == driver.record(2).library);
    assert(cache.conversions(1, 10) == 1 && cache.libraries(1, 10) == 1);
    assert(cache.coldWaits(2) == 0);

    cache.releaseAll();
    for (auto& worker : workers) { assert(finished(worker)); worker.get(); }
    assert(finished(draw) && draw.get());
    for (unsigned i = 1; i <= 4; ++i) {
        const auto entry = driver.record(100 + i);
        assert(entry.calls == 1 && entry.handle && entry.library);
        assert(entry.role == unsigned(i == 4 ? SKYRIM_PERF_ROLE_DRAW : SKYRIM_PERF_ROLE_PREFETCH));
        assert(cache.conversions(i, 77) == 1 && cache.libraries(i, 77) == 1);
    }
    assert(pipeline.size() == 6 && cache.published() == 5);
    std::printf("exact_state=1 module_cold=%u prefetch_at_metal_before_release=%u independent_draw_progress=%u ready_hits=PASS\n",
                unsigned(moduleCold), before, unsigned(moduleCold));
}

static void same_identity() {
    MvkIntegration::Cache cache;
    Driver driver(cache);
    Manager manager;
    TestPipeline pipeline(driver, manager);
    Pass pass;
    driver.bind(201, 1, 77);
    driver.bind(202, 1, 77); // Different DXVK state, same module/config.
    driver.bind(203, 1, 78); // Different config in the same module.
    cache.block(1, 77);
    std::future<void> owner;
    std::future<VkPipeline> sameState, sameKey, sameModule;
    ReleaseOnExit release{cache};
    owner = std::async(std::launch::async, [&] { pipeline.build(state(201), &pass); });
    assert(cache.waitForMetal(1, 77));
    sameState = std::async(std::launch::async, [&] { return pipeline.get(state(201), &pass); });
    assert(driver.waitForFlight(201));
    sameKey = std::async(std::launch::async, [&] { return pipeline.get(state(202), &pass); });
    sameModule = std::async(std::launch::async, [&] { return pipeline.get(state(203), &pass); });
    assert(cache.waitForCold(202) && cache.waitForCold(203));
    assert(driver.record(201).calls == 1 && cache.coldWaits(201) == 0);
    assert(cache.conversions(1, 77) == 1 && cache.libraries(1, 77) == 1);
    assert(cache.conversions(1, 78) == 0 && cache.libraries(1, 78) == 0);

    cache.releaseAll();
    assert(finished(owner)); owner.get();
    assert(finished(sameState) && finished(sameKey) && finished(sameModule));
    const auto shared = sameState.get(), otherState = sameKey.get(), otherConfig = sameModule.get();
    assert(shared == driver.record(201).handle && shared != otherState && shared != otherConfig);
    assert(driver.record(201).calls == 1 && driver.record(202).calls == 1 && driver.record(203).calls == 1);
    assert(driver.record(201).library == driver.record(202).library);
    assert(driver.record(201).library != driver.record(203).library);
    assert(cache.conversions(1, 77) == 1 && cache.libraries(1, 77) == 1);
    assert(cache.conversions(1, 78) == 1 && cache.libraries(1, 78) == 1);
    assert(pipeline.size() == 3 && cache.published() == 2);
    std::puts("same_dxvk_state_single_generation=PASS same_mvk_key_single_generation=PASS same_module_serialization=PASS");
}

int main(int argc, char** argv) {
    assert(argc == 2 && (std::strcmp(argv[1], "0") == 0 || std::strcmp(argv[1], "1") == 0));
    assert(skyrim::exactGraphicsStateEnabled());
    distinct_modules(std::strcmp(argv[1], "1") == 0);
    assert(MvkIntegration::allLibrariesDestroyed());
    same_identity();
    assert(MvkIntegration::allLibrariesDestroyed());
    std::puts("cache_lifetime=PASS authored_boundaries_only=1 gpu_or_game_performance_verified=0");
}
