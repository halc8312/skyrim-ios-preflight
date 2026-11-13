/* SPDX-License-Identifier: GPL-3.0-only
 * Authored Vulkan driver data only; this is not a GPU/cache-hit benchmark. */
#include "pipeline_cache.h"
#include "pipeline_cache_control.h"
#include <cassert>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <thread>
#include <memory>
#include <cstdio>

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
    std::function<void()> duringGet;
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
        if (!out) { *size = d.excessive ? 65 * 1024 * 1024 : d.data.size(); return VK_SUCCESS; }
        d.inGet = true;
        if (d.duringGet) d.duringGet();
        d.inGet = false;
        if (d.incomplete) { *size = 0; return VK_INCOMPLETE; }
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
                      VkPhysicalDeviceProperties p = properties()) {
    return std::make_unique<skyrim::PipelineCache>(reinterpret_cast<VkDevice>(&driver),
        Driver::create, Driver::get, Driver::destroy, p, dir.c_str(), source);
}
static void checkpoint() {
    skyrim_pipeline_checkpoint_save(skyrim_pipeline_checkpoint_begin());
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
    puts("pipeline_cache_roundtrip_rejection_cancellation_dirty_generation_lifetime=PASS");
}
