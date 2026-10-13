//
// Frame capture: the "Capture" button saves the displayed image (after exposure and tone mapping, without the UI)
// as a PNG next to the executable, e.g. capture_2026-10-07_14-32-05.png.
//
// The display image is copied into a host-visible buffer in the frame's own command buffer, right after the blit
// to the swapchain reads it. Once that frame's fence has signaled (at the start of the next loop iteration), the
// pixels are read and written with stb_image_write.
//

#include <vulkan/vulkan.h>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <stb_image_write.h>
#include "renderer/Renderer.h"

// Records the copy display image -> capture buffer. The display image is in TRANSFER_SRC_OPTIMAL (after the blit).
void Renderer::recordCapture(VkCommandBuffer commandBuffer) {
    if (!captureRequested || captureBuffer != VK_NULL_HANDLE) {
        return;
    }
    captureRequested = false;
    captureExtent = outputExtent;
    captureFrame = currentFrame;
    createBuffer(static_cast<VkDeviceSize>(captureExtent.width) * captureExtent.height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, captureBuffer, captureMemory);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {captureExtent.width, captureExtent.height, 1};
    vkCmdCopyImageToBuffer(commandBuffer, displayImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, captureBuffer, 1, &region);

    // The CPU reads it after the fence
    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = captureBuffer;
    barrier.offset = 0;
    barrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, nullptr, 1, &barrier, 0, nullptr);
}

// A file name that doesn't exist yet: capture_<date>_<time>.png, with _2, _3, ... for several in the same second
static std::string captureFileName() {
    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    char stamp[64];
    std::strftime(stamp, sizeof(stamp), "capture_%Y-%m-%d_%H-%M-%S", &local);
    std::string name = std::string(stamp) + ".png";
    for (int i = 2; std::filesystem::exists(name); i++) {
        name = std::string(stamp) + "_" + std::to_string(i) + ".png";
    }
    return name;
}

// Writes the capture once its frame has finished on the GPU. Called once per loop iteration, before the UI is built.
void Renderer::finishCapture() {
    if (captureBuffer == VK_NULL_HANDLE) {
        return;
    }
    // The fence of the frame that recorded the copy: not reset again before that slot's next drawFrame
    vkWaitForFences(device, 1, &inFlightFences[captureFrame], VK_TRUE, UINT64_MAX);

    // The display image holds linear values (the blit to the sRGB swapchain encodes them): encode the same way, so the
    // file looks like the screen
    uint8_t toSrgb[256];
    for (int i = 0; i < 256; i++) {
        float c = i / 255.0f;
        float s = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
        toSrgb[i] = static_cast<uint8_t>(std::lround(std::clamp(s, 0.0f, 1.0f) * 255.0f));
    }
    size_t pixelCount = static_cast<size_t>(captureExtent.width) * captureExtent.height;
    std::vector<uint8_t> pixels(pixelCount * 4);
    void* mapped = nullptr;
    vmaMapMemory(allocator, captureMemory, &mapped);
    const uint8_t* source = static_cast<const uint8_t*>(mapped);
    for (size_t i = 0; i < pixelCount; i++) {
        pixels[i * 4] = toSrgb[source[i * 4]];
        pixels[i * 4 + 1] = toSrgb[source[i * 4 + 1]];
        pixels[i * 4 + 2] = toSrgb[source[i * 4 + 2]];
        pixels[i * 4 + 3] = 255;
    }
    vmaUnmapMemory(allocator, captureMemory);
    vkDestroyBuffer(device, captureBuffer, nullptr);
    vmaFreeMemory(allocator, captureMemory);
    captureBuffer = VK_NULL_HANDLE;

    std::string name = captureFileName();
    if (stbi_write_png(name.c_str(), static_cast<int>(captureExtent.width), static_cast<int>(captureExtent.height), 4,
                       pixels.data(), static_cast<int>(captureExtent.width) * 4)) {
        captureMessage = "saved " + name;
    } else {
        captureMessage = "could not write " + name;
    }
    std::cout << "capture: " << captureMessage << std::endl;
}
