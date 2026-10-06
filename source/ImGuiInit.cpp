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
    // Always visible, whichever tab is open
    ImGui::Text("GPU: %s", physicalDeviceProperties.deviceName);
    ImGui::Text("%.1f FPS (%.3f ms)", ImGui::GetIO().Framerate, 1000.0f / ImGui::GetIO().Framerate);
    ImGui::Text("GPU: cache %.2f ms, ReSTIR %.2f, trace %.2f, DLSS %.2f, exposure %.2f, display %.2f",
                gpuPassTimes[0], gpuPassTimes[1], gpuPassTimes[2], gpuPassTimes[3], gpuPassTimes[4], gpuPassTimes[5]);
    ImGui::Text("%u triangles", primitiveCount);
    if (settings.accumulate && !dlssActive()) {
        ImGui::Text("%u frames accumulated", accumulatedFrames);
    }
    if (settings.debugView != 0) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "Debug view on (Debug tab)");
    }

    // The window sizes itself to the open tab
    if (ImGui::BeginTabBar("settingsTabs")) {
        if (ImGui::BeginTabItem("Scene")) {
            buildSceneUi();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Camera")) {
            buildCameraUi();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Lighting")) {
            buildLightingUi();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Rendering")) {
            buildRenderingUi();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Debug")) {
            buildDebugUi();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();

    if (showImGuiDemo) {
        ImGui::ShowDemoWindow(&showImGuiDemo);
    }

    ImGui::Render();
}

void Application::buildCameraUi() {
    ImGui::TextDisabled("WASD move, Space/LShift up/down, LCtrl faster,");
    ImGui::TextDisabled("hold right mouse button to look around");
    bool fullscreenChecked = fullscreen;
    if (ImGui::Checkbox("Fullscreen (F11)", &fullscreenChecked)) {
        setFullscreen(fullscreenChecked);
    }
    ImGui::DragFloat3("Position", &camera.position.x, 0.05f);
    ImGui::DragFloat("Yaw", &camera.yaw, 0.5f);
    ImGui::SliderFloat("Pitch", &camera.pitch, -89.0f, 89.0f);
    ImGui::SliderFloat("Field of view", &camera.fov, 20.0f, 120.0f);
    ImGui::SliderFloat("Move speed", &camera.moveSpeed, 0.1f, 20.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
}

// ImGui needs unique IDs per widget; "##sun" suffixes are hidden from the labels.
void Application::buildLightingUi() {
    ImGui::SeparatorText("Sun");
    ImGui::Checkbox("Enabled##sun", &settings.sunEnabled);
    ImGui::SliderFloat("Azimuth", &settings.sunAzimuth, -180.0f, 180.0f);
    ImGui::SliderFloat("Elevation", &settings.sunElevation, -10.0f, 90.0f);
    ImGui::SliderFloat("Size (degrees)", &settings.sunAngularRadius, 0.0f, 15.0f, "%.2f");
    ImGui::ColorEdit3("Color##sun", &settings.sunColor.x);
    ImGui::SliderFloat("Intensity##sun", &settings.sunIntensity, 0.0f, 5.0f);
    ImGui::Checkbox("Shadows", &settings.shadowsEnabled);

    ImGui::SeparatorText("Sky");
    buildEnvironmentUi();  // Environment.cpp

    ImGui::SeparatorText("Lights from models");
    buildPunctualLightSettingsUi();  // Lights.cpp
}

void Application::buildRenderingUi() {
    ImGui::SeparatorText("Renderer");
    buildRenderModeUi();
    ImGui::BeginDisabled(dlssActive());  // DLSS averages over time itself
    ImGui::Checkbox("Accumulate", &settings.accumulate);
    ImGui::EndDisabled();
    if (settings.accumulate && !dlssActive()) {
        ImGui::SameLine();
        ImGui::Text("%u frames (%u samples/pixel)", accumulatedFrames, accumulatedFrames * settings.samplesPerPixel);
        ImGui::SameLine();
        if (ImGui::SmallButton("Restart")) {
            resetAccumulation = true;
        }
    }

    ImGui::SeparatorText("Light paths");
    ImGui::SliderInt("Samples per pixel", &settings.samplesPerPixel, 1, 64);
    ImGui::SliderInt("Max bounces", &settings.maxBounces, 1, 16);
    ImGui::TextDisabled("1 = direct sunlight only, more = light bouncing between surfaces");
    ImGui::Combo("Emissive light", &settings.emissiveSampling,
                 "Bounces only (old)\0Light sampling only\0Both, weighted (MIS)\0");
    ImGui::TextDisabled("how light from glowing surfaces is found; compare with Accumulate off");
    ImGui::Combo("Light picking", &settings.lightPicking, "By power (whole scene)\0Light tree\0");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Which glowing triangle a shadow ray goes to. By power: the brighter and larger, the more\n"
                          "often, wherever it is. Light tree: also by how near it is and whether the surface faces it,\n"
                          "so in scenes with many small lights (Bistro at night) the nearby ones get the rays.");
    }
    ImGui::TextDisabled("%zu glowing triangles, tree: %zu nodes, %u levels", sampledEmissiveTriangles, lightTree.size(),
                        lightTreeDepth);
    ImGui::Checkbox("Texture mipmaps", &settings.textureMipmaps);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("On: each hit reads textures at the size that fits how much of the surface one pixel sees\n"
                          "(smaller copies for distant or blurry hits): faster, no shimmering in the distance.\n"
                          "Off: always the full-size texture.");
    }
    ImGui::BeginDisabled(!settings.textureMipmaps);
    ImGui::Checkbox("Normal map filtering", &settings.normalMapFiltering);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Toksvig: where a smaller mip level of a normal map averages its bumps away (distant or\n"
                          "blurry hits), the reflection gets as much rougher as the bumps would have spread it.\n"
                          "Off: distant bumpy surfaces reflect as sharply as polished ones (and sparkle).\n"
                          "Needs Texture mipmaps.");
    }
    ImGui::Checkbox("Blur caustics", &settings.blurCaustics);
    ImGui::TextDisabled("light reflected by shiny surfaces onto matte ones becomes a soft glow\n"
                        "instead of slowly converging sparkles (fireflies)");
    buildRadianceCacheUi();
    buildRestirUi();

    ImGui::SeparatorText("Image");
    buildExposureUi();
    ImGui::Checkbox("Tone mapping (ACES)", &settings.toneMapping);
}

