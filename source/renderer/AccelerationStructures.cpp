//
// Acceleration structures: a BLAS per mesh (built when its model loads, compacted), and the TLAS over all instances,
// built or updated in the frame's command buffer whenever objects move. Part of the GPU scene (GpuScene.h).
//

#include <vulkan/vulkan.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include "renderer/GpuScene.h"
#include "renderer/VulkanExtensions.h"

// An empty acceleration structure of the given size, living inside a buffer we own. Filled by a build or a copy.
AccelerationStructure GpuScene::createAccelerationStructure(VkAccelerationStructureTypeKHR type, VkDeviceSize size,
                                                            VmaPool pool, bool ownMemory) {
    AccelerationStructure as;
    gpu.createBuffer(size,
                     VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, as.buffer, as.memory, pool, ownMemory);

    VkAccelerationStructureCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    createInfo.buffer = as.buffer;
    createInfo.size = size;
    createInfo.type = type;
    if (vkext::vkCreateAccelerationStructureKHR(gpu.device, &createInfo, nullptr, &as.handle) != VK_SUCCESS) {
        throw std::runtime_error("failed to create acceleration structure!");
    }

    VkAccelerationStructureDeviceAddressInfoKHR addressInfo{};
    addressInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    addressInfo.accelerationStructure = as.handle;
    as.address = vkext::vkGetAccelerationStructureDeviceAddressKHR(gpu.device, &addressInfo);
    return as;
}

// Scratch memory is only used during a build. Returns its (aligned) device address.
VkDeviceAddress GpuScene::createScratchBuffer(VkDeviceSize size, VkBuffer& buffer, VmaAllocation& memory, VmaPool pool,
                                              bool ownMemory) {
    VkPhysicalDeviceAccelerationStructurePropertiesKHR asProperties{};
    asProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
    VkPhysicalDeviceProperties2 properties2{};
    properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties2.pNext = &asProperties;
    vkGetPhysicalDeviceProperties2(gpu.physicalDevice, &properties2);
    VkDeviceSize alignment = asProperties.minAccelerationStructureScratchOffsetAlignment;

    gpu.createBuffer(size + alignment,
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, buffer, memory, pool, ownMemory);
    VkDeviceAddress address = gpu.getBufferDeviceAddress(buffer);
    return (address + alignment - 1) / alignment * alignment;
}

