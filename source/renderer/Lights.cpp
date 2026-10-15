//
// Light list for sampling emissive surfaces ("next event estimation" for glowing triangles).
//
// For the sun we aim a shadow ray at it from every hit. Glowing surfaces get the same treatment here:
// every triangle with an emissive material goes into a list (in world space), and at every hit the shader
// picks one at random, picks a random point on it and checks with a shadow ray whether that point is visible.
// Brighter and larger triangles are picked more often, so the samples go where most light comes from.
// The lights that come with the models (KHR_lights_punctual) are collected here too, see the end.
//

#include <vulkan/vulkan.h>
#include <stdexcept>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <limits>
#include <glm/gtc/constants.hpp>
#include <imgui/imgui.h>
#include "renderer/Renderer.h"
#include "core/Configuration.h"

// Staging for all streamed buffers: a fixed size, however big they get (a staging copy of each, per frame in flight,
// took ~700 MB of pinned system memory for Jungle Ruins' instances)
void Renderer::createUploadRings() {
    uploadRings.resize(Configuration::maxFramesInFlight);
    for (UploadRing& ring : uploadRings) {
        createBuffer(uploadRingSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     ring.buffer, ring.memory, VK_NULL_HANDLE, true);
        vmaMapMemory(allocator, ring.memory, &ring.mapped);
    }
    uploadRingUsed = 0;
}

void Renderer::destroyUploadRings() {
    for (UploadRing& ring : uploadRings) {
        vmaUnmapMemory(allocator, ring.memory);
        vkDestroyBuffer(device, ring.buffer, nullptr);
        vmaFreeMemory(allocator, ring.memory);
    }
    uploadRings.clear();
}

void Renderer::createStreamedBuffer(StreamedBuffer& streamed, VkDeviceSize capacity, VkBufferUsageFlags usage,
                                    VkPipelineStageFlags readers) {
    streamed.capacity = capacity;
    streamed.usage = usage;
    streamed.readers = readers;
    createBuffer(capacity, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                 streamed.buffer, streamed.memory, VK_NULL_HANDLE, true);
}

void Renderer::destroyStreamedBuffer(StreamedBuffer& streamed) {
    vkDestroyBuffer(device, streamed.buffer, nullptr);
    vmaFreeMemory(allocator, streamed.memory);
    streamed = {};
}

// Grows the buffer if `size` doesn't fit. Returns true if it was recreated (the descriptor set must then be
// updated). Only call while no frame is in flight that uses it.
bool Renderer::ensureStreamedCapacity(StreamedBuffer& streamed, VkDeviceSize size) {
    if (size <= streamed.capacity) {
        return false;
    }
    VkDeviceSize newCapacity = std::max(size, streamed.capacity * 2);
    VkBufferUsageFlags usage = streamed.usage;
    VkPipelineStageFlags readers = streamed.readers;
    destroyStreamedBuffer(streamed);
    createStreamedBuffer(streamed, newCapacity, usage, readers);
    return true;
}

// The pieces into the buffer: packed into this frame's part of the upload ring and copied over, after the buffer's
// readers in the previous frame are done with the old contents, before its readers in this one
void Renderer::recordStreamedCopies(VkCommandBuffer commandBuffer, StreamedBuffer& streamed,
                                    const std::vector<StreamedPiece>& pieces) {
    auto aligned = [](VkDeviceSize size) { return (size + 15) & ~VkDeviceSize(15); };
    VkDeviceSize total = 0;
    for (const StreamedPiece& piece : pieces) {
        total += aligned(piece.size);
    }

    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = streamed.buffer;
    barrier.offset = 0;
    barrier.size = VK_WHOLE_SIZE;
    if (total <= uploadRingSize - uploadRingUsed) {
        UploadRing& ring = uploadRings[currentFrame];
        std::vector<VkBufferCopy> copies;
        copies.reserve(pieces.size());
        for (const StreamedPiece& piece : pieces) {
            std::memcpy(static_cast<uint8_t*>(ring.mapped) + uploadRingUsed, piece.data, piece.size);
            copies.push_back({uploadRingUsed, piece.offset, piece.size});
            uploadRingUsed += aligned(piece.size);
        }
        // (Shader reads, and acceleration structure builds reading their input, both count as shader reads)
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(commandBuffer, streamed.readers, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 1, &barrier, 0, nullptr);
        vkCmdCopyBuffer(commandBuffer, ring.buffer, streamed.buffer, static_cast<uint32_t>(copies.size()),
                        copies.data());
    } else {
        uploadStreamedNow(streamed, pieces);
    }
    // (Copies submitted before this command buffer, by uploadStreamedNow, are covered too)
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, streamed.readers,
                         0, 0, nullptr, 1, &barrier, 0, nullptr);
}

