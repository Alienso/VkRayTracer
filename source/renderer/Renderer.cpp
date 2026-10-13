//
// Created by Alienson on 19.1.2024..
//

#include <vulkan/vulkan.h>
#include <stdexcept>
#include <imgui/imgui.h>
#include <imgui/backends/imgui_impl_vulkan.h>
#include <imgui/backends/imgui_impl_glfw.h>
#include "renderer/Renderer.h"
#include <cstring>
#include <algorithm>
#include "core/Configuration.h"

static void destroyDebugUtilsMessengerEXT(VkInstance instance, VkDebugUtilsMessengerEXT debugMessenger, const VkAllocationCallbacks* pAllocator);

void Renderer::init(GLFWwindow* targetWindow, const RenderList& list) {
    window = targetWindow;
    renderList = &list;
    initStreamline();  // DLSS: must come before the instance exists
    createInstance();
    setupDebugMessenger();
    createSurface();
    pickPhysicalDevice();
    createLogicalDevice();
    createAllocator();
    createSwapChain();
    createImageViews();
    createRenderPass();
    createFramebuffers();
    createCommandPool();
    createCommandBuffers();
    createSyncObjects();

    createUniformBuffer();
    createSpecularAlbedoTable();
    createBlueNoiseTexture();
    createVolumeTextures();
    createTopLevelAccelerationStructure(16);  // grows when more instances are needed
    createStreamedBuffer(motionData, sizeof(glm::mat4) * 16);  // and this with the objects (updateScene)
    createLightBuffers();
    createPortalBuffer();
    createEnvironment();
    updateScene();  // uploads the geometry, builds the BLASes and collects the instances (and the render list)
    updateEnvironment();  // the sky's sampling table, uploaded with the first frame
    chooseExtents();
    createRayTracingImages();
    createDisplayImages();
    createExposureResources();
    createRadianceCache();
    createRayTracingDescriptors();
    createRayTracingPipeline();
    createDisplayResources();  // reads the radiance image and the exposure buffer
    createTimestampQueries();

    initImGui();
}

void Renderer::beginFrame(float deltaTime) {
    frameDeltaTime = deltaTime;
    finishCapture();  // a capture recorded last frame -> PNG
    ImGui_ImplVulkan_NewFrame();
}

RendererStats Renderer::stats() const {
    RendererStats stats;
    stats.deviceName = physicalDeviceProperties.deviceName;
    stats.gpuMemory = gpuMemory();
    for (int i = 0; i < 6; i++) {
        stats.gpuPassMs[i] = gpuPassTimes[i];
    }
    stats.triangles = primitiveCount;
    stats.instances = frameInstances.size();
    stats.meshes = gpuAssets.meshInfos.size();
    stats.textures = gpuAssets.textures.size();
    stats.textureMemory = gpuAssets.textureMemorySize;
    stats.emissiveTriangles = emissiveTriangles.size();
    stats.sampledEmissiveTriangles = sampledEmissiveTriangles;
    stats.blasBuildSize = gpuAssets.blasSizeBeforeCompaction;
    stats.blasSize = gpuAssets.blasSizeAfterCompaction;
    stats.accumulating = settings.accumulate && !dlssActive();
    stats.accumulatedFrames = accumulatedFrames;
    stats.debugView = settings.debugView != 0;
    stats.captureMessage = captureMessage;
    stats.environmentMapSize = renderList != nullptr && environmentMapReady() ? environmentMapSize : glm::uvec2(0);
    return stats;
}

// Summed over the device-local heaps. Cheap enough for every frame (the driver keeps the numbers).
GpuMemory Renderer::gpuMemory() const {
    GpuMemory memory;
    VmaBudget budgets[VK_MAX_MEMORY_HEAPS];
    vmaGetHeapBudgets(allocator, budgets);
    const VkPhysicalDeviceMemoryProperties* heaps;
    vmaGetMemoryProperties(allocator, &heaps);
    for (uint32_t heap = 0; heap < heaps->memoryHeapCount; heap++) {
        memory.memoryObjects += budgets[heap].statistics.blockCount;
        memory.allocations += budgets[heap].statistics.allocationCount;
    }
    if (!memoryBudgetSupported) {
        return memory;
    }
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{};
    budget.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
    VkPhysicalDeviceMemoryProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
    properties.pNext = &budget;
    vkGetPhysicalDeviceMemoryProperties2(physicalDevice, &properties);
    for (uint32_t heap = 0; heap < properties.memoryProperties.memoryHeapCount; heap++) {
        if (properties.memoryProperties.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            memory.used += budget.heapUsage[heap];
            memory.budget += budget.heapBudget[heap];
        }
    }
    return memory;
}

