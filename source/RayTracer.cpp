//
// Ray tracing: scene geometry, acceleration structures, output images and the compute pipeline.
//

#include <vulkan/vulkan.h>
#include <stdexcept>
#include <cmath>
#include <string>
#include "Application.h"
#include "Configuration.h"
#include "VulkanExtensions.h"

std::vector<char> readFile(const std::string& filename);

static const VkFormat rtColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
static const VkFormat rtDepthFormat = VK_FORMAT_R32_SFLOAT;

void Application::createSceneGeometry() {
    // Same layout as the old raster triangle: red top, green bottom-right, blue bottom-left.
    // World space: +Y up, camera looks down -Z.
    std::vector<glm::vec3> vertices = {
            { 0.0f,  0.5f, 0.0f},
            { 0.5f, -0.5f, 0.0f},
            {-0.5f, -0.5f, 0.0f}
    };
    std::vector<uint32_t> indices = {0, 1, 2};

    // Acceleration structure builds read their inputs through device addresses.
    VkBufferUsageFlags usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                               VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;  // the shader will read normals/UVs from these in Phase 3

    createDeviceLocalBuffer(vertices.data(), sizeof(vertices[0]) * vertices.size(), usage, vertexBuffer, vertexBufferMemory);
    createDeviceLocalBuffer(indices.data(), sizeof(indices[0]) * indices.size(), usage, indexBuffer, indexBufferMemory);
}

// Shared by BLAS and TLAS: query sizes, allocate storage + scratch, build on the GPU, wait.
AccelerationStructure Application::buildAccelerationStructure(VkAccelerationStructureTypeKHR type,
                                                              const VkAccelerationStructureGeometryKHR& geometry,
                                                              uint32_t primitiveCount) {
    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
    buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    buildInfo.type = type;
    buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries = &geometry;

    // 1: ask the driver how much memory the structure and the temporary build data need
    VkAccelerationStructureBuildSizesInfoKHR sizeInfo{};
    sizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    vkext::vkGetAccelerationStructureBuildSizesKHR(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                                   &buildInfo, &primitiveCount, &sizeInfo);

    // 2: the acceleration structure lives inside a buffer we own
    AccelerationStructure as;
    createBuffer(sizeInfo.accelerationStructureSize,
                 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, as.buffer, as.memory);

    VkAccelerationStructureCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    createInfo.buffer = as.buffer;
    createInfo.size = sizeInfo.accelerationStructureSize;
    createInfo.type = type;
    if (vkext::vkCreateAccelerationStructureKHR(device, &createInfo, nullptr, &as.handle) != VK_SUCCESS) {
        throw std::runtime_error("failed to create acceleration structure!");
    }

    // 3: scratch memory, only used during the build. Its address has to be aligned.
    VkPhysicalDeviceAccelerationStructurePropertiesKHR asProperties{};
    asProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
    VkPhysicalDeviceProperties2 properties2{};
    properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties2.pNext = &asProperties;
    vkGetPhysicalDeviceProperties2(physicalDevice, &properties2);
    VkDeviceSize scratchAlignment = asProperties.minAccelerationStructureScratchOffsetAlignment;

    VkBuffer scratchBuffer;
    VkDeviceMemory scratchMemory;
    createBuffer(sizeInfo.buildScratchSize + scratchAlignment,
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, scratchBuffer, scratchMemory);
    VkDeviceAddress scratchAddress = getBufferDeviceAddress(scratchBuffer);
    scratchAddress = (scratchAddress + scratchAlignment - 1) / scratchAlignment * scratchAlignment;

    // 4: build
    buildInfo.dstAccelerationStructure = as.handle;
    buildInfo.scratchData.deviceAddress = scratchAddress;

    VkAccelerationStructureBuildRangeInfoKHR rangeInfo{};
    rangeInfo.primitiveCount = primitiveCount;
    const VkAccelerationStructureBuildRangeInfoKHR* pRangeInfo = &rangeInfo;

    immediateSubmit([&](VkCommandBuffer cmd) {
        vkext::vkCmdBuildAccelerationStructuresKHR(cmd, 1, &buildInfo, &pRangeInfo);
    });

    vkDestroyBuffer(device, scratchBuffer, nullptr);
    vkFreeMemory(device, scratchMemory, nullptr);

    VkAccelerationStructureDeviceAddressInfoKHR addressInfo{};
    addressInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    addressInfo.accelerationStructure = as.handle;
    as.address = vkext::vkGetAccelerationStructureDeviceAddressKHR(device, &addressInfo);

    return as;
}

void Application::destroyAccelerationStructure(AccelerationStructure& as) {
    vkext::vkDestroyAccelerationStructureKHR(device, as.handle, nullptr);
    vkDestroyBuffer(device, as.buffer, nullptr);
    vkFreeMemory(device, as.memory, nullptr);
    as = {};
}

// Bottom level: the triangles of one mesh, in the mesh's own coordinate system.
void Application::createBottomLevelAccelerationStructure() {
    VkAccelerationStructureGeometryTrianglesDataKHR triangles{};
    triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    triangles.vertexData.deviceAddress = getBufferDeviceAddress(vertexBuffer);
    triangles.vertexStride = sizeof(glm::vec3);
    triangles.maxVertex = 2;  // highest vertex index used
    triangles.indexType = VK_INDEX_TYPE_UINT32;
    triangles.indexData.deviceAddress = getBufferDeviceAddress(indexBuffer);

    VkAccelerationStructureGeometryKHR geometry{};
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geometry.geometry.triangles = triangles;
    geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;

    blas = buildAccelerationStructure(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, geometry, 1);
}