// Too much for the ring (everything changed in a big scene: loading it, adding an object): copied right away through
// a temporary staging buffer of the ring's size, a ring's worth at a time, the CPU waiting for each. The frames
// still in flight are done with the buffer first. This frame's command buffer isn't submitted yet, so its readers
// see the new contents. Rare, and a few dozen milliseconds for Jungle Ruins' instances.
void Renderer::uploadStreamedNow(StreamedBuffer& streamed, const std::vector<StreamedPiece>& pieces) {
    vkQueueWaitIdle(graphicsQueue);
    VkDeviceSize total = 0;
    for (const StreamedPiece& piece : pieces) {
        total += piece.size;
    }
    VkDeviceSize chunk = std::min(total, uploadRingSize);
    VkBuffer staging;
    VmaAllocation stagingMemory;
    void* mapped;
    createBuffer(chunk, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 staging, stagingMemory, VK_NULL_HANDLE, true);
    vmaMapMemory(allocator, stagingMemory, &mapped);

    std::vector<VkBufferCopy> copies;
    VkDeviceSize used = 0;
    auto flush = [&]() {
        immediateSubmit([&](VkCommandBuffer cmd) {
            vkCmdCopyBuffer(cmd, staging, streamed.buffer, static_cast<uint32_t>(copies.size()), copies.data());
        });
        copies.clear();
        used = 0;
    };
    for (const StreamedPiece& piece : pieces) {
        for (VkDeviceSize done = 0; done < piece.size;) {
            if (used == chunk) {
                flush();
            }
            VkDeviceSize size = std::min(piece.size - done, chunk - used);
            std::memcpy(static_cast<uint8_t*>(mapped) + used, static_cast<const uint8_t*>(piece.data) + done, size);
            copies.push_back({used, piece.offset + done, size});
            used += size;
            done += size;
        }
    }
    if (!copies.empty()) {
        flush();
    }

    vmaUnmapMemory(allocator, stagingMemory);
    vkDestroyBuffer(device, staging, nullptr);
    vmaFreeMemory(allocator, stagingMemory);
}

void Renderer::recordStreamedUpload(VkCommandBuffer commandBuffer, StreamedBuffer& streamed, const void* data, VkDeviceSize size) {
    if (size == 0) {
        return;
    }
    recordStreamedCopies(commandBuffer, streamed, {{data, 0, size}});
}

// The same for some ranges of elements: each goes to the same place in the buffer
void Renderer::recordStreamedUpload(VkCommandBuffer commandBuffer, StreamedBuffer& streamed, const void* data,
                                    VkDeviceSize elementSize, const std::vector<InstanceRange>& ranges) {
    if (ranges.empty()) {
        return;
    }
    // Sorted, overlapping and touching ones joined: the same element may have changed in several frames
    std::vector<InstanceRange> merged = ranges;
    std::sort(merged.begin(), merged.end(),
              [](const InstanceRange& a, const InstanceRange& b) { return a.first < b.first; });
    size_t kept = 0;
    for (size_t i = 1; i < merged.size(); i++) {
        InstanceRange& last = merged[kept];
        if (merged[i].first <= last.first + last.count) {
            last.count = std::max(last.count, merged[i].first + merged[i].count - last.first);
        } else {
            merged[++kept] = merged[i];
        }
    }
    merged.resize(kept + 1);

    std::vector<StreamedPiece> pieces;
    pieces.reserve(merged.size());
    for (const InstanceRange& range : merged) {
        VkDeviceSize offset = range.first * elementSize;
        pieces.push_back({static_cast<const uint8_t*>(data) + offset, offset, range.count * elementSize});
    }
    recordStreamedCopies(commandBuffer, streamed, pieces);
}