void GpuScene::destroyAccelerationStructure(AccelerationStructure& as) {
    vkext::vkDestroyAccelerationStructureKHR(gpu.device, as.handle, nullptr);
    vkDestroyBuffer(gpu.device, as.buffer, nullptr);
    vmaFreeMemory(gpu.allocator, as.memory);
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
void GpuScene::syncBottomLevelAccelerationStructures() {
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

void GpuScene::destroyBottomLevelAccelerationStructures() {
    for (AccelerationStructure& blas : gpuAssets.blases) {
        destroyAccelerationStructure(blas);
    }
    gpuAssets.blases.clear();
    gpuAssets.meshIds.clear();
    gpuAssets.blasBuildSizes.clear();
    gpuAssets.blasSizes.clear();
}

// Builds the BLASes of these meshes (indices into meshInfos), into gpuAssets.blases at the same indices.
//
// In batches of at most blasBatchBytes of uncompacted structures: each batch is built, compacted and its uncompacted
// structures freed before the next one starts. All at once, a big load would need every uncompacted structure next to
// its compacted copy (Jungle Ruins: 729 MB next to 269 MB). Each batch waits for the GPU twice, so a load with many
// batches takes a little longer.
void GpuScene::createBottomLevelAccelerationStructures(const std::vector<uint32_t>& meshes) {
    size_t count = meshes.size();
    if (count == 0) {
        return;
    }
    const VkDeviceSize blasBatchBytes = 256ull << 20;
    VkDeviceAddress vertexAddress = gpu.getBufferDeviceAddress(gpuAssets.vertexBuffer);
    VkDeviceAddress indexAddress = gpu.getBufferDeviceAddress(gpuAssets.indexBuffer);

    std::vector<VkAccelerationStructureGeometryKHR> geometries(count);
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> buildInfos(count);
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges(count);
    VkDeviceSize maxScratchSize = 0;

    // 1: describe every mesh, ask for sizes
    for (size_t i = 0; i < count; i++) {
        const MeshInfo& mesh = gpuAssets.meshInfos[meshes[i]];

        VkAccelerationStructureGeometryTrianglesDataKHR triangles{};
        triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        triangles.vertexData.deviceAddress = vertexAddress + mesh.vertexOffset * sizeof(GpuVertex);  // this mesh's part
        triangles.vertexStride = sizeof(GpuVertex);  // position is the first member, the rest is skipped
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
        vkext::vkGetAccelerationStructureBuildSizesKHR(gpu.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                                       &buildInfos[i], &ranges[i].primitiveCount, &sizeInfo);
        maxScratchSize = std::max(maxScratchSize, sizeInfo.buildScratchSize);
        gpuAssets.blasBuildSizes[meshes[i]] = sizeInfo.accelerationStructureSize;
    }

    // The build's short-lived memory (the uncompacted structures and the scratch buffer) comes from a pool of its own,
    // destroyed at the end. Taken from the shared blocks, it would leave holes between the compacted structures and
    // the textures once freed (Jungle Ruins: ~130 MB).
    VkBufferCreateInfo typicalBuffer{};
    typicalBuffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    typicalBuffer.size = 65536;
    typicalBuffer.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    VmaAllocationCreateInfo typicalAllocation{};
    typicalAllocation.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VmaPoolCreateInfo poolInfo{};  // block size 0: VMA's (64 MB, see createAllocator), big ones get their own memory
    VmaPool buildPool;
    if (vmaFindMemoryTypeIndexForBufferInfo(gpu.allocator, &typicalBuffer, &typicalAllocation, &poolInfo.memoryTypeIndex) !=
        VK_SUCCESS || vmaCreatePool(gpu.allocator, &poolInfo, &buildPool) != VK_SUCCESS) {
        throw std::runtime_error("failed to create the BLAS build memory pool!");
    }

    // The builds run one after another, so they can all share one scratch buffer.
    VkBuffer scratchBuffer;
    VmaAllocation scratchMemory;
    VkDeviceAddress scratchAddress = createScratchBuffer(maxScratchSize, scratchBuffer, scratchMemory, buildPool);

    VkQueryPoolCreateInfo queryPoolInfo{};
    queryPoolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    queryPoolInfo.queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
    queryPoolInfo.queryCount = static_cast<uint32_t>(count);
    VkQueryPool queryPool;
    if (vkCreateQueryPool(gpu.device, &queryPoolInfo, nullptr, &queryPool) != VK_SUCCESS) {
        throw std::runtime_error("failed to create acceleration structure query pool!");
    }

    for (size_t first = 0; first < count;) {
        // This batch: [first, end), at least one mesh (a bigger one gets a batch of its own)
        size_t end = first;
        VkDeviceSize batchBytes = 0;
        while (end < count && (end == first || batchBytes + gpuAssets.blasBuildSizes[meshes[end]] <= blasBatchBytes)) {
            batchBytes += gpuAssets.blasBuildSizes[meshes[end]];
            end++;
        }
        uint32_t batchCount = static_cast<uint32_t>(end - first);

        // 2: create the (oversized) structures, build them, then ask for their compacted sizes
        std::vector<AccelerationStructure> uncompacted(batchCount);
        std::vector<VkAccelerationStructureKHR> handles(batchCount);
        for (uint32_t b = 0; b < batchCount; b++) {
            uncompacted[b] = createAccelerationStructure(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
                                                         gpuAssets.blasBuildSizes[meshes[first + b]], buildPool);
            handles[b] = uncompacted[b].handle;
            buildInfos[first + b].dstAccelerationStructure = uncompacted[b].handle;
        }
        gpu.immediateSubmit([&](VkCommandBuffer cmd) {
            vkCmdResetQueryPool(cmd, queryPool, static_cast<uint32_t>(first), batchCount);

            // Between builds: the next build reuses the scratch memory, so the previous one must be done with it.
            // After the last one this also makes the results visible to the size query.
            VkMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
            barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;

            for (size_t i = first; i < end; i++) {
                buildInfos[i].scratchData.deviceAddress = scratchAddress;
                const VkAccelerationStructureBuildRangeInfoKHR* pRange = &ranges[i];
                vkext::vkCmdBuildAccelerationStructuresKHR(cmd, 1, &buildInfos[i], &pRange);
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                                     VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                                     0, 1, &barrier, 0, nullptr, 0, nullptr);
            }
            vkext::vkCmdWriteAccelerationStructuresPropertiesKHR(cmd, batchCount, handles.data(),
                                                                 VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR,
                                                                 queryPool, static_cast<uint32_t>(first));
        });

        std::vector<VkDeviceSize> compactedSizes(batchCount);
        vkGetQueryPoolResults(gpu.device, queryPool, static_cast<uint32_t>(first), batchCount,
                              sizeof(VkDeviceSize) * batchCount, compactedSizes.data(), sizeof(VkDeviceSize),
                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);

        // 3: copy into right-sized structures, free the oversized ones
        for (uint32_t b = 0; b < batchCount; b++) {
            gpuAssets.blases[meshes[first + b]] = createAccelerationStructure(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
                                                                              compactedSizes[b]);
            gpuAssets.blasSizes[meshes[first + b]] = compactedSizes[b];
        }
        gpu.immediateSubmit([&](VkCommandBuffer cmd) {
            for (uint32_t b = 0; b < batchCount; b++) {
                VkCopyAccelerationStructureInfoKHR copyInfo{};
                copyInfo.sType = VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR;
                copyInfo.src = uncompacted[b].handle;
                copyInfo.dst = gpuAssets.blases[meshes[first + b]].handle;
                copyInfo.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
                vkext::vkCmdCopyAccelerationStructureKHR(cmd, &copyInfo);
            }
        });
        for (AccelerationStructure& as : uncompacted) {
            destroyAccelerationStructure(as);
        }
        first = end;
    }

    vkDestroyQueryPool(gpu.device, queryPool, nullptr);
    vkDestroyBuffer(gpu.device, scratchBuffer, nullptr);
    vmaFreeMemory(gpu.allocator, scratchMemory);
    vmaDestroyPool(gpu.allocator, buildPool);  // empty by now
}

// Top level: placed copies (instances) of bottom level structures, each with a transform. It can be updated in place
// (refit) while only the instances' transforms change (recordTopLevelBuild).
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
    buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                      VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries = &geometry;
}

