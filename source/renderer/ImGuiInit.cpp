//
// Created by Alienson on 24.1.2024..
//

#include <functional>
#include <stdexcept>
#include <algorithm>
#include <imgui/imgui.h>
#include <imgui/backends/imgui_impl_vulkan.h>

#include "renderer/Renderer.h"
#include <vulkan/vulkan.h>

void Renderer::initImGui(){
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

    // The ImGui context and its GLFW side are the program's (Application::run); this is the Vulkan side, drawing
    // the UI over the image in the render pass
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

// Under the editor's Sun settings: the renderer's shadow switch
void Renderer::buildShadowsUi() {
    ImGui::Checkbox("Shadows", &settings.shadowsEnabled);
}

void Renderer::buildRenderingUi() {
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
    ImGui::Combo("Random numbers", &settings.pathSampling, "White noise\0Sobol\0Sobol + blue noise\0");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("For the main decisions of a path (camera offset, sun, sky, glowing triangles, bounces).\n"
                          "White noise: independent random numbers.\n"
                          "Sobol: Owen-scrambled Sobol points, a pixel's samples spread evenly: it settles faster.\n"
                          "+ blue noise: the points shifted per pixel by a blue-noise texture, so neighboring pixels'\n"
                          "errors differ too: fine-grained noise instead of blotches, easier for DLSS to filter.");
    }
    ImGui::SliderInt("Max bounces", &settings.maxBounces, 1, 64);
    ImGui::TextDisabled("1 = direct light only (sun, sky, lamps, glowing surfaces), more = light bouncing between surfaces");
    ImGui::Combo("Emissive light", &settings.emissiveSampling,
                 "Bounces only (old)\0Light sampling only\0Both, weighted (MIS)\0");
    ImGui::TextDisabled("how light from glowing surfaces is found; compare with Accumulate off");
    ImGui::Checkbox("Glowing triangles by solid angle", &settings.emissiveSolidAngle);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Where on a glowing triangle shadow rays (and ReSTIR's candidates) go.\n"
                          "On: spread evenly over the triangle as the surface sees it (its solid angle, Arvo 1995).\n"
                          "Off: evenly over its area. For a large panel close to a surface, its near part fills most of\n"
                          "the surface's view of it but gets few samples: noisier there.");
    }
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
    ImGui::Checkbox("Basic shader", &settings.basicShader);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("The trace shader without the code for portals, lamps (punctual lights), glass, cut-outs\n"
                          "and diffuse transmission: less code, fewer registers, about 14%% faster. The image stays\n"
                          "the same only in scenes without them: there portals and lamps vanish, glass turns opaque\n"
                          "and cut-outs solid. Switching rebuilds the shader (the window stands still a few seconds).");
    }
    buildRadianceCacheUi();
    buildRestirUi();

    ImGui::SeparatorText("Image");
    buildExposureUi();
    ImGui::Checkbox("Tone mapping (ACES)", &settings.toneMapping);
    ImGui::Checkbox("Fog grain", &settings.fogGrain);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Film-like grain where the camera sees fog, added to the finished image (after DLSS, which\n"
                          "removes the path tracer's own noise). A look only: the light is unchanged. Also in\n"
                          "Accumulation and in captures; turn it off for a clean image.");
    }
    ImGui::BeginDisabled(!settings.fogGrain);
    ImGui::SliderFloat("Grain strength", &settings.fogGrainStrength, 0.0f, 1.0f);
    ImGui::SliderFloat("Grain size", &settings.fogGrainSize, 1.0f, 4.0f, "%.1f px");
    ImGui::EndDisabled();
}

