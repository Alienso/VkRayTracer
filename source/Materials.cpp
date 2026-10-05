//
// Materials and textures on the GPU.
//
// All textures sit in one big descriptor array ("bindless"): a material stores plain integer indices into it,
// and the shader picks the texture by index at the hit. Materials live in a storage buffer.
//

#include <vulkan/vulkan.h>
#include <stdexcept>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <glm/gtc/constants.hpp>
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
        gpu.baseColorUvTransform = m.baseColorUvTransform;
        gpu.metallicRoughnessUvTransform = m.metallicRoughnessUvTransform;
        gpu.transmissionUvTransform = m.transmissionUvTransform;
        gpu.emissiveUvTransform = m.emissiveUvTransform;
        gpu.normalUvTransform = m.normalUvTransform;
        gpu.solid = m.solid ? 1 : 0;
        // Beer's law: after distance d, exp(-absorption * d) of the light is left. glTF gives the color left
        // after attenuationDistance, so absorption = -ln(color) / distance per channel.
        if (m.solid && m.attenuationDistance > 0.0f) {
            glm::vec3 color = glm::clamp(m.attenuationColor, glm::vec3(1e-4f), glm::vec3(1.0f));
            gpu.absorption = -glm::log(color) / m.attenuationDistance;
        }
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
    cacheClearPending = true;  // the cached light came from the old materials
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

// ---------------------------------------------------------------------------------------------
// Specular albedo table, for energy compensation of the GGX reflection
//
// The GGX model lets light reflect off exactly one microfacet; light that would hit a second one is simply
// lost, up to ~70% for very rough surfaces. The table stores how much a reflection actually returns, per
// view angle and roughness, and the shader uses it to put the lost light back (Turquin 2019) and to give the
// diffuse layer of a plastic exactly the light the reflection doesn't take.
//
// With Schlick's Fresnel F = F0 + (1 - F0) * (1 - v.h)^5, the total reflected is linear in F0:
//   E(F0) = F0 * scale + bias,  scale = average of (1 - (1 - v.h)^5),  bias = average of (1 - v.h)^5
// so two numbers per entry cover every F0 (the "split sum" table of real-time engines). For a white
// reflection (F0 = 1) the total is scale + bias.
// These are the same formulas as in shaders/raytrace.comp; the two must agree.

namespace {
    float smithG1(float cosTheta, float alpha) {
        float a2 = alpha * alpha;
        return 2.0f * cosTheta / (cosTheta + std::sqrt(a2 + (1.0f - a2) * cosTheta * cosTheta));
    }

    float smithVisibility(float cosL, float cosV, float alpha) {
        float a2 = alpha * alpha;
        float gv = cosL * std::sqrt(cosV * cosV * (1.0f - a2) + a2);
        float gl = cosV * std::sqrt(cosL * cosL * (1.0f - a2) + a2);
        return 0.5f / std::max(gv + gl, 1e-12f);
    }

    // Visible normal sampling (Dupuy & Benyoub 2023)
    glm::vec3 sampleGgxVisibleNormal(glm::vec3 v, float alpha, float u0, float u1) {
        glm::vec3 vStretched = glm::normalize(glm::vec3(v.x * alpha, v.y * alpha, v.z));
        float phi = 2.0f * glm::pi<float>() * u0;
        float z = (1.0f - u1) * (1.0f + vStretched.z) - vStretched.z;
        float sinTheta = std::sqrt(std::clamp(1.0f - z * z, 0.0f, 1.0f));
        glm::vec3 h = glm::vec3(sinTheta * std::cos(phi), sinTheta * std::sin(phi), z) + vStretched;
        return glm::normalize(glm::vec3(h.x * alpha, h.y * alpha, std::max(h.z, 0.0f)));
    }
}

void Application::createSpecularAlbedoTable() {
    const uint32_t n = specularAlbedoSize;
    const uint32_t strata = 48;  // samples per entry: strata^2, spread evenly instead of randomly (less noise)
    std::vector<glm::vec2> table(n * n);

    // Entry [roughness * n + angle]. Grid points include both ends (cos = 1, roughness = 0 and 1), the shader
    // interpolates between them. cos = 0 itself is undefined, the shader never goes below 1e-3 either.
    for (uint32_t r = 0; r < n; r++) {
        float roughness = static_cast<float>(r) / (n - 1);
        float alpha = std::max(roughness * roughness, 1e-3f);  // ggxAlpha in the shader
        for (uint32_t c = 0; c < n; c++) {
            float cosV = std::max(static_cast<float>(c) / (n - 1), 1e-3f);
            glm::vec3 v(std::sqrt(1.0f - cosV * cosV), 0.0f, cosV);

            // The shader's sampling: weight = BRDF * cos / pdf = G2 / G1 for a white reflection
            double scale = 0.0, bias = 0.0;
            for (uint32_t i = 0; i < strata; i++) {
                for (uint32_t j = 0; j < strata; j++) {
                    glm::vec3 h = sampleGgxVisibleNormal(v, alpha, (i + 0.5f) / strata, (j + 0.5f) / strata);
                    glm::vec3 l = glm::reflect(-v, h);
                    if (l.z <= 0.0f) {
                        continue;
                    }
                    float weight = smithVisibility(l.z, v.z, alpha) * 4.0f * l.z * v.z / smithG1(v.z, alpha);
                    float fresnelTerm = std::pow(1.0f - std::clamp(glm::dot(v, h), 0.0f, 1.0f), 5.0f);
                    scale += weight * (1.0f - fresnelTerm);
                    bias += weight * fresnelTerm;
                }
            }
            table[r * n + c] = glm::vec2(scale, bias) / static_cast<float>(strata * strata);
        }
    }

    createDeviceLocalBuffer(table.data(), sizeof(glm::vec2) * table.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                            specularAlbedoBuffer, specularAlbedoMemory);
}

void Application::destroySpecularAlbedoTable() {
    vkDestroyBuffer(device, specularAlbedoBuffer, nullptr);
    vkFreeMemory(device, specularAlbedoMemory, nullptr);
}