// Top level: placed copies (instances) of bottom level structures, each with a transform.
void Application::createTopLevelAccelerationStructure() {
    VkAccelerationStructureInstanceKHR instance{};
    instance.transform = {{
            {1.0f, 0.0f, 0.0f, 0.0f},
            {0.0f, 1.0f, 0.0f, 0.0f},
            {0.0f, 0.0f, 1.0f, 0.0f}
    }};  // 3x4 row-major, identity
    instance.instanceCustomIndex = 0;  // free for our own use, e.g. an object/material index
    instance.mask = 0xFF;
    instance.instanceShaderBindingTableRecordOffset = 0;
    instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    instance.accelerationStructureReference = blas.address;

    createDeviceLocalBuffer(&instance, sizeof(instance),
                            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
                            instanceBuffer, instanceBufferMemory);

    VkAccelerationStructureGeometryKHR geometry{};
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    geometry.geometry.instances.arrayOfPointers = VK_FALSE;
    geometry.geometry.instances.data.deviceAddress = getBufferDeviceAddress(instanceBuffer);

    tlas = buildAccelerationStructure(VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, geometry, 1);
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

    // The depth image stays in GENERAL forever. The color image is transitioned every frame.
    immediateSubmit([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = rtDepthImage;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
    });
}

void Application::destroyRayTracingImages() {
    vkDestroyImageView(device, rtColorImageView, nullptr);
    vkDestroyImage(device, rtColorImage, nullptr);
    vkFreeMemory(device, rtColorImageMemory, nullptr);
    vkDestroyImageView(device, rtDepthImageView, nullptr);
    vkDestroyImage(device, rtDepthImage, nullptr);
    vkFreeMemory(device, rtDepthImageMemory, nullptr);
}

void Application::createRayTracingDescriptors() {
    // binding 0: TLAS, binding 1: color output, binding 2: depth output
    std::vector<VkDescriptorSetLayoutBinding> bindings(3);
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &rtDescriptorSetLayout) != VK_SUCCESS) {
        throw std::runtime_error("failed to create ray tracing descriptor set layout!");
    }

    VkDescriptorPoolSize poolSizes[] = {
            {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}
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

// Called again after a swapchain recreation, because the output images are new.
void Application::updateRayTracingDescriptorSet() {
    VkWriteDescriptorSetAccelerationStructureKHR asInfo{};
    asInfo.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    asInfo.accelerationStructureCount = 1;
    asInfo.pAccelerationStructures = &tlas.handle;

    VkDescriptorImageInfo colorInfo{VK_NULL_HANDLE, rtColorImageView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo depthInfo{VK_NULL_HANDLE, rtDepthImageView, VK_IMAGE_LAYOUT_GENERAL};

    std::vector<VkWriteDescriptorSet> writes(3);
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

    vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

void Application::createRayTracingPipeline() {
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(RayTracingPushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &rtDescriptorSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstantRange;
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
void Application::recordRayTracing(VkCommandBuffer commandBuffer, uint32_t imageIndex) {
    VkImageSubresourceRange colorRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    // 1: make the output images writable. The previous frame may still be blitting from the color
    //    image, so wait for transfers. Its old contents are not needed, hence UNDEFINED.
    VkImageMemoryBarrier preBarriers[2]{};
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

    for (auto& barrier : preBarriers) {
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.subresourceRange = colorRange;
    }
    vkCmdPipelineBarrier(commandBuffer,
                         VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 2, preBarriers);

    // 2: trace
    glm::vec3 forward = glm::normalize(cameraTarget - cameraPosition);
    glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
    glm::vec3 up = glm::cross(right, forward);

    RayTracingPushConstants pushConstants{};
    pushConstants.cameraPosition = glm::vec4(cameraPosition, 1.0f);
    pushConstants.cameraForward = glm::vec4(forward, 0.0f);
    pushConstants.cameraRight = glm::vec4(right, 0.0f);
    pushConstants.cameraUp = glm::vec4(up, 0.0f);
    pushConstants.jitter = glm::vec2(0.0f);
    pushConstants.tanHalfFov = std::tan(glm::radians(Configuration::fov) * 0.5f);
    pushConstants.aspectRatio = (float) swapChainExtent.width / (float) swapChainExtent.height;

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, rtPipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, rtPipelineLayout, 0, 1, &rtDescriptorSet, 0, nullptr);
    vkCmdPushConstants(commandBuffer, rtPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pushConstants), &pushConstants);
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

    destroyAccelerationStructure(tlas);
    destroyAccelerationStructure(blas);

    vkDestroyBuffer(device, instanceBuffer, nullptr);
    vkFreeMemory(device, instanceBufferMemory, nullptr);
    vkDestroyBuffer(device, indexBuffer, nullptr);
    vkFreeMemory(device, indexBufferMemory, nullptr);
    vkDestroyBuffer(device, vertexBuffer, nullptr);
    vkFreeMemory(device, vertexBufferMemory, nullptr);
}
