//
// Automatic exposure: measures how bright the rendered image is and picks the exposure for it, like a
// camera's light meter. The measuring happens on the GPU (shaders/exposure.comp) right after tracing:
// a histogram of the accumulated image's brightness, then one small pass that turns it into an exposure.
// The trace shader uses that exposure on the next frame; the CPU only gets a copy for the UI.
//

#include <vulkan/vulkan.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include "../include/imgui/imgui.h"
#include "Application.h"
#include "Configuration.h"

std::vector<char> readFile(const std::string& filename);

// Histogram range in stops (log2 of luminance). Darker counts as black and isn't metered; brighter lands in
// the top bin (the sun disc is about 2^10). 32 stops in 255 bins = an eighth of a stop per bin.
static const float histogramMinLog2 = -20.0f;
static const float histogramLog2Range = 32.0f;

void Application::createExposureResources() {
    // State buffer: histogram counters start at 0, the exposure at 1 (used by the very first frame)
    createBuffer(sizeof(ExposureState),
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, exposureBuffer, exposureMemory);
    immediateSubmit([&](VkCommandBuffer cmd) {
        vkCmdFillBuffer(cmd, exposureBuffer, 0, VK_WHOLE_SIZE, 0);
        float one = 1.0f;
        uint32_t oneBits;
        std::memcpy(&oneBits, &one, sizeof(one));
        vkCmdFillBuffer(cmd, exposureBuffer, offsetof(ExposureState, exposure), sizeof(float), oneBits);
    });

    // Readback copies: the GPU writes the one of the current frame in flight, the CPU reads it once that
    // frame's fence has been waited on (at the start of recording the same frame slot again).
    exposureReadbackBuffers.resize(Configuration::maxFramesInFlight);
    exposureReadbackMemories.resize(Configuration::maxFramesInFlight);
    exposureReadbackMapped.resize(Configuration::maxFramesInFlight);
    for (int i = 0; i < Configuration::maxFramesInFlight; i++) {
        createBuffer(sizeof(ExposureState), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     exposureReadbackBuffers[i], exposureReadbackMemories[i]);
        vkMapMemory(device, exposureReadbackMemories[i], 0, sizeof(ExposureState), 0, &exposureReadbackMapped[i]);
        std::memset(exposureReadbackMapped[i], 0, sizeof(ExposureState));
    }

    // Descriptors: 0 = accumulation image (read), 1 = state buffer
    VkDescriptorSetLayoutBinding bindings[2] = {
            {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &exposureDescriptorSetLayout) != VK_SUCCESS) {
        throw std::runtime_error("failed to create exposure descriptor set layout!");
    }

    VkDescriptorPoolSize poolSizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &exposureDescriptorPool) != VK_SUCCESS) {
        throw std::runtime_error("failed to create exposure descriptor pool!");
    }

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = exposureDescriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &exposureDescriptorSetLayout;
    if (vkAllocateDescriptorSets(device, &allocInfo, &exposureDescriptorSet) != VK_SUCCESS) {
        throw std::runtime_error("failed to allocate exposure descriptor set!");
    }
    updateExposureDescriptorSet();

    // Pipeline: both passes use the same shader, the push constant "pass" picks one
    VkPushConstantRange pushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ExposurePushConstants)};
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &exposureDescriptorSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &exposurePipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("failed to create exposure pipeline layout!");
    }

    VkShaderModule shaderModule = createShaderModule(readFile("exposure.comp.spv"));
    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = shaderModule;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = exposurePipelineLayout;
    if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &exposurePipeline) != VK_SUCCESS) {
        throw std::runtime_error("failed to create exposure pipeline!");
    }
    vkDestroyShaderModule(device, shaderModule, nullptr);
}

