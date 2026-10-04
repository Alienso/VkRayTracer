//
// Ray tracing: scene geometry, acceleration structures, output images and the compute pipeline.
//

#include <vulkan/vulkan.h>
#include <stdexcept>
#include <cmath>
#include <string>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <glm/gtc/matrix_transform.hpp>
#include "Application.h"
#include "Configuration.h"
#include "VulkanExtensions.h"

std::vector<char> readFile(const std::string& filename);

static const VkFormat rtColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
static const VkFormat rtDepthFormat = VK_FORMAT_R32_SFLOAT;
static const VkFormat accumulationFormat = VK_FORMAT_R32G32B32A32_SFLOAT;  // float, so tiny 1/n contributions don't round away

static const char* modelsDirectory = "models";  // copied next to the executable from resource/models
static const char* defaultModel = "DamagedHelmet.glb";
static const float modelSize = 2.0f;  // loaded models are scaled so their largest side is this long

void Application::createUniformBuffer() {
    createBuffer(sizeof(SceneUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, uniformBuffer, uniformBufferMemory);

    camera.position = {0.0f, 1.6f, 4.0f};
    camera.fov = Configuration::fov;
    camera.lookAt({0.0f, 0.9f, 0.0f});
}

void Application::findAvailableModels() {
    availableModels.clear();
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(modelsDirectory, error)) {
        std::string extension = entry.path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
        if (entry.is_regular_file() && (extension == ".glb" || extension == ".gltf")) {
            availableModels.push_back(entry.path().filename().string());
        }
    }
    std::sort(availableModels.begin(), availableModels.end());
    if (availableModels.empty()) {
        std::cerr << "no .glb/.gltf models found in '" << modelsDirectory << "', showing only the floor" << std::endl;
    }

    auto it = std::find(availableModels.begin(), availableModels.end(), defaultModel);
    loadedModel = it != availableModels.end() ? static_cast<int>(it - availableModels.begin())
                                              : (availableModels.empty() ? -1 : 0);
}

// A large floor quad, always part of the scene so models have something to stand on and cast shadows onto.
static Mesh createFloorMesh() {
    Mesh floor;
    floor.name = "floor";
    const float half = 6.0f;
    glm::vec3 up = {0.0f, 1.0f, 0.0f};
    floor.vertices = {
            {{-half, 0.0f, -half}, up, {0.0f, 0.0f}},
            {{ half, 0.0f, -half}, up, {1.0f, 0.0f}},
            {{ half, 0.0f,  half}, up, {1.0f, 1.0f}},
            {{-half, 0.0f,  half}, up, {0.0f, 1.0f}},
    };
    floor.indices = {0, 1, 2, 0, 2, 3};
    floor.baseColor = {0.75f, 0.75f, 0.75f, 1.0f};
    floor.hasNormals = true;
    return floor;
}

