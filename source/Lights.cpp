//
// Light list for sampling emissive surfaces ("next event estimation" for glowing triangles).
//
// For the sun we aim a shadow ray at it from every hit. Glowing surfaces get the same treatment here:
// every triangle with an emissive material goes into a list (in world space), and at every hit the shader
// picks one at random, picks a random point on it and checks with a shadow ray whether that point is visible.
// Brighter and larger triangles are picked more often, so the samples go where most light comes from.
//

#include <vulkan/vulkan.h>
#include <stdexcept>
#include <cstring>
#include <algorithm>
#include "Application.h"
#include "Configuration.h"

void Application::createStreamedBuffer(StreamedBuffer& streamed, VkDeviceSize capacity) {
    streamed.capacity = capacity;
    createBuffer(capacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, streamed.buffer, streamed.memory);

    streamed.staging.resize(Configuration::maxFramesInFlight);
    streamed.stagingMemory.resize(Configuration::maxFramesInFlight);
    streamed.stagingMapped.resize(Configuration::maxFramesInFlight);
    for (int i = 0; i < Configuration::maxFramesInFlight; i++) {
        createBuffer(capacity, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     streamed.staging[i], streamed.stagingMemory[i]);
        vkMapMemory(device, streamed.stagingMemory[i], 0, capacity, 0, &streamed.stagingMapped[i]);
    }
}

void Application::destroyStreamedBuffer(StreamedBuffer& streamed) {
    for (size_t i = 0; i < streamed.staging.size(); i++) {
        vkUnmapMemory(device, streamed.stagingMemory[i]);
        vkDestroyBuffer(device, streamed.staging[i], nullptr);
        vkFreeMemory(device, streamed.stagingMemory[i], nullptr);
    }
    vkDestroyBuffer(device, streamed.buffer, nullptr);
    vkFreeMemory(device, streamed.memory, nullptr);
    streamed = {};
}

// Grows the buffer if `size` doesn't fit. Returns true if it was recreated (the descriptor set must then be
// updated). Only call while no frame is in flight that uses it.
bool Application::ensureStreamedCapacity(StreamedBuffer& streamed, VkDeviceSize size) {
    if (size <= streamed.capacity) {
        return false;
    }
    VkDeviceSize newCapacity = std::max(size, streamed.capacity * 2);
    destroyStreamedBuffer(streamed);
    createStreamedBuffer(streamed, newCapacity);
    return true;
}

void Application::recordStreamedUpload(VkCommandBuffer commandBuffer, StreamedBuffer& streamed, const void* data, VkDeviceSize size) {
    if (size == 0) {
        return;
    }
    std::memcpy(streamed.stagingMapped[currentFrame], data, size);

    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = streamed.buffer;
    barrier.offset = 0;
    barrier.size = VK_WHOLE_SIZE;

    // The previous frame's shader may still read the old contents.
    barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 1, &barrier, 0, nullptr);

    VkBufferCopy copy{0, 0, size};
    vkCmdCopyBuffer(commandBuffer, streamed.staging[currentFrame], streamed.buffer, 1, &copy);

    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 1, &barrier, 0, nullptr);
}

void Application::createLightBuffers() {
    // Start small, they grow when needed. Never empty: descriptors can't point at nothing.
    createStreamedBuffer(emissiveTriangleBuffer, 1024 * sizeof(EmissiveTriangle));
    createStreamedBuffer(instanceLightBuffer, 256 * sizeof(int32_t));
}

void Application::destroyLightBuffers() {
    destroyStreamedBuffer(emissiveTriangleBuffer);
    destroyStreamedBuffer(instanceLightBuffer);
}

static float luminance(glm::vec3 color) {
    return glm::dot(color, glm::vec3(0.2126f, 0.7152f, 0.0722f));
}