void Renderer::createLightBuffers() {
    // Start small, they grow when needed. Never empty: descriptors can't point at nothing.
    createStreamedBuffer(emissiveTriangleBuffer, 1024 * sizeof(EmissiveTriangle));
    createStreamedBuffer(instanceLightBuffer, 256 * sizeof(int32_t));
    createStreamedBuffer(punctualLightBuffer, 64 * sizeof(GpuPunctualLight));
    createStreamedBuffer(lightTreeBuffer, 2048 * sizeof(LightTreeNode));
}

void Renderer::destroyLightBuffers() {
    destroyStreamedBuffer(emissiveTriangleBuffer);
    destroyStreamedBuffer(instanceLightBuffer);
    destroyStreamedBuffer(punctualLightBuffer);
    destroyStreamedBuffer(lightTreeBuffer);
}

static float luminance(glm::vec3 color) {
    return glm::dot(color, glm::vec3(0.2126f, 0.7152f, 0.0722f));
}

// Collects the emissive triangles of this frame's instances (renderList's, one to one with the TLAS's) in world
// space.
void Renderer::buildLightList(bool instancesChanged) {
    lightListPicking = settings.lightPicking;
    auto emissionOf = [&](const Mesh& mesh) {
        const Material& material = assets.materials()[mesh.materialIndex].material;
        return material.emissiveColor * material.emissiveStrength;
    };
    // Which instances glow: only looked up when instances or materials changed (scenes with vegetation have millions)
    if (instancesChanged) {
        emissiveInstances.clear();
        for (size_t i = 0; i < renderList->instances.size(); i++) {
            if (luminance(emissionOf(assets.meshes()[renderList->instances[i].mesh])) > 0.0f) {
                emissiveInstances.push_back(static_cast<uint32_t>(i));
            }
        }
        instanceFirstLight.assign(renderList->instances.size(), -1);
        instanceLightsUploadPending = true;
    }
    emissiveTriangles.clear();
    std::vector<int32_t> firstLights(emissiveInstances.size(), -1);  // per glowing instance, as instanceFirstLight
    double totalWeight = 0.0;

    for (size_t e = 0; e < emissiveInstances.size(); e++) {
        uint32_t i = emissiveInstances[e];
        const Mesh& mesh = assets.meshes()[renderList->instances[i].mesh];
        const SceneMaterial& sceneMaterial = assets.materials()[mesh.materialIndex];
        const Material& material = sceneMaterial.material;
        glm::vec3 emission = emissionOf(mesh);
        // With an emissive texture, each triangle is weighted by the texture's average over it (computed at
        // load time), so the dark parts are (almost) never sampled. A triangle averaging black gets probability
        // 0 but stays in the list, so the shader still finds every hit triangle at firstLight + primitive index.
        // If a few bright texels were missed by the average, bounce rays still find them (MIS gives them full
        // weight there), so the image stays correct.
        bool useTexture = material.emissiveTexture >= 0 && sceneMaterial.useEmissiveTexture &&
                          mesh.emissiveTextureAverage.size() * 3 == mesh.indices.size();

        // Every triangle of the mesh, in order, so the shader finds a hit triangle at firstLight + primitive index.
        firstLights[e] = static_cast<int32_t>(emissiveTriangles.size());
        const glm::mat4& transform = renderList->instances[i].world;
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
        // Only when it is used: it's most of the rebuild's time (picking by power needs just the CDF)
        if (lightListPicking == static_cast<int>(LightPicking::Tree)) {
            buildLightTree();
        } else {
            lightTree.clear();
            lightTreeDepth = 0;
        }
    } else {
        // Glowing materials, but all triangles have zero area or a black texture: nothing to sample.
        emissiveTriangles.clear();
        firstLights.assign(firstLights.size(), -1);
        sampledEmissiveTriangles = 0;
        lightTree.clear();
        lightTreeDepth = 0;
    }

    std::vector<uint64_t> layout;
    for (size_t e = 0; e < emissiveInstances.size(); e++) {
        int32_t& first = instanceFirstLight[emissiveInstances[e]];
        if (first != firstLights[e]) {
            first = firstLights[e];
            instanceLightsUploadPending = true;
        }
        if (first >= 0) {
            layout.push_back(assets.meshIds()[renderList->instances[emissiveInstances[e]].mesh]);
            layout.push_back(static_cast<uint64_t>(first));
        }
    }
    if (layout != lightLayout) {
        lightLayout = std::move(layout);
        lightIndicesChanged = true;
    }
    lightsUploadPending = true;
}