// ReSTIR DI (see RESTIR_DI_PLAN.md)
void Renderer::buildRestirUi() {
    ImGui::SeparatorText("ReSTIR DI");
    bool& restirDi = settings.restirDi(settings.renderMode);
    bool accumulation = settings.renderMode == static_cast<int>(RenderMode::Accumulation);
    ImGui::Checkbox(accumulation ? "ReSTIR DI (Accumulation)" : "ReSTIR DI (DLSS modes)", &restirDi);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Direct light at the first surface the camera sees (if rough enough): several candidate light\n"
                          "samples per pixel, the most promising one kept and reused by the next frame and nearby\n"
                          "pixels. Less noise with many lights (Bistro at night). The sun keeps its own shadow ray.\n"
                          "Set separately for Accumulation (off by default: the plain path tracer, the reference)\n"
                          "and the DLSS modes (on by default).");
    }
    ImGui::BeginDisabled(!restirDi);
    ImGui::SliderInt("Light candidates", &settings.restirLightCandidates, 1, 16);
    ImGui::SliderInt("Sky candidates", &settings.restirSkyCandidates, 1, 8);
    ImGui::SliderInt("BRDF candidates", &settings.restirBrdfCandidates, 0, 4);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Light candidates: points on glowing triangles (light tree), each per pixel and frame.\n"
                          "Sky candidates: directions picked by the sky's brightness (whenever the sky is sampled: see Sample the sky).\n"
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
    ImGui::Checkbox("Reuse neighbors", &settings.restirSpatial);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Each pixel also resamples a few nearby pixels' samples (a pass of its own, ~3 ms in Bistro).\n"
                          "Less noise, flicker and ghosting, above all where the camera uncovers something (no\n"
                          "history there).");
    }
    ImGui::BeginDisabled(!settings.restirSpatial);
    ImGui::SameLine();
    ImGui::Checkbox("Unbiased##spatial", &settings.restirSpatialVisibility);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Shadow rays from the neighbors' surfaces when a pixel keeps its own sample. Without them,\n"
                          "areas near shadows come out too dark (Sponza with the sky: 9.7%% at a radius of 30).");
    }
    ImGui::SliderInt("Neighbors", &settings.restirSpatialNeighbors, 0, 8);
    ImGui::SliderFloat("Radius", &settings.restirSpatialRadius, 2.0f, 60.0f, "%.0f px");
    ImGui::SliderInt("Neighbors (uncovered)", &settings.restirDisocclusionNeighbors, 0, 16);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Neighbors for pixels that found no history (just uncovered by the camera or an object).");
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    // ReSTIR GI (see RESTIR_GI_PLAN.md)
    ImGui::SeparatorText("ReSTIR GI");
    bool& restirGi = settings.restirGi(settings.renderMode);
    ImGui::Checkbox(accumulation ? "ReSTIR GI (Accumulation)" : "ReSTIR GI (DLSS modes)", &restirGi);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Light reaching the first surface via another one (bounce light), at the surfaces ReSTIR DI\n"
                          "would use (Min. roughness above): where the first bounce went and the light found there\n"
                          "become a sample, reused by the next frame and nearby pixels. Independent of ReSTIR DI.\n"
                          "Set separately for Accumulation and the DLSS modes (off by default in both: see LIMITATIONS.md).");
    }
    ImGui::BeginDisabled(!restirGi);
    ImGui::SliderFloat("Min. roughness (bounce)", &settings.restirGiMinRoughness, 0.0f, 1.0f, "%.2f");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Second surfaces at least this rough give a sample (0: all but glass). The light a smooth\n"
                          "surface sends depends on where it is seen from: reused by another pixel it is a little off.");
    }
    ImGui::Checkbox("Reuse last frame##gi", &settings.restirGiTemporal);
    ImGui::BeginDisabled(!settings.restirGiTemporal);
    ImGui::SameLine();
    ImGui::Checkbox("Unbiased##gitemporal", &settings.restirGiTemporalVisibility);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("A ray from last frame's surface per pixel. Without it, areas that see light through gaps\n"
                          "come out too dark (Sponza's side aisle: 6%%).");
    }
    ImGui::SameLine();
    ImGui::Checkbox("Shuffle pixels##gi", &settings.restirGiPermutation);
    ImGui::SliderFloat("Max. history##gi", &settings.restirGiMaxHistory, 1.0f, 50.0f, "%.0f x");
    ImGui::SliderInt("Max. age##gi", &settings.restirGiMaxAge, 1, 100, "%d frames");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Max. history: last frame's samples count for at most this many times this frame's.\n"
                          "Max. age: a sample is dropped after this many frames (its light may be out of date).");
    }
    ImGui::EndDisabled();
    ImGui::Checkbox("Reuse neighbors##gi", &settings.restirGiSpatial);
    ImGui::BeginDisabled(!settings.restirGiSpatial);
    ImGui::SameLine();
    ImGui::Checkbox("Unbiased##gispatial", &settings.restirGiSpatialVisibility);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Rays from the neighbors' surfaces for the kept sample, as ReSTIR DI's. Off (RTXDI's\n"
                          "default): areas near occluders can come out a little too dark.");
    }
    ImGui::SliderInt("Neighbors##gi", &settings.restirGiSpatialNeighbors, 0, 8);
    ImGui::SliderFloat("Radius##gi", &settings.restirGiSpatialRadius, 2.0f, 60.0f, "%.0f px");
    ImGui::SliderInt("Neighbors (uncovered)##gi", &settings.restirGiDisocclusionNeighbors, 0, 16);
    ImGui::EndDisabled();
    ImGui::Checkbox("Boiling filter", &settings.restirGiBoilingFilter);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Drops a sample that brings far more light than the others nearby, so one lucky sample\n"
                          "doesn't spread into a blotch that lasts for frames. Stronger: drops more (takes light away).");
    }
    ImGui::BeginDisabled(!settings.restirGiBoilingFilter);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::SliderFloat("Strength##boiling", &settings.restirGiBoilingStrength, 0.01f, 1.0f, "%.2f");
    ImGui::EndDisabled();
    ImGui::Checkbox("Clamp sample light", &settings.restirGiClampRadiance);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Limits the light of one GI sample to this brightness on screen (after exposure). Fewer\n"
                          "bright blotches, a little darker.");
    }
    ImGui::BeginDisabled(!settings.restirGiClampRadiance);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::SliderFloat("##giClamp", &settings.restirGiRadianceClampLevel, 0.25f, 64.0f, "%.2f",
                       ImGuiSliderFlags_Logarithmic);
    ImGui::EndDisabled();
    ImGui::EndDisabled();
}