// The accumulation image is recreated with the swapchain; called from updateRayTracingDescriptorSet.
void Application::updateExposureDescriptorSet() {
    VkDescriptorImageInfo imageInfo{VK_NULL_HANDLE, accumulationImageView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo bufferInfo{exposureBuffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet writes[2]{};
    for (int i = 0; i < 2; i++) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = exposureDescriptorSet;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
    }
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[0].pImageInfo = &imageInfo;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &bufferInfo;
    vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
}

// Recorded right after the trace dispatch. Skipped (no cost) while exposure is manual or a debug view is shown.
void Application::recordExposure(VkCommandBuffer commandBuffer) {
    // This frame slot's fence was waited on before recording: its copy from a few frames ago is complete.
    if (exposureInfoValid || exposureWasAuto) {
        std::memcpy(&exposureInfo, exposureReadbackMapped[currentFrame], sizeof(ExposureState));
        exposureInfoValid = exposureInfo.exposure > 0.0f;
    }

    bool active = settings.autoExposure && settings.debugView == static_cast<int>(DebugView::None);
    if (active && !exposureWasAuto) {
        exposureReset = true;  // just switched on: jump to the right exposure instead of fading in
    }
    exposureWasAuto = active;
    if (!active) {
        return;
    }

    // The trace shader's writes to the accumulation image must land before the histogram reads it
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);

    ExposurePushConstants push{};
    push.reset = exposureReset ? 1 : 0;
    push.centerWeighted = settings.centerWeighted ? 1 : 0;
    push.minLog2 = histogramMinLog2;
    push.log2Range = histogramLog2Range;
    push.lowFraction = settings.meterLowPercent / 100.0f;
    push.highFraction = std::max(settings.meterHighPercent, settings.meterLowPercent + 1.0f) / 100.0f;
    push.key = settings.exposureKey;
    push.minExposure = std::exp2(settings.minExposureStops);
    push.maxExposure = std::exp2(std::max(settings.maxExposureStops, settings.minExposureStops));
    push.brightenSpeed = settings.brightenSpeed;
    push.darkenSpeed = settings.darkenSpeed;
    push.deltaTime = std::min(frameDeltaTime, 0.25f);  // a long stall (loading a model) shouldn't jump
    exposureReset = false;

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, exposurePipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, exposurePipelineLayout, 0, 1,
                            &exposureDescriptorSet, 0, nullptr);

    // Pass 0: histogram, one invocation per pixel
    push.pass = 0;
    vkCmdPushConstants(commandBuffer, exposurePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(commandBuffer, (swapChainExtent.width + 15) / 16, (swapChainExtent.height + 15) / 16, 1);
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);

    // Pass 1: histogram -> exposure, a single workgroup
    push.pass = 1;
    vkCmdPushConstants(commandBuffer, exposurePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(commandBuffer, 1, 1, 1);

    // The result is read by the next frame's shaders (trace: exposure, histogram: counters) and copied for the UI
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);
    VkBufferCopy copy{0, 0, sizeof(ExposureState)};
    vkCmdCopyBuffer(commandBuffer, exposureBuffer, exposureReadbackBuffers[currentFrame], 1, &copy);

    VkMemoryBarrier hostBarrier{};
    hostBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    hostBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    hostBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 1, &hostBarrier, 0, nullptr, 0, nullptr);
}