// Collects the emissive triangles of this frame's instances (frameInstances) in world space.
void Application::buildLightList() {
    emissiveTriangles.clear();
    instanceFirstLight.assign(frameInstances.size(), -1);
    double totalWeight = 0.0;

    for (size_t i = 0; i < frameInstances.size(); i++) {
        const Mesh& mesh = sceneMeshes[frameInstanceMeshes[i]];
        const SceneMaterial& sceneMaterial = sceneMaterials[mesh.materialIndex];
        const Material& material = sceneMaterial.material;
        glm::vec3 emission = material.emissiveColor * material.emissiveStrength;
        if (luminance(emission) <= 0.0f) {
            continue;
        }
        // With an emissive texture, each triangle is weighted by the texture's average over it (computed at
        // load time), so the dark parts are (almost) never sampled. A triangle averaging black gets probability
        // 0 but stays in the list, so the shader still finds every hit triangle at firstLight + primitive index.
        // If a few bright texels were missed by the average, bounce rays still find them (MIS gives them full
        // weight there), so the image stays correct.
        bool useTexture = material.emissiveTexture >= 0 && sceneMaterial.useEmissiveTexture &&
                          mesh.emissiveTextureAverage.size() * 3 == mesh.indices.size();

        // Every triangle of the mesh, in order, so the shader finds a hit triangle at firstLight + primitive index.
        instanceFirstLight[i] = static_cast<int32_t>(emissiveTriangles.size());
        const glm::mat4& transform = frameInstanceTransforms[i];
        for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
            const SceneVertex& v0 = mesh.vertices[mesh.indices[t]];
            const SceneVertex& v1 = mesh.vertices[mesh.indices[t + 1]];
            const SceneVertex& v2 = mesh.vertices[mesh.indices[t + 2]];

            EmissiveTriangle triangle{};
            triangle.p0 = glm::vec3(transform * glm::vec4(v0.position, 1.0f));
            triangle.p1 = glm::vec3(transform * glm::vec4(v1.position, 1.0f));
            triangle.p2 = glm::vec3(transform * glm::vec4(v2.position, 1.0f));
            triangle.uv0 = v0.uv;
            triangle.uv1 = v1.uv;
            triangle.uv2 = v2.uv;
            triangle.materialIndex = mesh.materialIndex;
            triangle.area = 0.5f * glm::length(glm::cross(triangle.p1 - triangle.p0, triangle.p2 - triangle.p0));
            glm::vec3 color = useTexture ? emission * mesh.emissiveTextureAverage[t / 3] : emission;
            triangle.probability = triangle.area * luminance(color);  // a weight for now, normalized below
            totalWeight += triangle.probability;
            emissiveTriangles.push_back(triangle);
        }
    }

    sampledEmissiveTriangles = static_cast<size_t>(std::count_if(emissiveTriangles.begin(), emissiveTriangles.end(),
                                                                 [](const EmissiveTriangle& t) { return t.probability > 0.0f; }));

    // Weights -> probabilities, and the running sum (CDF) the shader searches through.
    if (totalWeight > 0.0) {
        double sum = 0.0;
        for (EmissiveTriangle& triangle : emissiveTriangles) {
            triangle.probability = static_cast<float>(triangle.probability / totalWeight);
            sum += triangle.probability;
            triangle.cdf = static_cast<float>(sum);
        }
        emissiveTriangles.back().cdf = 1.0f;  // no rounding gap at the end
    } else {
        // Glowing materials, but all triangles have zero area or a black texture: nothing to sample.
        emissiveTriangles.clear();
        instanceFirstLight.assign(frameInstances.size(), -1);
        sampledEmissiveTriangles = 0;
    }
    lightsUploadPending = true;
}

void Application::recordLightUpload(VkCommandBuffer commandBuffer) {
    recordStreamedUpload(commandBuffer, emissiveTriangleBuffer, emissiveTriangles.data(),
                         sizeof(EmissiveTriangle) * emissiveTriangles.size());
    recordStreamedUpload(commandBuffer, instanceLightBuffer, instanceFirstLight.data(),
                         sizeof(int32_t) * instanceFirstLight.size());
    lightsUploadPending = false;
}