// The light tree over the triangles that can be picked (probability > 0). Each node holds the box around its
// triangles and their summed probability; the shader walks down from the root and at every node picks a child by
// how much light it could send to the shading point (see pickFromLightTree). Built by splitting the triangles at
// the median of their centers along the box's longest axis, down to one triangle per leaf, so the depth is
// log2(triangles) rounded up, and the 32 bits of treeTrail are plenty.
void Renderer::buildLightTree() {
    lightTree.clear();
    lightTreeDepth = 0;

    std::vector<uint32_t> triangles;
    for (uint32_t i = 0; i < emissiveTriangles.size(); i++) {
        emissiveTriangles[i].treeTrail = 0;
        if (emissiveTriangles[i].probability > 0.0f) {
            triangles.push_back(i);
        }
    }
    if (triangles.empty()) {
        return;
    }
    lightTree.reserve(2 * triangles.size() - 1);

    auto center = [&](uint32_t i) {
        const EmissiveTriangle& t = emissiveTriangles[i];
        return (t.p0 + t.p1 + t.p2) / 3.0f;
    };

    // Fills node `nodeIndex` with triangles[first, last). `trail` is the way down to it, `depth` its level.
    auto build = [&](auto&& self, uint32_t nodeIndex, size_t first, size_t last, uint32_t trail, uint32_t depth) -> void {
        LightTreeNode node{};
        node.boundsMin = glm::vec3(std::numeric_limits<float>::max());
        node.boundsMax = glm::vec3(-std::numeric_limits<float>::max());
        glm::vec3 centerMin = node.boundsMin;
        glm::vec3 centerMax = node.boundsMax;
        double power = 0.0;
        for (size_t i = first; i < last; i++) {
            const EmissiveTriangle& t = emissiveTriangles[triangles[i]];
            node.boundsMin = glm::min(node.boundsMin, glm::min(t.p0, glm::min(t.p1, t.p2)));
            node.boundsMax = glm::max(node.boundsMax, glm::max(t.p0, glm::max(t.p1, t.p2)));
            glm::vec3 c = center(triangles[i]);
            centerMin = glm::min(centerMin, c);
            centerMax = glm::max(centerMax, c);
            power += t.probability;
        }
        node.power = static_cast<float>(power);
        lightTreeDepth = std::max(lightTreeDepth, depth);

        if (last - first == 1) {
            node.child = -static_cast<int32_t>(triangles[first]) - 1;
            emissiveTriangles[triangles[first]].treeTrail = trail;
            lightTree[nodeIndex] = node;
            return;
        }

        // Split along the axis where the centers spread most; nth_element puts the median in place.
        glm::vec3 extent = centerMax - centerMin;
        int axis = extent.x >= extent.y && extent.x >= extent.z ? 0 : (extent.y >= extent.z ? 1 : 2);
        size_t middle = first + (last - first) / 2;
        std::nth_element(triangles.begin() + static_cast<std::ptrdiff_t>(first),
                         triangles.begin() + static_cast<std::ptrdiff_t>(middle),
                         triangles.begin() + static_cast<std::ptrdiff_t>(last),
                         [&](uint32_t a, uint32_t b) { return center(a)[axis] < center(b)[axis]; });

        // The two children sit next to each other, so the node only needs the first one's index.
        uint32_t left = static_cast<uint32_t>(lightTree.size());
        lightTree.emplace_back();
        lightTree.emplace_back();
        node.child = static_cast<int32_t>(left);
        lightTree[nodeIndex] = node;
        self(self, left, first, middle, trail, depth + 1);
        self(self, left + 1, middle, last, trail | (1u << depth), depth + 1);
    };
    lightTree.emplace_back();
    build(build, 0, 0, triangles.size(), 0, 0);
}

