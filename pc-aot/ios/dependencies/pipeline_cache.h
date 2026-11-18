/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <vulkan/vulkan.h>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace skyrim {
struct CacheCheckpointReport;
/* Owns one internally-synchronized Vulkan cache. Optional disk state is an
 * acceleration hint only. No cache failures are converted into game success. */
class PipelineCache {
public:
    PipelineCache(VkDevice device, PFN_vkCreatePipelineCache create,
        PFN_vkGetPipelineCacheData getData, PFN_vkDestroyPipelineCache destroy,
        const VkPhysicalDeviceProperties& properties, const char *directory,
        const char *sourceNamespace, const char *legacyDirectory = nullptr);
    ~PipelineCache();
    PipelineCache(const PipelineCache&) = delete;
    PipelineCache& operator=(const PipelineCache&) = delete;
    VkPipelineCache handle() const { return handle_; }
    VkResult result() const { return result_; }
    void compiled() { generation_.fetch_add(1, std::memory_order_relaxed); }
    // Covers an actual graphics/compute pipeline API invocation, including
    // failure and exception paths. No cache lock is held during compilation.
    class CompileScope {
    public:
        explicit CompileScope(PipelineCache& cache) noexcept : cache_(cache) { cache_.compileStarted(); }
        ~CompileScope() noexcept { cache_.compileFinished(); }
        CompileScope(const CompileScope&) = delete;
        CompileScope& operator=(const CompileScope&) = delete;
    private:
        PipelineCache& cache_;
    };
    static void checkpointAll(uint64_t ticket);
    static uint64_t foregroundBegin();
private:
    void compileStarted() noexcept;
    void compileFinished() noexcept;
    bool foregroundQuiet(uint64_t now) const;
    void checkpoint(uint64_t ticket);
    bool validPayload(const std::vector<uint8_t>& bytes) const;
    std::vector<uint8_t> load(const std::string& path) const;
    bool save(const std::vector<uint8_t>& bytes, uint64_t ticket, CacheCheckpointReport& report) const;
    VkDevice device_;
    PFN_vkGetPipelineCacheData getData_;
    PFN_vkDestroyPipelineCache destroy_;
    VkPhysicalDeviceProperties properties_;
    VkPipelineCache handle_ = VK_NULL_HANDLE;
    VkResult result_ = VK_ERROR_INITIALIZATION_FAILED;
    std::string identity_, path_;
    std::atomic<uint64_t> generation_{0};
    std::atomic<uint32_t> activeCompiles_{0};
    std::atomic<uint64_t> lastCompileNs_{0};
    uint64_t savedGeneration_ = 0; // checkpoint registry lock protects this
    PipelineCache *next_ = nullptr;
    bool registered_ = false;
    bool importPending_ = false; // first imported publication must not replace a target
};
}