void Renderer::render(const RenderList& list) {
    renderList = &list;
    updateScene();  // the render list's changes on the GPU (new models, instances, lights)
    updateEnvironment();  // a new HDR map, or new sky colors to sample by
    applyRenderMode();  // accumulation <-> DLSS modes: new render size
    applyOutputSize();  // window size <-> e.g. 4K: new images
    applyPerPixelBuffers();  // ReSTIR or the guides switched on or off: new images

    drawFrame();
}

void Renderer::shutdown() {
    vkp.deviceWaitIdle(device);
    finishCapture();
    cleanup();
}

void Renderer::cleanup() {
    cleanupImGui();
    cleanupSwapChain();
    destroyRayTracingImages();
    destroyDisplayImages();
    cleanupRayTracing();

    vkDestroyRenderPass(device, renderPass, nullptr);

    destroyRenderFinishedSemaphores();

    for (size_t i = 0; i < Configuration::maxFramesInFlight; i++) {
        vkDestroySemaphore(device, imageAvailableSemaphores[i], nullptr);
        vkDestroyFence(device, inFlightFences[i], nullptr);
    }

    vkDestroyCommandPool(device, commandPool, nullptr);
    shutdownStreamline();
    vmaDestroyAllocator(allocator);  // everything allocated from it must be freed by now
    vkDestroyDevice(device, nullptr);

    if (Configuration::enableValidationLayers) {
        destroyDebugUtilsMessengerEXT(instance, debugMessenger, nullptr);
    }

    vkDestroySurfaceKHR(instance, surface, nullptr);
    vkDestroyInstance(instance, nullptr);
}

void Renderer::drawFrame() {
    vkWaitForFences(device, 1, &inFlightFences[currentFrame], VK_TRUE, UINT64_MAX);
    deferredDeletion.collect(frameNumber);  // the frame that last used this slot is done: its releases can go
    updateTracePipelines();

    uint32_t imageIndex;
    VkResult result = vkp.acquireNextImage(device, swapChain, UINT64_MAX, imageAvailableSemaphores[currentFrame], VK_NULL_HANDLE, &imageIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapChain();
        return;
    } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        throw std::runtime_error("failed to acquire swap chain image!");
    }

    vkResetFences(device, 1, &inFlightFences[currentFrame]);
    vkResetCommandBuffer(commandBuffers[currentFrame], 0);
    recordCommandBuffer(commandBuffers[currentFrame], imageIndex);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

    // The swapchain image is first touched by the blit (TRANSFER), so only that stage has to wait.
    // The ray tracing dispatch before it can start right away.
    VkSemaphore waitSemaphores[] = {imageAvailableSemaphores[currentFrame]};
    VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_TRANSFER_BIT};
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffers[currentFrame];

    VkSemaphore signalSemaphores[] = {renderFinishedSemaphores[imageIndex]};
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSemaphores;

    if (vkQueueSubmit(graphicsQueue, 1, &submitInfo, inFlightFences[currentFrame]) != VK_SUCCESS) {
        throw std::runtime_error("failed to submit draw command buffer!");
    }

    VkSwapchainKHR swapChains[] = {swapChain};
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = signalSemaphores;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = swapChains;
    presentInfo.pImageIndices = &imageIndex;
    presentInfo.pResults = nullptr;

    result = vkp.queuePresent(presentQueue, &presentInfo);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || framebufferResized) {
        framebufferResized = false;
        recreateSwapChain();
    } else if (result != VK_SUCCESS) {
        throw std::runtime_error("failed to present swap chain image!");
    }

    currentFrame = (currentFrame + 1) % Configuration::maxFramesInFlight;
    frameNumber++;
}