void Renderer::buildDebugUi() {
    ImGui::Combo("Debug view", &settings.debugView,
                 "Off (path traced)\0Normals\0Texture coordinates\0Mesh index\0Material index\0"
                 "Base color\0Metallic\0Roughness\0Transmission\0Emissive\0Tangents\0"
                 "Radiance cache\0ReSTIR M\0ReSTIR light\0"
                 "DLSS input: depth\0DLSS input: motion vectors\0DLSS input: diffuse albedo\0"
                 "DLSS input: specular albedo\0DLSS input: normals\0DLSS input: roughness\0"
                 "DLSS input: specular hit distance\0DLSS input: reprojection error\0");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Radiance cache: the indirect light the cache holds for what the camera sees, cell by cell\n"
                          "(magenta: no cell yet, dark red: too few samples).\n"
                          "ReSTIR M: how many candidates each pixel's light sample stands for, from this frame's only\n"
                          "(red) through yellow and green to 64x (blue). Grey: no ReSTIR there.\n"
                          "ReSTIR light: the kind of light each pixel's sample is on: glowing triangle (orange),\n"
                          "punctual light (purple), sky (blue), none found (dark red), blocked (black, W = 0).\n"
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
}

// Records commands with `function`, submits them and waits until the GPU is done.
// Only meant for one-off setup work (uploads), not for per-frame rendering.
void Renderer::immediateSubmit(std::function<void(VkCommandBuffer cmd)>&& function)
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


void Renderer::cleanupImGui(){
    ImGui_ImplVulkan_Shutdown();
    vkDestroyDescriptorPool(device, imGuiPool, nullptr);
}
