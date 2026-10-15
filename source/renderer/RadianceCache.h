//
// Radiance cache: a hash table of surface cells (on the GPU only) holding the indirect light that arrives there. Update
// paths (the trace shader with CACHE_UPDATE_PASS = 1) add samples to it, this pass averages them (cache_resolve.comp),
// and the image paths stop after a diffuse bounce and read it. See RadianceCache.cpp.
//

#ifndef VULKANDEMO_RADIANCECACHE_H
#define VULKANDEMO_RADIANCECACHE_H

#include <vulkan/vulkan.h>
#include <functional>
#include <vector>
#include "renderer/GpuContext.h"
#include "renderer/RendererTypes.h"

class RadianceCache {
public:
    // The renderer's: settings (the UI edits them), frame slot, render size, and the trace shader's descriptor set,
    // pipeline layout and cache update pipeline (the update paths are the trace shader's)
    RadianceCache(GpuContext& gpu, const RenderSettings& settings, const uint32_t& currentFrame,
                  const VkExtent2D& renderExtent, const VkDescriptorSet& rtDescriptorSet,
                  const VkPipelineLayout& rtPipelineLayout,
                  std::function<VkPipeline()> cacheUpdatePipeline)
        : gpu(gpu), settings(settings), currentFrame(currentFrame), renderExtent(renderExtent),
          rtDescriptorSet(rtDescriptorSet), rtPipelineLayout(rtPipelineLayout),
          cacheUpdatePipeline(std::move(cacheUpdatePipeline)) {}

    void createRadianceCache();
    // Clears it if asked (cacheClearPending), traces the update paths, averages, reads back the cell count
    void recordRadianceCache(VkCommandBuffer commandBuffer);
    void destroyRadianceCache();

    static constexpr uint32_t cacheCapacity = 1u << 20;  // entries (40 MB in all)
    static constexpr float cacheMaxWeight = 256.0f;
    static constexpr uint32_t cacheMaxAge = 128;
    VkBuffer cacheKeyBuffer, cacheAccumulatorBuffer, cacheValueBuffer, cacheAgeBuffer, cacheStatsBuffer;
    VmaAllocation cacheKeyMemory, cacheAccumulatorMemory, cacheValueMemory, cacheAgeMemory, cacheStatsMemory;
    std::vector<VkBuffer> cacheStatsReadbackBuffers;
    std::vector<VmaAllocation> cacheStatsReadbackMemories;
    std::vector<void*> cacheStatsReadbackMapped;
    uint32_t cacheUsedEntries = 0;      // for the UI, a few frames late
    bool cacheClearPending = true;      // empty the cache before the next update (new scene, lighting changed)
    bool cacheQueriesSuspended = false; // noise reference: plain path tracing (the cache keeps filling)
    VkDescriptorSetLayout cacheDescriptorSetLayout;
    VkDescriptorPool cacheDescriptorPool;
    VkDescriptorSet cacheDescriptorSet;
    VkPipelineLayout cachePipelineLayout;
    VkPipeline cachePipeline;

private:
    GpuContext& gpu;
    const RenderSettings& settings;
    const uint32_t& currentFrame;
    const VkExtent2D& renderExtent;
    const VkDescriptorSet& rtDescriptorSet;
    const VkPipelineLayout& rtPipelineLayout;
    std::function<VkPipeline()> cacheUpdatePipeline;
};

#endif //VULKANDEMO_RADIANCECACHE_H
