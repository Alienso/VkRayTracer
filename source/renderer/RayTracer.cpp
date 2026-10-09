//
// Ray tracing: scene geometry, acceleration structures, output images and the compute pipeline.
//

#include <unordered_map>
#include <vulkan/vulkan.h>
#include <stdexcept>
#include <cmath>
#include <string>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <glm/gtc/constants.hpp>
#include "renderer/Renderer.h"
#include "core/Configuration.h"
#include "renderer/VulkanExtensions.h"

std::vector<char> readFile(const std::string& filename);

static const VkFormat radianceFormat = VK_FORMAT_R32G32B32A32_SFLOAT;  // float, so tiny 1/n contributions don't round away
// DLSS inputs. 32-bit where there are only 1-2 channels (exact depth and motion), 16-bit for the colors and normals.
static const VkFormat linearDepthFormat = VK_FORMAT_R32_SFLOAT;
static const VkFormat motionVectorFormat = VK_FORMAT_R32G32_SFLOAT;
static const VkFormat guideColorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;  // albedos, normal + roughness
static const VkFormat hitDistanceFormat = VK_FORMAT_R32_SFLOAT;

// Sub-pixel camera offsets for DLSS: the Halton sequence (bases 2 and 3) covers the pixel evenly over any run of
// frames. Returns 0..1.
static float halton(uint32_t index, uint32_t base) {
    float fraction = 1.0f;
    float result = 0.0f;
    while (index > 0) {
        fraction /= static_cast<float>(base);
        result += fraction * static_cast<float>(index % base);
        index /= base;
    }
    return result;
}

