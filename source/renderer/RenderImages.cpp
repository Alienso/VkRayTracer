//
// The renderer's images and their sizes: the output size (the window, or a fixed one), the render size (smaller in the
// DLSS modes), and remaking everything that depends on them when the window, the output size, the render mode or a
// feature needing per-pixel buffers changes.
//

#include <vulkan/vulkan.h>
#include <algorithm>
#include <cmath>
#include <iterator>
#include "renderer/Renderer.h"

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
    renderExtent = dlssActive() ? dlss.dlssRenderExtent(activeRenderMode, outputExtent) : outputExtent;
}

// A new RenderSettings::outputSize: new images. Called once per frame before drawing.
void Renderer::applyOutputSize() {
    settings.outputSize = std::clamp(settings.outputSize, 0, static_cast<int>(std::size(outputHeights)) - 1);
    if (settings.outputSize == activeOutputSize) {
        return;
    }
    vkp.deviceWaitIdle(gpu.device);
    dlss.freeDlssResources();  // made for the old size
    activeOutputSize = settings.outputSize;
    recreateRenderImages();
    hasPreviousViewProjection = false;
}

// Everything whose size depends on the window or the render mode. The GPU must be idle.
void Renderer::recreateRenderImages() {
    trace.destroyRayTracingImages();
    destroyDisplayImages();
    chooseExtents();
    trace.createRayTracingImages();
    createDisplayImages();
    trace.updateRayTracingDescriptorSet();
    exposure.updateExposureDescriptorSet();
    display.updateDisplayDescriptorSet();
    noise.updateNoiseDescriptorSets();  // the displayed image changed (and the reference may no longer fit)
    resetAccumulation = true;
    dlssReset = true;
}

// What this frame's settings need at full size (see PerPixelBuffers): ReSTIR's buffers take 364 bytes per render
// pixel (GI 120 of them), the guides 34, the history 8, 3.4 GB at 4K. By default Accumulation needs none, the DLSS
// modes ReSTIR DI's and the guides.
PerPixelBuffers Renderer::wantedPerPixelBuffers() {
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
    if (wantedPerPixelBuffers() == trace.perPixelBuffers) {
        return;
    }
    vkp.deviceWaitIdle(gpu.device);
    recreateRenderImages();
}

VkImageView Renderer::displayedHdrView() const {
    return dlssActive() ? dlssOutputImage.view : trace.radianceImageView;
}

void Renderer::createDisplayImages() {
    display.createDisplayImage();

    // DLSS modes: where DLSS writes the denoised, upscaled frame. Stays in GENERAL.
    if (dlssActive()) {
        gpu.createImage(outputExtent, dlssOutputFormat, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        dlssOutputImage);
        gpu.immediateSubmit([&](VkCommandBuffer cmd) {
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
    display.destroyDisplayImage();
    if (dlssOutputImage.image != VK_NULL_HANDLE) {
        gpu.destroyImage(dlssOutputImage);
    }
}

// Called once per frame before drawing: switches between accumulation and the DLSS modes (new render size), and
// between DLSS models (DLSS starts over with the new network).
void Renderer::applyRenderMode() {
    if (!dlss.dlssSupported) {
        settings.renderMode = static_cast<int>(RenderMode::Accumulation);
    }
    if (!dlss.dlssModelAvailable(settings.dlssModel)) {
        // The newest model the loaded DLSS version has
        settings.dlssModel = static_cast<int>(dlss.dlssCnnVersion ? DlssModel::TransformerE : DlssModel::TransformerF);
    }
    if (settings.dlssModel != dlss.activeDlssModel) {
        vkp.deviceWaitIdle(gpu.device);
        dlss.freeDlssResources();
        dlss.activeDlssModel = settings.dlssModel;
        dlssReset = true;
    }
    if (settings.renderMode == activeRenderMode) {
        return;
    }
    vkp.deviceWaitIdle(gpu.device);
    dlss.freeDlssResources();
    activeRenderMode = settings.renderMode;
    recreateRenderImages();
    hasPreviousViewProjection = false;
    jitterIndex = 0;
}
