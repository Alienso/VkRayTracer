//
// Frame capture: the displayed image of a frame (after exposure and tone mapping, without the UI) as a PNG. See
// Capture.cpp.
//

#ifndef VULKANDEMO_CAPTURE_H
#define VULKANDEMO_CAPTURE_H

#include <vulkan/vulkan.h>
#include <string>
#include <vector>
#include "renderer/GpuContext.h"

// request sets captureRequested, the frame copies the display image into captureBuffer (recordCapture), the next loop
// iteration writes it as a PNG (finishCapture)
class FrameCapture {
public:
    // The renderer's: its frame fences and slot, the display image and its size
    FrameCapture(GpuContext& gpu, const std::vector<VkFence>& inFlightFences, const uint32_t& currentFrame,
                 const VkImage& displayImage, const VkExtent2D& outputExtent)
        : gpu(gpu), inFlightFences(inFlightFences), currentFrame(currentFrame), displayImage(displayImage),
          outputExtent(outputExtent) {}

    // The next frame's image, without the UI, as a PNG: into fileName, or capture_<date>_<time>.png when empty.
    // Written at the start of the frame after (finishCapture).
    void request(const std::string& fileName) {
        captureRequested = true;
        captureFileRequested = fileName;
    }
    [[nodiscard]] bool pending() const { return captureRequested || captureBuffer != VK_NULL_HANDLE; }

    // Records the copy display image -> capture buffer (after the blit to the swapchain)
    void recordCapture(VkCommandBuffer commandBuffer);
    // Writes the capture once its frame has finished on the GPU. Once per loop iteration, before the UI is built.
    void finishCapture();

    bool captureRequested = false;
    std::string captureFileRequested;  // "": a name from the date and time
    std::string captureFile;           // of the copy recorded
    VkBuffer captureBuffer = VK_NULL_HANDLE;
    VmaAllocation captureMemory = VK_NULL_HANDLE;
    VkExtent2D captureExtent{};
    uint32_t captureFrame = 0;   // frame in flight that recorded the copy
    std::string captureMessage;  // shown next to the button

private:
    GpuContext& gpu;
    const std::vector<VkFence>& inFlightFences;
    const uint32_t& currentFrame;
    const VkImage& displayImage;
    const VkExtent2D& outputExtent;
};

#endif //VULKANDEMO_CAPTURE_H
