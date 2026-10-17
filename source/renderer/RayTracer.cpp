//
// The trace pass: the images the trace shader writes, its descriptors, its pipelines (one shader, 21 pipelines) and its
// dispatches. See RayTracer.h. The frame around it (uniforms, the order of the passes): Frame.cpp.
//

#include <unordered_map>
#include <vulkan/vulkan.h>
#include <atomic>
#include <thread>
#include <stdexcept>
#include <cmath>
#include <string>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <glm/gtc/constants.hpp>
#include "renderer/RayTracer.h"
#include "core/Configuration.h"
#include "renderer/VulkanExtensions.h"

std::vector<char> readFile(const std::string& filename);

void RayTracer::createUniformBuffer() {
    gpu.createBuffer(sizeof(SceneUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, uniformBuffer, uniformBufferMemory);
}

// The images the trace shader writes, at render size (the display image is in Display.cpp)
void RayTracer::createRayTracingImages() {
    // Full size only for the features that are on (PerPixelBuffers), the rest 1 x 1
    perPixelBuffers = wantedPerPixelBuffers();
    VkExtent2D guideExtent = perPixelBuffers.guides ? renderExtent : VkExtent2D{1, 1};
    VkExtent2D historyExtent = perPixelBuffers.history ? renderExtent : VkExtent2D{1, 1};
    VkDeviceSize pixels = static_cast<VkDeviceSize>(renderExtent.width) * renderExtent.height;
    VkDeviceSize restirPixels = perPixelBuffers.restir ? pixels : 1;
    VkDeviceSize restirGiPixels = perPixelBuffers.restirGi ? pixels : 1;

    // Everything DLSS reads is also "sampled": DLSS switches the images to SHADER_READ_ONLY_OPTIMAL while reading.
    const VkImageUsageFlags dlssInput = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    gpu.createImage(renderExtent.width, renderExtent.height, radianceFormat, dlssInput,
                    radianceImage, radianceImageMemory, radianceImageView, 1, true);
    // The DLSS inputs. The diffuse albedo is copied into the history image for the reprojection error view.
    gpu.createImage(guideExtent, linearDepthFormat, dlssInput, linearDepthImage);
    gpu.createImage(guideExtent, motionVectorFormat, dlssInput, motionVectorImage);
    gpu.createImage(guideExtent, guideColorFormat, dlssInput | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, diffuseAlbedoImage);
    gpu.createImage(guideExtent, guideColorFormat, dlssInput, specularAlbedoImage);
    gpu.createImage(guideExtent, guideColorFormat, dlssInput, normalRoughnessImage);
    gpu.createImage(guideExtent, hitDistanceFormat, dlssInput, specularHitDistanceImage);
    gpu.createImage(historyExtent, guideColorFormat, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    guideHistoryImage);
    // Written before it is read in every frame that uses it, so it needs no clearing
    gpu.createBuffer(sizeof(GpuPrimaryHit) * restirPixels, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, primaryHitBuffer, primaryHitMemory, VK_NULL_HANDLE, true);
    gpu.createBuffer(sizeof(GpuBounceHit) * restirPixels, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, bounceHitBuffer, bounceHitMemory, VK_NULL_HANDLE, true);
    // this frame's, last frame's, and the ReSTIR pass's result for the spatial pass
    gpu.createBuffer(sizeof(GpuReservoir) * 3 * restirPixels, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, reservoirBuffer, reservoirMemory, VK_NULL_HANDLE, true);
    gpu.createBuffer(sizeof(GpuRestirSurface) * 2 * restirPixels, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, restirSurfaceBuffer, restirSurfaceMemory, VK_NULL_HANDLE, true);
    // ReSTIR GI: this frame's, last frame's, and the image pass's fresh samples
    gpu.createBuffer(sizeof(GpuGiReservoir) * 3 * restirGiPixels, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, giReservoirBuffer, giReservoirMemory, VK_NULL_HANDLE, true);
    restirHistoryAvailable = false;  // new buffers: nothing to reuse yet
    restirGiHistoryAvailable = false;

    // All stay in GENERAL forever
    gpu.immediateSubmit([&](VkCommandBuffer cmd) {
        VkImage images[] = {radianceImage, linearDepthImage.image, motionVectorImage.image, diffuseAlbedoImage.image,
                            specularAlbedoImage.image, normalRoughnessImage.image, specularHitDistanceImage.image,
                            guideHistoryImage.image};
        constexpr uint32_t imageCount = static_cast<uint32_t>(std::size(images));
        VkImageMemoryBarrier barriers[imageCount]{};
        for (uint32_t i = 0; i < imageCount; i++) {
            barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barriers[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barriers[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barriers[i].image = images[i];
            barriers[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            barriers[i].srcAccessMask = 0;
            barriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        }
        // (the transfer stage too: the clear below must wait for the layout change)
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, imageCount, barriers);
        // The history starts out black (the reprojection view shows an error for one frame, not garbage)
        VkClearColorValue black{};
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, guideHistoryImage.image, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &range);
        VkMemoryBarrier clearBarrier{};
        clearBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        clearBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        clearBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 1, &clearBarrier, 0, nullptr, 0, nullptr);
    });
    hasPreviousViewProjection = false;  // new size: last frame's matrices don't fit

    resetAccumulation = true;  // new image, its contents are garbage
}

void RayTracer::destroyRayTracingImages() {
    for (RenderImage* image : {&linearDepthImage, &motionVectorImage, &diffuseAlbedoImage, &specularAlbedoImage,
                               &normalRoughnessImage, &specularHitDistanceImage, &guideHistoryImage}) {
        gpu.destroyImage(*image);
    }
    vkDestroyImageView(gpu.device, radianceImageView, nullptr);
    vkDestroyImage(gpu.device, radianceImage, nullptr);
    vmaFreeMemory(gpu.allocator, radianceImageMemory);
    vkDestroyBuffer(gpu.device, primaryHitBuffer, nullptr);
    vmaFreeMemory(gpu.allocator, primaryHitMemory);
    vkDestroyBuffer(gpu.device, bounceHitBuffer, nullptr);
    vmaFreeMemory(gpu.allocator, bounceHitMemory);
    vkDestroyBuffer(gpu.device, reservoirBuffer, nullptr);
    vmaFreeMemory(gpu.allocator, reservoirMemory);
    vkDestroyBuffer(gpu.device, restirSurfaceBuffer, nullptr);
    vmaFreeMemory(gpu.allocator, restirSurfaceMemory);
    vkDestroyBuffer(gpu.device, giReservoirBuffer, nullptr);
    vmaFreeMemory(gpu.allocator, giReservoirMemory);
}

void RayTracer::createRayTracingDescriptors() {
    // 0: TLAS, 1: radiance (HDR output), 2: linear depth, 3: scene uniforms, 4: vertices, 5: indices,
    // 6: mesh infos, 7: all textures, 8: materials, 9: emissive triangles, 10: first light per instance,
    // 11: specular albedo table, 12: per-instance matrices and motion index, 13-17: motion vectors, diffuse albedo,
    // specular albedo, normal + roughness, specular hit distance, 18-20: radiance cache keys, accumulators, averages,
    // 21: environment map, 22: environment sampling distribution, 23: punctual lights, 24: light tree,
    // 25: primary hits, 26: ReSTIR reservoirs, 27: ReSTIR surfaces, 28: first bounces traced by the ReSTIR pass,
    // 29: ReSTIR GI reservoirs, 30: blue noise, 31: portals, 32: 3D textures (the fog's, Volumes.cpp),
    // 33: per-object motion, 34: clip regions (objects part way through portals)
    std::vector<VkDescriptorSetLayoutBinding> bindings(35);
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[3] = {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[4] = {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[5] = {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[6] = {6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[7] = {7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, maxTextures, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[8] = {8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[9] = {9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[10] = {10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[11] = {11, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[12] = {12, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    for (uint32_t i = 13; i <= 17; i++) {
        bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    }
    for (uint32_t i = 18; i <= 20; i++) {
        bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    }
    bindings[21] = {21, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[22] = {22, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[23] = {23, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[24] = {24, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[25] = {25, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[26] = {26, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[27] = {27, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[28] = {28, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[29] = {29, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[30] = {30, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[31] = {31, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[32] = {32, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, GpuScene::maxVolumeTextures, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[33] = {33, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[34] = {34, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

    // The texture array is "partially bound": only the first gpuAssets.textures.size() entries are filled in.
    // That's allowed as long as the shader never reads the empty ones.
    std::vector<VkDescriptorBindingFlags> bindingFlags(bindings.size(), 0);
    bindingFlags[7] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
    bindingFlags[32] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsInfo{};
    bindingFlagsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    bindingFlagsInfo.bindingCount = static_cast<uint32_t>(bindingFlags.size());
    bindingFlagsInfo.pBindingFlags = bindingFlags.data();

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    layoutInfo.pNext = &bindingFlagsInfo;
    if (vkCreateDescriptorSetLayout(gpu.device, &layoutInfo, nullptr, &rtDescriptorSetLayout) != VK_SUCCESS) {
        throw std::runtime_error("failed to create ray tracing descriptor set layout!");
    }

    VkDescriptorPoolSize poolSizes[] = {
            {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 7},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 23},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, maxTextures + 1 + GpuScene::maxVolumeTextures}
    };
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = (uint32_t)std::size(poolSizes);
    poolInfo.pPoolSizes = poolSizes;
    if (vkCreateDescriptorPool(gpu.device, &poolInfo, nullptr, &rtDescriptorPool) != VK_SUCCESS) {
        throw std::runtime_error("failed to create ray tracing descriptor pool!");
    }

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = rtDescriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &rtDescriptorSetLayout;
    if (vkAllocateDescriptorSets(gpu.device, &allocInfo, &rtDescriptorSet) != VK_SUCCESS) {
        throw std::runtime_error("failed to allocate ray tracing descriptor set!");
    }

    updateRayTracingDescriptorSet();
}

// Called again after a swapchain recreation (new output images) and after loading a scene (new TLAS and buffers).
void RayTracer::updateRayTracingDescriptorSet() {
    VkWriteDescriptorSetAccelerationStructureKHR asInfo{};
    asInfo.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    asInfo.accelerationStructureCount = 1;
    asInfo.pAccelerationStructures = &scene.tlas.handle;

    VkDescriptorImageInfo radianceInfo{VK_NULL_HANDLE, radianceImageView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo depthInfo{VK_NULL_HANDLE, linearDepthImage.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo uniformInfo{uniformBuffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo vertexInfo{scene.gpuAssets.vertexBuffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo indexInfo{scene.gpuAssets.indexBuffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo meshInfo{scene.gpuAssets.meshInfoBuffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo materialInfo{scene.gpuAssets.materialBuffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo emissiveTriangleInfo{scene.emissiveTriangleBuffer.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo instanceLightInfo{scene.instanceLightBuffer.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo specularAlbedoInfo{specularAlbedoBuffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo instanceDataInfo{scene.instanceData.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo guideInfos[5] = {
            {VK_NULL_HANDLE, motionVectorImage.view, VK_IMAGE_LAYOUT_GENERAL},
            {VK_NULL_HANDLE, diffuseAlbedoImage.view, VK_IMAGE_LAYOUT_GENERAL},
            {VK_NULL_HANDLE, specularAlbedoImage.view, VK_IMAGE_LAYOUT_GENERAL},
            {VK_NULL_HANDLE, normalRoughnessImage.view, VK_IMAGE_LAYOUT_GENERAL},
            {VK_NULL_HANDLE, specularHitDistanceImage.view, VK_IMAGE_LAYOUT_GENERAL},
    };
    VkDescriptorBufferInfo cacheInfos[3] = {
            {cache.cacheKeyBuffer, 0, VK_WHOLE_SIZE},
            {cache.cacheAccumulatorBuffer, 0, VK_WHOLE_SIZE},
            {cache.cacheValueBuffer, 0, VK_WHOLE_SIZE},
    };
    VkDescriptorImageInfo environmentInfo{scene.environmentSampler, scene.environmentTexture.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo environmentDistributionInfo{scene.environmentDistributionBuffer.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo punctualLightInfo{scene.punctualLightBuffer.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo lightTreeInfo{scene.lightTreeBuffer.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo primaryHitInfo{primaryHitBuffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo reservoirInfo{reservoirBuffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo restirSurfaceInfo{restirSurfaceBuffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo bounceHitInfo{bounceHitBuffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo giReservoirInfo{giReservoirBuffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo blueNoiseInfo{blueNoiseBuffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo portalInfo{scene.portalBuffer.buffer, 0, VK_WHOLE_SIZE};
    std::vector<VkDescriptorImageInfo> textureInfos;
    for (const GpuTexture& texture : scene.gpuAssets.textures) {
        textureInfos.push_back({texture.sampler, texture.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    }

    std::vector<VkDescriptorImageInfo> volumeInfos;
    for (const GpuTexture& texture : scene.gpuAssets.volumeTextures) {
        volumeInfos.push_back({texture.sampler, texture.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    }

    VkDescriptorBufferInfo motionInfo{scene.motionData.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo clipRegionInfo{scene.clipRegionBuffer.buffer, 0, VK_WHOLE_SIZE};
    std::vector<VkWriteDescriptorSet> writes(35);
    for (uint32_t i = 0; i < writes.size(); i++) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = rtDescriptorSet;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
    }
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    writes[0].pNext = &asInfo;  // acceleration structures are passed through pNext
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].pImageInfo = &radianceInfo;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[2].pImageInfo = &depthInfo;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[3].pBufferInfo = &uniformInfo;
    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[4].pBufferInfo = &vertexInfo;
    writes[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[5].pBufferInfo = &indexInfo;
    writes[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[6].pBufferInfo = &meshInfo;
    writes[7].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[7].descriptorCount = static_cast<uint32_t>(textureInfos.size());
    writes[7].pImageInfo = textureInfos.data();
    writes[8].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[8].pBufferInfo = &materialInfo;
    writes[9].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[9].pBufferInfo = &emissiveTriangleInfo;
    writes[10].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[10].pBufferInfo = &instanceLightInfo;
    writes[11].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[11].pBufferInfo = &specularAlbedoInfo;
    writes[12].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[12].pBufferInfo = &instanceDataInfo;
    for (uint32_t i = 13; i <= 17; i++) {
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[i].pImageInfo = &guideInfos[i - 13];
    }
    for (uint32_t i = 18; i <= 20; i++) {
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &cacheInfos[i - 18];
    }
    writes[21].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[21].pImageInfo = &environmentInfo;
    writes[22].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[22].pBufferInfo = &environmentDistributionInfo;
    writes[23].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[23].pBufferInfo = &punctualLightInfo;
    writes[24].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[24].pBufferInfo = &lightTreeInfo;
    writes[25].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[25].pBufferInfo = &primaryHitInfo;
    writes[26].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[26].pBufferInfo = &reservoirInfo;
    writes[27].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[27].pBufferInfo = &restirSurfaceInfo;
    writes[28].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[28].pBufferInfo = &bounceHitInfo;
    writes[29].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[29].pBufferInfo = &giReservoirInfo;
    writes[30].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[30].pBufferInfo = &blueNoiseInfo;
    writes[31].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[31].pBufferInfo = &portalInfo;
    writes[32].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[32].descriptorCount = static_cast<uint32_t>(volumeInfos.size());  // (the noise is always there)
    writes[32].pImageInfo = volumeInfos.data();
    writes[33].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[33].pBufferInfo = &motionInfo;
    writes[34].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[34].pBufferInfo = &clipRegionInfo;
    if (textureInfos.empty()) {
        writes.erase(writes.begin() + 7);  // nothing to write yet; a write with 0 descriptors isn't allowed
    }

    vkUpdateDescriptorSets(gpu.device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

void RayTracer::createRayTracingPipeline() {
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &rtDescriptorSetLayout;
    if (vkCreatePipelineLayout(gpu.device, &layoutInfo, nullptr, &rtPipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("failed to create ray tracing pipeline layout!");
    }
    createTracePipelines(settings.basicShader);
}

void RayTracer::createTracePipelines(bool basic) {
    VkShaderModule shaderModule = gpu.createShaderModule(readFile(Configuration::shaderPath("raytrace.comp.spv")));

    // Seven pipelines from the one shader: the image (both constants 0), the radiance cache's update paths
    // (CACHE_UPDATE_PASS = 1), ReSTIR's camera rays and DI candidates (RESTIR_PASS = 1), DI's spatial reuse
    // (RESTIR_PASS = 2), GI's shading (RESTIR_PASS = 3), temporal (RESTIR_PASS = 4) and spatial reuse
    // (RESTIR_PASS = 5). The constants let the compiler drop the other passes' code from each. And each again with
    // the fog's code (FOG = 1 and 2, see tracePipeline). Compiled on threads of their own: one after the other they
    // took about 3 seconds each. The basic shader (FEATURES = 0): all of them without the code for portals, lamps,
    // glass, cut-outs and diffuse transmission.
    VkPipeline* pipelines[] = {&rtPipeline, &rtCacheUpdatePipeline, &rtRestirPipeline, &rtRestirSpatialPipeline,
                               &rtRestirGiShadePipeline, &rtRestirGiTemporalPipeline, &rtRestirGiSpatialPipeline};
    const uint32_t passes = static_cast<uint32_t>(std::size(pipelines));
    std::vector<std::thread> compilers;
    std::atomic<bool> failed{false};
    for (uint32_t variant = 0; variant < 3 * passes; variant++) {
        uint32_t pass = variant % passes;
        uint32_t fog = variant / passes;
        VkPipeline* pipeline = fog > 0 ? &rtFogPipelines[fog - 1][pass] : pipelines[pass];
        compilers.emplace_back([this, pass, fog, basic, pipeline, shaderModule, &failed] {
            uint32_t constants[4] = {pass == 1 ? 1u : 0u, pass >= 2 ? pass - 1 : 0u, fog, basic ? 0u : ~0u};
            VkSpecializationMapEntry entries[4] = {{0, 0, sizeof(uint32_t)}, {1, sizeof(uint32_t), sizeof(uint32_t)},
                                                   {2, 2 * sizeof(uint32_t), sizeof(uint32_t)},
                                                   {3, 3 * sizeof(uint32_t), sizeof(uint32_t)}};
            VkSpecializationInfo specialization{4, entries, sizeof(constants), constants};
            VkComputePipelineCreateInfo pipelineInfo{};
            pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            pipelineInfo.stage.module = shaderModule;
            pipelineInfo.stage.pName = "main";
            pipelineInfo.stage.pSpecializationInfo = &specialization;
            pipelineInfo.layout = rtPipelineLayout;
            if (gpu.pipelineStatisticsSupported) {
                pipelineInfo.flags = VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
            }
            if (vkCreateComputePipelines(gpu.device, gpu.pipelineCache, 1, &pipelineInfo, nullptr, pipeline) != VK_SUCCESS) {
                failed = true;
            }
        });
    }
    for (std::thread& compiler : compilers) {
        compiler.join();
    }
    if (failed) {
        throw std::runtime_error("failed to create ray tracing pipeline!");
    }

    vkDestroyShaderModule(gpu.device, shaderModule, nullptr);
    basicTraceShader = basic;

    if (gpu.pipelineStatisticsSupported) {
        std::pair<const char*, VkPipeline> pipelines[] = {
            {"Image paths", rtPipeline},
            {"Cache update paths", rtCacheUpdatePipeline},
            {"ReSTIR candidates", rtRestirPipeline},
            {"ReSTIR spatial reuse", rtRestirSpatialPipeline},
            {"ReSTIR GI shading", rtRestirGiShadePipeline},
            {"ReSTIR GI temporal reuse", rtRestirGiTemporalPipeline},
            {"ReSTIR GI spatial reuse", rtRestirGiSpatialPipeline},
            {"Image paths with fog", rtFogPipelines[0][0]},
            {"Cache update paths with fog", rtFogPipelines[0][1]},
            {"Image paths with varying fog", rtFogPipelines[1][0]},
            {"Cache update paths with varying fog", rtFogPipelines[1][1]}};
        rtPipelineStatisticsList.clear();
        rtPipelineStatistics.clear();
        for (const auto& [name, pipeline] : pipelines) {
            rtPipelineStatisticsList.push_back({name, readPipelineStatistics(pipeline)});
            rtPipelineStatistics += std::string(name) + ":\n";
            for (const auto& [statistic, value] : rtPipelineStatisticsList.back().values) {
                rtPipelineStatistics += "  " + statistic + ": " + value + "\n";
            }
        }
        std::cout << "Trace shader statistics (from the driver"
                  << (basic ? ", basic shader" : "") << "):\n" << rtPipelineStatistics << std::flush;
    }
}

void RayTracer::destroyTracePipelines() {
    for (VkPipeline* pipeline : {&rtPipeline, &rtCacheUpdatePipeline, &rtRestirPipeline, &rtRestirSpatialPipeline,
                                 &rtRestirGiShadePipeline, &rtRestirGiTemporalPipeline, &rtRestirGiSpatialPipeline}) {
        vkDestroyPipeline(gpu.device, *pipeline, nullptr);
        *pipeline = VK_NULL_HANDLE;
    }
    for (auto& level : rtFogPipelines) {
        for (VkPipeline& pipeline : level) {
            vkDestroyPipeline(gpu.device, pipeline, nullptr);
            pipeline = VK_NULL_HANDLE;
        }
    }
}

// Before a frame: the basic shader switched on or off in the UI. Rebuilds all trace pipelines (a few seconds, the
// window stands still meanwhile).
void RayTracer::updateTracePipelines() {
    if (settings.basicShader == basicTraceShader) {
        return;
    }
    waitIdle();  // the pipelines are replaced, no frame may use them
    destroyTracePipelines();
    createTracePipelines(settings.basicShader);
    gpu.savePipelineCache();  // the newly compiled ones, for the next switch (or run)
    resetAccumulation = true;  // a different shader: a different image where the scene has the features
}

VkPipeline RayTracer::tracePipeline(VkPipeline plain) const {
    const VkPipeline plainOnes[] = {rtPipeline, rtCacheUpdatePipeline, rtRestirPipeline, rtRestirSpatialPipeline,
                                    rtRestirGiShadePipeline, rtRestirGiTemporalPipeline, rtRestirGiSpatialPipeline};
    for (size_t i = 0; fogLevel > 0 && i < std::size(plainOnes); i++) {
        if (plainOnes[i] == plain) {
            return rtFogPipelines[fogLevel - 1][i];
        }
    }
    return plain;
}

// The driver's statistics about a compiled pipeline, one "name: value" line each (on NVIDIA e.g. the registers per
// thread, which limit how many threads an SM can run at once: fewer threads hide the waits for rays and memory worse).
std::vector<std::pair<std::string, std::string>> RayTracer::readPipelineStatistics(VkPipeline pipeline) {
    std::vector<std::pair<std::string, std::string>> values;
    if (vkext::vkGetPipelineExecutablePropertiesKHR == nullptr || vkext::vkGetPipelineExecutableStatisticsKHR == nullptr) {
        return values;
    }
    VkPipelineInfoKHR pipelineInfo{VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR, nullptr, pipeline};
    uint32_t executableCount = 0;
    vkext::vkGetPipelineExecutablePropertiesKHR(gpu.device, &pipelineInfo, &executableCount, nullptr);
    for (uint32_t executable = 0; executable < executableCount; executable++) {
        VkPipelineExecutableInfoKHR executableInfo{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR, nullptr, pipeline, executable};
        uint32_t statisticCount = 0;
        vkext::vkGetPipelineExecutableStatisticsKHR(gpu.device, &executableInfo, &statisticCount, nullptr);
        std::vector<VkPipelineExecutableStatisticKHR> statistics(statisticCount, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR});
        vkext::vkGetPipelineExecutableStatisticsKHR(gpu.device, &executableInfo, &statisticCount, statistics.data());
        for (const VkPipelineExecutableStatisticKHR& statistic : statistics) {
            std::string value;
            switch (statistic.format) {
                case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR: value = statistic.value.b32 ? "yes" : "no"; break;
                case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR: value = std::to_string(statistic.value.i64); break;
                case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR: value = std::to_string(statistic.value.u64); break;
                case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR: value = std::to_string(statistic.value.f64); break;
                default: value = "?"; break;
            }
            values.emplace_back(statistic.name, value);
        }
    }
    return values;
}

// The camera rays first (their hits go to the primary hit buffer), and (DI) a light sample per pixel, reused from its
// neighbors (spatial reuse); nothing without ReSTIR. Binds the trace shader's descriptor set (also for the image paths).
void RayTracer::recordRestir(VkCommandBuffer commandBuffer, const SceneUniforms& uniforms) {
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, rtPipelineLayout, 0, 1, &rtDescriptorSet, 0, nullptr);
    if (uniforms.restirDi != 0 || uniforms.restirGi != 0) {
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, tracePipeline(rtRestirPipeline));
        vkCmdDispatch(commandBuffer, (renderExtent.width + 7) / 8, (renderExtent.height + 7) / 8, 1);
        VkMemoryBarrier hitsBarrier{};
        hitsBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        hitsBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        hitsBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &hitsBarrier, 0, nullptr, 0, nullptr);
        // Spatial reuse: reads the ReSTIR pass's reservoirs (a part of their own), writes this frame's
        if (uniforms.restirSpatialNeighbors + uniforms.restirDisocclusionNeighbors > 0) {
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, tracePipeline(rtRestirSpatialPipeline));
            vkCmdDispatch(commandBuffer, (renderExtent.width + 7) / 8, (renderExtent.height + 7) / 8, 1);
            vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0, 1, &hitsBarrier, 0, nullptr, 0, nullptr);
        }
    }
}

// The image paths; then ReSTIR GI's passes, if it is on
void RayTracer::recordImagePaths(VkCommandBuffer commandBuffer, const SceneUniforms& uniforms) {
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, tracePipeline(rtPipeline));
    vkCmdDispatch(commandBuffer, (renderExtent.width + 7) / 8, (renderExtent.height + 7) / 8, 1);
    // ReSTIR GI: the image pass left a fresh sample per pixel; reused, then its light added to the image (timed as
    // part of "trace")
    if (uniforms.restirGi != 0) {
        VkMemoryBarrier giBarrier{};
        giBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        giBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        giBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        bool spatial = uniforms.restirGiSpatialNeighbors + uniforms.restirGiDisocclusionNeighbors > 0;
        for (VkPipeline pipeline : {rtRestirGiTemporalPipeline, rtRestirGiSpatialPipeline, rtRestirGiShadePipeline}) {
            if (pipeline == rtRestirGiSpatialPipeline && !spatial) {
                continue;
            }
            vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &giBarrier, 0, nullptr, 0, nullptr);
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, tracePipeline(pipeline));
            vkCmdDispatch(commandBuffer, (renderExtent.width + 7) / 8, (renderExtent.height + 7) / 8, 1);
        }
    }
}
