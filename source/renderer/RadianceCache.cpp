//
// Radiance cache: a world-space hash table of small surface cells with the light that leaves them, so that image
// paths can stop after one diffuse bounce and read the light from there instead of tracing many more bounces.
// Similar to NVIDIA's SHaRC ("spatially hashed radiance cache"). The idea and the shader side are described in
// shaders/raytrace.comp ("Radiance cache"); per frame:
//   1. update paths (raytrace.comp, CACHE_UPDATE_PASS = 1): one full path per few pixels adds samples to cells
//   2. resolve (shaders/cache_resolve.comp): new samples are blended into each cell's average, old cells freed
//   3. the image paths (raytrace.comp) read the averages
//

#include <vulkan/vulkan.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <imgui/imgui.h>
#include "renderer/Renderer.h"
#include "core/Configuration.h"

std::vector<char> readFile(const std::string& filename);

void Renderer::createRadianceCache() {
    const VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    createBuffer(sizeof(uint32_t) * cacheCapacity, usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, cacheKeyBuffer, cacheKeyMemory,
                 VK_NULL_HANDLE, true);
    createBuffer(sizeof(glm::vec4) * cacheCapacity, usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                 cacheAccumulatorBuffer, cacheAccumulatorMemory, VK_NULL_HANDLE, true);
    createBuffer(sizeof(glm::vec4) * cacheCapacity, usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, cacheValueBuffer, cacheValueMemory,
                 VK_NULL_HANDLE, true);
    createBuffer(sizeof(uint32_t) * cacheCapacity, usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, cacheAgeBuffer, cacheAgeMemory,
                 VK_NULL_HANDLE, true);
    createBuffer(sizeof(uint32_t), usage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                 cacheStatsBuffer, cacheStatsMemory);
    cacheClearPending = true;  // emptied by the first frame

    // The number of cells in use comes back to the CPU for the UI, like the exposure (one copy per frame in flight)
    cacheStatsReadbackBuffers.resize(Configuration::maxFramesInFlight);
    cacheStatsReadbackMemories.resize(Configuration::maxFramesInFlight);
    cacheStatsReadbackMapped.resize(Configuration::maxFramesInFlight);
    for (int i = 0; i < Configuration::maxFramesInFlight; i++) {
        createBuffer(sizeof(uint32_t), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     cacheStatsReadbackBuffers[i], cacheStatsReadbackMemories[i]);
        vmaMapMemory(allocator, cacheStatsReadbackMemories[i], &cacheStatsReadbackMapped[i]);
        std::memset(cacheStatsReadbackMapped[i], 0, sizeof(uint32_t));
    }

    // Resolve pass: 0 keys, 1 accumulators, 2 averages, 3 ages, 4 stats
    VkDescriptorSetLayoutBinding bindings[5];
    for (uint32_t i = 0; i < 5; i++) {
        bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 5;
    layoutInfo.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &cacheDescriptorSetLayout) != VK_SUCCESS) {
        throw std::runtime_error("failed to create radiance cache descriptor set layout!");
    }
    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 5};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &cacheDescriptorPool) != VK_SUCCESS) {
        throw std::runtime_error("failed to create radiance cache descriptor pool!");
    }
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = cacheDescriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &cacheDescriptorSetLayout;
    if (vkAllocateDescriptorSets(device, &allocInfo, &cacheDescriptorSet) != VK_SUCCESS) {
        throw std::runtime_error("failed to allocate radiance cache descriptor set!");
    }
    VkBuffer buffers[5] = {cacheKeyBuffer, cacheAccumulatorBuffer, cacheValueBuffer, cacheAgeBuffer, cacheStatsBuffer};
    VkDescriptorBufferInfo bufferInfos[5];
    VkWriteDescriptorSet writes[5]{};
    for (uint32_t i = 0; i < 5; i++) {
        bufferInfos[i] = {buffers[i], 0, VK_WHOLE_SIZE};
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = cacheDescriptorSet;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &bufferInfos[i];
    }
    vkUpdateDescriptorSets(device, 5, writes, 0, nullptr);

    VkPushConstantRange pushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(CacheResolvePushConstants)};
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &cacheDescriptorSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &cachePipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("failed to create radiance cache pipeline layout!");
    }
    VkShaderModule shaderModule = createShaderModule(readFile("cache_resolve.comp.spv"));
    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = shaderModule;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = cachePipelineLayout;
    if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &cachePipeline) != VK_SUCCESS) {
        throw std::runtime_error("failed to create radiance cache pipeline!");
    }
    vkDestroyShaderModule(device, shaderModule, nullptr);
}