// Uploads `data` through a staging buffer into a new device local buffer.
void Renderer::createDeviceLocalBuffer(const void* data, VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buffer, VmaAllocation& bufferMemory) {
    VkBuffer stagingBuffer;
    VmaAllocation stagingBufferMemory;
    createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stagingBuffer, stagingBufferMemory);

    void* mapped;
    vmaMapMemory(allocator, stagingBufferMemory, &mapped);
    memcpy(mapped, data, (size_t) size);
    vmaUnmapMemory(allocator, stagingBufferMemory);

    createBuffer(size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, buffer, bufferMemory);
    copyBuffer(stagingBuffer, buffer, size);

    vkDestroyBuffer(device, stagingBuffer, nullptr);
    vmaFreeMemory(allocator, stagingBufferMemory);
}

VkDeviceAddress Renderer::getBufferDeviceAddress(VkBuffer buffer) {
    VkBufferDeviceAddressInfo addressInfo{};
    addressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    addressInfo.buffer = buffer;
    return vkGetBufferDeviceAddress(device, &addressInfo);
}

// 2D, device local image plus a matching view of all its mip levels. Starts in VK_IMAGE_LAYOUT_UNDEFINED.
void Renderer::createImage(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage, VkImage& image, VmaAllocation& imageMemory, VkImageView& imageView, uint32_t mipLevels, bool ownMemory) {
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = mipLevels;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = usage;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    allocationInfo.flags = ownMemory ? VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT : 0;
    if (vmaCreateImage(allocator, &imageInfo, &allocationInfo, &image, &imageMemory, nullptr) != VK_SUCCESS) {
        throw std::runtime_error("failed to create image!");
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, 1};

    if (vkCreateImageView(device, &viewInfo, nullptr, &imageView) != VK_SUCCESS) {
        throw std::runtime_error("failed to create image view!");
    }
}

void Renderer::createImage(VkExtent2D extent, VkFormat format, VkImageUsageFlags usage, RenderImage& image) {
    createImage(extent.width, extent.height, format, usage, image.image, image.memory, image.view, 1, true);
}

void Renderer::destroyImage(RenderImage& image) {
    vkDestroyImageView(device, image.view, nullptr);
    vkDestroyImage(device, image.image, nullptr);
    vmaFreeMemory(allocator, image.memory);
    image = RenderImage{};
}

void Renderer::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer& buffer, VmaAllocation& bufferMemory,
                            VmaPool pool, bool ownMemory) {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    // The memory type: the first that has these properties, as before VMA (with requiredFlags and no usage, VMA picks
    // the first fitting type too). Buffers whose GPU address we query need memory with the DEVICE_ADDRESS flag: VMA
    // allocates all its memory that way (createAllocator). A pool: memory set aside for short-lived buffers.
    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.requiredFlags = properties;
    allocationInfo.pool = pool;
    allocationInfo.flags = ownMemory ? VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT : 0;
    if (vmaCreateBuffer(allocator, &bufferInfo, &allocationInfo, &buffer, &bufferMemory, nullptr) != VK_SUCCESS) {
        throw std::runtime_error("failed to create buffer!");
    }
}

void Renderer::copyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size) {
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = commandPool;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer;
    vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(commandBuffer, &beginInfo);

    VkBufferCopy copyRegion{};
    copyRegion.srcOffset = 0; // Optional
    copyRegion.dstOffset = 0; // Optional
    copyRegion.size = size;
    vkCmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, 1, &copyRegion);

    vkEndCommandBuffer(commandBuffer);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    vkQueueSubmit(graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphicsQueue);

    vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);
}

// The memory object an allocation lives in (shared with other allocations unless it got one of its own)
VkDeviceMemory Renderer::deviceMemoryOf(VmaAllocation allocation) const {
    VmaAllocationInfo info;
    vmaGetAllocationInfo(allocator, allocation, &info);
    return info.deviceMemory;
}



static void destroyDebugUtilsMessengerEXT(VkInstance instance, VkDebugUtilsMessengerEXT debugMessenger, const VkAllocationCallbacks* pAllocator) {
    auto func = (PFN_vkDestroyDebugUtilsMessengerEXT) vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
    if (func != nullptr) {
        func(instance, debugMessenger, pAllocator);
    }
}
