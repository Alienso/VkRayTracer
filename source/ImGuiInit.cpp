//
// Created by Alienson on 24.1.2024..
//

#include <functional>
#include <stdexcept>
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

    ImGui::Begin("Stats");
    ImGui::Text("GPU: %s", physicalDeviceProperties.deviceName);
    ImGui::Text("%.1f FPS (%.3f ms)", ImGui::GetIO().Framerate, 1000.0f / ImGui::GetIO().Framerate);
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
