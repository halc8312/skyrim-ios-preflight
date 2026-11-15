/* SPDX-License-Identifier: GPL-3.0-only
 * Authored Vulkan driver data only; this is not a GPU/cache-hit benchmark. */
#include "pipeline_cache.h"
#include "pipeline_cache_control.h"
#include "performance_metrics.h"
#include <stdexcept>
#include <atomic>
static std::atomic<uint64_t> testClock{0};
static std::atomic<bool> invalidClock{false};
extern "C" uint64_t fixture_cache_now_ns() {
    if (invalidClock.load()) return 0;
    uint64_t value = testClock.load();
    return value ? value : skyrim_perf_now_ns();
}
#include <cassert>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <thread>
#include <memory>
#include <cstdio>
#include <cerrno>
#include <unistd.h>

static std::atomic<bool> rejectImportLink{false}, rejectPublishedCleanup{false}, importLinked{false};
extern "C" int fixture_cache_link(const char *from, const char *to) {
    if (rejectImportLink.load()) { errno = ENOTSUP; return -1; }
    int result = link(from, to);
    if (!result) importLinked = true;
    return result;
}
extern "C" int fixture_cache_unlink(const char *name) {
    if (rejectPublishedCleanup.load() && importLinked.load()) { errno = EACCES; return -1; }
    return unlink(name);
}

