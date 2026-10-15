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
    // The ImGui context and its GLFW side are the program's (Application::run); this is the Vulkan side, drawing
    // the UI over the image in the render pass
    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.ApiVersion = VK_API_VERSION_1_3;  // as the instance's
    init_info.Instance = gpu.instance;
    init_info.PhysicalDevice = gpu.physicalDevice;
    init_info.Device = gpu.device;
    init_info.QueueFamily = gpu.graphicsQueueFamily;
    init_info.Queue = gpu.graphicsQueue;
    // ImGui makes its own descriptor pool, room for its font texture and textures we may show in the UI later
    init_info.DescriptorPoolSize = 100;
    init_info.MinImageCount = static_cast<uint32_t>(swapChainImages.size());
    init_info.ImageCount = static_cast<uint32_t>(swapChainImages.size());
    // ImGui creates its pipeline against our render pass, subpass 0
    init_info.PipelineInfoMain.RenderPass = renderPass;
    init_info.PipelineInfoMain.Subpass = 0;
    init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    // The font texture is uploaded by ImGui itself when it first draws (and again when it changes)
    if (!ImGui_ImplVulkan_Init(&init_info)) {
        throw std::runtime_error("failed to initialize imGui Vulkan backend!");
    }
}

void Renderer::cleanupImGui(){
    ImGui_ImplVulkan_Shutdown();
}
