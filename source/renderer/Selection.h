//
// Selection pass: the editor's clicks and outline (shaders/selection.comp). A pick traces the camera ray through the
// clicked point and reads back the instance it hits; the outline traces the camera rays over the rectangle the
// selection covers on the image (at output size) and marks the pixels whose first surface is selected, for the
// display pass to draw the outline around. Both stop at the first surface like the path tracer's rays (cut-outs,
// invisible surfaces passed), through the trace pass's descriptor set.
//

#ifndef VULKANDEMO_SELECTION_H
#define VULKANDEMO_SELECTION_H

#include <vulkan/vulkan.h>
#include <optional>
#include <vector>
#include <glm/glm.hpp>
#include "renderer/GpuContext.h"
#include "renderer/RayTracer.h"
#include "renderer/RendererTypes.h"
#include "scene/RenderList.h"

// A pick's answer: the TLAS instance (RenderList::instances) the ray hit, for the list of that revision
struct PickResult {
    bool hit = false;
    uint32_t instance = 0;
    uint64_t instanceRevision = 0;  // RenderList::instanceRevision when it was traced
};

class SelectionPass {
public:
    static constexpr uint32_t maxRanges = 4096;  // instance ranges outlined (merged when next to each other)

    // The renderer's: the trace pass (its descriptor set: the TLAS, geometry, materials), the output size, the frame
    // slot
    SelectionPass(GpuContext& gpu, RayTracer& trace, const VkExtent2D& outputExtent, const uint32_t& currentFrame)
        : gpu(gpu), trace(trace), outputExtent(outputExtent), currentFrame(currentFrame) {}

    void createMaskImage();  // at output size, with the display image
    void destroyMaskImage();
    void createResources();  // after the trace pass's descriptors
    void updateDescriptorSet();  // the mask image was recreated
    void destroyResources();

    // A click at point (0..1 over the image, from the top left) for the list of that revision: traced in the next
    // frame, its answer from takePick once that frame is done
    void requestPick(glm::vec2 point, uint64_t instanceRevision);
    // At the start of a frame, its slot's fence waited: the pick that slot traced last time is in
    void readPick();
    std::optional<PickResult> takePick();

    // This frame's selection: its instance ranges and the boxes around it (world space); empty: no outline
    void setSelection(std::vector<InstanceRange> ranges, std::vector<WorldBox> boxes);
    // After the trace (the TLAS is built): the pick and the outline mask. viewProjection: this frame's, unjittered.
    void record(VkCommandBuffer commandBuffer, const Camera& camera, const glm::mat4& viewProjection);

    // Where the mask was written this frame (x0, y0, x1, y1 in output pixels; empty: no outline) and the mask
    glm::ivec4 outlineRect = glm::ivec4(0);
    RenderImage maskImage;  // R8: 1 where the first surface is selected (inside outlineRect)

private:
    GpuContext& gpu;
    RayTracer& trace;
    const VkExtent2D& outputExtent;
    const uint32_t& currentFrame;

    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkBuffer rangeBuffer = VK_NULL_HANDLE;  // uvec2 per range, written with vkCmdUpdateBuffer when it changes
    VmaAllocation rangeMemory = VK_NULL_HANDLE;
    VkBuffer pickBuffer = VK_NULL_HANDLE;   // uvec4 per frame slot, host visible
    VmaAllocation pickMemory = VK_NULL_HANDLE;
    void* pickMapped = nullptr;

    std::vector<InstanceRange> ranges;
    std::vector<InstanceRange> uploadedRanges;
    bool rangesUploaded = false;
    std::vector<WorldBox> boxes;
    // A click waiting to be traced, and per frame slot the one it traced (read back when the slot comes round)
    bool pickRequested = false;
    glm::vec2 pickPoint = glm::vec2(0.0f);
    uint64_t pickRevision = 0;
    std::vector<int64_t> slotPickRevision;  // -1: the slot traced no pick
    std::optional<PickResult> pickResult;

    glm::ivec4 screenRect(const glm::mat4& viewProjection) const;
};

#endif //VULKANDEMO_SELECTION_H
