//
// Created by Alienson on 24.1.2024..
//

#include <functional>
#include "../include/imgui/imgui.h"
#include "../include/imgui/backends/imgui_impl_vulkan.h"
#include "../include/imgui/backends/imgui_impl_glfw.h"

#include "Application.h"
#include <vulkan/vulkan.h>

void Application::initImGui(){
    // 1: create descriptor pool for IMGUI
    //  the size of the pool is very oversize, but it's copied from imgui demo
    //  itself.
    VkDescriptorPoolSize pool_sizes[] = { { VK_DESCRIPTOR_TYPE_SAMPLER, 1000 },
                                          { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
                                          { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000 },
                                          { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000 },
                                          { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000 },
                                          { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000 },
                                          { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000 },
                                          { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000 },
                                          { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000 },
                                          { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000 },
                                          { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000 } };

    VkDescriptorPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = 1000;
    pool_info.poolSizeCount = (uint32_t)std::size(pool_sizes);
    pool_info.pPoolSizes = pool_sizes;

    vkCreateDescriptorPool(device, &pool_info, nullptr, &imGuiPool);

    // 2: initialize imgui library

    // this initializes the core structures of imgui
    ImGui::CreateContext();

    // this initializes imgui for SDL
    ImGui_ImplGlfw_InitForVulkan(window, true); //?

    // this initializes imgui for Vulkan
    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.Instance = instance;
    init_info.PhysicalDevice = physicalDevice;
    init_info.Device = device;
    init_info.Queue = graphicsQueue;
    init_info.DescriptorPool = imGuiPool;
    init_info.MinImageCount = 3;
    init_info.ImageCount = 3;
    /*init_info.UseDynamicRendering = true;
    init_info.ColorAttachmentFormat = _swapchainImageFormat;*/

    init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

    ImGui_ImplVulkan_Init(&init_info, renderPass); //?

    // execute a gpu command to upload imgui font textures
    Application::immediateSubmit([&](VkCommandBuffer cmd) { ImGui_ImplVulkan_CreateFontsTexture(cmd); });
    //ImGui_ImplVulkan_CreateFontsTexture(imGuiCommandBuffer);

    // clear font textures from cpu data
    //ImGui_ImplVulkan_DestroyFontUploadObjects();

    // add the destroy the imgui created structures
    /*_mainDeletionQueue.push_function([=]() {
        vkDestroyDescriptorPool(device, imguiPool, nullptr);
        ImGui_ImplVulkan_Shutdown();
    });*/
}

VkCommandBufferBeginInfo command_buffer_begin_info(VkCommandBufferUsageFlags flags){
    VkCommandBufferBeginInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    info.pNext = nullptr;

    info.pInheritanceInfo = nullptr;
    info.flags = flags;
    return info;
}

/*VkCommandBuffer command_buffer_submit_info(VkCommandBuffer cmd){
    //VkCommandBuffer info{};
    info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    info.pNext = nullptr;
    info.commandBuffer = cmd;
    info.deviceMask = 0;

    return info;
}*/


VkSubmitInfo submit_info(VkCommandBuffer* cmd, VkSemaphoreSubmitInfo* signalSemaphoreInfo,
                                  VkSemaphoreSubmitInfo* waitSemaphoreInfo){
    VkSubmitInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    info.pNext = nullptr;

    info.waitSemaphoreCount = waitSemaphoreInfo == nullptr ? 0 : 1;
    info.pWaitSemaphores = nullptr;

    info.signalSemaphoreCount = signalSemaphoreInfo == nullptr ? 0 : 1;
    info.pSignalSemaphores = nullptr;

    info.commandBufferCount = 1;
    info.pCommandBuffers = cmd;

    return info;
}


void Application::immediateSubmit(std::function<void(VkCommandBuffer cmd)>&& function)
{
    (vkResetFences(device, 1, &imGuiFence));
    (vkResetCommandBuffer(imGuiCommandBuffer, 0));

    VkCommandBuffer cmd = imGuiCommandBuffer;

    VkCommandBufferBeginInfo cmdBeginInfo = command_buffer_begin_info(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);

    (vkBeginCommandBuffer(cmd, &cmdBeginInfo));

    function(cmd);

    (vkEndCommandBuffer(cmd));

    VkCommandBuffer cmdinfo = imGuiCommandBuffer;//command_buffer_submit_info(cmd);
    VkSubmitInfo submit = submit_info(&cmdinfo, nullptr, nullptr);

    // submit command buffer to the queue and execute it.
    //  _renderFence will now block until the graphic commands finish execution
    (vkQueueSubmit(graphicsQueue, 1, &submit, imGuiFence));

    (vkWaitForFences(device, 1, &imGuiFence, true, 9999999999));
}


void Application::cleanupImGui(){
    vkDestroyFence(device, imGuiFence, nullptr);
    vkDestroyDescriptorPool(device, imGuiPool, nullptr);
    ImGui_ImplVulkan_Shutdown();
}