// ReSTIR DI (work in progress, see RESTIR_DI_PLAN.md)
void Application::buildRestirUi() {
    ImGui::SeparatorText("ReSTIR DI");
    ImGui::Checkbox("ReSTIR DI", &settings.restirDi);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Direct light at the first surface the camera sees (if rough enough): several candidate light\n"
                          "samples per pixel, the most promising one kept and reused by the next frame. Less noise with\n"
                          "many lights (Bistro at night). The sun keeps its own shadow ray.");
    }
    ImGui::BeginDisabled(!settings.restirDi);
    ImGui::SliderInt("Light candidates", &settings.restirLightCandidates, 1, 16);
    ImGui::SliderInt("Sky candidates", &settings.restirSkyCandidates, 1, 8);
    ImGui::SliderInt("BRDF candidates", &settings.restirBrdfCandidates, 0, 4);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Light candidates: points on glowing triangles (light tree), each per pixel and frame.\n"
                          "Sky candidates: directions picked by the sky's brightness (HDR maps only).\n"
                          "BRDF candidates: directions the surface reflects into, a ray each (the light a glossy\n"
                          "surface mirrors).");
    }
    ImGui::SliderFloat("Min. roughness", &settings.restirMinRoughness, 0.0f, 1.0f, "%.2f");
    ImGui::Checkbox("Reuse last frame", &settings.restirTemporal);
    ImGui::BeginDisabled(!settings.restirTemporal);
    ImGui::SameLine();
    ImGui::Checkbox("Unbiased", &settings.restirTemporalVisibility);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Two more shadow rays per pixel, from last frame's surface. Without them, areas near shadow\n"
                          "edges come out too dark (Sponza with the sky: 3.7%%, Bistro at night: 0.6%%).");
    }
    ImGui::Checkbox("Shuffle pixels", &settings.restirPermutation);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Last frame's samples come from pixels up to 3 away, shuffled differently every frame.\n"
                          "Off: each pixel keeps its samples, and DLSS paints their error into the image.");
    }
    ImGui::SliderFloat("Max. history", &settings.restirMaxHistory, 1.0f, 50.0f, "%.0f x");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Last frame's samples count for at most this many times this frame's candidates. Higher:\n"
                          "less noise on still images, slower reaction to changes.");
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
}

void Application::buildDebugUi() {
    ImGui::Combo("Debug view", &settings.debugView,
                 "Off (path traced)\0Normals\0Texture coordinates\0Mesh index\0Material index\0"
                 "Base color\0Metallic\0Roughness\0Transmission\0Emissive\0Tangents\0"
                 "Radiance cache\0"
                 "DLSS input: depth\0DLSS input: motion vectors\0DLSS input: diffuse albedo\0"
                 "DLSS input: specular albedo\0DLSS input: normals\0DLSS input: roughness\0"
                 "DLSS input: specular hit distance\0DLSS input: reprojection error\0");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Radiance cache: the indirect light the cache holds for what the camera sees, cell by cell\n"
                          "(magenta: no cell yet, dark red: too few samples).\n"
                          "DLSS inputs: the images DLSS will get, written during normal path tracing.\n"
                          "Depth / hit distance: grey = near to far, light blue = sky, black = unknown.\n"
                          "Motion vectors: grey = still, red/green = moving left/up.\n"
                          "Reprojection error: last frame's albedo fetched through the motion vectors,\n"
                          "minus this frame's. Should stay black while the camera or objects move,\n"
                          "except at edges that just came into view (blue: off-screen last frame).");
    }
    if (settings.debugView != 0) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Off")) {
            settings.debugView = 0;
        }
    }

    buildNoiseUi();

    if (!rtPipelineStatistics.empty() && ImGui::CollapsingHeader("Trace shader (from the driver)")) {
        ImGui::TextUnformatted(rtPipelineStatistics.c_str());
        ImGui::TextDisabled("More registers per thread = fewer threads per SM at once, so waiting for rays\n"
                            "and memory is hidden worse. Counts for the whole shader, even unused branches.");
    }

    ImGui::Separator();
    ImGui::Checkbox("Show ImGui demo", &showImGuiDemo);
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