namespace fs = std::filesystem;
static VkPhysicalDeviceProperties properties() {
    VkPhysicalDeviceProperties p{};
    p.vendorID = 123; p.deviceID = 456; p.driverVersion = 7; p.apiVersion = 8;
    for (unsigned i = 0; i < VK_UUID_SIZE; ++i) p.pipelineCacheUUID[i] = uint8_t(i * 13);
    return p;
}
static std::vector<uint8_t> payload() {
    auto p = properties();
    std::vector<uint8_t> b(96, 0x5a);
    uint32_t h[] = {32, 1, p.vendorID, p.deviceID};
    std::memcpy(b.data(), h, 16); std::memcpy(b.data() + 16, p.pipelineCacheUUID, 16);
    return b;
}
struct Driver {
    std::vector<uint8_t> data = payload(), offered;
    unsigned creates = 0, gets = 0, destroys = 0;
    bool rejectLoaded = false, rejectAll = false, incomplete = false, excessive = false;
    std::atomic<bool> inGet{false};
    std::function<void()> duringGet, duringSize;
    VkResult dataResult = VK_SUCCESS;
    unsigned races = 0;
    bool shortSuccess = false;
    static VKAPI_ATTR VkResult VKAPI_CALL create(VkDevice raw,
        const VkPipelineCacheCreateInfo *info, const VkAllocationCallbacks *allocator, VkPipelineCache *out) {
        auto& d = *reinterpret_cast<Driver*>(raw); ++d.creates;
        assert(!allocator && info->sType == VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO && !info->flags && !info->pNext);
        if (info->initialDataSize) {
            const auto *p = static_cast<const uint8_t*>(info->pInitialData);
            d.offered.assign(p, p + info->initialDataSize);
            if (d.rejectLoaded) return VK_ERROR_INITIALIZATION_FAILED;
        } else assert(!info->pInitialData);
        if (d.rejectAll) return VK_ERROR_OUT_OF_HOST_MEMORY;
        *out = (VkPipelineCache)uintptr_t(42);
        return VK_SUCCESS;
    }
    static VKAPI_ATTR VkResult VKAPI_CALL get(VkDevice raw, VkPipelineCache cache, size_t *size, void *out) {
        auto& d = *reinterpret_cast<Driver*>(raw); ++d.gets;
        assert(cache == (VkPipelineCache)uintptr_t(42));
        if (!out) { if (d.duringSize) d.duringSize(); *size = d.excessive ? 65 * 1024 * 1024 : d.data.size(); return VK_SUCCESS; }
        d.inGet = true;
        if (d.duringGet) d.duringGet();
        d.inGet = false;
        if (d.dataResult != VK_SUCCESS) return d.dataResult;
        if (d.incomplete || d.races) {
            if (d.races) --d.races;
            *size = 0; return d.shortSuccess ? VK_SUCCESS : VK_INCOMPLETE;
        }
        assert(*size >= d.data.size());
        *size = d.data.size(); std::memcpy(out, d.data.data(), *size);
        return VK_SUCCESS;
    }
    static VKAPI_ATTR void VKAPI_CALL destroy(VkDevice raw, VkPipelineCache cache, const VkAllocationCallbacks *allocator) {
        auto& d = *reinterpret_cast<Driver*>(raw);
        assert(!allocator && !d.inGet && cache == (VkPipelineCache)uintptr_t(42)); ++d.destroys;
    }
};
static auto makeCache(Driver& driver, const fs::path& dir, const char *source = "authored-fixture-v1",
                      VkPhysicalDeviceProperties p = properties(), const char *legacy = nullptr) {
    return std::make_unique<skyrim::PipelineCache>(reinterpret_cast<VkDevice>(&driver),
        Driver::create, Driver::get, Driver::destroy, p, dir.c_str(), source, legacy);
}
static void checkpoint() {
    std::fputs("CACHE_CASE_BEGIN\n", stderr);
    skyrim_pipeline_checkpoint_save(skyrim_pipeline_checkpoint_begin());
    std::fputs("CACHE_CASE_END\n", stderr);
}
static std::vector<uint8_t> readFile(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
static void writeFile(const fs::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    assert(f.good());
}
int main(int argc, char **argv) {
    assert(argc == 2);
    fs::path dir = argv[1], file = dir / "pipeline-v1.cache";
    fs::create_directory(dir);
    std::vector<uint8_t> good;
    {
        Driver d;
        auto cache = makeCache(d, dir);
        assert(cache->result() == VK_SUCCESS && d.offered.empty());
        cache->compiled(); cache->compiled();
        assert(d.gets == 0 && !fs::exists(file)); // no active-frame I/O
        writeFile(file.string() + ".pending", {0, 1, 2});
        checkpoint();
        assert(d.gets == 2 && fs::exists(file) && !fs::exists(file.string() + ".pending"));
        good = readFile(file);
        checkpoint(); assert(d.gets == 2); // unchanged cache is not serialized again
        cache.reset(); assert(d.destroys == 1);
    }
    {
        Driver d; auto cache = makeCache(d, dir);
        assert(d.offered == d.data && d.creates == 1);
    }
    for (auto which : {0, 1, 2, 3}) {
        Driver d; auto p = properties();
        if (which == 0) ++p.vendorID;
        if (which == 1) ++p.driverVersion;
        if (which == 2) ++p.pipelineCacheUUID[0];
        auto cache = makeCache(d, dir, which == 3 ? "changed-source" : "authored-fixture-v1", p);
        assert(d.offered.empty() && cache->result() == VK_SUCCESS);
    }
    for (size_t pos : {size_t(0), size_t(8), size_t(16), size_t(24), size_t(32), good.size() - 1}) {
        auto bad = good; bad[pos] ^= 0xff; writeFile(file, bad);
        Driver d; auto cache = makeCache(d, dir);
        assert(d.offered.empty() && cache->result() == VK_SUCCESS);
    }
    writeFile(file, std::vector<uint8_t>(good.begin(), good.end() - 1));
    { Driver d; auto cache = makeCache(d, dir); assert(d.offered.empty()); }
    writeFile(file, good);
    {
        fs::path other = dir / "authored-other-file";
        writeFile(other, good); fs::remove(file); fs::create_symlink(other, file);
        Driver d; auto cache = makeCache(d, dir); assert(d.offered.empty());
        fs::remove(file); fs::remove(other); writeFile(file, good);
    }
    {
        Driver d; d.rejectLoaded = true;
        auto cache = makeCache(d, dir);
        assert(d.creates == 2 && cache->result() == VK_SUCCESS && d.offered == d.data);
    }
    {
        Driver d; d.rejectAll = true;
        auto cache = makeCache(d, dir);
        assert(d.creates == 2 && cache->result() == VK_ERROR_OUT_OF_HOST_MEMORY);
        cache.reset(); assert(d.destroys == 0);
    }
    // A changing driver snapshot must not lose the last valid disk file.
    for (bool shortSuccess : {false, true}) {
        Driver d; auto cache = makeCache(d, dir); cache->compiled();
        d.races = 1; d.shortSuccess = shortSuccess;
        checkpoint(); assert(d.gets == 4 && readFile(file) == good);
        checkpoint(); assert(d.gets == 4);
    }
    {
        Driver d; auto cache = makeCache(d, dir); cache->compiled();
        d.incomplete = true; checkpoint();
        assert(d.gets == 6 && readFile(file) == good && !fs::exists(file.string()+".pending"));
    }
    {
        Driver d; auto cache = makeCache(d, dir); cache->compiled();
        d.races = 1; d.duringGet = [&]{ d.excessive = true; };
        checkpoint(); assert(d.gets == 3 && readFile(file) == good);
    }
    {
        Driver d; auto cache = makeCache(d, dir); cache->compiled();
        d.dataResult = VK_ERROR_OUT_OF_HOST_MEMORY;
        checkpoint(); assert(d.gets == 2 && readFile(file) == good);
    }
    {
        Driver d; auto cache = makeCache(d, dir); cache->compiled();
        d.duringSize = []{ throw std::runtime_error("authored driver exception"); };
        checkpoint(); assert(d.gets == 1 && readFile(file) == good);
    }
    for (bool beforeData : {true, false}) {
        Driver d; auto cache = makeCache(d, dir); cache->compiled();
        testClock = 100;
        auto expire = []{ testClock = UINT64_C(1000000100); };
        if (beforeData) d.duringSize = expire;
        else { d.races = 1; d.duringGet = expire; }
        checkpoint(); testClock = 0;
        assert(d.gets == (beforeData ? 1u : 2u) && readFile(file) == good);
    }
    for (bool beforeData : {true, false}) {
        Driver d; auto cache = makeCache(d, dir); cache->compiled();
        uint64_t ticket = skyrim_pipeline_checkpoint_begin();
        auto cancel = [ticket]{ skyrim_pipeline_checkpoint_cancel(ticket); };
        if (beforeData) d.duringSize = cancel;
        else { d.races = 1; d.duringGet = cancel; }
        skyrim_pipeline_checkpoint_save(ticket);
        assert(d.gets == (beforeData ? 1u : 2u) && readFile(file) == good);
    }
    // The generation of the successful retry, not the first attempt, is saved.
    {
        Driver d; auto cache = makeCache(d, dir); cache->compiled(); d.races = 1;
        d.duringGet = [&]{ if (d.races) cache->compiled(); };
        checkpoint(); assert(d.gets == 4);
        d.duringGet = {}; checkpoint(); assert(d.gets == 4);
    }
    std::puts("bounded_snapshot_retry=PASS");
    {
        Driver d; auto cache = makeCache(d, dir); cache->compiled();
        d.incomplete = true; checkpoint(); assert(readFile(file) == good);
        d.incomplete = false; d.excessive = true; checkpoint(); assert(readFile(file) == good);
        d.excessive = false;
        d.data[0] ^= 1; checkpoint(); assert(readFile(file) == good);
        d.data[0] ^= 1;
        auto ticket = skyrim_pipeline_checkpoint_begin();
        d.duringGet = [ticket]{ skyrim_pipeline_checkpoint_cancel(ticket); };
        skyrim_pipeline_checkpoint_save(ticket); assert(readFile(file) == good);
        // A compile arriving during capture must remain dirty after successful publication.
        d.duringGet = [&cache]{ cache->compiled(); };
        checkpoint(); unsigned before = d.gets;
        d.duringGet = {}; checkpoint(); assert(d.gets == before + 2);
        checkpoint(); assert(d.gets == before + 2);
    }
    {
        Driver d; auto cache = makeCache(d, dir); cache->compiled();
        std::atomic<bool> entered{false}, release{false}, destroying{false};
        d.duringGet = [&]{ entered = true; while (!release) std::this_thread::yield(); };
        std::thread saver(checkpoint);
        while (!entered) std::this_thread::yield();
        std::thread destructor([&]{ destroying = true; cache.reset(); });
        while (!destroying) std::this_thread::yield();
        release = true; saver.join(); destructor.join();
        assert(d.destroys == 1);
    }
    {
        Driver d; auto cache = makeCache(d, dir); cache->compiled();
        auto old = skyrim_pipeline_checkpoint_begin();
        auto current = skyrim_pipeline_checkpoint_begin();
        skyrim_pipeline_checkpoint_save(old); assert(d.gets == 0);
        skyrim_pipeline_checkpoint_save(current); assert(d.gets == 2);
    }
    // Import is an acceleration hint, never an excuse to replace an existing
    // target (including an invalid target), or remove the legacy file.
    {
        fs::path legacy = dir.parent_path() / "legacy-files", target = dir.parent_path() / "persistent-files";
        fs::create_directory(legacy); fs::create_directory(target);
        fs::path legacyFile = legacy / "pipeline-v1.cache", targetFile = target / "pipeline-v1.cache";
        writeFile(legacyFile, good);
        {
            Driver d; auto cache = makeCache(d, target, "authored-fixture-v1", properties(), legacy.c_str());
            assert(d.offered == d.data && !fs::exists(targetFile));
            checkpoint(); // import is dirty even without a new pipeline
            assert(d.gets == 2 && readFile(targetFile) == good && readFile(legacyFile) == good);
        }
        writeFile(targetFile, {1, 2, 3});
        {
            Driver d; auto cache = makeCache(d, target, "authored-fixture-v1", properties(), legacy.c_str());
            assert(d.offered.empty()); checkpoint();
            assert(d.gets == 0 && readFile(targetFile) == std::vector<uint8_t>({1, 2, 3}));
        }
        fs::remove(targetFile);
        writeFile(legacyFile, {1, 2, 3});
        {
            Driver d; auto cache = makeCache(d, target, "authored-fixture-v1", properties(), legacy.c_str());
            assert(d.offered.empty()); checkpoint(); assert(d.gets == 0 && !fs::exists(targetFile));
        }
        writeFile(legacyFile, good);
        {
            Driver d; d.rejectLoaded = true;
            auto cache = makeCache(d, target, "authored-fixture-v1", properties(), legacy.c_str());
            assert(d.creates == 2); checkpoint(); assert(d.gets == 0 && !fs::exists(targetFile));
        }
        {
            Driver d; auto cache = makeCache(d, target, "authored-fixture-v1", properties(), legacy.c_str());
            writeFile(targetFile, {9, 8, 7}); // another writer wins after our initial absence check
            checkpoint(); assert(readFile(targetFile) == std::vector<uint8_t>({9, 8, 7}));
            assert(!fs::exists(targetFile.string()+".pending") && readFile(legacyFile) == good);
            checkpoint(); assert(readFile(targetFile) == std::vector<uint8_t>({9, 8, 7}));
        }
        fs::remove(targetFile);
        {
            Driver d; auto cache = makeCache(d, target, "authored-fixture-v1", properties(), legacy.c_str());
            rejectImportLink = true; checkpoint(); rejectImportLink = false;
            assert(!fs::exists(targetFile) && !fs::exists(targetFile.string()+".pending"));
            importLinked = false; rejectPublishedCleanup = true;
            checkpoint(); rejectPublishedCleanup = false;
            assert(readFile(targetFile) == good && fs::exists(targetFile.string()+".pending"));
            unsigned before = d.gets;
            checkpoint(); assert(d.gets == before); // publication succeeded despite cleanup error
            cache->compiled(); checkpoint(); // next ordinary save can remove stale pending
            assert(d.gets == before+2 && !fs::exists(targetFile.string()+".pending"));
        }
        assert(readFile(legacyFile) == good);
    }
    puts("validated_absent_target_import=PASS");
    {
        testClock = UINT64_C(1000000000000000);
        Driver d; auto cache = makeCache(d, dir); cache->compiled();
        testClock += UINT64_C(11000000000);
        assert(!skyrim_pipeline_checkpoint_foreground_begin()); // default OFF
        skyrim_pipeline_checkpoint_foreground_permission(1);
        {
            skyrim::PipelineCache::CompileScope active(*cache);
            testClock += UINT64_C(11000000000);
            assert(!skyrim_pipeline_checkpoint_foreground_begin()); // active is not quiet
        }
        assert(!skyrim_pipeline_checkpoint_foreground_begin()); // recently finished
        testClock += UINT64_C(10000000000);
        auto ticket = skyrim_pipeline_checkpoint_foreground_begin(); assert(ticket);
        assert(!skyrim_pipeline_checkpoint_foreground_begin()); // reservation
        d.duringSize = [&] { skyrim::PipelineCache::CompileScope active(*cache); cache->compiled(); };
        skyrim_pipeline_checkpoint_save(ticket);
        assert(d.gets == 1 && readFile(file) == good); // new work cancels before data query
        assert(!skyrim_pipeline_checkpoint_foreground_begin()); // cooldown
        d.duringSize = {};
        testClock += UINT64_C(61000000000);
        ticket = skyrim_pipeline_checkpoint_foreground_begin(); assert(ticket);
        skyrim_pipeline_checkpoint_foreground_permission(0);
        skyrim_pipeline_checkpoint_save(ticket);
        assert(d.gets == 1 && readFile(file) == good); // permission loss before worker starts
        skyrim_pipeline_checkpoint_foreground_permission(1);
        testClock += UINT64_C(61000000000);
        ticket = skyrim_pipeline_checkpoint_foreground_begin(); assert(ticket);
        std::atomic<bool> entered{false}, release{false};
        d.duringGet = [&] { entered = true; while (!release) std::this_thread::yield(); };
        std::thread saver([&] { skyrim_pipeline_checkpoint_save(ticket); });
        while (!entered) std::this_thread::yield();
        assert(!skyrim_pipeline_checkpoint_foreground_begin());
        skyrim_pipeline_checkpoint_save(ticket); // duplicate does not wait or invoke the driver
        skyrim_pipeline_checkpoint_foreground_permission(0); // does not wait on blocked driver
        assert(d.gets == 3);
        release = true; saver.join();
        assert(readFile(file) == good && !fs::exists(file.string()+".pending"));
        d.duringGet = {};
        skyrim_pipeline_checkpoint_foreground_permission(1);
        testClock += UINT64_C(61000000000);
        ticket = skyrim_pipeline_checkpoint_foreground_begin(); assert(ticket);
        skyrim_pipeline_checkpoint_save(ticket);
        assert(d.gets == 5 && readFile(file) == good);
        testClock += UINT64_C(61000000000);
        assert(!skyrim_pipeline_checkpoint_foreground_begin()); // clean
        cache->compiled();
        try { skyrim::PipelineCache::CompileScope failed(*cache); throw std::runtime_error("authored"); }
        catch (const std::runtime_error&) {}
        assert(!skyrim_pipeline_checkpoint_foreground_begin()); // exception decremented active, refreshed quiet time
        testClock += UINT64_C(10000000000);
        ticket = skyrim_pipeline_checkpoint_foreground_begin(); assert(ticket);
        skyrim_pipeline_checkpoint_cancel(ticket);
        skyrim_pipeline_checkpoint_foreground_permission(0);
        testClock = 0;
    }
    puts("foreground_admission_cancel_singleflight=PASS");
    {
        testClock = UINT64_C(2000000000000000);
        Driver d; auto cache = makeCache(d, dir); cache->compiled();
        skyrim_pipeline_checkpoint_foreground_permission(1);
        auto earlier = std::make_unique<skyrim::PipelineCache::CompileScope>(*cache);
        auto later = std::make_unique<skyrim::PipelineCache::CompileScope>(*cache);
        const uint64_t base = testClock.load();
        testClock = base + UINT64_C(20000000000); later.reset();
        testClock = base + UINT64_C(10000000000); earlier.reset(); // older sampled finish published last
        testClock = base + UINT64_C(25000000000);
        assert(!skyrim_pipeline_checkpoint_foreground_begin());
        testClock = base + UINT64_C(30000000000);
        auto ticket = skyrim_pipeline_checkpoint_foreground_begin(); assert(ticket);
        skyrim_pipeline_checkpoint_cancel(ticket);
        {
            skyrim::PipelineCache::CompileScope badClock(*cache);
            invalidClock = true;
        }
        invalidClock = false;
        testClock = base + UINT64_C(100000000000);
        assert(!skyrim_pipeline_checkpoint_foreground_begin()); // clock failure remains closed
        skyrim_pipeline_checkpoint_foreground_permission(0);
        testClock = 0;
    }
    puts("monotonic_completion_and_clock_failure=PASS");
    puts("pipeline_cache_roundtrip_rejection_cancellation_dirty_generation_lifetime=PASS");
}