// Builds the CPU side of the scene (floor + model), then replaces the GPU side.
// modelIndex = -1 loads only the floor.
void Application::loadScene(int modelIndex) {
    std::vector<Mesh> meshes = {createFloorMesh()};
    std::vector<MeshInstance> instances = {{0, glm::mat4(1.0f)}};

    if (modelIndex >= 0 && modelIndex < static_cast<int>(availableModels.size())) {
        std::string path = std::string(modelsDirectory) + "/" + availableModels[modelIndex];
        try {
            // Load before touching the current scene, so a broken file leaves it intact.
            Model model = loadGltfModel(path);

            // Models come in any size and position (millimeters, centimeters, ...). Scale the largest side to
            // modelSize and stand the model on the floor in the middle. Phase 4 makes this editable.
            glm::vec3 extent = model.boundsMax - model.boundsMin;
            float scale = modelSize / std::max(std::max(extent.x, extent.y), std::max(extent.z, 1e-6f));
            glm::vec3 bottomCenter = {(model.boundsMin.x + model.boundsMax.x) * 0.5f, model.boundsMin.y,
                                      (model.boundsMin.z + model.boundsMax.z) * 0.5f};
            glm::mat4 placement = glm::scale(glm::mat4(1.0f), glm::vec3(scale)) * glm::translate(glm::mat4(1.0f), -bottomCenter);

            uint32_t firstMesh = static_cast<uint32_t>(meshes.size());
            for (Mesh& mesh : model.meshes) {
                meshes.push_back(std::move(mesh));
            }
            for (const MeshInstance& instance : model.instances) {
                instances.push_back({firstMesh + instance.meshIndex, placement * instance.transform});
            }
            loadedModel = modelIndex;
            loadError.clear();
        } catch (const std::exception& e) {
            loadError = e.what();
            std::cerr << loadError << std::endl;
            if (sceneLoaded) {
                return;  // keep showing the previous scene
            }
            loadedModel = -1;
        }
    } else {
        loadedModel = -1;
    }

    // The GPU may still be tracing against the old buffers.
    vkDeviceWaitIdle(device);
    if (sceneLoaded) {
        destroySceneGeometry();
    }

    sceneInstances = instances;
    uploadSceneGeometry(meshes);
    createBottomLevelAccelerationStructures();
    createTopLevelAccelerationStructure();
    sceneLoaded = true;

    primitiveCount = 0;
    for (const MeshInstance& instance : sceneInstances) {
        primitiveCount += meshInfos[instance.meshIndex].indexCount / 3;
    }

    if (rtDescriptorSet != VK_NULL_HANDLE) {
        updateRayTracingDescriptorSet();  // new TLAS and buffers
    }
    resetAccumulation = true;
}

// Packs all meshes into one vertex buffer and one index buffer. Indices stay relative to their mesh;
// MeshInfo remembers where each mesh starts.
void Application::uploadSceneGeometry(const std::vector<Mesh>& meshes) {
    std::vector<SceneVertex> vertices;
    std::vector<uint32_t> indices;
    meshInfos.clear();
    for (const Mesh& mesh : meshes) {
        MeshInfo info{};
        info.baseColor = mesh.baseColor;
        info.vertexOffset = static_cast<uint32_t>(vertices.size());
        info.indexOffset = static_cast<uint32_t>(indices.size());
        info.vertexCount = static_cast<uint32_t>(mesh.vertices.size());
        info.indexCount = static_cast<uint32_t>(mesh.indices.size());
        info.hasNormals = mesh.hasNormals ? 1 : 0;
        meshInfos.push_back(info);

        vertices.insert(vertices.end(), mesh.vertices.begin(), mesh.vertices.end());
        indices.insert(indices.end(), mesh.indices.begin(), mesh.indices.end());
    }

    // Acceleration structure builds read their inputs through device addresses, the shader reads them as storage buffers.
    VkBufferUsageFlags usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                               VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    createDeviceLocalBuffer(vertices.data(), sizeof(vertices[0]) * vertices.size(), usage, vertexBuffer, vertexBufferMemory);
    createDeviceLocalBuffer(indices.data(), sizeof(indices[0]) * indices.size(), usage, indexBuffer, indexBufferMemory);
    createDeviceLocalBuffer(meshInfos.data(), sizeof(meshInfos[0]) * meshInfos.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                            meshInfoBuffer, meshInfoBufferMemory);
}

void Application::destroySceneGeometry() {
    destroyAccelerationStructure(tlas);
    for (AccelerationStructure& blas : blases) {
        destroyAccelerationStructure(blas);
    }
    blases.clear();

    vkDestroyBuffer(device, instanceBuffer, nullptr);
    vkFreeMemory(device, instanceBufferMemory, nullptr);
    vkDestroyBuffer(device, meshInfoBuffer, nullptr);
    vkFreeMemory(device, meshInfoBufferMemory, nullptr);
    vkDestroyBuffer(device, indexBuffer, nullptr);
    vkFreeMemory(device, indexBufferMemory, nullptr);
    vkDestroyBuffer(device, vertexBuffer, nullptr);
    vkFreeMemory(device, vertexBufferMemory, nullptr);
    sceneLoaded = false;
}

