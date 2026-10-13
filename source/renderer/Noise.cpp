//
// Noise measurement: how far the displayed image is from the converged result, frame by frame.
//
// 1. Reference: the current view is accumulated for many frames (Accumulation mode), then copied aside.
// 2. Measurement: accumulation (or DLSS) starts over, and every frame the displayed HDR image is compared with the
//    reference (shaders/noise.comp): the mean relative squared error of the luminance, and the overall brightness
//    compared to the reference (a technique that is biased shows up there, and as an error that stops shrinking).
// In Accumulation mode the error of an unbiased renderer halves when the frame count doubles; the product of the
// error after one frame and the time per frame tells how fast a technique converges (lower = better).
//
// Results go to the UI and to stdout ("noise: ..."), so scripted runs can read them (see noiseAutoRun).
//

#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <imgui/imgui.h>
#include "renderer/Renderer.h"
#include "core/Configuration.h"

std::vector<char> readFile(const std::string& filename);

static const char* renderModeName(int mode) {
    const char* names[] = {"Accumulation", "DLSS DLAA", "DLSS Quality", "DLSS Balanced", "DLSS Performance",
                           "DLSS Ultra Performance"};
    return mode >= 0 && mode < static_cast<int>(std::size(names)) ? names[mode] : "?";
}

// Created when first needed (the reference image takes ~23 MB at 1600x900). The GPU must be idle, so this is
// only called from the UI and from scripted runs, never while frames are in flight.
void Renderer::ensureNoiseResources() {
    vkp.deviceWaitIdle(device);

    if (!noiseResourcesCreated) {
        // 0 = displayed HDR image, 1 = reference, 2 = workgroup sums (one set per frame in flight)
        VkDescriptorSetLayoutBinding bindings[3] = {
                {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        };
        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 3;
        layoutInfo.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &noiseDescriptorSetLayout) != VK_SUCCESS) {
            throw std::runtime_error("failed to create noise descriptor set layout!");
        }

        const uint32_t sets = Configuration::maxFramesInFlight;
        VkDescriptorPoolSize poolSizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * sets},
                                            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, sets}};
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = sets;
        poolInfo.poolSizeCount = 2;
        poolInfo.pPoolSizes = poolSizes;
        if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &noiseDescriptorPool) != VK_SUCCESS) {
            throw std::runtime_error("failed to create noise descriptor pool!");
        }
        std::vector<VkDescriptorSetLayout> layouts(sets, noiseDescriptorSetLayout);
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = noiseDescriptorPool;
        allocInfo.descriptorSetCount = sets;
        allocInfo.pSetLayouts = layouts.data();
        noiseDescriptorSets.resize(sets);
        if (vkAllocateDescriptorSets(device, &allocInfo, noiseDescriptorSets.data()) != VK_SUCCESS) {
            throw std::runtime_error("failed to allocate noise descriptor sets!");
        }

        VkPushConstantRange pushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(NoisePushConstants)};
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &noiseDescriptorSetLayout;
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &pushRange;
        if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &noisePipelineLayout) != VK_SUCCESS) {
            throw std::runtime_error("failed to create noise pipeline layout!");
        }
        VkShaderModule shaderModule = createShaderModule(readFile("noise.comp.spv"));
        VkComputePipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = shaderModule;
        pipelineInfo.stage.pName = "main";
        pipelineInfo.layout = noisePipelineLayout;
        if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &noisePipeline) != VK_SUCCESS) {
            throw std::runtime_error("failed to create noise pipeline!");
        }
        vkDestroyShaderModule(device, shaderModule, nullptr);
        noisePending.assign(Configuration::maxFramesInFlight, NoisePending{});
        noiseResourcesCreated = true;
    }

    // Reference image at output size (the size of the displayed image in every mode)
    if (noiseReferenceImage.image == VK_NULL_HANDLE || noiseReferenceExtent.width != outputExtent.width ||
        noiseReferenceExtent.height != outputExtent.height) {
        destroyImage(noiseReferenceImage);
        createImage(outputExtent, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT, noiseReferenceImage);
        immediateSubmit([&](VkCommandBuffer cmd) {
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = noiseReferenceImage.image;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &barrier);
        });
        noiseReferenceExtent = outputExtent;
        noiseReferenceValid = false;
    }

    uint32_t groups = ((outputExtent.width + 15) / 16) * ((outputExtent.height + 15) / 16);
    if (groups > noiseGroupCapacity) {
        for (size_t i = 0; i < noiseSumBuffers.size(); i++) {
            vkUnmapMemory(device, noiseSumMemories[i]);
            vkDestroyBuffer(device, noiseSumBuffers[i], nullptr);
            vkFreeMemory(device, noiseSumMemories[i], nullptr);
        }
        noiseSumBuffers.resize(Configuration::maxFramesInFlight);
        noiseSumMemories.resize(Configuration::maxFramesInFlight);
        noiseSumMapped.resize(Configuration::maxFramesInFlight);
        for (int i = 0; i < Configuration::maxFramesInFlight; i++) {
            createBuffer(2 * sizeof(glm::vec4) * groups, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         noiseSumBuffers[i], noiseSumMemories[i]);
            vkMapMemory(device, noiseSumMemories[i], 0, VK_WHOLE_SIZE, 0, &noiseSumMapped[i]);
        }
        noiseGroupCapacity = groups;
    }
    updateNoiseDescriptorSets();
}

