//
// The editor's picks and outline on the GPU. See Selection.h.
//

#include <vulkan/vulkan.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include "renderer/Selection.h"
#include "core/Configuration.h"

std::vector<char> readFile(const std::string& filename);

// Must match SelectionPush in shaders/selection.comp
struct SelectionPushConstants {
    glm::vec4 origin;
    glm::vec4 forward;
    glm::vec4 right;
    glm::vec4 up;
    glm::ivec4 rect;
    glm::vec2 pickPoint;
    uint32_t mode;
    uint32_t rangeCount;
    glm::uvec2 outputSize;
    uint32_t pickSlot;
    uint32_t padding;
};
static_assert(sizeof(SelectionPushConstants) == 112, "must match the push constants in shaders/selection.comp");

static const VkFormat maskFormat = VK_FORMAT_R8_UNORM;
static const int outlineMargin = 4;  // pixels around the boxes' rectangle (the outline is drawn outside the mask)

void SelectionPass::createMaskImage() {
    gpu.createImage(outputExtent, maskFormat, VK_IMAGE_USAGE_STORAGE_BIT, maskImage);
    gpu.immediateSubmit([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = maskImage.image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
    });
    outlineRect = glm::ivec4(0);
}

void SelectionPass::destroyMaskImage() {
    if (maskImage.image != VK_NULL_HANDLE) {
        gpu.destroyImage(maskImage);
    }
}

void SelectionPass::createResources() {
    VkDescriptorSetLayoutBinding bindings[3] = {
            {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 3;
    layoutInfo.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(gpu.device, &layoutInfo, nullptr, &setLayout) != VK_SUCCESS) {
        throw std::runtime_error("failed to create the selection descriptor set layout!");
    }
    VkDescriptorPoolSize poolSizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    if (vkCreateDescriptorPool(gpu.device, &poolInfo, nullptr, &pool) != VK_SUCCESS) {
        throw std::runtime_error("failed to create the selection descriptor pool!");
    }
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = pool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &setLayout;
    if (vkAllocateDescriptorSets(gpu.device, &allocInfo, &set) != VK_SUCCESS) {
        throw std::runtime_error("failed to allocate the selection descriptor set!");
    }

    gpu.createBuffer(maxRanges * sizeof(glm::uvec2), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, rangeBuffer, rangeMemory);
    gpu.createBuffer(Configuration::maxFramesInFlight * sizeof(glm::uvec4), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, pickBuffer, pickMemory);
    vmaMapMemory(gpu.allocator, pickMemory, &pickMapped);
    std::memset(pickMapped, 0, Configuration::maxFramesInFlight * sizeof(glm::uvec4));
    slotPickRevision.assign(Configuration::maxFramesInFlight, -1);
    rangesUploaded = false;
    updateDescriptorSet();

    VkDescriptorSetLayout setLayouts[2] = {trace.rtDescriptorSetLayout, setLayout};
    VkPushConstantRange pushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SelectionPushConstants)};
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 2;
    pipelineLayoutInfo.pSetLayouts = setLayouts;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(gpu.device, &pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("failed to create the selection pipeline layout!");
    }
    VkShaderModule shaderModule = gpu.createShaderModule(readFile(Configuration::shaderPath("selection.comp.spv")));
    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = shaderModule;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = pipelineLayout;
    if (vkCreateComputePipelines(gpu.device, gpu.pipelineCache, 1, &pipelineInfo, nullptr, &pipeline) != VK_SUCCESS) {
        throw std::runtime_error("failed to create the selection pipeline!");
    }
    vkDestroyShaderModule(gpu.device, shaderModule, nullptr);
}

