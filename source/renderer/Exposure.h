//
// Automatic exposure: a histogram of the displayed HDR image's brightness and the exposure adapted to it, on the GPU,
// read back for the UI and the uniforms. The display pass reads the exposure from exposureBuffer. See Exposure.cpp.
//

#ifndef VULKANDEMO_EXPOSURE_H
#define VULKANDEMO_EXPOSURE_H

#include <vulkan/vulkan.h>
#include <functional>
#include <vector>
#include "renderer/GpuContext.h"
#include "renderer/RendererTypes.h"

class ExposurePass {
public:
    // The renderer's: settings (the UI edits them), frame slot, output size, frame time, and the HDR image shown
    // (the radiance image, or DLSS's output)
    ExposurePass(GpuContext& gpu, const RenderSettings& settings, const uint32_t& currentFrame,
                 const VkExtent2D& outputExtent, const float& frameDeltaTime,
                 std::function<VkImageView()> displayedHdrView)
        : gpu(gpu), settings(settings), currentFrame(currentFrame), outputExtent(outputExtent),
          frameDeltaTime(frameDeltaTime), displayedHdrView(std::move(displayedHdrView)) {}

    void createExposureResources();
    void updateExposureDescriptorSet();
    void recordExposure(VkCommandBuffer commandBuffer);
    void destroyExposureResources();

    VkBuffer exposureBuffer;
    VmaAllocation exposureMemory;
    std::vector<VkBuffer> exposureReadbackBuffers;  // one per frame in flight
    std::vector<VmaAllocation> exposureReadbackMemories;
    std::vector<void*> exposureReadbackMapped;
    ExposureState exposureInfo{};  // latest copy for the UI
    bool exposureInfoValid = false;
    bool exposureReset = true;     // snap to the target instead of adapting (start, auto just switched on)
    bool exposureWasAuto = false;
    VkDescriptorSetLayout exposureDescriptorSetLayout;
    VkDescriptorPool exposureDescriptorPool;
    VkDescriptorSet exposureDescriptorSet;
    VkPipelineLayout exposurePipelineLayout;
    VkPipeline exposurePipeline;

private:
    GpuContext& gpu;
    const RenderSettings& settings;
    const uint32_t& currentFrame;
    const VkExtent2D& outputExtent;
    const float& frameDeltaTime;
    std::function<VkImageView()> displayedHdrView;
};

#endif //VULKANDEMO_EXPOSURE_H
