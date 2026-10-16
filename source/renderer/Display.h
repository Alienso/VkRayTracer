//
// Display pass: the last step before the screen. The linear HDR image (the radiance image, or DLSS's output) with
// exposure and tone mapping (shaders/display.comp) into an 8-bit image at output size, which the renderer blits into
// the swapchain; or one of the debug views. The editor's selection outline is drawn here too. See Display.cpp.
//

#ifndef VULKANDEMO_DISPLAY_H
#define VULKANDEMO_DISPLAY_H

#include <vulkan/vulkan.h>
#include <functional>
#include "renderer/GpuContext.h"
#include "renderer/RendererTypes.h"

class DisplayPass {
public:
    // The renderer's: settings, swapchain format, output and render size, this frame's fog level and number, which
    // per-pixel buffers exist, the exposure buffer, and the images it reads (the trace pass's, the HDR image shown)
    DisplayPass(GpuContext& gpu, const RenderSettings& settings, const VkFormat& swapChainImageFormat,
                const VkExtent2D& outputExtent, const VkExtent2D& renderExtent, const uint32_t& fogLevel,
                const uint64_t& frameNumber, const PerPixelBuffers& perPixelBuffers, const VkBuffer& exposureBuffer,
                const VkImageView& radianceImageView, const RenderImage& linearDepthImage,
                const RenderImage& motionVectorImage, const RenderImage& diffuseAlbedoImage,
                const RenderImage& specularAlbedoImage, const RenderImage& normalRoughnessImage,
                const RenderImage& specularHitDistanceImage, const RenderImage& guideHistoryImage,
                const RenderImage& selectionMask, const glm::ivec4& outlineRect,
                std::function<VkImageView()> displayedHdrView)
        : gpu(gpu), settings(settings), swapChainImageFormat(swapChainImageFormat), outputExtent(outputExtent),
          renderExtent(renderExtent), fogLevel(fogLevel), frameNumber(frameNumber), perPixelBuffers(perPixelBuffers),
          exposureBuffer(exposureBuffer), radianceImageView(radianceImageView), linearDepthImage(linearDepthImage),
          motionVectorImage(motionVectorImage), diffuseAlbedoImage(diffuseAlbedoImage),
          specularAlbedoImage(specularAlbedoImage), normalRoughnessImage(normalRoughnessImage),
          specularHitDistanceImage(specularHitDistanceImage), guideHistoryImage(guideHistoryImage),
          selectionMask(selectionMask), outlineRect(outlineRect), displayedHdrView(std::move(displayedHdrView)) {}

    // The display image, at output size (recreated with it)
    void createDisplayImage();
    void destroyDisplayImage();
    void createDisplayResources();
    void updateDisplayDescriptorSet();  // the images were recreated
    void recordDisplay(VkCommandBuffer commandBuffer);
    void destroyDisplayResources();

    VkImage displayImage;
    VmaAllocation displayImageMemory;
    VkImageView displayImageView;
    VkDescriptorSetLayout displayDescriptorSetLayout;
    VkDescriptorPool displayDescriptorPool;
    VkDescriptorSet displayDescriptorSet;
    VkPipelineLayout displayPipelineLayout;
    VkPipeline displayPipeline;

private:
    GpuContext& gpu;
    const RenderSettings& settings;
    const VkFormat& swapChainImageFormat;
    const VkExtent2D& outputExtent;
    const VkExtent2D& renderExtent;
    const uint32_t& fogLevel;
    const uint64_t& frameNumber;
    const PerPixelBuffers& perPixelBuffers;
    const VkBuffer& exposureBuffer;
    const VkImageView& radianceImageView;
    const RenderImage& linearDepthImage;
    const RenderImage& motionVectorImage;
    const RenderImage& diffuseAlbedoImage;
    const RenderImage& specularAlbedoImage;
    const RenderImage& normalRoughnessImage;
    const RenderImage& specularHitDistanceImage;
    const RenderImage& guideHistoryImage;
    const RenderImage& selectionMask;  // the editor's outline (SelectionPass)
    const glm::ivec4& outlineRect;
    std::function<VkImageView()> displayedHdrView;
};

#endif //VULKANDEMO_DISPLAY_H