void Renderer::createUniformBuffer() {
    createBuffer(sizeof(SceneUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, uniformBuffer, uniformBufferMemory);
}

// An empty acceleration structure of the given size, living inside a buffer we own. Filled by a build or a copy.
AccelerationStructure Renderer::createAccelerationStructure(VkAccelerationStructureTypeKHR type, VkDeviceSize size) {
    AccelerationStructure as;
    createBuffer(size,
                 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, as.buffer, as.memory);

    VkAccelerationStructureCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    createInfo.buffer = as.buffer;
    createInfo.size = size;
    createInfo.type = type;
    if (vkext::vkCreateAccelerationStructureKHR(device, &createInfo, nullptr, &as.handle) != VK_SUCCESS) {
        throw std::runtime_error("failed to create acceleration structure!");
    }

    VkAccelerationStructureDeviceAddressInfoKHR addressInfo{};
    addressInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    addressInfo.accelerationStructure = as.handle;
    as.address = vkext::vkGetAccelerationStructureDeviceAddressKHR(device, &addressInfo);
    return as;
}

// Scratch memory is only used during a build. Returns its (aligned) device address.
VkDeviceAddress Renderer::createScratchBuffer(VkDeviceSize size, VkBuffer& buffer, VkDeviceMemory& memory) {
    VkPhysicalDeviceAccelerationStructurePropertiesKHR asProperties{};
    asProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
    VkPhysicalDeviceProperties2 properties2{};
    properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties2.pNext = &asProperties;
    vkGetPhysicalDeviceProperties2(physicalDevice, &properties2);
    VkDeviceSize alignment = asProperties.minAccelerationStructureScratchOffsetAlignment;

    createBuffer(size + alignment,
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, buffer, memory);
    VkDeviceAddress address = getBufferDeviceAddress(buffer);
    return (address + alignment - 1) / alignment * alignment;
}

void Renderer::destroyAccelerationStructure(AccelerationStructure& as) {
    vkext::vkDestroyAccelerationStructureKHR(device, as.handle, nullptr);
    vkDestroyBuffer(device, as.buffer, nullptr);
    vkFreeMemory(device, as.memory, nullptr);
    as = {};
}

// Bottom level: one per mesh, the triangles in the mesh's own coordinate system.
//
// The driver has to reserve memory for the worst case before it builds. After the build it knows the
// real size, so we ask for it ("compacted size"), copy each BLAS into a buffer of exactly that size and
// free the original. That usually saves a good part of the memory.
//
// A BLAS holds its own copy of the triangles, so it doesn't care where the mesh sits in the packed buffers: when the
// asset store changes, the BLASes of meshes still there are kept, only the new meshes' are built.
void Renderer::syncBottomLevelAccelerationStructures() {
    const std::vector<uint64_t>& ids = assets.meshIds();
    std::unordered_map<uint64_t, size_t> previous;
    for (size_t i = 0; i < gpuAssets.meshIds.size(); i++) {
        previous[gpuAssets.meshIds[i]] = i;
    }
    std::vector<bool> kept(gpuAssets.blases.size(), false);
    std::vector<AccelerationStructure> blases(ids.size());
    std::vector<VkDeviceSize> buildSizes(ids.size(), 0);
    std::vector<VkDeviceSize> sizes(ids.size(), 0);
    std::vector<uint32_t> toBuild;
    for (size_t i = 0; i < ids.size(); i++) {
        auto it = previous.find(ids[i]);
        if (it != previous.end()) {
            blases[i] = gpuAssets.blases[it->second];
            buildSizes[i] = gpuAssets.blasBuildSizes[it->second];
            sizes[i] = gpuAssets.blasSizes[it->second];
            kept[it->second] = true;
        } else {
            toBuild.push_back(static_cast<uint32_t>(i));
        }
    }
    for (size_t i = 0; i < kept.size(); i++) {
        if (!kept[i]) {
            releaseBlas(gpuAssets.blases[i]);
        }
    }
    gpuAssets.blases = std::move(blases);
    gpuAssets.meshIds = ids;
    gpuAssets.blasBuildSizes = std::move(buildSizes);
    gpuAssets.blasSizes = std::move(sizes);

    createBottomLevelAccelerationStructures(toBuild);

    gpuAssets.blasSizeBeforeCompaction = 0;
    gpuAssets.blasSizeAfterCompaction = 0;
    for (size_t i = 0; i < gpuAssets.blases.size(); i++) {
        gpuAssets.blasSizeBeforeCompaction += gpuAssets.blasBuildSizes[i];
        gpuAssets.blasSizeAfterCompaction += gpuAssets.blasSizes[i];
    }
}

void Renderer::destroyBottomLevelAccelerationStructures() {
    for (AccelerationStructure& blas : gpuAssets.blases) {
        destroyAccelerationStructure(blas);
    }
    gpuAssets.blases.clear();
    gpuAssets.meshIds.clear();
    gpuAssets.blasBuildSizes.clear();
    gpuAssets.blasSizes.clear();
}

// Builds the BLASes of these meshes (indices into meshInfos), into gpuAssets.blases at the same indices
void Renderer::createBottomLevelAccelerationStructures(const std::vector<uint32_t>& meshes) {
    size_t count = meshes.size();
    if (count == 0) {
        return;
    }
    VkDeviceAddress vertexAddress = getBufferDeviceAddress(gpuAssets.vertexBuffer);
    VkDeviceAddress indexAddress = getBufferDeviceAddress(gpuAssets.indexBuffer);

    std::vector<VkAccelerationStructureGeometryKHR> geometries(count);
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> buildInfos(count);
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges(count);
    std::vector<AccelerationStructure> uncompacted(count);
    VkDeviceSize maxScratchSize = 0;

    // 1: describe every mesh, ask for sizes, create the (oversized) structures
    for (size_t i = 0; i < count; i++) {
        const MeshInfo& mesh = gpuAssets.meshInfos[meshes[i]];

        VkAccelerationStructureGeometryTrianglesDataKHR triangles{};
        triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        triangles.vertexData.deviceAddress = vertexAddress + mesh.vertexOffset * sizeof(SceneVertex);  // this mesh's part
        triangles.vertexStride = sizeof(SceneVertex);  // position is the first member, the rest is skipped
        triangles.maxVertex = mesh.vertexCount - 1;    // highest vertex index used
        triangles.indexType = VK_INDEX_TYPE_UINT32;
        triangles.indexData.deviceAddress = indexAddress + mesh.indexOffset * sizeof(uint32_t);

        geometries[i].sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        geometries[i].geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geometries[i].geometry.triangles = triangles;
        geometries[i].flags = VK_GEOMETRY_OPAQUE_BIT_KHR;

        buildInfos[i].sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        buildInfos[i].type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        buildInfos[i].flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                              VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
        buildInfos[i].mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        buildInfos[i].geometryCount = 1;
        buildInfos[i].pGeometries = &geometries[i];

        ranges[i].primitiveCount = mesh.indexCount / 3;

        VkAccelerationStructureBuildSizesInfoKHR sizeInfo{};
        sizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
        vkext::vkGetAccelerationStructureBuildSizesKHR(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                                       &buildInfos[i], &ranges[i].primitiveCount, &sizeInfo);

        uncompacted[i] = createAccelerationStructure(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, sizeInfo.accelerationStructureSize);
        buildInfos[i].dstAccelerationStructure = uncompacted[i].handle;
        maxScratchSize = std::max(maxScratchSize, sizeInfo.buildScratchSize);
        gpuAssets.blasBuildSizes[meshes[i]] = sizeInfo.accelerationStructureSize;
    }

    // The builds run one after another, so they can all share one scratch buffer.
    VkBuffer scratchBuffer;
    VkDeviceMemory scratchMemory;
    VkDeviceAddress scratchAddress = createScratchBuffer(maxScratchSize, scratchBuffer, scratchMemory);

    VkQueryPoolCreateInfo queryPoolInfo{};
    queryPoolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    queryPoolInfo.queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
    queryPoolInfo.queryCount = static_cast<uint32_t>(count);
    VkQueryPool queryPool;
    if (vkCreateQueryPool(device, &queryPoolInfo, nullptr, &queryPool) != VK_SUCCESS) {
        throw std::runtime_error("failed to create acceleration structure query pool!");
    }

    // 2: build all, then ask for their compacted sizes
    std::vector<VkAccelerationStructureKHR> handles(count);
    for (size_t i = 0; i < count; i++) {
        handles[i] = uncompacted[i].handle;
    }
    immediateSubmit([&](VkCommandBuffer cmd) {
        vkCmdResetQueryPool(cmd, queryPool, 0, static_cast<uint32_t>(count));

        // Between builds: the next build reuses the scratch memory, so the previous one must be done with it.
        // After the last one this also makes the results visible to the size query.
        VkMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;

        for (size_t i = 0; i < count; i++) {
            buildInfos[i].scratchData.deviceAddress = scratchAddress;
            const VkAccelerationStructureBuildRangeInfoKHR* pRange = &ranges[i];
            vkext::vkCmdBuildAccelerationStructuresKHR(cmd, 1, &buildInfos[i], &pRange);
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                                 VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                                 0, 1, &barrier, 0, nullptr, 0, nullptr);
        }
        vkext::vkCmdWriteAccelerationStructuresPropertiesKHR(cmd, static_cast<uint32_t>(count), handles.data(),
                                                             VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR,
                                                             queryPool, 0);
    });

    std::vector<VkDeviceSize> compactedSizes(count);
    vkGetQueryPoolResults(device, queryPool, 0, static_cast<uint32_t>(count), sizeof(VkDeviceSize) * count,
                          compactedSizes.data(), sizeof(VkDeviceSize), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    vkDestroyQueryPool(device, queryPool, nullptr);
    vkDestroyBuffer(device, scratchBuffer, nullptr);
    vkFreeMemory(device, scratchMemory, nullptr);

    // 3: copy into right-sized structures
    for (size_t i = 0; i < count; i++) {
        gpuAssets.blases[meshes[i]] = createAccelerationStructure(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, compactedSizes[i]);
        gpuAssets.blasSizes[meshes[i]] = compactedSizes[i];
    }
    immediateSubmit([&](VkCommandBuffer cmd) {
        for (size_t i = 0; i < count; i++) {
            VkCopyAccelerationStructureInfoKHR copyInfo{};
            copyInfo.sType = VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR;
            copyInfo.src = uncompacted[i].handle;
            copyInfo.dst = gpuAssets.blases[meshes[i]].handle;
            copyInfo.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
            vkext::vkCmdCopyAccelerationStructureKHR(cmd, &copyInfo);
        }
    });

    for (AccelerationStructure& as : uncompacted) {
        destroyAccelerationStructure(as);
    }
}

// Top level: placed copies (instances) of bottom level structures, each with a transform.
// Both describing calls (size query and build) must use the same geometry type and flags.
static void describeTopLevel(VkDeviceAddress instanceData, VkAccelerationStructureGeometryKHR& geometry,
                             VkAccelerationStructureBuildGeometryInfoKHR& buildInfo) {
    geometry = {};
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    geometry.geometry.instances.arrayOfPointers = VK_FALSE;
    geometry.geometry.instances.data.deviceAddress = instanceData;

    buildInfo = {};
    buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries = &geometry;
}

// Creates an (unbuilt) TLAS with room for `capacity` instances, plus everything needed to rebuild it each frame.
void Renderer::createTopLevelAccelerationStructure(uint32_t capacity) {
    tlasCapacity = capacity;

    // Per instance: last frame's position from this frame's (motion vectors) and its matrices, uploaded when they change
    createStreamedBuffer(instanceData, sizeof(GpuInstance) * capacity);
    instanceUploadPending = true;  // the new buffer is empty

    // Instance buffers stay mapped: the CPU writes the instances right before the build is recorded.
    instanceBuffers.resize(Configuration::maxFramesInFlight);
    instanceBufferMemories.resize(Configuration::maxFramesInFlight);
    instanceBuffersMapped.resize(Configuration::maxFramesInFlight);
    VkDeviceSize instanceBufferSize = sizeof(VkAccelerationStructureInstanceKHR) * capacity;
    for (int i = 0; i < Configuration::maxFramesInFlight; i++) {
        createBuffer(instanceBufferSize,
                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     instanceBuffers[i], instanceBufferMemories[i]);
        vkMapMemory(device, instanceBufferMemories[i], 0, instanceBufferSize, 0, &instanceBuffersMapped[i]);
    }

    // Sizes for the worst case (full capacity). Builds with fewer instances fit as well.
    VkAccelerationStructureGeometryKHR geometry;
    VkAccelerationStructureBuildGeometryInfoKHR buildInfo;
    describeTopLevel(0, geometry, buildInfo);  // addresses are ignored when only asking for sizes
    VkAccelerationStructureBuildSizesInfoKHR sizeInfo{};
    sizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    vkext::vkGetAccelerationStructureBuildSizesKHR(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                                   &buildInfo, &capacity, &sizeInfo);

    tlas = createAccelerationStructure(VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, sizeInfo.accelerationStructureSize);
    tlasScratchAddress = createScratchBuffer(sizeInfo.buildScratchSize, tlasScratchBuffer, tlasScratchMemory);
    tlasDirty = true;  // nothing built yet
}

void Renderer::destroyTopLevelAccelerationStructure() {
    destroyAccelerationStructure(tlas);
    destroyStreamedBuffer(instanceData);
    vkDestroyBuffer(device, tlasScratchBuffer, nullptr);
    vkFreeMemory(device, tlasScratchMemory, nullptr);
    for (size_t i = 0; i < instanceBuffers.size(); i++) {
        vkUnmapMemory(device, instanceBufferMemories[i]);
        vkDestroyBuffer(device, instanceBuffers[i], nullptr);
        vkFreeMemory(device, instanceBufferMemories[i], nullptr);
    }
    instanceBuffers.clear();
    instanceBufferMemories.clear();
    instanceBuffersMapped.clear();
    tlasCapacity = 0;
}

// Rebuilds the TLAS from frameInstances, on the GPU, before this frame's ray tracing.
// Rebuilding from scratch every time something moves is fine: a TLAS over a few hundred instances
// builds in microseconds. (Big scenes would use an "update" (refit) instead.)
void Renderer::recordTopLevelBuild(VkCommandBuffer commandBuffer) {
    uint32_t instanceCount = static_cast<uint32_t>(frameInstances.size());
    std::memcpy(instanceBuffersMapped[currentFrame], frameInstances.data(),
                sizeof(VkAccelerationStructureInstanceKHR) * instanceCount);

    VkAccelerationStructureGeometryKHR geometry;
    VkAccelerationStructureBuildGeometryInfoKHR buildInfo;
    describeTopLevel(getBufferDeviceAddress(instanceBuffers[currentFrame]), geometry, buildInfo);
    buildInfo.dstAccelerationStructure = tlas.handle;
    buildInfo.scratchData.deviceAddress = tlasScratchAddress;

    // The previous frame may still be tracing rays through the TLAS (compute shader) or building it
    // (scratch memory). Both must finish before we overwrite them.
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    vkCmdPipelineBarrier(commandBuffer,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);

    VkAccelerationStructureBuildRangeInfoKHR rangeInfo{};
    rangeInfo.primitiveCount = instanceCount;
    const VkAccelerationStructureBuildRangeInfoKHR* pRangeInfo = &rangeInfo;
    vkext::vkCmdBuildAccelerationStructuresKHR(commandBuffer, 1, &buildInfo, &pRangeInfo);

    // This frame's rays must wait for the finished TLAS.
    barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);

    builtInstances = frameInstances;
    tlasDirty = false;
    resetAccumulation = true;  // the scene changed, the accumulated image is outdated
}

