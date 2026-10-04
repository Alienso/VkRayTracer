//
// Materials and textures on the GPU.
//
// All textures sit in one big descriptor array ("bindless"): a material stores plain integer indices into it,
// and the shader picks the texture by index at the hit. Materials live in a storage buffer.
//

#include <vulkan/vulkan.h>
#include <stdexcept>
#include <cstring>
#include <algorithm>
#include "Application.h"

static VkSamplerAddressMode toVulkan(TextureWrap wrap) {
    switch (wrap) {
        case TextureWrap::ClampToEdge: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case TextureWrap::MirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

// Textures with the same filter and wrap modes share a sampler.
VkSampler Application::getSampler(const TextureData& texture) {
    uint32_t key = (texture.nearest ? 1u : 0u) | (static_cast<uint32_t>(texture.wrapU) << 1) | (static_cast<uint32_t>(texture.wrapV) << 3);
    auto it = samplers.find(key);
    if (it != samplers.end()) {
        return it->second;
    }

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = texture.nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    samplerInfo.minFilter = samplerInfo.magFilter;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;  // only level 0 for now (see the shader)
    samplerInfo.addressModeU = toVulkan(texture.wrapU);
    samplerInfo.addressModeV = toVulkan(texture.wrapV);
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.maxLod = 0.0f;

    VkSampler sampler;
    if (vkCreateSampler(device, &samplerInfo, nullptr, &sampler) != VK_SUCCESS) {
        throw std::runtime_error("failed to create texture sampler!");
    }
    samplers[key] = sampler;
    return sampler;
}

// Uploads the textures loaded since the last call. Textures are never removed, so earlier indices stay valid.
void Application::uploadNewTextures() {
    if (sceneTextures.size() > maxTextures) {
        throw std::runtime_error("too many textures for the descriptor array!");
    }

    for (size_t i = gpuTextures.size(); i < sceneTextures.size(); i++) {
        TextureData& texture = sceneTextures[i];
        GpuTexture gpu;

        // sRGB format: the GPU converts to linear when sampling, so the shader always works with linear values.
        VkFormat format = texture.srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        createImage(texture.width, texture.height, format, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    gpu.image, gpu.memory, gpu.view);
        gpu.sampler = getSampler(texture);

        VkDeviceSize size = texture.pixels.size();
        VkBuffer stagingBuffer;
        VkDeviceMemory stagingMemory;
        createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     stagingBuffer, stagingMemory);
        void* mapped;
        vkMapMemory(device, stagingMemory, 0, size, 0, &mapped);
        std::memcpy(mapped, texture.pixels.data(), size);
        vkUnmapMemory(device, stagingMemory);

        immediateSubmit([&](VkCommandBuffer cmd) {
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = gpu.image;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &barrier);

            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {texture.width, texture.height, 1};
            vkCmdCopyBufferToImage(cmd, stagingBuffer, gpu.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &barrier);
        });

        vkDestroyBuffer(device, stagingBuffer, nullptr);
        vkFreeMemory(device, stagingMemory, nullptr);

        textureMemorySize += size;
        texture.pixels.clear();
        texture.pixels.shrink_to_fit();  // the GPU has its copy now
        gpuTextures.push_back(gpu);
    }
}

void Application::destroyTextures() {
    for (GpuTexture& texture : gpuTextures) {
        vkDestroyImageView(device, texture.view, nullptr);
        vkDestroyImage(device, texture.image, nullptr);
        vkFreeMemory(device, texture.memory, nullptr);
    }
    gpuTextures.clear();
    for (auto& [key, sampler] : samplers) {
        vkDestroySampler(device, sampler, nullptr);
    }
    samplers.clear();
}

std::vector<GpuMaterial> Application::packMaterials() const {
    std::vector<GpuMaterial> packed;
    packed.reserve(sceneMaterials.size());
    for (const SceneMaterial& sceneMaterial : sceneMaterials) {
        const Material& m = sceneMaterial.material;
        GpuMaterial gpu{};
        gpu.baseColor = m.baseColor;
        gpu.emissive = m.emissiveColor * m.emissiveStrength;
        gpu.metallic = m.metallic;
        gpu.roughness = m.roughness;
        gpu.transmission = m.transmission;
        gpu.ior = m.ior;
        gpu.alphaCutoff = m.alphaCutoff;
        gpu.alphaMode = static_cast<uint32_t>(m.alphaMode);
        gpu.baseColorTexture = sceneMaterial.useBaseColorTexture ? m.baseColorTexture : -1;
        gpu.metallicRoughnessTexture = sceneMaterial.useMetallicRoughnessTexture ? m.metallicRoughnessTexture : -1;
        gpu.transmissionTexture = sceneMaterial.useTransmissionTexture ? m.transmissionTexture : -1;
        gpu.emissiveTexture = sceneMaterial.useEmissiveTexture ? m.emissiveTexture : -1;
        gpu.normalTexture = sceneMaterial.useNormalTexture ? m.normalTexture : -1;
        gpu.normalScale = m.normalScale;
        packed.push_back(gpu);
    }
    return packed;
}

// Recreated whenever a model is loaded (the material count changes). TRANSFER_DST: edits are written into it later.
void Application::createMaterialBuffer() {
    std::vector<GpuMaterial> packed = packMaterials();
    createDeviceLocalBuffer(packed.data(), sizeof(GpuMaterial) * packed.size(),
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            materialBuffer, materialBufferMemory);
    materialBufferCreated = true;
    materialsDirty = false;
}

void Application::destroyMaterialBuffer() {
    vkDestroyBuffer(device, materialBuffer, nullptr);
    vkFreeMemory(device, materialBufferMemory, nullptr);
    materialBufferCreated = false;
}

// A material was edited in the UI: copy all materials into the buffer, inside this frame's command buffer
// (like the uniforms), so editing never has to wait for the GPU.
void Application::recordMaterialUpdate(VkCommandBuffer commandBuffer) {
    std::vector<GpuMaterial> packed = packMaterials();
    VkDeviceSize size = sizeof(GpuMaterial) * packed.size();

    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = materialBuffer;
    barrier.offset = 0;
    barrier.size = VK_WHOLE_SIZE;

    // The previous frame's shader may still be reading the materials.
    barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 1, &barrier, 0, nullptr);

    // vkCmdUpdateBuffer takes at most 65536 bytes per call
    const VkDeviceSize maxChunk = 65536;
    const auto* bytes = reinterpret_cast<const uint8_t*>(packed.data());
    for (VkDeviceSize offset = 0; offset < size; offset += maxChunk) {
        vkCmdUpdateBuffer(commandBuffer, materialBuffer, offset, std::min(maxChunk, size - offset), bytes + offset);
    }

    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 1, &barrier, 0, nullptr);

    materialsDirty = false;
    resetAccumulation = true;
}
