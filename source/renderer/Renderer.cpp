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


void Renderer::init(GLFWwindow* targetWindow, const RenderList& list) {
    window = targetWindow;
    renderList = &list;
    dlss.initStreamline();  // DLSS: must come before the instance exists
    GpuContext::Extras extras;  // DLSS: what Streamline needs of the instance and the device
    extras.instanceExtensions = dlss.streamlineInstanceExtensions;
    extras.addDeviceRequirements = [this](std::vector<const char*>& extensions, VkPhysicalDeviceVulkan12Features& features12,
                                          VkPhysicalDeviceVulkan13Features& features13) {
        dlss.addStreamlineDeviceRequirements(extensions, features12, features13);
    };
    extras.deviceCreated = [this]() { dlss.connectStreamline(); };
    gpu.create(window, extras);
    createSwapChain();
    createImageViews();
    createRenderPass();
    createFramebuffers();
    createCommandBuffers();
    createSyncObjects();

    trace.createUniformBuffer();
    trace.createSpecularAlbedoTable();
    trace.createBlueNoiseTexture();
    scene.waitIdle = [this]() { vkp.deviceWaitIdle(gpu.device); };  // (through Streamline with DLSS)
    scene.create();
    // Uploads the geometry, builds the BLASes and collects the instances (and the render list), and the sky's sampling
    // table (uploaded with the first frame)
    scene.update(list, frameNumber);
    applySceneChanges();
    chooseExtents();
    trace.createRayTracingImages();
    createDisplayImages();
    exposure.createExposureResources();
    cache.createRadianceCache();
    trace.createRayTracingDescriptors();
    trace.createRayTracingPipeline();
    display.createDisplayResources();  // reads the radiance image and the exposure buffer
    createTimestampQueries();

    initImGui();
}

void Renderer::beginFrame(float deltaTime) {
    frameDeltaTime = deltaTime;
    capture.finishCapture();  // a capture recorded last frame -> PNG
    ImGui_ImplVulkan_NewFrame();
}

RendererStats Renderer::stats() const {
    RendererStats stats;
    stats.deviceName = gpu.physicalDeviceProperties.deviceName;
    stats.gpuMemory = gpuMemory();
    for (int i = 0; i < 6; i++) {
        stats.gpuPassMs[i] = gpuPassTimes[i];
        stats.lastGpuPassMs[i] = lastPassTimes[i];
    }
    stats.gpuTimesRead = timestampFramesRead;
    stats.triangles = scene.primitiveCount;
    stats.instances = scene.frameInstances.size();
    stats.meshes = scene.gpuAssets.meshInfos.size();
    stats.textures = scene.gpuAssets.textures.size();
    stats.textureMemory = scene.gpuAssets.textureMemorySize;
    stats.emissiveTriangles = scene.emissiveTriangles.size();
    stats.sampledEmissiveTriangles = scene.sampledEmissiveTriangles;
    stats.blasBuildSize = scene.gpuAssets.blasSizeBeforeCompaction;
    stats.blasSize = scene.gpuAssets.blasSizeAfterCompaction;
    stats.accumulating = settings.accumulate && !dlssActive();
    stats.accumulatedFrames = accumulatedFrames;
    stats.debugView = settings.debugView != 0;
    stats.captureMessage = capture.captureMessage;
    stats.environmentMapSize = renderList != nullptr && scene.environmentMapReady() ? scene.environmentMapSize : glm::uvec2(0);
    return stats;
}

// Summed over the device-local heaps. Cheap enough for every frame (the driver keeps the numbers).
GpuMemory Renderer::gpuMemory() const {
    GpuMemory memory;
    VmaBudget budgets[VK_MAX_MEMORY_HEAPS];
    vmaGetHeapBudgets(gpu.allocator, budgets);
    const VkPhysicalDeviceMemoryProperties* heaps;
    vmaGetMemoryProperties(gpu.allocator, &heaps);
    for (uint32_t heap = 0; heap < heaps->memoryHeapCount; heap++) {
        memory.memoryObjects += budgets[heap].statistics.blockCount;
        memory.allocations += budgets[heap].statistics.allocationCount;
    }
    if (!gpu.memoryBudgetSupported) {
        return memory;
    }
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{};
    budget.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
    VkPhysicalDeviceMemoryProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
    properties.pNext = &budget;
    vkGetPhysicalDeviceMemoryProperties2(gpu.physicalDevice, &properties);
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
    scene.update(list, frameNumber);  // the render list's changes on the GPU (new models, instances, lights, sky)
    applySceneChanges();
    applyRenderMode();  // accumulation <-> DLSS modes: new render size
    applyOutputSize();  // window size <-> e.g. 4K: new images
    applyPerPixelBuffers();  // ReSTIR or the guides switched on or off: new images

    drawFrame();
}

void Renderer::shutdown() {
    vkp.deviceWaitIdle(gpu.device);
    capture.finishCapture();
    gpu.savePipelineCache();
    cleanup();
}

void Renderer::cleanup() {
    cleanupImGui();
    cleanupSwapChain();
    trace.destroyRayTracingImages();
    destroyDisplayImages();
    cleanupRayTracing();

    vkDestroyRenderPass(gpu.device, renderPass, nullptr);

    destroyRenderFinishedSemaphores();

    for (size_t i = 0; i < Configuration::maxFramesInFlight; i++) {
        vkDestroySemaphore(gpu.device, imageAvailableSemaphores[i], nullptr);
        vkDestroyFence(gpu.device, inFlightFences[i], nullptr);
    }

    dlss.shutdownStreamline();
    gpu.destroy();
}

void Renderer::applySceneChanges() {
    GpuScene::Changes& changes = scene.changes;
    if (changes.resetAccumulation) {
        resetAccumulation = true;
    }
    if (changes.clearCache) {
        cache.cacheClearPending = true;
    }
    if (changes.resetDlss) {
        dlssReset = true;
    }
    if (changes.descriptors && trace.rtDescriptorSet != VK_NULL_HANDLE) {  // (made after the first update, in init)
        trace.updateRayTracingDescriptorSet();
    }
    changes = {};
}

void Renderer::drawFrame() {
    vkWaitForFences(gpu.device, 1, &inFlightFences[currentFrame], VK_TRUE, UINT64_MAX);
    scene.beginFrame(currentFrame, frameNumber);  // the frame that last used this slot is done
    trace.updateTracePipelines();

    uint32_t imageIndex;
    VkResult result = vkp.acquireNextImage(gpu.device, swapChain, UINT64_MAX, imageAvailableSemaphores[currentFrame], VK_NULL_HANDLE, &imageIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapChain();
        return;
    } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        throw std::runtime_error("failed to acquire swap chain image!");
    }

    vkResetFences(gpu.device, 1, &inFlightFences[currentFrame]);
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

    if (vkQueueSubmit(gpu.graphicsQueue, 1, &submitInfo, inFlightFences[currentFrame]) != VK_SUCCESS) {
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

    result = vkp.queuePresent(gpu.presentQueue, &presentInfo);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || framebufferResized) {
        framebufferResized = false;
        recreateSwapChain();
    } else if (result != VK_SUCCESS) {
        throw std::runtime_error("failed to present swap chain image!");
    }

    currentFrame = (currentFrame + 1) % Configuration::maxFramesInFlight;
    frameNumber++;
}