// Creates an (unbuilt) TLAS with room for `capacity` instances, plus everything needed to rebuild it each frame.
void GpuScene::createTopLevelAccelerationStructure(uint32_t capacity) {
    tlasCapacity = capacity;

    // Per instance: its matrix and the index of its motion for the shader, uploaded when they change
    createStreamedBuffer(instanceData, sizeof(GpuInstance) * capacity);
    // The instances the TLAS is built from
    createStreamedBuffer(tlasInstanceData, sizeof(VkAccelerationStructureInstanceKHR) * capacity,
                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                         VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR);
    instanceDataPending.addAll();  // the new buffers are empty
    tlasInstancePending.addAll();

    // Sizes for the worst case (full capacity). Builds with fewer instances fit as well.
    VkAccelerationStructureGeometryKHR geometry;
    VkAccelerationStructureBuildGeometryInfoKHR buildInfo;
    describeTopLevel(0, geometry, buildInfo);  // addresses are ignored when only asking for sizes
    VkAccelerationStructureBuildSizesInfoKHR sizeInfo{};
    sizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    vkext::vkGetAccelerationStructureBuildSizesKHR(gpu.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                                   &buildInfo, &capacity, &sizeInfo);

    tlas = createAccelerationStructure(VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, sizeInfo.accelerationStructureSize,
                                       VK_NULL_HANDLE, true);
    tlasScratchAddress = createScratchBuffer(std::max(sizeInfo.buildScratchSize, sizeInfo.updateScratchSize),
                                             tlasScratchBuffer, tlasScratchMemory, VK_NULL_HANDLE, true);
    tlasDirty = true;  // nothing built yet
    tlasRebuildNeeded = true;
}

