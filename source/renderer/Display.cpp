//
// Display pass: the last step before the screen. Takes the linear HDR image (shaders/display.comp), applies
// exposure and tone mapping and writes an 8-bit image at output size, which is blitted into the swapchain.
//
// Kept apart from the trace shader so that something can sit in between: DLSS needs linear HDR input at the
// render size and produces an HDR image at the output size, which then gets exposure and tone mapping here.
//

#include <vulkan/vulkan.h>
#include <algorithm>
#include <cmath>
#include <iterator>
#include <stdexcept>
#include <string>
#include "renderer/Renderer.h"
#include "core/Configuration.h"

std::vector<char> readFile(const std::string& filename);

static const VkFormat displayFormat = VK_FORMAT_R8G8B8A8_UNORM;

// Output = the window, or a fixed number of rows (e.g. 2160 for a 4K capture) as wide as the window's shape: it is
// shrunk to fit the window when shown, and Capture saves it at full size. Render = what the path tracer traces: the
// same in accumulation mode, smaller in the DLSS modes (the size DLSS asks for, e.g. 67% per axis for Quality).
void Renderer::chooseExtents() {
    outputExtent = swapChainExtent;
    uint32_t rows = outputHeights[std::clamp(activeOutputSize, 0, static_cast<int>(std::size(outputHeights)) - 1)];
    if (rows > 0 && swapChainExtent.height > 0) {
        double aspect = static_cast<double>(swapChainExtent.width) / swapChainExtent.height;
        outputExtent = {static_cast<uint32_t>(std::lround(rows * aspect / 2.0)) * 2, rows};  // even, like video sizes
    }
    renderExtent = dlssActive() ? dlssRenderExtent(activeRenderMode, outputExtent) : outputExtent;
}

// A new RenderSettings::outputSize: new images. Called once per frame before drawing.
void Renderer::applyOutputSize() {
    settings.outputSize = std::clamp(settings.outputSize, 0, static_cast<int>(std::size(outputHeights)) - 1);
    if (settings.outputSize == activeOutputSize) {
        return;
    }
    vkp.deviceWaitIdle(device);
    freeDlssResources();  // made for the old size
    activeOutputSize = settings.outputSize;
    recreateRenderImages();
    hasPreviousViewProjection = false;
}

// Everything whose size depends on the window or the render mode. The GPU must be idle.
void Renderer::recreateRenderImages() {
    destroyRayTracingImages();
    destroyDisplayImages();
    chooseExtents();
    createRayTracingImages();
    createDisplayImages();
    updateRayTracingDescriptorSet();
    updateExposureDescriptorSet();
    updateDisplayDescriptorSet();
    updateNoiseDescriptorSets();  // the displayed image changed (and the reference may no longer fit)
    resetAccumulation = true;
    dlssReset = true;
}

// What this frame's settings need at full size (see PerPixelBuffers): ReSTIR's buffers take 364 bytes per render
// pixel (GI 120 of them), the guides 34, the history 8, 3.4 GB at 4K. By default Accumulation needs none, the DLSS
// modes ReSTIR DI's and the guides.
Renderer::PerPixelBuffers Renderer::wantedPerPixelBuffers() {
    PerPixelBuffers wanted;
    wanted.restirGi = settings.restirGi(settings.renderMode);
    wanted.restir = settings.restirDi(settings.renderMode) || wanted.restirGi;
    wanted.guides = dlssActive() || isGuideView(settings.debugView);
    wanted.history = settings.debugView == static_cast<int>(DebugView::GuideReprojectionError);
    return wanted;
}

// A feature that needs one of them was switched on (or the last one using it off): new images. Called once per frame
// before drawing.
void Renderer::applyPerPixelBuffers() {
    if (wantedPerPixelBuffers() == perPixelBuffers) {
        return;
    }
    vkp.deviceWaitIdle(device);
    recreateRenderImages();
}

VkImageView Renderer::displayedHdrView() const {
    return dlssActive() ? dlssOutputImage.view : radianceImageView;
}

void Renderer::createDisplayImages() {
    // The display image gets blitted into the swapchain, which converts the format (RGBA -> BGRA, linear -> sRGB).
    VkFormatProperties swapchainFormatProperties;
    vkGetPhysicalDeviceFormatProperties(physicalDevice, swapChainImageFormat, &swapchainFormatProperties);
    if (!(swapchainFormatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
        throw std::runtime_error("swap chain format does not support blitting into it!");
    }

    // Transitioned every frame (written here, then read by the blit), so no initial layout change is needed
    createImage(outputExtent.width, outputExtent.height, displayFormat,
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                displayImage, displayImageMemory, displayImageView, 1, true);

    // DLSS modes: where DLSS writes the denoised, upscaled frame. Stays in GENERAL.
    if (dlssActive()) {
        createImage(outputExtent, dlssOutputFormat, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    dlssOutputImage);
        immediateSubmit([&](VkCommandBuffer cmd) {
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = dlssOutputImage.image;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &barrier);
        });
    }
}

void Renderer::destroyDisplayImages() {
    vkDestroyImageView(device, displayImageView, nullptr);
    vkDestroyImage(device, displayImage, nullptr);
    vmaFreeMemory(allocator, displayImageMemory);
    if (dlssOutputImage.image != VK_NULL_HANDLE) {
        destroyImage(dlssOutputImage);
    }
}

// Descriptors: 0 = HDR image (read: DLSS output or the radiance image), 1 = display image (write),
// 2 = exposure state (read), 3-9 = the DLSS input images and the history image (read, for the guide debug views),
// 10 = the radiance image at render size (read, for the trace shader's debug views and, in its alpha, the fog amount
// for the fog grain)
static const uint32_t displayBindingCount = 11;

