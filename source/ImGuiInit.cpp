//
// Created by Alienson on 24.1.2024..
//

#include <functional>
#include <stdexcept>
#include <algorithm>
#include "../include/imgui/imgui.h"
#include "../include/imgui/backends/imgui_impl_vulkan.h"
#include "../include/imgui/backends/imgui_impl_glfw.h"

#include "Application.h"
#include <vulkan/vulkan.h>

void Application::initImGui(){
    // 1: create descriptor pool for IMGUI
    //  ImGui itself only needs a combined image sampler for its font texture,
    //  the extra room is for textures we may show in the UI later.
    VkDescriptorPoolSize pool_sizes[] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 100 } };

    VkDescriptorPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = 100;
    pool_info.poolSizeCount = (uint32_t)std::size(pool_sizes);
    pool_info.pPoolSizes = pool_sizes;

    if (vkCreateDescriptorPool(device, &pool_info, nullptr, &imGuiPool) != VK_SUCCESS) {
        throw std::runtime_error("failed to create imGui descriptor pool!");
    }

    // 2: initialize imgui library

    // this initializes the core structures of imgui
    ImGui::CreateContext();

    // this initializes imgui for GLFW (true = install input callbacks, chaining any existing ones)
    ImGui_ImplGlfw_InitForVulkan(window, true);

    // this initializes imgui for Vulkan
    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.Instance = instance;
    init_info.PhysicalDevice = physicalDevice;
    init_info.Device = device;
    init_info.Queue = graphicsQueue;
    init_info.DescriptorPool = imGuiPool;
    init_info.MinImageCount = static_cast<uint32_t>(swapChainImages.size());
    init_info.ImageCount = static_cast<uint32_t>(swapChainImages.size());
    init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

    // ImGui creates its pipeline against our render pass, subpass 0
    if (!ImGui_ImplVulkan_Init(&init_info, renderPass)) {
        throw std::runtime_error("failed to initialize imGui Vulkan backend!");
    }

    // execute a gpu command to upload imgui font textures, then free the staging buffer
    immediateSubmit([&](VkCommandBuffer cmd) { ImGui_ImplVulkan_CreateFontsTexture(cmd); });
    ImGui_ImplVulkan_DestroyFontUploadObjects();
}