void Application::destroyExposureResources() {
    vkDestroyPipeline(device, exposurePipeline, nullptr);
    vkDestroyPipelineLayout(device, exposurePipelineLayout, nullptr);
    vkDestroyDescriptorPool(device, exposureDescriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(device, exposureDescriptorSetLayout, nullptr);
    for (int i = 0; i < Configuration::maxFramesInFlight; i++) {
        vkUnmapMemory(device, exposureReadbackMemories[i]);
        vkDestroyBuffer(device, exposureReadbackBuffers[i], nullptr);
        vkFreeMemory(device, exposureReadbackMemories[i], nullptr);
    }
    vkDestroyBuffer(device, exposureBuffer, nullptr);
    vkFreeMemory(device, exposureMemory, nullptr);
}

// Part of the "Rendering" section
void Application::buildExposureUi() {
    ImGui::Checkbox("Auto exposure", &settings.autoExposure);
    if (!settings.autoExposure) {
        ImGui::SliderFloat("Exposure", &settings.exposure, 0.05f, 8.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
        return;
    }

    ImGui::SameLine();
    if (exposureInfoValid) {
        ImGui::Text("x%.2f (%+.1f stops)", exposureInfo.exposure * std::exp2(settings.exposureCompensation),
                    std::log2(exposureInfo.exposure) + settings.exposureCompensation);
    }
    ImGui::SliderFloat("Compensation", &settings.exposureCompensation, -4.0f, 4.0f, "%+.1f stops");

    if (!ImGui::TreeNode("Auto exposure settings")) {
        return;
    }

    // Histogram of the last metered frame: brightness from dark (left) to bright (right), log scale.
    // The highlighted band is the metered part, the line is the measured average.
    if (exposureInfoValid) {
        float bins[exposureBinCount - 1];
        float largest = 1.0f;
        for (uint32_t i = 1; i < exposureBinCount; i++) {
            bins[i - 1] = static_cast<float>(exposureInfo.lastHistogram[i]);
            largest = std::max(largest, bins[i - 1]);
        }
        ImGui::PlotHistogram("##histogram", bins, exposureBinCount - 1, 0, nullptr, 0.0f, largest,
                             ImVec2(ImGui::GetContentRegionAvail().x, 70.0f));
        ImVec2 min = ImGui::GetItemRectMin();
        ImVec2 max = ImGui::GetItemRectMax();
        auto xOf = [&](float luminance) {
            float t = (std::log2(std::max(luminance, 1e-30f)) - histogramMinLog2) / histogramLog2Range;
            return min.x + std::clamp(t, 0.0f, 1.0f) * (max.x - min.x);
        };
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(ImVec2(xOf(exposureInfo.lowLuminance), min.y), ImVec2(xOf(exposureInfo.highLuminance), max.y),
                            IM_COL32(80, 200, 120, 50));
        float average = xOf(exposureInfo.averageLuminance);
        draw->AddLine(ImVec2(average, min.y), ImVec2(average, max.y), IM_COL32(255, 220, 80, 255), 2.0f);
        ImGui::TextDisabled("metered: luminance %.3g .. %.3g, average %.3g", exposureInfo.lowLuminance,
                            exposureInfo.highLuminance, exposureInfo.averageLuminance);
    }

    ImGui::SliderFloat("Ignore darkest", &settings.meterLowPercent, 0.0f, 98.0f, "%.0f%%");
    ImGui::SliderFloat("Ignore above", &settings.meterHighPercent, 2.0f, 100.0f, "%.0f%%");
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Pixels are ranked from dark to bright. Only the ones between the two\n"
                          "percentages are averaged: deep shadows and a black background don't\n"
                          "make the image brighter, the sun, the sky and fireflies don't make it darker.");
    }
    ImGui::Checkbox("Center-weighted", &settings.centerWeighted);
    ImGui::SameLine();
    ImGui::TextDisabled("(the middle of the image counts up to 4x)");
    ImGui::SliderFloat("Key", &settings.exposureKey, 0.05f, 0.5f, "%.2f");
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("The measured average is made this bright before tone mapping.\n"
                          "0.18 = middle grey; Compensation does the same in stops.");
    }
    ImGui::DragFloatRange2("Limits", &settings.minExposureStops, &settings.maxExposureStops, 0.1f, -10.0f, 12.0f,
                           "%+.1f stops", "%+.1f stops");
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("How far auto exposure may darken (x%.2f) or brighten (x%.1f) the image.\n"
                          "Keeps very dark scenes from turning into grey noise.",
                          std::exp2(settings.minExposureStops), std::exp2(settings.maxExposureStops));
    }
    ImGui::SliderFloat("Brighten speed", &settings.brightenSpeed, 0.1f, 10.0f, "%.1f /s", ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Darken speed", &settings.darkenSpeed, 0.1f, 10.0f, "%.1f /s", ImGuiSliderFlags_Logarithmic);
    if (ImGui::Button("Snap now")) {
        exposureReset = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(skip the adaptation once)");
    ImGui::TreePop();
}