// Also called from recreateRenderImages (GPU idle): the displayed image is another one after a mode switch.
void Renderer::updateNoiseDescriptorSets() {
    if (!noiseResourcesCreated) {
        return;
    }
    if (noiseReferenceExtent.width != outputExtent.width || noiseReferenceExtent.height != outputExtent.height) {
        // The window changed size: the reference no longer fits (it is recreated by the next "Make reference")
        noiseReferenceValid = false;
        if (noiseState != NoiseState::Idle) {
            noiseState = NoiseState::Idle;
            noiseStatus = "stopped: the window changed size";
        }
        return;
    }
    for (int i = 0; i < Configuration::maxFramesInFlight; i++) {
        VkDescriptorImageInfo measuredInfo{VK_NULL_HANDLE, displayedHdrView(), VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo referenceInfo{VK_NULL_HANDLE, noiseReferenceImage.view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorBufferInfo sumInfo{noiseSumBuffers[i], 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet writes[3]{};
        for (uint32_t b = 0; b < 3; b++) {
            writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[b].dstSet = noiseDescriptorSets[i];
            writes[b].dstBinding = b;
            writes[b].descriptorCount = 1;
            writes[b].descriptorType = b < 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        }
        writes[0].pImageInfo = &measuredInfo;
        writes[1].pImageInfo = &referenceInfo;
        writes[2].pBufferInfo = &sumInfo;
        vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);
    }
}

// Accumulates the current view for noiseReferenceFrames frames, then keeps a copy (see recordNoise)
void Renderer::startNoiseReference() {
    ensureNoiseResources();
    for (NoisePending& pending : noisePending) {
        pending.active = false;
    }
    settings.renderMode = static_cast<int>(RenderMode::Accumulation);
    settings.accumulate = true;
    cacheQueriesSuspended = true;  // the reference is plain path tracing (unbiased); the cache keeps filling
    noiseReferenceValid = false;
    resetAccumulation = true;
    noiseState = NoiseState::BuildingReference;
    noiseStatus = "building the reference";
}

// Starts once the reference is ready and renderMode is active (see recordNoise)
void Renderer::startNoiseMeasurement(int renderMode) {
    noiseMeasureMode = renderMode;
    settings.renderMode = renderMode;
    noiseResults.clear();
    noiseState = NoiseState::WaitingToMeasure;
    noiseStatus = std::string("measuring: ") + renderModeName(renderMode);
}

// The current frame slot's fence has been waited on: its sums from a few frames ago are complete.
void Renderer::readNoiseResults() {
    if (!noiseResourcesCreated) {
        return;
    }
    NoisePending& pending = noisePending[currentFrame];
    if (!pending.active) {
        return;
    }
    pending.active = false;

    // Doubles: a million small errors added up in floats would lose precision
    const glm::vec4* sums = static_cast<const glm::vec4*>(noiseSumMapped[currentFrame]);
    double error = 0.0, measured = 0.0, reference = 0.0, broken = 0.0, regionMeasured = 0.0, regionReference = 0.0;
    for (uint32_t i = 0; i < pending.groups; i++) {
        error += sums[2 * i].x;
        measured += sums[2 * i].y;
        reference += sums[2 * i].z;
        broken += sums[2 * i].w;
        regionMeasured += sums[2 * i + 1].x;
        regionReference += sums[2 * i + 1].y;
    }
    double pixels = static_cast<double>(noiseReferenceExtent.width) * noiseReferenceExtent.height - broken;
    if (pixels <= 0.0) {
        return;
    }

    if (pending.pass == 0) {
        noiseReferenceLuminance = static_cast<float>(reference / pixels);
        noiseReferenceValid = noiseReferenceLuminance > 0.0f;
        std::cout << "noise: reference " << pending.frame << " frames, average luminance " << noiseReferenceLuminance
                  << (broken > 0.0 ? ", with NaN/infinite pixels" : "") << std::endl;
        if (!noiseReferenceValid) {
            noiseStatus = "the reference is black";
            noiseState = NoiseState::Idle;
        } else if (noiseState == NoiseState::Idle) {
            noiseStatus = "reference ready";
        }
        return;
    }

    if (noiseState != NoiseState::Measuring) {
        return;  // stopped in the meantime
    }
    NoiseResult result{};
    result.frame = pending.frame;
    result.relativeMse = static_cast<float>(error / pixels);
    result.brightness = reference > 0.0 ? static_cast<float>(measured / reference) : 0.0f;
    result.regionBrightness = regionReference > 0.0 ? static_cast<float>(regionMeasured / regionReference) : 0.0f;
    // readTimestamps read this frame slot's timestamps just before: the same frame
    result.traceMs = lastPassTimes[0] + lastPassTimes[1] + lastPassTimes[2];  // radiance cache + ReSTIR + trace
    result.restirMs = lastPassTimes[1];
    result.dlssMs = lastPassTimes[3];
    noiseResults.push_back(result);
    if (result.frame >= static_cast<uint32_t>(noiseMeasureFrames)) {
        finishNoiseMeasurement();
    }
}

void Renderer::finishNoiseMeasurement() {
    noiseState = NoiseState::Idle;
    float traceMs = 0.0f, restirMs = 0.0f, dlssMs = 0.0f;
    for (const NoiseResult& result : noiseResults) {
        traceMs += result.traceMs;
        restirMs += result.restirMs;
        dlssMs += result.dlssMs;
    }
    float count = static_cast<float>(std::max<size_t>(noiseResults.size(), 1));
    traceMs /= count;
    restirMs /= count;
    dlssMs /= count;
    char line[256];
    std::snprintf(line, sizeof(line), "measured %s%s%s%s: trace %.2f ms (ReSTIR %.2f), DLSS %.2f ms per frame",
                  renderModeName(noiseMeasureMode), settings.radianceCache ? " + radiance cache" : "",
                  settings.restirDi(noiseMeasureMode) ? " + ReSTIR DI" : "",
                  settings.restirGi(noiseMeasureMode) ? " + ReSTIR GI" : "", traceMs, restirMs, dlssMs);
    noiseStatus = line;
    std::cout << "noise: " << line << std::endl;
    std::cout << "noise: frame  relMSE      rel. RMSE  brightness  region" << std::endl;
    for (const NoiseResult& result : noiseResults) {
        bool powerOfTwo = (result.frame & (result.frame - 1)) == 0;
        if (powerOfTwo || &result == &noiseResults.back()) {
            std::snprintf(line, sizeof(line), "noise: %5u  %.4e  %6.2f%%    %6.2f%%     %6.2f%%", result.frame,
                          result.relativeMse, 100.0f * std::sqrt(result.relativeMse), 100.0f * result.brightness,
                          100.0f * result.regionBrightness);
            std::cout << line << std::endl;
        }
    }
    if (!noiseResults.empty()) {
        // Error x time: what one frame's noise costs. Halving it = reaching the same quality in half the time.
        std::snprintf(line, sizeof(line), "noise: frame 1 relMSE x ms = %.4e", noiseResults[0].relativeMse * (traceMs + dlssMs));
        std::cout << line << std::endl;
    }
    if (noiseAutoExit) {
        noiseQuitRequested = true;  // the program closes its window
    }
}

// Recorded after the exposure pass: the displayed HDR image (radiance or DLSS output) is final by then.
void Renderer::recordNoise(VkCommandBuffer commandBuffer) {
    if (noiseAutoRun) {
        noiseAutoRun = false;
        noiseAutoContinue = true;
        startNoiseReference();  // waits for the GPU; fine, this command buffer isn't submitted yet
    }
    readNoiseResults();

    uint32_t pass = 0;
    switch (noiseState) {
        case NoiseState::Idle:
            return;
        case NoiseState::BuildingReference:
            // accumulatedFrames already counts this frame (incremented in buildSceneUniforms)
            if (dlssActive() || settings.debugView != static_cast<int>(DebugView::None) ||
                accumulatedFrames < static_cast<uint32_t>(noiseReferenceFrames)) {
                return;
            }
            pass = 0;
            noiseFrame = accumulatedFrames;
            noiseState = NoiseState::Idle;
            cacheQueriesSuspended = false;
            noiseStatus = "reference: " + std::to_string(accumulatedFrames) + " frames";
            if (noiseAutoContinue) {
                noiseAutoContinue = false;
                startNoiseMeasurement(noiseAutoMode);  // scripted: go on with the measurement
            }
            break;
        case NoiseState::WaitingToMeasure:
            // The reference's average is needed for the floor (it arrives a few frames after the copy)
            if (!noiseReferenceValid || activeRenderMode != noiseMeasureMode) {
                return;
            }
            // Start over from the next frame on: accumulation from 0, DLSS without history
            resetAccumulation = true;
            dlssReset = true;
            noiseRestartsAtStart = accumulationRestarts + 1;  // the restart just asked for
            noiseFrame = 0;
            noiseState = NoiseState::Measuring;
            return;
        case NoiseState::Measuring:
            if (noiseFrame >= static_cast<uint32_t>(noiseMeasureFrames)) {
                return;  // all frames recorded, waiting for the last results
            }
            noiseFrame++;
            if (accumulationRestarts != noiseRestartsAtStart || settings.debugView != static_cast<int>(DebugView::None) ||
                activeRenderMode != noiseMeasureMode ||
                (!dlssActive() && settings.accumulate && accumulatedFrames != noiseFrame)) {
                noiseState = NoiseState::Idle;
                noiseStatus = "stopped: the view or a setting changed during the measurement";
                std::cout << "noise: " << noiseStatus << std::endl;
                return;
            }
            pass = 1;
            break;
    }

    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);

    NoisePushConstants push{};
    push.pass = pass;
    push.floorLuminance = 0.01f * noiseReferenceLuminance;
    bool region = noiseRegion.z > noiseRegion.x && noiseRegion.w > noiseRegion.y;
    push.regionMin = region ? glm::ivec2(noiseRegion.x, noiseRegion.y) : glm::ivec2(0);
    push.regionMax = region ? glm::ivec2(noiseRegion.z, noiseRegion.w)
                            : glm::ivec2(outputExtent.width, outputExtent.height);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, noisePipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, noisePipelineLayout, 0, 1,
                            &noiseDescriptorSets[currentFrame], 0, nullptr);
    vkCmdPushConstants(commandBuffer, noisePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    uint32_t groupsX = (outputExtent.width + 15) / 16, groupsY = (outputExtent.height + 15) / 16;
    vkCmdDispatch(commandBuffer, groupsX, groupsY, 1);

    VkMemoryBarrier hostBarrier{};
    hostBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    hostBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    hostBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 1, &hostBarrier, 0, nullptr, 0, nullptr);

    noisePending[currentFrame] = {pass, noiseFrame, groupsX * groupsY, true};
}

