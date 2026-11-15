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
        const char *sourceNamespace);
    ~PipelineCache();
    PipelineCache(const PipelineCache&) = delete;
    PipelineCache& operator=(const PipelineCache&) = delete;
    VkPipelineCache handle() const { return handle_; }
    VkResult result() const { return result_; }
    void compiled() { generation_.fetch_add(1, std::memory_order_relaxed); }
    static void checkpointAll(uint64_t ticket);
private:
    void checkpoint(uint64_t ticket);
    bool validPayload(const std::vector<uint8_t>& bytes) const;
    std::vector<uint8_t> load() const;
    bool save(const std::vector<uint8_t>& bytes, uint64_t ticket, CacheCheckpointReport& report) const;
    VkDevice device_;
    PFN_vkGetPipelineCacheData getData_;
    PFN_vkDestroyPipelineCache destroy_;
    VkPhysicalDeviceProperties properties_;
    VkPipelineCache handle_ = VK_NULL_HANDLE;
    VkResult result_ = VK_ERROR_INITIALIZATION_FAILED;
    std::string identity_, path_;
    std::atomic<uint64_t> generation_{0};
    uint64_t savedGeneration_ = 0; // checkpoint registry lock protects this
    PipelineCache *next_ = nullptr;
    bool registered_ = false;
};
}