// The images the trace shader writes, at render size (the display image is in Display.cpp)
void Renderer::createRayTracingImages() {
    // Everything DLSS reads is also "sampled": DLSS switches the images to SHADER_READ_ONLY_OPTIMAL while reading.
    const VkImageUsageFlags dlssInput = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    createImage(renderExtent.width, renderExtent.height, radianceFormat, dlssInput,
                radianceImage, radianceImageMemory, radianceImageView);
    // The DLSS inputs. The diffuse albedo is copied into the history image for the reprojection error view.
    createImage(renderExtent, linearDepthFormat, dlssInput, linearDepthImage);
    createImage(renderExtent, motionVectorFormat, dlssInput, motionVectorImage);
    createImage(renderExtent, guideColorFormat, dlssInput | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, diffuseAlbedoImage);
    createImage(renderExtent, guideColorFormat, dlssInput, specularAlbedoImage);
    createImage(renderExtent, guideColorFormat, dlssInput, normalRoughnessImage);
    createImage(renderExtent, hitDistanceFormat, dlssInput, specularHitDistanceImage);
    createImage(renderExtent, guideColorFormat, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                guideHistoryImage);
    // Written before it is read in every frame that uses it, so it needs no clearing
    createBuffer(sizeof(GpuPrimaryHit) * renderExtent.width * renderExtent.height, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, primaryHitBuffer, primaryHitMemory);
    VkDeviceSize pixels = static_cast<VkDeviceSize>(renderExtent.width) * renderExtent.height;
    createBuffer(sizeof(GpuBounceHit) * pixels, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                 bounceHitBuffer, bounceHitMemory);
    // this frame's, last frame's, and the ReSTIR pass's result for the spatial pass
    createBuffer(sizeof(GpuReservoir) * 3 * pixels, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, reservoirBuffer, reservoirMemory);
    createBuffer(sizeof(GpuRestirSurface) * 2 * pixels, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, restirSurfaceBuffer, restirSurfaceMemory);
    // ReSTIR GI: this frame's, last frame's, and the image pass's fresh samples
    createBuffer(sizeof(GpuGiReservoir) * 3 * pixels, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, giReservoirBuffer, giReservoirMemory);
    restirHistoryAvailable = false;  // new buffers: nothing to reuse yet
    restirGiHistoryAvailable = false;

    // All stay in GENERAL forever
    immediateSubmit([&](VkCommandBuffer cmd) {
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
            barriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        }
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
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

void Renderer::destroyRayTracingImages() {
    for (RenderImage* image : {&linearDepthImage, &motionVectorImage, &diffuseAlbedoImage, &specularAlbedoImage,
                               &normalRoughnessImage, &specularHitDistanceImage, &guideHistoryImage}) {
        destroyImage(*image);
    }
    vkDestroyImageView(device, radianceImageView, nullptr);
    vkDestroyImage(device, radianceImage, nullptr);
    vkFreeMemory(device, radianceImageMemory, nullptr);
    vkDestroyBuffer(device, primaryHitBuffer, nullptr);
    vkFreeMemory(device, primaryHitMemory, nullptr);
    vkDestroyBuffer(device, bounceHitBuffer, nullptr);
    vkFreeMemory(device, bounceHitMemory, nullptr);
    vkDestroyBuffer(device, reservoirBuffer, nullptr);
    vkFreeMemory(device, reservoirMemory, nullptr);
    vkDestroyBuffer(device, restirSurfaceBuffer, nullptr);
    vkFreeMemory(device, restirSurfaceMemory, nullptr);
    vkDestroyBuffer(device, giReservoirBuffer, nullptr);
    vkFreeMemory(device, giReservoirMemory, nullptr);
}

void Renderer::createRayTracingDescriptors() {
    // 0: TLAS, 1: radiance (HDR output), 2: linear depth, 3: scene uniforms, 4: vertices, 5: indices,
    // 6: mesh infos, 7: all textures, 8: materials, 9: emissive triangles, 10: first light per instance,
    // 11: specular albedo table, 12: per-instance motion and matrices, 13-17: motion vectors, diffuse albedo,
    // specular albedo, normal + roughness, specular hit distance, 18-20: radiance cache keys, accumulators, averages,
    // 21: environment map, 22: environment sampling distribution, 23: punctual lights, 24: light tree,
    // 25: primary hits, 26: ReSTIR reservoirs, 27: ReSTIR surfaces, 28: first bounces traced by the ReSTIR pass,
    // 29: ReSTIR GI reservoirs, 30: blue noise
    std::vector<VkDescriptorSetLayoutBinding> bindings(32);
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

    // The texture array is "partially bound": only the first gpuAssets.textures.size() entries are filled in.
    // That's allowed as long as the shader never reads the empty ones.
    std::vector<VkDescriptorBindingFlags> bindingFlags(bindings.size(), 0);
    bindingFlags[7] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsInfo{};
    bindingFlagsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    bindingFlagsInfo.bindingCount = static_cast<uint32_t>(bindingFlags.size());
    bindingFlagsInfo.pBindingFlags = bindingFlags.data();

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    layoutInfo.pNext = &bindingFlagsInfo;
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &rtDescriptorSetLayout) != VK_SUCCESS) {
        throw std::runtime_error("failed to create ray tracing descriptor set layout!");
    }

    VkDescriptorPoolSize poolSizes[] = {
            {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 7},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 21},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, maxTextures + 1}
    };
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = (uint32_t)std::size(poolSizes);
    poolInfo.pPoolSizes = poolSizes;
    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &rtDescriptorPool) != VK_SUCCESS) {
        throw std::runtime_error("failed to create ray tracing descriptor pool!");
    }

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = rtDescriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &rtDescriptorSetLayout;
    if (vkAllocateDescriptorSets(device, &allocInfo, &rtDescriptorSet) != VK_SUCCESS) {
        throw std::runtime_error("failed to allocate ray tracing descriptor set!");
    }

    updateRayTracingDescriptorSet();
}