void Renderer::destroyNoiseResources() {
    if (!noiseResourcesCreated) {
        return;
    }
    vkDestroyPipeline(device, noisePipeline, nullptr);
    vkDestroyPipelineLayout(device, noisePipelineLayout, nullptr);
    vkDestroyDescriptorPool(device, noiseDescriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(device, noiseDescriptorSetLayout, nullptr);
    destroyImage(noiseReferenceImage);
    for (size_t i = 0; i < noiseSumBuffers.size(); i++) {
        vkUnmapMemory(device, noiseSumMemories[i]);
        vkDestroyBuffer(device, noiseSumBuffers[i], nullptr);
        vkFreeMemory(device, noiseSumMemories[i], nullptr);
    }
    noiseResourcesCreated = false;
}

void Renderer::buildNoiseUi() {
    ImGui::SeparatorText("Noise measurement");
    ImGui::TextDisabled("Keep the camera still. The reference is the current view, accumulated\n"
                        "for a long time; the measurement then starts over and compares every frame.");
    bool idle = noiseState == NoiseState::Idle;
    ImGui::BeginDisabled(!idle);
    ImGui::SliderInt("Reference frames", &noiseReferenceFrames, 256, 65536, "%d", ImGuiSliderFlags_Logarithmic);
    if (ImGui::Button("Make reference")) {
        startNoiseReference();
    }
    ImGui::SameLine();
    if (noiseState == NoiseState::BuildingReference) {
        ImGui::Text("%u / %d frames", dlssActive() ? 0u : accumulatedFrames, noiseReferenceFrames);
    } else if (noiseReferenceValid) {
        ImGui::TextDisabled("reference ready (average luminance %.3g)", noiseReferenceLuminance);
    } else {
        ImGui::TextDisabled("no reference");
    }

    ImGui::SliderInt("Measured frames", &noiseMeasureFrames, 1, 1024, "%d", ImGuiSliderFlags_Logarithmic);
    ImGui::DragInt4("Region", &noiseRegion.x, 1.0f, 0, static_cast<int>(std::max(outputExtent.width, outputExtent.height)));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("x0, y0, x1, y1 in pixels: a part of the image whose brightness is also measured\n"
                          "on its own (\"region\" column). All 0 = the whole image.");
    }
    ImGui::BeginDisabled(!noiseReferenceValid);
    if (ImGui::Button("Measure")) {
        startNoiseMeasurement(settings.renderMode);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("(in the current renderer)");
    ImGui::EndDisabled();
    if (noiseState == NoiseState::Measuring) {
        ImGui::Text("frame %u / %d", noiseFrame, noiseMeasureFrames);
    } else if (!noiseStatus.empty()) {
        ImGui::TextDisabled("%s", noiseStatus.c_str());
    }

    if (!noiseResults.empty() && ImGui::BeginTable("noise", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("frame");
        ImGui::TableSetupColumn("rel. MSE");
        ImGui::TableSetupColumn("rel. RMSE");
        ImGui::TableSetupColumn("brightness");
        ImGui::TableSetupColumn("region");
        ImGui::TableHeadersRow();
        for (const NoiseResult& result : noiseResults) {
            if ((result.frame & (result.frame - 1)) != 0 && &result != &noiseResults.back()) {
                continue;  // powers of two (and the last one)
            }
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%u", result.frame);
            ImGui::TableNextColumn();
            ImGui::Text("%.3e", result.relativeMse);
            ImGui::TableNextColumn();
            ImGui::Text("%.2f%%", 100.0f * std::sqrt(result.relativeMse));
            ImGui::TableNextColumn();
            ImGui::Text("%.2f%%", 100.0f * result.brightness);
            ImGui::TableNextColumn();
            ImGui::Text("%.2f%%", 100.0f * result.regionBrightness);
        }
        ImGui::EndTable();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("rel. MSE: mean of ((frame - reference) / reference)^2 over the image (luminance).\n"
                          "Unbiased: halves when the frame count doubles. Stops shrinking: biased.\n"
                          "brightness: the frame's total light compared to the reference's.\n"
                          "region: the same inside the Region rectangle above (light leaking into a corner shows up there).");
    }
}