// Builds this frame's UI. Must be called once per frame before drawFrame().
void Application::buildImGui() {
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::Begin("Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::Text("GPU: %s", physicalDeviceProperties.deviceName);
    ImGui::Text("%.1f FPS (%.3f ms)", ImGui::GetIO().Framerate, 1000.0f / ImGui::GetIO().Framerate);
    ImGui::Text("%u triangles", primitiveCount);

    if (ImGui::CollapsingHeader("Scene", ImGuiTreeNodeFlags_DefaultOpen)) {
        const char* preview = loadedModel >= 0 ? availableModels[loadedModel].c_str() : "(floor only)";
        if (ImGui::BeginCombo("Model", preview)) {
            if (ImGui::Selectable("(floor only)", loadedModel < 0) && loadedModel >= 0) {
                requestedModel = static_cast<int>(availableModels.size());  // out of range = no model
            }
            for (int i = 0; i < static_cast<int>(availableModels.size()); i++) {
                if (ImGui::Selectable(availableModels[i].c_str(), i == loadedModel) && i != loadedModel) {
                    requestedModel = i;  // loaded at the start of the next frame
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Rescan")) {
            std::string current = loadedModel >= 0 ? availableModels[loadedModel] : "";
            findAvailableModels();
            auto it = std::find(availableModels.begin(), availableModels.end(), current);
            loadedModel = it != availableModels.end() ? static_cast<int>(it - availableModels.begin()) : -1;
        }
        ImGui::TextDisabled("put .glb/.gltf files into resource/models");
        if (!loadError.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", loadError.c_str());
        }
        ImGui::Text("%zu meshes, %zu instances", meshInfos.size(), sceneInstances.size());
        ImGui::Text("BLAS memory: %.2f MB -> %.2f MB compacted",
                    blasSizeBeforeCompaction / (1024.0 * 1024.0), blasSizeAfterCompaction / (1024.0 * 1024.0));
    }

    if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextDisabled("WASD move, Space/LShift up/down, LCtrl faster,");
        ImGui::TextDisabled("hold right mouse button to look around");
        ImGui::DragFloat3("Position", &camera.position.x, 0.05f);
        ImGui::DragFloat("Yaw", &camera.yaw, 0.5f);
        ImGui::SliderFloat("Pitch", &camera.pitch, -89.0f, 89.0f);
        ImGui::SliderFloat("Field of view", &camera.fov, 20.0f, 120.0f);
        ImGui::SliderFloat("Move speed", &camera.moveSpeed, 0.1f, 20.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
    }

    // ImGui needs unique IDs per widget; "##sun"/"##sky" suffixes are hidden from the labels.
    if (ImGui::CollapsingHeader("Sun", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("Azimuth", &settings.sunAzimuth, -180.0f, 180.0f);
        ImGui::SliderFloat("Elevation", &settings.sunElevation, -10.0f, 90.0f);
        ImGui::SliderFloat("Size (degrees)", &settings.sunAngularRadius, 0.0f, 15.0f, "%.2f");
        ImGui::ColorEdit3("Color##sun", &settings.sunColor.x);
        ImGui::SliderFloat("Intensity##sun", &settings.sunIntensity, 0.0f, 5.0f);
        ImGui::Checkbox("Shadows", &settings.shadowsEnabled);
    }

    if (ImGui::CollapsingHeader("Sky", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::ColorEdit3("Zenith", &settings.skyZenithColor.x);
        ImGui::ColorEdit3("Horizon", &settings.skyHorizonColor.x);
        ImGui::ColorEdit3("Ground", &settings.groundColor.x);
        ImGui::SliderFloat("Intensity##sky", &settings.skyIntensity, 0.0f, 3.0f);
    }

    if (ImGui::CollapsingHeader("Rendering", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("Exposure", &settings.exposure, 0.05f, 8.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
        ImGui::Checkbox("Tone mapping (ACES)", &settings.toneMapping);
        ImGui::Combo("Debug view", &settings.debugView, "Off (path traced)\0Normals\0Texture coordinates\0Mesh index\0");
        ImGui::SliderInt("Samples per pixel", &settings.samplesPerPixel, 1, 64);
        ImGui::SliderInt("Max bounces", &settings.maxBounces, 1, 16);
        ImGui::TextDisabled("1 = direct sunlight only, more = light bouncing between surfaces");
        ImGui::Checkbox("Accumulate", &settings.accumulate);
        if (settings.accumulate) {
            ImGui::SameLine();
            ImGui::Text("%u frames (%u samples/pixel)", accumulatedFrames, accumulatedFrames * settings.samplesPerPixel);
            ImGui::SameLine();
            if (ImGui::SmallButton("Restart")) {
                resetAccumulation = true;
            }
        }
    }

    ImGui::Separator();
    ImGui::Checkbox("Show ImGui demo", &showImGuiDemo);
    ImGui::End();

    if (showImGuiDemo) {
        ImGui::ShowDemoWindow(&showImGuiDemo);
    }

    ImGui::Render();
}

// Records commands with `function`, submits them and waits until the GPU is done.
// Only meant for one-off setup work (uploads), not for per-frame rendering.
void Application::immediateSubmit(std::function<void(VkCommandBuffer cmd)>&& function)
{
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = commandPool;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device, &allocInfo, &cmd);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(cmd, &beginInfo);
    function(cmd);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;

    vkQueueSubmit(graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphicsQueue);

    vkFreeCommandBuffers(device, commandPool, 1, &cmd);
}


void Application::cleanupImGui(){
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    vkDestroyDescriptorPool(device, imGuiPool, nullptr);
}