// Called again after a swapchain recreation (new output images) and after loading a scene (new TLAS and buffers).
void Renderer::updateRayTracingDescriptorSet() {
    VkWriteDescriptorSetAccelerationStructureKHR asInfo{};
    asInfo.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    asInfo.accelerationStructureCount = 1;
    asInfo.pAccelerationStructures = &tlas.handle;

    VkDescriptorImageInfo radianceInfo{VK_NULL_HANDLE, radianceImageView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo depthInfo{VK_NULL_HANDLE, linearDepthImage.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo uniformInfo{uniformBuffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo vertexInfo{gpuAssets.vertexBuffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo indexInfo{gpuAssets.indexBuffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo meshInfo{gpuAssets.meshInfoBuffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo materialInfo{gpuAssets.materialBuffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo emissiveTriangleInfo{emissiveTriangleBuffer.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo instanceLightInfo{instanceLightBuffer.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo specularAlbedoInfo{specularAlbedoBuffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo instanceDataInfo{instanceData.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo guideInfos[5] = {
            {VK_NULL_HANDLE, motionVectorImage.view, VK_IMAGE_LAYOUT_GENERAL},
            {VK_NULL_HANDLE, diffuseAlbedoImage.view, VK_IMAGE_LAYOUT_GENERAL},
            {VK_NULL_HANDLE, specularAlbedoImage.view, VK_IMAGE_LAYOUT_GENERAL},
            {VK_NULL_HANDLE, normalRoughnessImage.view, VK_IMAGE_LAYOUT_GENERAL},
            {VK_NULL_HANDLE, specularHitDistanceImage.view, VK_IMAGE_LAYOUT_GENERAL},
    };
    VkDescriptorBufferInfo cacheInfos[3] = {
            {cacheKeyBuffer, 0, VK_WHOLE_SIZE},
            {cacheAccumulatorBuffer, 0, VK_WHOLE_SIZE},
            {cacheValueBuffer, 0, VK_WHOLE_SIZE},
    };
    VkDescriptorImageInfo environmentInfo{environmentSampler, environmentTexture.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo environmentDistributionInfo{environmentDistributionBuffer.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo punctualLightInfo{punctualLightBuffer.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo lightTreeInfo{lightTreeBuffer.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo primaryHitInfo{primaryHitBuffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo reservoirInfo{reservoirBuffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo restirSurfaceInfo{restirSurfaceBuffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo bounceHitInfo{bounceHitBuffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo giReservoirInfo{giReservoirBuffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo blueNoiseInfo{blueNoiseBuffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo portalInfo{portalBuffer.buffer, 0, VK_WHOLE_SIZE};
    std::vector<VkDescriptorImageInfo> textureInfos;
    for (const GpuTexture& texture : gpuAssets.textures) {
        textureInfos.push_back({texture.sampler, texture.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    }

    std::vector<VkWriteDescriptorSet> writes(32);
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
    if (textureInfos.empty()) {
        writes.erase(writes.begin() + 7);  // nothing to write yet; a write with 0 descriptors isn't allowed
    }

    vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

void Renderer::createRayTracingPipeline() {
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &rtDescriptorSetLayout;
    if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &rtPipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("failed to create ray tracing pipeline layout!");
    }

    VkShaderModule shaderModule = createShaderModule(readFile("raytrace.comp.spv"));

    // Seven pipelines from the one shader: the image (both constants 0), the radiance cache's update paths
    // (CACHE_UPDATE_PASS = 1), ReSTIR's camera rays and DI candidates (RESTIR_PASS = 1), DI's spatial reuse
    // (RESTIR_PASS = 2), GI's shading (RESTIR_PASS = 3), temporal (RESTIR_PASS = 4) and spatial reuse
    // (RESTIR_PASS = 5). The constants let the compiler drop the other passes' code from each.
    VkPipeline* pipelines[] = {&rtPipeline, &rtCacheUpdatePipeline, &rtRestirPipeline, &rtRestirSpatialPipeline,
                               &rtRestirGiShadePipeline, &rtRestirGiTemporalPipeline, &rtRestirGiSpatialPipeline};
    for (uint32_t pass = 0; pass < std::size(pipelines); pass++) {
        uint32_t constants[2] = {pass == 1 ? 1u : 0u, pass >= 2 ? pass - 1 : 0u};
        VkSpecializationMapEntry entries[2] = {{0, 0, sizeof(uint32_t)}, {1, sizeof(uint32_t), sizeof(uint32_t)}};
        VkSpecializationInfo specialization{2, entries, sizeof(constants), constants};
        VkComputePipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = shaderModule;
        pipelineInfo.stage.pName = "main";
        pipelineInfo.stage.pSpecializationInfo = &specialization;
        pipelineInfo.layout = rtPipelineLayout;
        if (pipelineStatisticsSupported) {
            pipelineInfo.flags = VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
        }
        if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, pipelines[pass]) != VK_SUCCESS) {
            throw std::runtime_error("failed to create ray tracing pipeline!");
        }
    }

    vkDestroyShaderModule(device, shaderModule, nullptr);

    if (pipelineStatisticsSupported) {
        rtPipelineStatistics = "Image paths:\n" + readPipelineStatistics(rtPipeline) +
                               "Cache update paths:\n" + readPipelineStatistics(rtCacheUpdatePipeline) +
                               "ReSTIR candidates:\n" + readPipelineStatistics(rtRestirPipeline) +
                               "ReSTIR spatial reuse:\n" + readPipelineStatistics(rtRestirSpatialPipeline) +
                               "ReSTIR GI shading:\n" + readPipelineStatistics(rtRestirGiShadePipeline) +
                               "ReSTIR GI temporal reuse:\n" + readPipelineStatistics(rtRestirGiTemporalPipeline) +
                               "ReSTIR GI spatial reuse:\n" + readPipelineStatistics(rtRestirGiSpatialPipeline);
        std::cout << "Trace shader statistics (from the driver):\n" << rtPipelineStatistics << std::flush;
    }
}

// The driver's statistics about a compiled pipeline, one "name: value" line each (on NVIDIA e.g. the registers per
// thread, which limit how many threads an SM can run at once: fewer threads hide the waits for rays and memory worse).
std::string Renderer::readPipelineStatistics(VkPipeline pipeline) {
    if (vkext::vkGetPipelineExecutablePropertiesKHR == nullptr || vkext::vkGetPipelineExecutableStatisticsKHR == nullptr) {
        return "";
    }
    VkPipelineInfoKHR pipelineInfo{VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR, nullptr, pipeline};
    uint32_t executableCount = 0;
    vkext::vkGetPipelineExecutablePropertiesKHR(device, &pipelineInfo, &executableCount, nullptr);
    std::string text;
    for (uint32_t executable = 0; executable < executableCount; executable++) {
        VkPipelineExecutableInfoKHR executableInfo{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR, nullptr, pipeline, executable};
        uint32_t statisticCount = 0;
        vkext::vkGetPipelineExecutableStatisticsKHR(device, &executableInfo, &statisticCount, nullptr);
        std::vector<VkPipelineExecutableStatisticKHR> statistics(statisticCount, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR});
        vkext::vkGetPipelineExecutableStatisticsKHR(device, &executableInfo, &statisticCount, statistics.data());
        for (const VkPipelineExecutableStatisticKHR& statistic : statistics) {
            std::string value;
            switch (statistic.format) {
                case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR: value = statistic.value.b32 ? "yes" : "no"; break;
                case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR: value = std::to_string(statistic.value.i64); break;
                case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR: value = std::to_string(statistic.value.u64); break;
                case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR: value = std::to_string(statistic.value.f64); break;
                default: value = "?"; break;
            }
            text += std::string("  ") + statistic.name + ": " + value + "\n";
        }
    }
    return text;
}

SceneUniforms Renderer::buildSceneUniforms() {
    const Camera& camera = renderList->camera;
    const SceneEnvironment& environment = renderList->environment;
    float azimuth = glm::radians(environment.sunAzimuth);
    float elevation = glm::radians(environment.sunElevation);
    glm::vec3 towardsSun = {std::cos(elevation) * std::sin(azimuth),
                            std::sin(elevation),
                            std::cos(elevation) * std::cos(azimuth)};

    SceneUniforms uniforms{};  // zero-initialized, so padding compares equal in memcmp below
    float aspectRatio = (float) renderExtent.width / (float) renderExtent.height;
    frameView = camera.viewMatrix();
    frameProjection = camera.projectionMatrix(aspectRatio, Configuration::cameraNear, Configuration::cameraFar);
    frameViewProjection = frameProjection * frameView;
    framePreviousViewProjection = hasPreviousViewProjection ? previousViewProjection : frameViewProjection;
    uniforms.viewProjection = frameViewProjection;
    uniforms.previousViewProjection = framePreviousViewProjection;
    previousViewProjection = frameViewProjection;
    hasPreviousViewProjection = true;
    uniforms.cameraPosition = glm::vec4(camera.position, 1.0f);
    uniforms.cameraForward = glm::vec4(camera.forward(), 0.0f);
    uniforms.cameraRight = glm::vec4(camera.right(), 0.0f);
    uniforms.cameraUp = glm::vec4(camera.up(), 0.0f);
    uniforms.jitter = glm::vec2(0.0f);
    uniforms.tanHalfFov = std::tan(glm::radians(camera.fov) * 0.5f);
    uniforms.aspectRatio = aspectRatio;
    uniforms.sunDirection = glm::vec4(towardsSun, std::cos(glm::radians(environment.sunAngularRadius)));
    // Sun off = sun color 0 (the shader then skips its shadow rays). Black environment = all sky colors 0.
    // An HDR map replaces the gradient (whose colors are then 0) once it is loaded (see updateEnvironment).
    float sun = environment.sunEnabled ? environment.sunIntensity : 0.0f;
    float sky = environment.background == static_cast<int>(Background::Sky) ? environment.skyIntensity : 0.0f;
    uniforms.sunColor = glm::vec4(environment.sunColor * sun, 0.0f);
    // The disc: its total light PI * sunColor (see tracePath) spread evenly over its solid angle. A disc of size 0
    // can only be found by shadow rays.
    float sunSolidAngle = 2.0f * glm::pi<float>() * (1.0f - uniforms.sunDirection.w);
    uniforms.sunOn = glm::any(glm::greaterThan(glm::vec3(uniforms.sunColor), glm::vec3(0.0f))) ? 1 : 0;
    uniforms.sunMis = uniforms.sunOn && sunSolidAngle > 1e-6f && settings.shadowsEnabled ? 1 : 0;
    uniforms.sunPdf = 1.0f / std::max(sunSolidAngle, 1e-6f);
    uniforms.sunRadianceScale = glm::pi<float>() / std::max(sunSolidAngle, 1e-4f);
    uniforms.skyZenithColor = glm::vec4(environment.skyZenithColor * sky, 0.0f);
    uniforms.skyHorizonColor = glm::vec4(environment.skyHorizonColor * sky, 0.0f);
    uniforms.groundColor = glm::vec4(environment.groundColor * sky, 0.0f);
    if (environment.background == static_cast<int>(Background::White)) {
        uniforms.skyZenithColor = uniforms.skyHorizonColor = uniforms.groundColor = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);
    }
    uniforms.environmentMap = environmentMapReady() ? 1 : 0;
    uniforms.environmentSampling = environmentCells.x > 0 ? 1 : 0;
    uniforms.environmentWidth = environmentCells.x;
    uniforms.environmentHeight = environmentCells.y;
    uniforms.environmentRotation = glm::radians(environment.mapRotation);
    uniforms.environmentIntensity = environment.mapIntensity;
    uniforms.punctualLightCount = static_cast<uint32_t>(uploadedPunctualLights.size());
    uniforms.portalCount = static_cast<uint32_t>(uploadedPortals.size());
    uniforms.samplesPerPixel = static_cast<uint32_t>(std::max(settings.samplesPerPixel, 1));
    uniforms.maxBounces = static_cast<uint32_t>(std::max(settings.maxBounces, 1));
    uniforms.shadowsEnabled = settings.shadowsEnabled ? 1 : 0;
    uniforms.emissiveTriangleCount = static_cast<uint32_t>(emissiveTriangles.size());
    uniforms.emissiveSampling = static_cast<uint32_t>(settings.emissiveSampling);
    uniforms.lightTree = settings.lightPicking == static_cast<int>(LightPicking::Tree) && !lightTree.empty() ? 1 : 0;
    uniforms.blurCaustics = settings.blurCaustics ? 1 : 0;
    uniforms.textureMipmaps = settings.textureMipmaps ? 1 : 0;
    uniforms.normalMapFiltering = settings.normalMapFiltering ? 1 : 0;
    // Hit debug views overwrite the radiance image. Guide views are drawn by the display pass from the guide
    // images, while the trace shader path traces as usual.
    uniforms.debugView = isGuideView(settings.debugView) ? 0 : static_cast<uint32_t>(settings.debugView);
    // ReSTIR DI: candidates of each kind of light there is. Debug views trace their own camera ray (except the one
    // showing ReSTIR's reservoirs).
    bool restirView = uniforms.debugView == static_cast<uint32_t>(DebugView::RestirM) ||
                      uniforms.debugView == static_cast<uint32_t>(DebugView::RestirLight);
    uniforms.restirDi = settings.restirDi(settings.renderMode) && (uniforms.debugView == 0 || restirView) ? 1 : 0;
    bool sampleTriangles = !emissiveTriangles.empty() && settings.emissiveSampling != static_cast<int>(EmissiveSampling::BouncesOnly);
    uint32_t lightCandidates = static_cast<uint32_t>(std::max(settings.restirLightCandidates, 1));
    uniforms.restirTriangleCandidates = sampleTriangles ? lightCandidates : 0;
    // A few punctual lights keep a shadow ray each (exact, MAX_PUNCTUAL_LIGHTS_PER_HIT in the shader); ReSTIR is
    // for many
    uniforms.restirPunctualCandidates = uniforms.punctualLightCount > 8 ? std::min(lightCandidates, uniforms.punctualLightCount) : 0;
    // At least one whenever the sky is light-sampled: the bounce ray of a ReSTIR surface leaves the sky to it
    uniforms.restirSkyCandidates = uniforms.environmentSampling ? static_cast<uint32_t>(std::max(settings.restirSkyCandidates, 1)) : 0;
    uniforms.restirBrdfCandidates = static_cast<uint32_t>(std::max(settings.restirBrdfCandidates, 0));
    uniforms.restirMinRoughness = settings.restirMinRoughness;
    uniforms.restirTemporal = settings.restirTemporal ? (settings.restirTemporalVisibility ? 2 : 1) : 0;
    uniforms.restirMaxHistory = settings.restirMaxHistory;
    bool spatial = settings.restirSpatial && uniforms.restirDi != 0;
    uniforms.restirSpatialNeighbors = spatial ? static_cast<uint32_t>(std::max(settings.restirSpatialNeighbors, 0)) : 0;
    uniforms.restirSpatialRadius = settings.restirSpatialRadius;
    uniforms.restirSpatialVisibility = settings.restirSpatialVisibility ? 1 : 0;
    uniforms.restirDisocclusionNeighbors = spatial ? static_cast<uint32_t>(std::max(settings.restirDisocclusionNeighbors, 0)) : 0;
    // ReSTIR GI: independent of DI; the ReSTIR pass runs for either (its camera rays and surfaces)
    uniforms.restirGi = settings.restirGi(settings.renderMode) && uniforms.debugView == 0 ? 1 : 0;
    uniforms.restirGiMinRoughness = settings.restirGiMinRoughness;
    uniforms.restirGiTemporal = settings.restirGiTemporal ? 1 : 0;
    uniforms.restirGiMaxHistory = settings.restirGiMaxHistory;
    uniforms.restirGiMaxAge = static_cast<uint32_t>(std::max(settings.restirGiMaxAge, 1));
    uniforms.restirGiPermutation = settings.restirGiPermutation ? 1 : 0;
    uniforms.restirGiTemporalVisibility = settings.restirGiTemporalVisibility ? 1 : 0;
    uniforms.pathSampling = static_cast<uint32_t>(std::clamp(settings.pathSampling, 0, 2));
    uniforms.emissiveSolidAngle = settings.emissiveSolidAngle ? 1 : 0;
    bool giSpatial = settings.restirGiSpatial && uniforms.restirGi != 0;
    uniforms.restirGiSpatialNeighbors = giSpatial ? static_cast<uint32_t>(std::max(settings.restirGiSpatialNeighbors, 0)) : 0;
    uniforms.restirGiSpatialRadius = settings.restirGiSpatialRadius;
    uniforms.restirGiDisocclusionNeighbors = giSpatial ? static_cast<uint32_t>(std::max(settings.restirGiDisocclusionNeighbors, 0)) : 0;
    uniforms.restirGiSpatialVisibility = settings.restirGiSpatialVisibility ? 1 : 0;
    uniforms.restirGiBoilingFilter = settings.restirGiBoilingFilter ? std::max(settings.restirGiBoilingStrength, 1e-3f) : 0.0f;
    // How much of the scene one ray stands for (the start of its ray cone, see uvFootprint in the shader): half an
    // output pixel, vertically (pixels are square). The image ends up sharper than one render pixel: accumulation
    // averages many offsets within each pixel, and DLSS adds detail up to the output resolution (NVIDIA's guide
    // asks for a mip bias of log2(render / output size) - 1, which is this). A whole render pixel looks soft.
    uniforms.pixelSpreadAngle = 0.5f * std::atan(2.0f * uniforms.tanHalfFov / static_cast<float>(outputExtent.height));
    // DLSS mode: every frame on its own (DLSS does the averaging over time), plus the guides it needs
    uniforms.accumulate = settings.accumulate && !dlssActive() ? 1 : 0;
    uniforms.farDistance = Configuration::cameraFar;
    uniforms.writeGuides = dlssActive() || isGuideView(settings.debugView) ? 1 : 0;
    uniforms.cacheQuery = settings.radianceCache && !cacheQueriesSuspended ? 1 : 0;
    uniforms.cacheCapacity = cacheCapacity;
    // A cell about cacheCellPixels render pixels wide at distance 1, growing with the distance. Pixels as rendered
    // for the window's size: a bigger output (a 4K capture) keeps the same cells, else it would need many more.
    float windowRenderRows = static_cast<float>(renderExtent.height) * static_cast<float>(swapChainExtent.height)
                             / static_cast<float>(std::max(outputExtent.height, 1u));
    uniforms.cacheCellScale = 2.0f * uniforms.tanHalfFov / std::max(windowRenderRows, 1.0f) * settings.cacheCellPixels;
    uniforms.cacheMinWeight = settings.cacheMinSamples;
    uniforms.cacheUpdateTile = static_cast<uint32_t>(std::max(settings.cacheUpdateTile, 1));

    // The cache holds light that is no longer right after the lighting or the cells change: start it over.
    // (Moving objects and edited materials are followed within its history instead; see cacheHistoryFrames.)
    bool lightingChanged =
            std::memcmp(&uniforms.sunDirection, &previousUniforms.sunDirection,
                        offsetof(SceneUniforms, samplesPerPixel) - offsetof(SceneUniforms, sunDirection)) != 0 ||
            uniforms.maxBounces != previousUniforms.maxBounces || uniforms.shadowsEnabled != previousUniforms.shadowsEnabled ||
            uniforms.emissiveSampling != previousUniforms.emissiveSampling ||
            uniforms.blurCaustics != previousUniforms.blurCaustics ||
            uniforms.cacheCellScale != previousUniforms.cacheCellScale;
    if (lightingChanged) {
        cacheClearPending = true;
    }

    // Anything that changes the image (camera moved, slider dragged, ...) restarts accumulation.
    // frameIndex and accumulatedFrames change every frame by design, so those are filled in after comparing.
    // (Exposure and tone mapping aren't in here: they only affect the display pass, see recordDisplay.)
    if (resetAccumulation || std::memcmp(&uniforms, &previousUniforms, sizeof(SceneUniforms)) != 0) {
        accumulatedFrames = 0;
        accumulationRestarts++;
        resetAccumulation = false;
    }
    previousUniforms = uniforms;

    uniforms.frameIndex = frameIndex;
    uniforms.accumulatedFrames = accumulatedFrames;
    // ReSTIR history (after the comparison above: these change every frame). Last frame's reservoirs are usable if
    // ReSTIR ran last frame, at this size (createRayTracingImages), and no light was added, removed or moved: their
    // samples name lights by index in lists that are then rebuilt.
    uniforms.restirHistoryValid = restirHistoryAvailable && !restirLightsChanged ? 1 : 0;
    uniforms.restirCurrentHalf = restirCurrentHalf;
    // Shuffle of last frame's pixels (see restirTemporal): on (bit 4) and this frame's random offset (bits 0-3)
    uint32_t permutationHash = frameIndex * 0x9E3779B9u;
    permutationHash = (permutationHash ^ (permutationHash >> 16)) * 0x85EBCA6Bu;
    permutationHash ^= permutationHash >> 13;
    uniforms.restirPermutation = (settings.restirPermutation ? 16u : 0u) | (permutationHash & 15u);
    // GI samples carry the light they saw (Lo): after the lighting changed (as for the radiance cache, above) it is
    // out of date, so drop them, as when lights are added or removed. Lights that move rely on the age limit.
    uniforms.restirGiHistoryValid = restirGiHistoryAvailable && !restirLightsChanged && !lightingChanged ? 1 : 0;
    uniforms.previousCameraPosition = glm::vec4(previousCameraPosition, 1.0f);
    // Bounce light clamp (DLSS modes, see clampBounceLight in the shader). The limit is set on screen, so it is
    // divided by the exposure: the GPU's from a few frames ago (exposureInfo), or the manual one. After the
    // comparison above: it changes every frame with the auto exposure.
    float shownExposure = settings.autoExposure
                          ? (exposureInfoValid ? exposureInfo.exposure * std::exp2(settings.exposureCompensation) : 0.0f)
                          : settings.exposure;
    uniforms.bounceLightClamp = settings.clampBounceLight && dlssActive() && shownExposure > 0.0f
                                ? settings.bounceLightClampLevel / shownExposure : 0.0f;
    // ReSTIR GI's Lo clamp, the same way (any mode)
    uniforms.restirGiRadianceClamp = settings.restirGiClampRadiance && shownExposure > 0.0f
                                     ? settings.restirGiRadianceClampLevel / shownExposure : 0.0f;
    bool restirPass = uniforms.restirDi != 0 || uniforms.restirGi != 0;
    if (restirPass) {
        restirCurrentHalf = 1 - restirCurrentHalf;  // this frame's half is last frame's half next frame
    }
    restirHistoryAvailable = restirPass;
    restirGiHistoryAvailable = uniforms.restirGi != 0;
    previousCameraPosition = renderList->camera.position;
    // Jitter (DLSS mode): a different sub-pixel offset of the whole image every frame, so DLSS sees every part of
    // each pixel over a few frames. In pixels, -0.5..0.5. DLSS SR's guide asks for 8 phases per output pixel (more
    // when each render pixel covers several output pixels); RR's for "at least 32", as it has no reason to limit them.
    frameJitter = glm::vec2(0.0f);
    if (dlssActive()) {
        float scale = static_cast<float>(outputExtent.width) / static_cast<float>(renderExtent.width);
        uint32_t sequenceLength = std::max(32u, static_cast<uint32_t>(std::lround(8.0f * scale * scale)));
        jitterIndex = jitterIndex % sequenceLength + 1;
        frameJitter = glm::vec2(halton(jitterIndex, 2), halton(jitterIndex, 3)) - 0.5f;
    }
    uniforms.jitter = frameJitter;
    if (settings.accumulate && !dlssActive()) {
        accumulatedFrames++;
    }
    return uniforms;
}

// Trace into the radiance image (render size), DLSS (DLSS modes), measure the exposure, tone map into the display
// image (output size), then blit that into the swapchain image. Afterwards the swapchain image is in TRANSFER_DST_OPTIMAL,
// which is what the render pass expects.
void Renderer::recordRayTracing(VkCommandBuffer commandBuffer, uint32_t imageIndex) {
    VkImageSubresourceRange colorRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    readTimestamps();
    vkCmdResetQueryPool(commandBuffer, timestampQueryPool, currentFrame * timestampsPerFrame, timestampsPerFrame);

    // Lights uploaded this frame may have new indices: ReSTIR can't reuse last frame's samples then
    restirLightsChanged = lightsUploadPending || environmentUploadPending || punctualLightsUploadPending;

    // Objects moved (or the scene changed): rebuild the TLAS first. Sets resetAccumulation,
    // so it has to happen before the uniforms are built.
    if (tlasDirty) {
        recordTopLevelBuild(commandBuffer);
    }
    if (gpuAssets.materialsDirty) {
        recordMaterialUpdate(commandBuffer);
    }
    if (lightsUploadPending) {
        recordLightUpload(commandBuffer);
    }
    if (environmentUploadPending) {
        recordEnvironmentUpload(commandBuffer);
    }
    if (punctualLightsUploadPending) {
        recordPunctualLightUpload(commandBuffer);
    }
    if (portalsUploadPending) {
        recordPortalUpload(commandBuffer);
    }

    // 0: upload this frame's uniforms. vkCmdUpdateBuffer copies the data into the command buffer
    //    itself, so a single uniform buffer is enough even with several frames in flight.
    //    It must not overwrite the buffer while the previous frame's shader still reads it.
    VkBufferMemoryBarrier uniformBarrier{};
    uniformBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    uniformBarrier.srcAccessMask = VK_ACCESS_UNIFORM_READ_BIT;
    uniformBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    uniformBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    uniformBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    uniformBarrier.buffer = uniformBuffer;
    uniformBarrier.offset = 0;
    uniformBarrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 1, &uniformBarrier, 0, nullptr);

    SceneUniforms uniforms = buildSceneUniforms();
    vkCmdUpdateBuffer(commandBuffer, uniformBuffer, 0, sizeof(uniforms), &uniforms);
    frameIndex++;
    recordInstanceUpload(commandBuffer);

    uniformBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    uniformBarrier.dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT;

    // 1: make the output images writable. All images the trace shader writes stay in GENERAL; the previous
    //    frame's reads and writes of them (trace, exposure, display, the history copy) must finish first.
    VkMemoryBarrier shaderImagesBarrier{};
    shaderImagesBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    shaderImagesBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    shaderImagesBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

    // Written by the display pass later in this frame. The previous frame may still be blitting from it,
    // so wait for transfers. Its old contents are not needed, hence UNDEFINED.
    VkImageMemoryBarrier preBarriers[1]{};
    preBarriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    preBarriers[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    preBarriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    preBarriers[0].srcAccessMask = 0;
    preBarriers[0].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    preBarriers[0].image = displayImage;

    for (auto& barrier : preBarriers) {
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.subresourceRange = colorRange;
    }
    // (the uniform barrier rides along: uniform upload -> shader read; the motion upload has its own)
    vkCmdPipelineBarrier(commandBuffer,
                         VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &shaderImagesBarrier, 1, &uniformBarrier, 1, preBarriers);

    // 2: radiance cache: update paths fill it, then the new samples are averaged in
    writeTimestamp(commandBuffer, 0);
    recordRadianceCache(commandBuffer);
    writeTimestamp(commandBuffer, 1);

    // 3: ReSTIR: the camera rays first (their hits go to the primary hit buffer), and (DI) a light sample per pixel
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, rtPipelineLayout, 0, 1, &rtDescriptorSet, 0, nullptr);
    if (uniforms.restirDi != 0 || uniforms.restirGi != 0) {
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, rtRestirPipeline);
        vkCmdDispatch(commandBuffer, (renderExtent.width + 7) / 8, (renderExtent.height + 7) / 8, 1);
        VkMemoryBarrier hitsBarrier{};
        hitsBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        hitsBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        hitsBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &hitsBarrier, 0, nullptr, 0, nullptr);
        // Spatial reuse: reads the ReSTIR pass's reservoirs (a part of their own), writes this frame's
        if (uniforms.restirSpatialNeighbors + uniforms.restirDisocclusionNeighbors > 0) {
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, rtRestirSpatialPipeline);
            vkCmdDispatch(commandBuffer, (renderExtent.width + 7) / 8, (renderExtent.height + 7) / 8, 1);
            vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0, 1, &hitsBarrier, 0, nullptr, 0, nullptr);
        }
    }
    writeTimestamp(commandBuffer, 2);

    // 4: trace
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, rtPipeline);
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
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            vkCmdDispatch(commandBuffer, (renderExtent.width + 7) / 8, (renderExtent.height + 7) / 8, 1);
        }
    }
    writeTimestamp(commandBuffer, 3);

    // 5: DLSS mode: denoise + upscale the noisy frame into the DLSS output image
    recordDlss(commandBuffer);
    writeTimestamp(commandBuffer, 4);

    // 6: measure the image for the exposure, then exposure + tone mapping into the display image
    recordExposure(commandBuffer);
    recordNoise(commandBuffer);  // noise measurement, if one is running (not timed: part of "exposure")
    writeTimestamp(commandBuffer, 5);
    recordDisplay(commandBuffer);
    writeTimestamp(commandBuffer, 6);
    timestampsWritten[currentFrame] = true;

    // 7: display image -> blit source, swapchain image -> blit destination.
    //    The swapchain image only becomes available at the TRANSFER stage (see the semaphore wait in drawFrame).
    VkImageMemoryBarrier blitBarriers[2]{};
    blitBarriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    blitBarriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    blitBarriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    blitBarriers[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    blitBarriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    blitBarriers[0].image = displayImage;

    blitBarriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    blitBarriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    blitBarriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    blitBarriers[1].srcAccessMask = 0;
    blitBarriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    blitBarriers[1].image = swapChainImages[imageIndex];

    for (auto& barrier : blitBarriers) {
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.subresourceRange = colorRange;
    }
    vkCmdPipelineBarrier(commandBuffer,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 2, blitBarriers);

    // 8: blit: a copy plus format conversion, or shrunk to fit the window (output bigger than it, filtered)
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {(int32_t) outputExtent.width, (int32_t) outputExtent.height, 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[1] = {(int32_t) swapChainExtent.width, (int32_t) swapChainExtent.height, 1};
    vkCmdBlitImage(commandBuffer,
                   displayImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   swapChainImages[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &blit, outputExtent.width == swapChainExtent.width && outputExtent.height == swapChainExtent.height
                             ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);

    // 9: the "Capture" button: the display image (without the UI) into a buffer for a PNG
    recordCapture(commandBuffer);
}

void Renderer::createTimestampQueries() {
    VkQueryPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = timestampsPerFrame * Configuration::maxFramesInFlight;
    if (vkCreateQueryPool(device, &info, nullptr, &timestampQueryPool) != VK_SUCCESS) {
        throw std::runtime_error("failed to create timestamp query pool!");
    }
    timestampsWritten.assign(Configuration::maxFramesInFlight, false);
}

// The current frame slot's fence has been waited on: the timestamps it wrote last time are available.
void Renderer::readTimestamps() {
    if (!timestampsWritten[currentFrame]) {
        return;
    }
    uint64_t ticks[timestampsPerFrame];
    if (vkGetQueryPoolResults(device, timestampQueryPool, currentFrame * timestampsPerFrame, timestampsPerFrame,
                              sizeof(ticks), ticks, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) {
        return;
    }
    float msPerTick = physicalDeviceProperties.limits.timestampPeriod * 1e-6f;  // the period is in nanoseconds
    for (uint32_t i = 0; i + 1 < timestampsPerFrame; i++) {
        float ms = static_cast<float>(ticks[i + 1] - ticks[i]) * msPerTick;
        lastPassTimes[i] = ms;
        gpuPassTimes[i] = gpuPassTimes[i] == 0.0f ? ms : gpuPassTimes[i] * 0.95f + ms * 0.05f;
    }
}

// Bottom of pipe: the moment everything recorded before has finished
void Renderer::writeTimestamp(VkCommandBuffer commandBuffer, uint32_t index) {
    vkCmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestampQueryPool,
                        currentFrame * timestampsPerFrame + index);
}

// frameGpuInstances -> instanceData, when updateScene remade them (after the previous frame's shader is done reading)
void Renderer::recordInstanceUpload(VkCommandBuffer commandBuffer) {
    if (!instanceUploadPending || frameGpuInstances.empty()) {
        return;
    }
    recordStreamedUpload(commandBuffer, instanceData, frameGpuInstances.data(),
                         sizeof(GpuInstance) * frameGpuInstances.size());
    instanceUploadPending = false;
}

void Renderer::cleanupRayTracing() {
    vkDestroyPipeline(device, rtPipeline, nullptr);
    vkDestroyPipeline(device, rtCacheUpdatePipeline, nullptr);
    vkDestroyPipeline(device, rtRestirPipeline, nullptr);
    vkDestroyPipeline(device, rtRestirSpatialPipeline, nullptr);
    vkDestroyPipeline(device, rtRestirGiShadePipeline, nullptr);
    vkDestroyPipeline(device, rtRestirGiTemporalPipeline, nullptr);
    vkDestroyPipeline(device, rtRestirGiSpatialPipeline, nullptr);
    destroyRadianceCache();
    vkDestroyPipelineLayout(device, rtPipelineLayout, nullptr);
    vkDestroyDescriptorPool(device, rtDescriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(device, rtDescriptorSetLayout, nullptr);

    deferredDeletion.flush();  // the device is idle
    destroyTopLevelAccelerationStructure();
    destroyBottomLevelAccelerationStructures();
    destroySceneGeometry();
    destroyMaterialBuffer();
    destroySpecularAlbedoTable();
    destroyBlueNoiseTexture();
    destroyExposureResources();
    destroyNoiseResources();
    destroyDisplayResources();
    vkDestroyQueryPool(device, timestampQueryPool, nullptr);
    destroyTextures();
    destroyLightBuffers();
    destroyPortalBuffer();
    destroyEnvironment();

    vkDestroyBuffer(device, uniformBuffer, nullptr);
    vkFreeMemory(device, uniformBufferMemory, nullptr);
}