void Renderer::recordLightUpload(VkCommandBuffer commandBuffer) {
    recordStreamedUpload(commandBuffer, emissiveTriangleBuffer, emissiveTriangles.data(),
                         sizeof(EmissiveTriangle) * emissiveTriangles.size());
    if (instanceLightsUploadPending) {
        recordStreamedUpload(commandBuffer, instanceLightBuffer, instanceFirstLight.data(),
                             sizeof(int32_t) * instanceFirstLight.size());
        instanceLightsUploadPending = false;
    }
    recordStreamedUpload(commandBuffer, lightTreeBuffer, lightTree.data(), sizeof(LightTreeNode) * lightTree.size());
    lightsUploadPending = false;
}

// ---------------------------------------------------------------------------------------------
// Punctual lights (KHR_lights_punctual): point, spot and directional lights that come with a model. They have no
// size, so no ray can hit them: the shader only reaches them with shadow rays, one per light up to 8, else one to a
// light picked at random (see samplePunctualLight). Few lights per scene is the usual case, so there is no light tree
// for them.

// This frame's lights in world space, from the render list. Cheap, so every frame; uploaded only on changes.
void Renderer::buildPunctualLightList() {
    framePunctualLights.clear();
    if (!settings.punctualLights) {
        return;
    }
    for (const RenderLight& light : renderList->lights) {
        const glm::mat4& world = light.world;
        GpuPunctualLight gpu{};
        gpu.type = static_cast<uint32_t>(light.type);
        gpu.position = glm::vec3(world[3]);
        glm::vec3 axis = glm::vec3(world * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f));  // lights shine down -Z
        gpu.direction = glm::length(axis) > 0.0f ? glm::normalize(axis) : glm::vec3(0.0f, -1.0f, 0.0f);
        // Keeping the brightness at the same place in the scaled model means intensity * scale^2 (light falls off
        // with the distance squared).
        bool directional = light.type == LightType::Directional;
        float scale = light.scale;
        gpu.intensity = light.color * light.intensity * settings.punctualLightScale * (directional ? 1.0f : scale * scale);
        gpu.range = light.range * scale;
        float outer = std::clamp(light.outerConeAngle, 1e-4f, glm::half_pi<float>());
        float inner = std::clamp(light.innerConeAngle, 0.0f, outer);
        gpu.cosOuterCone = std::cos(outer);
        gpu.cosInnerCone = std::max(std::cos(inner), gpu.cosOuterCone + 1e-4f);  // no division by 0 in the shader
        if (luminance(gpu.intensity) > 0.0f) {
            framePunctualLights.push_back(gpu);
        }
    }
}

void Renderer::recordPunctualLightUpload(VkCommandBuffer commandBuffer) {
    recordStreamedUpload(commandBuffer, punctualLightBuffer, uploadedPunctualLights.data(),
                         sizeof(GpuPunctualLight) * uploadedPunctualLights.size());
    punctualLightsUploadPending = false;
}

// The "Lights from models" part of the Lighting panel
void Renderer::buildPunctualLightSettingsUi() {
    ImGui::Checkbox("Enabled##punctual", &settings.punctualLights);
    ImGui::SameLine();
    ImGui::TextDisabled("%zu lights", framePunctualLights.size());
    ImGui::SliderFloat("Scale##punctual", &settings.punctualLightScale, 0.001f, 1000.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Multiplies every light from the files (KHR_lights_punctual). They come in candela (point,\n"
                          "spot) and lux (directional). For comparison: the Sun at intensity 1 gives pi lux, a 100 cd\n"
                          "lamp 2 m away gives 25 lux. Edit single lights under Scene > object > Lights.");
    }
}