// Recorded before the image paths are traced, after the barrier that waits for the previous frame's shaders.
// The ray tracing descriptor set (which has the cache buffers too) must be bound by the caller.
void Renderer::recordRadianceCache(VkCommandBuffer commandBuffer) {
    // This frame slot's fence has been waited on: its copy from a few frames ago is complete
    std::memcpy(&cacheUsedEntries, cacheStatsReadbackMapped[currentFrame], sizeof(uint32_t));

    // Hit debug views don't trace paths, nothing to cache (except for the view that shows the cache)
    bool hitDebugView = settings.debugView != static_cast<int>(DebugView::None) && !isGuideView(settings.debugView) &&
                        settings.debugView != static_cast<int>(DebugView::RadianceCache);
    if (!settings.radianceCache || hitDebugView) {
        return;
    }

    // The previous frame's resolve and stats copy must be done before the buffers are filled again
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    if (cacheClearPending) {
        for (VkBuffer buffer : {cacheKeyBuffer, cacheAccumulatorBuffer, cacheValueBuffer, cacheAgeBuffer}) {
            vkCmdFillBuffer(commandBuffer, buffer, 0, VK_WHOLE_SIZE, 0);
        }
        cacheClearPending = false;
    }
    vkCmdFillBuffer(commandBuffer, cacheStatsBuffer, 0, VK_WHOLE_SIZE, 0);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);

    // 1: update paths, one per tile of cacheUpdateTile x cacheUpdateTile pixels (8x8 tiles per workgroup)
    uint32_t tile = static_cast<uint32_t>(std::max(settings.cacheUpdateTile, 1));
    uint32_t tilesX = (renderExtent.width + tile - 1) / tile, tilesY = (renderExtent.height + tile - 1) / tile;
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, tracePipeline(rtCacheUpdatePipeline));
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, rtPipelineLayout, 0, 1, &rtDescriptorSet, 0, nullptr);
    vkCmdDispatch(commandBuffer, (tilesX + 7) / 8, (tilesY + 7) / 8, 1);
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);

    // 2: resolve
    CacheResolvePushConstants push{};
    push.capacity = cacheCapacity;
    push.decay = 1.0f - 1.0f / std::max(settings.cacheHistoryFrames, 1.0f);
    push.maxWeight = cacheMaxWeight;
    push.maxAge = cacheMaxAge;
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, cachePipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, cachePipelineLayout, 0, 1, &cacheDescriptorSet, 0, nullptr);
    vkCmdPushConstants(commandBuffer, cachePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(commandBuffer, (cacheCapacity + 255) / 256, 1, 1);

    // The image paths read the averages next; the cell count goes to the CPU
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);
    VkBufferCopy copy{0, 0, sizeof(uint32_t)};
    vkCmdCopyBuffer(commandBuffer, cacheStatsBuffer, cacheStatsReadbackBuffers[currentFrame], 1, &copy);
    VkMemoryBarrier hostBarrier{};
    hostBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    hostBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    hostBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 1, &hostBarrier, 0, nullptr, 0, nullptr);
}

void Renderer::destroyRadianceCache() {
    vkDestroyPipeline(device, cachePipeline, nullptr);
    vkDestroyPipelineLayout(device, cachePipelineLayout, nullptr);
    vkDestroyDescriptorPool(device, cacheDescriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(device, cacheDescriptorSetLayout, nullptr);
    for (int i = 0; i < Configuration::maxFramesInFlight; i++) {
        vmaUnmapMemory(allocator, cacheStatsReadbackMemories[i]);
        vkDestroyBuffer(device, cacheStatsReadbackBuffers[i], nullptr);
        vmaFreeMemory(allocator, cacheStatsReadbackMemories[i]);
    }
    VkBuffer buffers[] = {cacheKeyBuffer, cacheAccumulatorBuffer, cacheValueBuffer, cacheAgeBuffer, cacheStatsBuffer};
    VmaAllocation memories[] = {cacheKeyMemory, cacheAccumulatorMemory, cacheValueMemory, cacheAgeMemory, cacheStatsMemory};
    for (int i = 0; i < 5; i++) {
        vkDestroyBuffer(device, buffers[i], nullptr);
        vmaFreeMemory(allocator, memories[i]);
    }
}

// Part of the "Rendering" section
void Renderer::buildRadianceCacheUi() {
    ImGui::Checkbox("Radiance cache", &settings.radianceCache);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Remembers how much indirect light leaves small patches of the surfaces. Paths stop\n"
                          "after their first diffuse bounce: they light that point directly and take the\n"
                          "indirect light from the cache. Much less noise in indirectly lit places, and faster.\n"
                          "Slightly blurs indirect light; reacts to changes in the lighting within a few dozen frames.");
    }
    if (settings.radianceCache) {
        ImGui::SameLine();
        ImGui::TextDisabled("%u cells (%.0f%% full)%s", cacheUsedEntries, 100.0f * cacheUsedEntries / cacheCapacity,
                            cacheQueriesSuspended ? ", not used for the noise reference" : "");
    }
    if (!ImGui::TreeNode("Radiance cache settings")) {
        return;
    }
    ImGui::SliderFloat("Cell size", &settings.cacheCellPixels, 1.0f, 32.0f, "%.1f pixels", ImGuiSliderFlags_Logarithmic);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Cells are about this wide on screen, wherever they are. Smaller: sharper indirect\n"
                          "light, but fewer samples per cell (noisier cache).");
    }
    ImGui::SliderInt("Update paths", &settings.cacheUpdateTile, 1, 16, "1 per %d x %d pixels");
    ImGui::SliderFloat("History", &settings.cacheHistoryFrames, 4.0f, 256.0f, "%.0f frames", ImGuiSliderFlags_Logarithmic);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("How long old samples keep counting. Longer: less noise in the cache, slower to follow\n"
                          "changes in the lighting.");
    }
    ImGui::SliderFloat("Minimum samples", &settings.cacheMinSamples, 1.0f, 64.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("A cell needs this many samples before paths use it; until then they trace on.");
    }
    if (ImGui::Button("Clear cache")) {
        cacheClearPending = true;
    }
    ImGui::TreePop();
}
