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
#include "../include/imgui/imgui.h"
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
    createStreamedBuffer(punctualLightBuffer, 64 * sizeof(GpuPunctualLight));
    createStreamedBuffer(lightTreeBuffer, 2048 * sizeof(LightTreeNode));
}

void Application::destroyLightBuffers() {
    destroyStreamedBuffer(emissiveTriangleBuffer);
    destroyStreamedBuffer(instanceLightBuffer);
    destroyStreamedBuffer(punctualLightBuffer);
    destroyStreamedBuffer(lightTreeBuffer);
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
        buildLightTree();
    } else {
        // Glowing materials, but all triangles have zero area or a black texture: nothing to sample.
        emissiveTriangles.clear();
        instanceFirstLight.assign(frameInstances.size(), -1);
        sampledEmissiveTriangles = 0;
        lightTree.clear();
        lightTreeDepth = 0;
    }
    lightsUploadPending = true;
}

// The light tree over the triangles that can be picked (probability > 0). Each node holds the box around its
// triangles and their summed probability; the shader walks down from the root and at every node picks a child by
// how much light it could send to the shading point (see pickFromLightTree). Built by splitting the triangles at
// the median of their centers along the box's longest axis, down to one triangle per leaf, so the depth is
// log2(triangles) rounded up, and the 32 bits of treeTrail are plenty.
void Application::buildLightTree() {
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

void Application::recordLightUpload(VkCommandBuffer commandBuffer) {
    recordStreamedUpload(commandBuffer, emissiveTriangleBuffer, emissiveTriangles.data(),
                         sizeof(EmissiveTriangle) * emissiveTriangles.size());
    recordStreamedUpload(commandBuffer, instanceLightBuffer, instanceFirstLight.data(),
                         sizeof(int32_t) * instanceFirstLight.size());
    recordStreamedUpload(commandBuffer, lightTreeBuffer, lightTree.data(), sizeof(LightTreeNode) * lightTree.size());
    lightsUploadPending = false;
}

// ---------------------------------------------------------------------------------------------
// Punctual lights (KHR_lights_punctual): point, spot and directional lights that come with a model. They have no
// size, so no ray can hit them: the shader only reaches them with shadow rays, one per light (see
// samplePunctualLights). Few lights per scene is the usual case, so there is no light picking here.

// This frame's lights in world space, from the visible objects. Cheap, so every frame; uploaded only on changes.
void Application::buildPunctualLightList() {
    framePunctualLights.clear();
    if (!settings.punctualLights) {
        return;
    }
    for (const SceneObject& object : sceneObjects) {
        if (!object.visible) {
            continue;
        }
        const SceneModel& model = sceneModels[object.modelIndex];
        glm::mat4 objectTransform = object.transform() * (object.originalSize ? glm::mat4(1.0f) : model.fit);
        // The file's distances (falloff, range) are scaled with the object; keeping the brightness at the same
        // place in the model means intensity * scale^2 (light falls off with the distance squared). The file's
        // own node scales don't count (glTF: they don't affect lights).
        float scale = std::cbrt(std::abs(glm::determinant(glm::mat3(objectTransform))));
        for (const PunctualLight& light : model.lights) {
            if (!light.enabled) {
                continue;
            }
            glm::mat4 world = objectTransform * light.transform;
            GpuPunctualLight gpu{};
            gpu.type = static_cast<uint32_t>(light.type);
            gpu.position = glm::vec3(world[3]);
            glm::vec3 axis = glm::vec3(world * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f));  // lights shine down -Z
            gpu.direction = glm::length(axis) > 0.0f ? glm::normalize(axis) : glm::vec3(0.0f, -1.0f, 0.0f);
            bool directional = light.type == LightType::Directional;
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
}

void Application::recordPunctualLightUpload(VkCommandBuffer commandBuffer) {
    recordStreamedUpload(commandBuffer, punctualLightBuffer, uploadedPunctualLights.data(),
                         sizeof(GpuPunctualLight) * uploadedPunctualLights.size());
    punctualLightsUploadPending = false;
}

// Lights belong to the model like materials, so editing one changes every object that shows that model.
void Application::buildPunctualLightUi(SceneModel& model) {
    if (model.lights.empty() ||
        !ImGui::TreeNode("lights", "Lights (%zu, shared by all %s objects)", model.lights.size(), model.name.c_str())) {
        return;
    }
    static const char* typeNames[] = {"directional", "point", "spot"};
    for (size_t i = 0; i < model.lights.size(); i++) {
        PunctualLight& light = model.lights[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::Checkbox("##enabled", &light.enabled);
        ImGui::SameLine();
        if (ImGui::TreeNode("light", "%s (%s)", light.name.c_str(), typeNames[static_cast<uint32_t>(light.type)])) {
            ImGui::ColorEdit3("Color", &light.color.x);
            ImGui::DragFloat(light.type == LightType::Directional ? "Intensity (lux)" : "Intensity (candela)",
                             &light.intensity, std::max(light.intensity * 0.01f, 0.001f), 0.0f, 1e6f, "%.3f");
            if (light.type != LightType::Directional) {
                ImGui::DragFloat("Range (0 = none)", &light.range, 0.05f, 0.0f, 1e4f);
            }
            if (light.type == LightType::Spot) {
                ImGui::SliderAngle("Inner cone", &light.innerConeAngle, 0.0f, 90.0f);
                ImGui::SliderAngle("Outer cone", &light.outerConeAngle, 0.0f, 90.0f);
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    ImGui::TreePop();
}

// The "Lights from models" part of the Lighting tab
void Application::buildPunctualLightSettingsUi() {
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