void Renderer::createDisplayResources() {
    VkDescriptorSetLayoutBinding bindings[displayBindingCount];
    for (uint32_t i = 0; i < displayBindingCount; i++) {
        VkDescriptorType type = i == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[i] = {i, type, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = displayBindingCount;
    layoutInfo.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &displayDescriptorSetLayout) != VK_SUCCESS) {
        throw std::runtime_error("failed to create display descriptor set layout!");
    }

    VkDescriptorPoolSize poolSizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, displayBindingCount - 1},
                                        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &displayDescriptorPool) != VK_SUCCESS) {
        throw std::runtime_error("failed to create display descriptor pool!");
    }

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = displayDescriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &displayDescriptorSetLayout;
    if (vkAllocateDescriptorSets(device, &allocInfo, &displayDescriptorSet) != VK_SUCCESS) {
        throw std::runtime_error("failed to allocate display descriptor set!");
    }
    updateDisplayDescriptorSet();

    VkPushConstantRange pushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(DisplayPushConstants)};
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &displayDescriptorSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &displayPipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("failed to create display pipeline layout!");
    }

    VkShaderModule shaderModule = createShaderModule(readFile("display.comp.spv"));
    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = shaderModule;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = displayPipelineLayout;
    if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &displayPipeline) != VK_SUCCESS) {
        throw std::runtime_error("failed to create display pipeline!");
    }
    vkDestroyShaderModule(device, shaderModule, nullptr);
}

// The images are recreated with the swapchain, the output size and the render mode; called from recreateRenderImages.
void Renderer::updateDisplayDescriptorSet() {
    VkImageView views[displayBindingCount] = {
            displayedHdrView(), displayImageView, VK_NULL_HANDLE,
            linearDepthImage.view, motionVectorImage.view, diffuseAlbedoImage.view, specularAlbedoImage.view,
            normalRoughnessImage.view, specularHitDistanceImage.view, guideHistoryImage.view,
            radianceImageView,
    };
    VkDescriptorImageInfo imageInfos[displayBindingCount]{};
    VkDescriptorBufferInfo exposureInfo{exposureBuffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet writes[displayBindingCount]{};
    for (uint32_t i = 0; i < displayBindingCount; i++) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = displayDescriptorSet;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        if (i == 2) {
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &exposureInfo;
        } else {
            imageInfos[i] = {VK_NULL_HANDLE, views[i], VK_IMAGE_LAYOUT_GENERAL};
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[i].pImageInfo = &imageInfos[i];
        }
    }
    vkUpdateDescriptorSets(device, displayBindingCount, writes, 0, nullptr);
}

// Recorded after the trace and the exposure measurement. displayImage is already in GENERAL (transitioned at the
// start of the frame, see recordRayTracing); the blit after this pass transitions it again.
void Renderer::recordDisplay(VkCommandBuffer commandBuffer) {
    // The HDR image (trace) and the exposure (exposure.comp) must be written before this pass reads them.
    // Compute -> compute only: waiting for transfers here would also wait for the exposure readback copy.
    VkMemoryBarrier memoryBarrier{};
    memoryBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    memoryBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    memoryBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &memoryBarrier, 0, nullptr, 0, nullptr);

    // Auto: the measured exposure (read on the GPU) times the compensation. Manual: the slider.
    DisplayPushConstants push{};
    push.exposure = settings.autoExposure ? std::exp2(settings.exposureCompensation) : settings.exposure;
    push.autoExposure = settings.autoExposure ? 1 : 0;
    push.toneMapping = settings.toneMapping ? 1 : 0;
    // Guide views map one to one onto DisplayView, starting at Depth
    DisplayView view = DisplayView::Color;
    if (isGuideView(settings.debugView)) {
        view = static_cast<DisplayView>(static_cast<uint32_t>(DisplayView::Depth) + settings.debugView
                                        - static_cast<int>(DebugView::GuideDepth));
    } else if (settings.debugView != static_cast<int>(DebugView::None) &&
               settings.debugView != static_cast<int>(DebugView::RadianceCache)) {
        view = DisplayView::PassThrough;  // values as they are (the radiance cache view is light: shown like the image)
    }
    push.view = static_cast<uint32_t>(view);
    push.farDistance = Configuration::cameraFar;
    push.grainStrength = settings.fogGrain ? settings.fogGrainStrength : 0.0f;
    push.grainSize = std::max(settings.fogGrainSize, 1.0f);
    push.frame = static_cast<uint32_t>(frameNumber);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, displayPipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, displayPipelineLayout, 0, 1,
                            &displayDescriptorSet, 0, nullptr);
    vkCmdPushConstants(commandBuffer, displayPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(commandBuffer, (outputExtent.width + 7) / 8, (outputExtent.height + 7) / 8, 1);

    // Reprojection error view: keep this frame's diffuse albedo for the next frame to compare against.
    // (The display pass above has read the old history; the trace shader wrote the diffuse albedo before that.)
    if (view == DisplayView::ReprojectionError && perPixelBuffers.history) {
        VkMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 1, &barrier, 0, nullptr, 0, nullptr);
        VkImageCopy copy{};
        copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.extent = {renderExtent.width, renderExtent.height, 1};
        vkCmdCopyImage(commandBuffer, diffuseAlbedoImage.image, VK_IMAGE_LAYOUT_GENERAL,
                       guideHistoryImage.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
        // Next frame: the display pass reads the history (the frame-start barrier covers the trace's writes)
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &barrier, 0, nullptr, 0, nullptr);
    }
}

void Renderer::destroyDisplayResources() {
    vkDestroyPipeline(device, displayPipeline, nullptr);
    vkDestroyPipelineLayout(device, displayPipelineLayout, nullptr);
    vkDestroyDescriptorPool(device, displayDescriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(device, displayDescriptorSetLayout, nullptr);
}
