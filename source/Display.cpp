//
// Display pass: the last step before the screen. Takes the linear HDR image (shaders/display.comp), applies
// exposure and tone mapping and writes an 8-bit image at output size, which is blitted into the swapchain.
//
// Kept apart from the trace shader so that something can sit in between: DLSS needs linear HDR input at the
// render size and produces an HDR image at the output size, which then gets exposure and tone mapping here.
//

#include <vulkan/vulkan.h>
#include <cmath>
#include <stdexcept>
#include <string>
#include "Application.h"

std::vector<char> readFile(const std::string& filename);

static const VkFormat displayFormat = VK_FORMAT_R8G8B8A8_UNORM;

// Output = the window. Render = what the path tracer traces; the same for now (DLSS will render smaller).
void Application::chooseExtents() {
    outputExtent = swapChainExtent;
    renderExtent = outputExtent;
}

void Application::createDisplayImages() {
    // The display image gets blitted into the swapchain, which converts the format (RGBA -> BGRA, linear -> sRGB).
    VkFormatProperties swapchainFormatProperties;
    vkGetPhysicalDeviceFormatProperties(physicalDevice, swapChainImageFormat, &swapchainFormatProperties);
    if (!(swapchainFormatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
        throw std::runtime_error("swap chain format does not support blitting into it!");
    }

    // Transitioned every frame (written here, then read by the blit), so no initial layout change is needed
    createImage(outputExtent.width, outputExtent.height, displayFormat,
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                displayImage, displayImageMemory, displayImageView);
}

void Application::destroyDisplayImages() {
    vkDestroyImageView(device, displayImageView, nullptr);
    vkDestroyImage(device, displayImage, nullptr);
    vkFreeMemory(device, displayImageMemory, nullptr);
}

void Application::createDisplayResources() {
    // Descriptors: 0 = HDR image (read), 1 = display image (write), 2 = exposure state (read)
    VkDescriptorSetLayoutBinding bindings[3] = {
            {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 3;
    layoutInfo.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &displayDescriptorSetLayout) != VK_SUCCESS) {
        throw std::runtime_error("failed to create display descriptor set layout!");
    }

    VkDescriptorPoolSize poolSizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &displayDescriptorPool) != VK_SUCCESS) {
        throw std::runtime_error("failed to create display descriptor pool!");
    }

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = displayDescriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &displayDescriptorSetLayout;
    if (vkAllocateDescriptorSets(device, &allocInfo, &displayDescriptorSet) != VK_SUCCESS) {
        throw std::runtime_error("failed to allocate display descriptor set!");
    }
    updateDisplayDescriptorSet();

    VkPushConstantRange pushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(DisplayPushConstants)};
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &displayDescriptorSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &displayPipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("failed to create display pipeline layout!");
    }

    VkShaderModule shaderModule = createShaderModule(readFile("display.comp.spv"));
    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = shaderModule;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = displayPipelineLayout;
    if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &displayPipeline) != VK_SUCCESS) {
        throw std::runtime_error("failed to create display pipeline!");
    }
    vkDestroyShaderModule(device, shaderModule, nullptr);
}

// Both images are recreated with the swapchain; called from recreateSwapChain.
void Application::updateDisplayDescriptorSet() {
    VkDescriptorImageInfo hdrInfo{VK_NULL_HANDLE, radianceImageView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo displayInfo{VK_NULL_HANDLE, displayImageView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo exposureInfo{exposureBuffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet writes[3]{};
    for (int i = 0; i < 3; i++) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = displayDescriptorSet;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
    }
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[0].pImageInfo = &hdrInfo;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].pImageInfo = &displayInfo;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[2].pBufferInfo = &exposureInfo;
    vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);
}

// Recorded after the trace and the exposure measurement. displayImage is already in GENERAL (transitioned at the
// start of the frame, see recordRayTracing); the blit after this pass transitions it again.
void Application::recordDisplay(VkCommandBuffer commandBuffer) {
    // The HDR image (trace) and the exposure (exposure.comp) must be written before this pass reads them.
    // Compute -> compute only: waiting for transfers here would also wait for the exposure readback copy.
    VkMemoryBarrier memoryBarrier{};
    memoryBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    memoryBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    memoryBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &memoryBarrier, 0, nullptr, 0, nullptr);

    // Auto: the measured exposure (read on the GPU) times the compensation. Manual: the slider.
    DisplayPushConstants push{};
    push.exposure = settings.autoExposure ? std::exp2(settings.exposureCompensation) : settings.exposure;
    push.autoExposure = settings.autoExposure ? 1 : 0;
    push.toneMapping = settings.toneMapping ? 1 : 0;
    push.passThrough = settings.debugView != static_cast<int>(DebugView::None) ? 1 : 0;

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, displayPipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, displayPipelineLayout, 0, 1,
                            &displayDescriptorSet, 0, nullptr);
    vkCmdPushConstants(commandBuffer, displayPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(commandBuffer, (outputExtent.width + 7) / 8, (outputExtent.height + 7) / 8, 1);
}

void Application::destroyDisplayResources() {
    vkDestroyPipeline(device, displayPipeline, nullptr);
    vkDestroyPipelineLayout(device, displayPipelineLayout, nullptr);
    vkDestroyDescriptorPool(device, displayDescriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(device, displayDescriptorSetLayout, nullptr);
}