void GpuScene::destroyTopLevelAccelerationStructure() {
    destroyAccelerationStructure(tlas);
    destroyStreamedBuffer(instanceData);
    destroyStreamedBuffer(tlasInstanceData);
    vkDestroyBuffer(gpu.device, tlasScratchBuffer, nullptr);
    vmaFreeMemory(gpu.allocator, tlasScratchMemory);
    tlasCapacity = 0;
}

// Builds the TLAS from frameInstances, on the GPU, before this frame's ray tracing (the instances that changed are
// copied first).
//
// While only transforms change (things move), the TLAS is updated in place instead of built anew: its tree stays,
// only the boxes are refit around the instances' new places. In Jungle Ruins (2.9 million instances) with one object
// moving, the GPU work before tracing took ~15 ms per frame with builds, ~5 ms with updates. The tree gets worse the
// further things move from where it was built, so it is built again once nothing moves (updateScene), or after
// maxTlasUpdates updates in a row.
void GpuScene::recordTopLevelBuild(VkCommandBuffer commandBuffer) {
    uint32_t instanceCount = static_cast<uint32_t>(frameInstances.size());
    if (tlasInstancePending.all) {
        recordStreamedUpload(commandBuffer, tlasInstanceData, frameInstances.data(),
                             sizeof(VkAccelerationStructureInstanceKHR) * instanceCount);
    } else {
        recordStreamedUpload(commandBuffer, tlasInstanceData, frameInstances.data(),
                             sizeof(VkAccelerationStructureInstanceKHR), tlasInstancePending.ranges);
    }
    tlasInstancePending.clear();

    VkAccelerationStructureGeometryKHR geometry;
    VkAccelerationStructureBuildGeometryInfoKHR buildInfo;
    describeTopLevel(gpu.getBufferDeviceAddress(tlasInstanceData.buffer), geometry, buildInfo);
    buildInfo.dstAccelerationStructure = tlas.handle;
    buildInfo.scratchData.deviceAddress = tlasScratchAddress;
    if (!tlasRebuildNeeded && tlasUpdates < maxTlasUpdates) {
        buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
        buildInfo.srcAccelerationStructure = tlas.handle;  // in place
        tlasUpdates++;
    } else {
        tlasUpdates = 0;
    }

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

    tlasDirty = false;
    tlasRebuildNeeded = false;
    if (!tlasTidyOnly) {
        changes.resetAccumulation = true;  // the scene changed, the accumulated image is outdated
    }
    tlasTidyOnly = false;
}

// What updateScene changed of frameGpuInstances -> instanceData, and frameMotions -> motionData (after the previous
// frame's shader is done reading)
void GpuScene::recordInstanceUpload(VkCommandBuffer commandBuffer) {
    if (instanceDataPending.all) {
        recordStreamedUpload(commandBuffer, instanceData, frameGpuInstances.data(),
                             sizeof(GpuInstance) * frameGpuInstances.size());
    } else {
        recordStreamedUpload(commandBuffer, instanceData, frameGpuInstances.data(), sizeof(GpuInstance),
                             instanceDataPending.ranges);
    }
    instanceDataPending.clear();
    if (motionUploadPending) {
        recordStreamedUpload(commandBuffer, motionData, frameMotions.data(), sizeof(glm::mat4) * frameMotions.size());
        motionUploadPending = false;
    }
}