// An empty acceleration structure of the given size, living inside a buffer we own. Filled by a build or a copy.
AccelerationStructure Application::createAccelerationStructure(VkAccelerationStructureTypeKHR type, VkDeviceSize size) {
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
VkDeviceAddress Application::createScratchBuffer(VkDeviceSize size, VkBuffer& buffer, VkDeviceMemory& memory) {
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

void Application::destroyAccelerationStructure(AccelerationStructure& as) {
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
void Application::createBottomLevelAccelerationStructures() {
    size_t count = meshInfos.size();
    VkDeviceAddress vertexAddress = getBufferDeviceAddress(vertexBuffer);
    VkDeviceAddress indexAddress = getBufferDeviceAddress(indexBuffer);

    std::vector<VkAccelerationStructureGeometryKHR> geometries(count);
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> buildInfos(count);
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges(count);
    std::vector<AccelerationStructure> uncompacted(count);
    VkDeviceSize maxScratchSize = 0;
    blasSizeBeforeCompaction = 0;

    // 1: describe every mesh, ask for sizes, create the (oversized) structures
    for (size_t i = 0; i < count; i++) {
        const MeshInfo& mesh = meshInfos[i];

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
        blasSizeBeforeCompaction += sizeInfo.accelerationStructureSize;
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
    blases.resize(count);
    blasSizeAfterCompaction = 0;
    for (size_t i = 0; i < count; i++) {
        blases[i] = createAccelerationStructure(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, compactedSizes[i]);
        blasSizeAfterCompaction += compactedSizes[i];
    }
    immediateSubmit([&](VkCommandBuffer cmd) {
        for (size_t i = 0; i < count; i++) {
            VkCopyAccelerationStructureInfoKHR copyInfo{};
            copyInfo.sType = VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR;
            copyInfo.src = uncompacted[i].handle;
            copyInfo.dst = blases[i].handle;
            copyInfo.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
            vkext::vkCmdCopyAccelerationStructureKHR(cmd, &copyInfo);
        }
    });

    for (AccelerationStructure& as : uncompacted) {
        destroyAccelerationStructure(as);
    }
}

// Top level: placed copies (instances) of bottom level structures, each with a transform.
void Application::createTopLevelAccelerationStructure() {
    std::vector<VkAccelerationStructureInstanceKHR> instances(sceneInstances.size());
    for (size_t i = 0; i < sceneInstances.size(); i++) {
        const MeshInstance& sceneInstance = sceneInstances[i];
        VkAccelerationStructureInstanceKHR& instance = instances[i];

        // Vulkan wants a 3x4 row-major matrix (the last row of an affine matrix is always 0 0 0 1).
        // glm is column-major: m[column][row].
        const glm::mat4& m = sceneInstance.transform;
        for (int row = 0; row < 3; row++) {
            for (int column = 0; column < 4; column++) {
                instance.transform.matrix[row][column] = m[column][row];
            }
        }
        instance.instanceCustomIndex = sceneInstance.meshIndex;  // 24 bits for us; the shader uses it to find the mesh
        instance.mask = 0xFF;
        instance.instanceShaderBindingTableRecordOffset = 0;
        instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        instance.accelerationStructureReference = blases[sceneInstance.meshIndex].address;
    }

    createDeviceLocalBuffer(instances.data(), sizeof(instances[0]) * instances.size(),
                            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
                            instanceBuffer, instanceBufferMemory);

    VkAccelerationStructureGeometryKHR geometry{};
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    geometry.geometry.instances.arrayOfPointers = VK_FALSE;
    geometry.geometry.instances.data.deviceAddress = getBufferDeviceAddress(instanceBuffer);

    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
    buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries = &geometry;

    uint32_t instanceCount = static_cast<uint32_t>(instances.size());
    VkAccelerationStructureBuildSizesInfoKHR sizeInfo{};
    sizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    vkext::vkGetAccelerationStructureBuildSizesKHR(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                                   &buildInfo, &instanceCount, &sizeInfo);

    tlas = createAccelerationStructure(VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, sizeInfo.accelerationStructureSize);

    VkBuffer scratchBuffer;
    VkDeviceMemory scratchMemory;
    buildInfo.dstAccelerationStructure = tlas.handle;
    buildInfo.scratchData.deviceAddress = createScratchBuffer(sizeInfo.buildScratchSize, scratchBuffer, scratchMemory);

    VkAccelerationStructureBuildRangeInfoKHR rangeInfo{};
    rangeInfo.primitiveCount = instanceCount;
    const VkAccelerationStructureBuildRangeInfoKHR* pRangeInfo = &rangeInfo;
    immediateSubmit([&](VkCommandBuffer cmd) {
        vkext::vkCmdBuildAccelerationStructuresKHR(cmd, 1, &buildInfo, &pRangeInfo);
    });

    vkDestroyBuffer(device, scratchBuffer, nullptr);
    vkFreeMemory(device, scratchMemory, nullptr);
}

void Application::createRayTracingImages() {
    // The color image gets blitted into the swapchain, which converts the format (RGBA -> BGRA, linear -> sRGB).
    VkFormatProperties swapchainFormatProperties;
    vkGetPhysicalDeviceFormatProperties(physicalDevice, swapChainImageFormat, &swapchainFormatProperties);
    if (!(swapchainFormatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
        throw std::runtime_error("swap chain format does not support blitting into it!");
    }

    createImage(swapChainExtent.width, swapChainExtent.height, rtColorFormat,
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                rtColorImage, rtColorImageMemory, rtColorImageView);
    createImage(swapChainExtent.width, swapChainExtent.height, rtDepthFormat,
                VK_IMAGE_USAGE_STORAGE_BIT,
                rtDepthImage, rtDepthImageMemory, rtDepthImageView);
    createImage(swapChainExtent.width, swapChainExtent.height, accumulationFormat,
                VK_IMAGE_USAGE_STORAGE_BIT,
                accumulationImage, accumulationImageMemory, accumulationImageView);

    // Depth and accumulation images stay in GENERAL forever. The color image is transitioned every frame.
    immediateSubmit([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier barriers[2]{};
        VkImage images[2] = {rtDepthImage, accumulationImage};
        for (int i = 0; i < 2; i++) {
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
                             0, 0, nullptr, 0, nullptr, 2, barriers);
    });

    resetAccumulation = true;  // new image, its contents are garbage
}

void Application::destroyRayTracingImages() {
    vkDestroyImageView(device, rtColorImageView, nullptr);
    vkDestroyImage(device, rtColorImage, nullptr);
    vkFreeMemory(device, rtColorImageMemory, nullptr);
    vkDestroyImageView(device, rtDepthImageView, nullptr);
    vkDestroyImage(device, rtDepthImage, nullptr);
    vkFreeMemory(device, rtDepthImageMemory, nullptr);
    vkDestroyImageView(device, accumulationImageView, nullptr);
    vkDestroyImage(device, accumulationImage, nullptr);
    vkFreeMemory(device, accumulationImageMemory, nullptr);
}

void Application::createRayTracingDescriptors() {
    // 0: TLAS, 1: color output, 2: depth output, 3: scene uniforms, 4: vertices, 5: indices, 6: accumulation, 7: mesh infos
    std::vector<VkDescriptorSetLayoutBinding> bindings(8);
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[3] = {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[4] = {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[5] = {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[6] = {6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[7] = {7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &rtDescriptorSetLayout) != VK_SUCCESS) {
        throw std::runtime_error("failed to create ray tracing descriptor set layout!");
    }

    VkDescriptorPoolSize poolSizes[] = {
            {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3}
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
void Application::updateRayTracingDescriptorSet() {
    VkWriteDescriptorSetAccelerationStructureKHR asInfo{};
    asInfo.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    asInfo.accelerationStructureCount = 1;
    asInfo.pAccelerationStructures = &tlas.handle;

    VkDescriptorImageInfo colorInfo{VK_NULL_HANDLE, rtColorImageView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo depthInfo{VK_NULL_HANDLE, rtDepthImageView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo uniformInfo{uniformBuffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo vertexInfo{vertexBuffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo indexInfo{indexBuffer, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo accumulationInfo{VK_NULL_HANDLE, accumulationImageView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo meshInfo{meshInfoBuffer, 0, VK_WHOLE_SIZE};

    std::vector<VkWriteDescriptorSet> writes(8);
    for (uint32_t i = 0; i < writes.size(); i++) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = rtDescriptorSet;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
    }
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    writes[0].pNext = &asInfo;  // acceleration structures are passed through pNext
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].pImageInfo = &colorInfo;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[2].pImageInfo = &depthInfo;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[3].pBufferInfo = &uniformInfo;
    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[4].pBufferInfo = &vertexInfo;
    writes[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[5].pBufferInfo = &indexInfo;
    writes[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[6].pImageInfo = &accumulationInfo;
    writes[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[7].pBufferInfo = &meshInfo;

    vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

void Application::createRayTracingPipeline() {
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &rtDescriptorSetLayout;
    if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &rtPipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("failed to create ray tracing pipeline layout!");
    }

    VkShaderModule shaderModule = createShaderModule(readFile("raytrace.comp.spv"));

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = shaderModule;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = rtPipelineLayout;
    if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &rtPipeline) != VK_SUCCESS) {
        throw std::runtime_error("failed to create ray tracing pipeline!");
    }

    vkDestroyShaderModule(device, shaderModule, nullptr);
}

// Ray trace into rtColorImage, then blit it into the swapchain image.
// Afterwards the swapchain image is in TRANSFER_DST_OPTIMAL, which is what the render pass expects.
SceneUniforms Application::buildSceneUniforms() {
    float azimuth = glm::radians(settings.sunAzimuth);
    float elevation = glm::radians(settings.sunElevation);
    glm::vec3 towardsSun = {std::cos(elevation) * std::sin(azimuth),
                            std::sin(elevation),
                            std::cos(elevation) * std::cos(azimuth)};

    SceneUniforms uniforms{};  // zero-initialized, so padding compares equal in memcmp below
    uniforms.cameraPosition = glm::vec4(camera.position, 1.0f);
    uniforms.cameraForward = glm::vec4(camera.forward(), 0.0f);
    uniforms.cameraRight = glm::vec4(camera.right(), 0.0f);
    uniforms.cameraUp = glm::vec4(camera.up(), 0.0f);
    uniforms.jitter = glm::vec2(0.0f);
    uniforms.tanHalfFov = std::tan(glm::radians(camera.fov) * 0.5f);
    uniforms.aspectRatio = (float) swapChainExtent.width / (float) swapChainExtent.height;
    uniforms.sunDirection = glm::vec4(towardsSun, std::cos(glm::radians(settings.sunAngularRadius)));
    uniforms.sunColor = glm::vec4(settings.sunColor * settings.sunIntensity, 0.0f);
    uniforms.skyZenithColor = glm::vec4(settings.skyZenithColor * settings.skyIntensity, 0.0f);
    uniforms.skyHorizonColor = glm::vec4(settings.skyHorizonColor * settings.skyIntensity, 0.0f);
    uniforms.groundColor = glm::vec4(settings.groundColor * settings.skyIntensity, 0.0f);
    uniforms.samplesPerPixel = static_cast<uint32_t>(std::max(settings.samplesPerPixel, 1));
    uniforms.maxBounces = static_cast<uint32_t>(std::max(settings.maxBounces, 1));
    uniforms.shadowsEnabled = settings.shadowsEnabled ? 1 : 0;
    uniforms.accumulate = settings.accumulate ? 1 : 0;

    // Anything that changes the image (camera moved, slider dragged, ...) restarts accumulation.
    // frameIndex and accumulatedFrames change every frame by design, and exposure/tone mapping only affect
    // the displayed image, not the accumulated one, so those are filled in after comparing.
    if (resetAccumulation || std::memcmp(&uniforms, &previousUniforms, sizeof(SceneUniforms)) != 0) {
        accumulatedFrames = 0;
        resetAccumulation = false;
    }
    previousUniforms = uniforms;

    uniforms.frameIndex = frameIndex;
    uniforms.accumulatedFrames = accumulatedFrames;
    uniforms.exposure = settings.exposure;
    uniforms.toneMapping = settings.toneMapping ? 1 : 0;
    uniforms.debugView = static_cast<uint32_t>(settings.debugView);
    if (settings.accumulate) {
        accumulatedFrames++;
    }
    return uniforms;
}

void Application::recordRayTracing(VkCommandBuffer commandBuffer, uint32_t imageIndex) {
    VkImageSubresourceRange colorRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

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

    uniformBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    uniformBarrier.dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT;

    // 1: make the output images writable. The previous frame may still be blitting from the color
    //    image, so wait for transfers. Its old contents are not needed, hence UNDEFINED.
    VkImageMemoryBarrier preBarriers[3]{};
    preBarriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    preBarriers[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    preBarriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    preBarriers[0].srcAccessMask = 0;
    preBarriers[0].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    preBarriers[0].image = rtColorImage;

    preBarriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    preBarriers[1].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    preBarriers[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    preBarriers[1].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    preBarriers[1].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    preBarriers[1].image = rtDepthImage;

    // The previous frame's read-modify-write of the running average must finish before this one starts.
    preBarriers[2].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    preBarriers[2].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    preBarriers[2].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    preBarriers[2].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    preBarriers[2].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    preBarriers[2].image = accumulationImage;

    for (auto& barrier : preBarriers) {
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.subresourceRange = colorRange;
    }
    // (the uniform barrier rides along: uniform upload -> shader read)
    vkCmdPipelineBarrier(commandBuffer,
                         VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 1, &uniformBarrier, 3, preBarriers);

    // 2: trace
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, rtPipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, rtPipelineLayout, 0, 1, &rtDescriptorSet, 0, nullptr);
    vkCmdDispatch(commandBuffer, (swapChainExtent.width + 7) / 8, (swapChainExtent.height + 7) / 8, 1);

    // 3: color image -> blit source, swapchain image -> blit destination.
    //    The swapchain image only becomes available at the TRANSFER stage (see the semaphore wait in drawFrame).
    VkImageMemoryBarrier blitBarriers[2]{};
    blitBarriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    blitBarriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    blitBarriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    blitBarriers[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    blitBarriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    blitBarriers[0].image = rtColorImage;

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

    // 4: blit (same size, so this is a copy plus format conversion)
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {(int32_t) swapChainExtent.width, (int32_t) swapChainExtent.height, 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[1] = {(int32_t) swapChainExtent.width, (int32_t) swapChainExtent.height, 1};
    vkCmdBlitImage(commandBuffer,
                   rtColorImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   swapChainImages[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &blit, VK_FILTER_NEAREST);
}

void Application::cleanupRayTracing() {
    vkDestroyPipeline(device, rtPipeline, nullptr);
    vkDestroyPipelineLayout(device, rtPipelineLayout, nullptr);
    vkDestroyDescriptorPool(device, rtDescriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(device, rtDescriptorSetLayout, nullptr);

    destroySceneGeometry();

    vkDestroyBuffer(device, uniformBuffer, nullptr);
    vkFreeMemory(device, uniformBufferMemory, nullptr);
}