void SelectionPass::updateDescriptorSet() {
    if (set == VK_NULL_HANDLE || maskImage.view == VK_NULL_HANDLE) {
        return;
    }
    VkDescriptorImageInfo maskInfo{VK_NULL_HANDLE, maskImage.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo rangeInfo{rangeBuffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo pickInfo{pickBuffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet writes[3]{};
    for (uint32_t i = 0; i < 3; i++) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    }
    writes[0].pImageInfo = &maskInfo;
    writes[1].pBufferInfo = &rangeInfo;
    writes[2].pBufferInfo = &pickInfo;
    vkUpdateDescriptorSets(gpu.device, 3, writes, 0, nullptr);
}

void SelectionPass::destroyResources() {
    vkDestroyPipeline(gpu.device, pipeline, nullptr);
    vkDestroyPipelineLayout(gpu.device, pipelineLayout, nullptr);
    vkDestroyDescriptorPool(gpu.device, pool, nullptr);
    vkDestroyDescriptorSetLayout(gpu.device, setLayout, nullptr);
    vmaUnmapMemory(gpu.allocator, pickMemory);
    vkDestroyBuffer(gpu.device, pickBuffer, nullptr);
    vmaFreeMemory(gpu.allocator, pickMemory);
    vkDestroyBuffer(gpu.device, rangeBuffer, nullptr);
    vmaFreeMemory(gpu.allocator, rangeMemory);
}

void SelectionPass::requestPick(glm::vec2 point, uint64_t instanceRevision) {
    pickRequested = true;
    pickPoint = point;
    pickRevision = instanceRevision;
}

void SelectionPass::readPick() {
    int64_t revision = slotPickRevision.empty() ? -1 : slotPickRevision[currentFrame];
    if (revision < 0) {
        return;
    }
    glm::uvec4 result;
    std::memcpy(&result, static_cast<const char*>(pickMapped) + currentFrame * sizeof(glm::uvec4), sizeof(result));
    pickResult = PickResult{result.x != 0, result.y, static_cast<uint64_t>(revision)};
    slotPickRevision[currentFrame] = -1;
}

std::optional<PickResult> SelectionPass::takePick() {
    std::optional<PickResult> result = pickResult;
    pickResult.reset();
    return result;
}

void SelectionPass::setSelection(std::vector<InstanceRange> newRanges, std::vector<WorldBox> newBoxes) {
    if (newRanges.size() > maxRanges) {
        newRanges.resize(maxRanges);  // (more than that many separate pieces: the rest has no outline)
    }
    ranges = std::move(newRanges);
    boxes = std::move(newBoxes);
}

// The output pixels the boxes cover (with a margin for the outline), clamped to the image; the whole image when a
// box reaches behind the camera
glm::ivec4 SelectionPass::screenRect(const glm::mat4& viewProjection) const {
    glm::ivec4 whole(0, 0, static_cast<int>(outputExtent.width), static_cast<int>(outputExtent.height));
    glm::vec2 low(1e30f), high(-1e30f);
    for (const WorldBox& box : boxes) {
        glm::mat4 toClip = viewProjection * box.transform;
        for (int corner = 0; corner < 8; corner++) {
            glm::vec3 p((corner & 1) ? box.max.x : box.min.x, (corner & 2) ? box.max.y : box.min.y,
                        (corner & 4) ? box.max.z : box.min.z);
            glm::vec4 clip = toClip * glm::vec4(p, 1.0f);
            if (clip.w <= 1e-4f) {
                return whole;
            }
            glm::vec2 pixel = (glm::vec2(clip) / clip.w * 0.5f + 0.5f) *
                              glm::vec2(outputExtent.width, outputExtent.height);
            low = glm::min(low, pixel);
            high = glm::max(high, pixel);
        }
    }
    if (boxes.empty()) {
        return glm::ivec4(0);
    }
    glm::ivec4 rect(static_cast<int>(std::floor(low.x)) - outlineMargin, static_cast<int>(std::floor(low.y)) - outlineMargin,
                    static_cast<int>(std::ceil(high.x)) + outlineMargin, static_cast<int>(std::ceil(high.y)) + outlineMargin);
    rect = glm::clamp(rect, glm::ivec4(0), glm::ivec4(whole.z, whole.w, whole.z, whole.w));
    return rect.x < rect.z && rect.y < rect.w ? rect : glm::ivec4(0);
}

void SelectionPass::record(VkCommandBuffer commandBuffer, const Camera& camera, const glm::mat4& viewProjection) {
    outlineRect = ranges.empty() ? glm::ivec4(0) : screenRect(viewProjection);
    bool outline = outlineRect.x < outlineRect.z;
    if (!pickRequested && !outline) {
        return;
    }

    if (outline && (!rangesUploaded || ranges.size() != uploadedRanges.size() ||
                    !std::equal(ranges.begin(), ranges.end(), uploadedRanges.begin(), [](const InstanceRange& a, const InstanceRange& b) {
                        return a.first == b.first && a.count == b.count;
                    }))) {
        // Last frame's selection pass may still read the ranges
        VkMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 1, &barrier, 0, nullptr, 0, nullptr);
        std::vector<glm::uvec2> data;
        data.reserve(ranges.size());
        for (const InstanceRange& range : ranges) {
            data.emplace_back(range.first, range.count);
        }
        // (vkCmdUpdateBuffer takes at most 65536 bytes: 8192 ranges)
        vkCmdUpdateBuffer(commandBuffer, rangeBuffer, 0, data.size() * sizeof(glm::uvec2), data.data());
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &barrier, 0, nullptr, 0, nullptr);
        uploadedRanges = ranges;
        rangesUploaded = true;
    }

    float aspect = static_cast<float>(outputExtent.width) / static_cast<float>(outputExtent.height);
    float tanHalfFov = std::tan(glm::radians(camera.fov) * 0.5f);
    SelectionPushConstants push{};
    push.origin = glm::vec4(camera.position, 1.0f);
    push.forward = glm::vec4(camera.forward(), 0.0f);
    push.right = glm::vec4(camera.right() * tanHalfFov * aspect, 0.0f);
    push.up = glm::vec4(camera.up() * tanHalfFov, 0.0f);
    push.outputSize = glm::uvec2(outputExtent.width, outputExtent.height);
    push.rangeCount = static_cast<uint32_t>(ranges.size());
    push.pickSlot = currentFrame;

    VkDescriptorSet sets[2] = {trace.rtDescriptorSet, set};
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 2, sets, 0, nullptr);
    if (pickRequested) {
        push.mode = 1;
        push.pickPoint = pickPoint * glm::vec2(outputExtent.width, outputExtent.height);
        vkCmdPushConstants(commandBuffer, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(commandBuffer, 1, 1, 1);
        // The CPU reads it once the frame is done (readPick)
        VkMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 1, &barrier, 0, nullptr, 0, nullptr);
        slotPickRevision[currentFrame] = static_cast<int64_t>(pickRevision);
        pickRequested = false;
    }
    if (outline) {
        push.mode = 0;
        push.rect = outlineRect;
        vkCmdPushConstants(commandBuffer, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        uint32_t width = static_cast<uint32_t>(outlineRect.z - outlineRect.x);
        uint32_t height = static_cast<uint32_t>(outlineRect.w - outlineRect.y);
        vkCmdDispatch(commandBuffer, (width + 7) / 8, (height + 7) / 8, 1);
    }
}
