//
// Scene -> GPU: the loaded assets become geometry buffers and BLASes, the scene's render list TLAS instances,
// every frame before drawing (updateScene). The renderer's side of the scene; the scene itself is Vulkan-free (Scene.h,
// AssetStore.h).
//
// Models are loaded once (meshes + one BLAS per mesh). Objects only add a transform, so moving, adding or
// removing objects just rebuilds the small TLAS. Only loading a new model re-uploads geometry.
//

#include <vulkan/vulkan.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include "renderer/Renderer.h"

// Can a ray pass through parts of this material? Then the shader has to look at every hit on it:
// for holes (alpha), and so shadow rays can pass through glass, tinted.
static bool needsHitTest(const Material& material) {
    return material.alphaMode != AlphaMode::Opaque || material.transmission > 0.0f;
}

static VkTransformMatrixKHR toVkTransform(const glm::mat4& m) {
    // Vulkan wants a 3x4 row-major matrix (the last row of an affine matrix is always 0 0 0 1).
    // glm is column-major: m[column][row].
    VkTransformMatrixKHR transform;
    for (int row = 0; row < 3; row++) {
        for (int column = 0; column < 4; column++) {
            transform.matrix[row][column] = m[column][row];
        }
    }
    return transform;
}

// Called every frame before drawFrame: applies UI changes to the GPU-side scene.
void Renderer::updateScene() {
    // 1: a model was loaded or unloaded (the asset store's revision moved on) -> new geometry buffers, BLASes for new
    //    meshes, uploads of new textures; what went is released. Rare, so simply wait for the GPU.
    // 0: a material was edited -> uploaded in this frame's command buffer (unless step 1 re-creates the buffer)
    if (assets.materialRevision() != gpuAssets.uploadedMaterialRevision) {
        gpuAssets.uploadedMaterialRevision = assets.materialRevision();
        gpuAssets.materialsDirty = true;
    }

    bool geometryReuploaded = false;
    if (assets.revision() != gpuAssets.uploadedRevision) {
        vkp.deviceWaitIdle(device);
        if (gpuAssets.geometryUploaded) {
            destroySceneGeometry();
        }
        if (gpuAssets.materialBufferCreated) {
            destroyMaterialBuffer();
        }
        syncTextures();
        createMaterialBuffer();
        uploadSceneGeometry();
        syncBottomLevelAccelerationStructures();
        gpuAssets.geometryUploaded = true;
        gpuAssets.uploadedRevision = assets.revision();
        // The TLAS points at the old BLASes (some now released): it must be rebuilt, even when there are no instances
        // now (the comparison in step 4 would find nothing changed then: empty before, empty now)
        builtInstances.clear();
        tlasDirty = true;
        geometryReuploaded = true;  // and the instances remade (new mesh infos), below
        dlssReset = true;        // a different scene: DLSS's history is useless
        if (rtDescriptorSet != VK_NULL_HANDLE) {
            updateRayTracingDescriptorSet();
        }
    }

    // 2: the scene's render list -> TLAS instances, one per list instance. Only remade when the list was (an object
    //    changed or moved), a material changed (the opaque flags), or the geometry was re-uploaded (BLAS addresses).
    bool rebuild = renderList->instanceRevision() != builtRenderListRevision || gpuAssets.materialsDirty || geometryReuploaded;
    if (rebuild) {
        builtRenderListRevision = renderList->instanceRevision();
        frameInstances.clear();
        frameGpuInstances.clear();
        primitiveCount = 0;
        instanceUploadPending = true;
        frameInstances.reserve(renderList->instances.size());
        frameGpuInstances.reserve(renderList->instances.size());
        for (const RenderInstance& renderInstance : renderList->instances) {
            const glm::mat4& world = renderInstance.world;
            VkAccelerationStructureInstanceKHR instance{};  // zeroed, so memcmp below compares only real data
            instance.transform = toVkTransform(world);
            instance.instanceCustomIndex = renderInstance.mesh;  // 24 bits for us; the shader uses it to find the mesh
            instance.mask = 0xFF;
            instance.instanceShaderBindingTableRecordOffset = 0;
            instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
            // Cut-out (alpha) materials are "not opaque": every ray asks the shader whether a hit on them counts.
            // Everything else stays on the fast path where the hardware decides alone. A material change remakes
            // the instances, so switching a material's alpha mode in the UI rebuilds the TLAS (the instances differ).
            const Material& material = assets.materials()[gpuAssets.meshInfos[renderInstance.mesh].materialIndex].material;
            instance.flags |= needsHitTest(material) ? VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR
                                                     : VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;
            instance.accelerationStructureReference = gpuAssets.blases[renderInstance.mesh].address;
            frameInstances.push_back(instance);
            frameGpuInstances.push_back({renderList->motions[renderInstance.motion], glm::mat4x3(world),
                                         glm::inverse(glm::mat3(world))});
            primitiveCount += gpuAssets.meshInfos[renderInstance.mesh].indexCount / 3;
        }
    }

    // 3: not enough room in the TLAS -> make a bigger one. Its handle changes, so the descriptor set
    //    must be updated, which is only allowed while no frame uses it.
    if (frameInstances.size() > tlasCapacity) {
        vkp.deviceWaitIdle(device);
        uint32_t newCapacity = std::max(static_cast<uint32_t>(frameInstances.size()), tlasCapacity * 2);
        destroyTopLevelAccelerationStructure();
        createTopLevelAccelerationStructure(newCapacity);
        builtInstances.clear();
        if (rtDescriptorSet != VK_NULL_HANDLE) {
            updateRayTracingDescriptorSet();
        }
    }

    // 4: anything moved (or appeared/disappeared)? Then the TLAS gets rebuilt in this frame's command buffer.
    if (rebuild && (frameInstances.size() != builtInstances.size() ||
        std::memcmp(frameInstances.data(), builtInstances.data(),
                    frameInstances.size() * sizeof(VkAccelerationStructureInstanceKHR)) != 0)) {
        tlasDirty = true;
    }

    // 5: punctual lights, every frame (a handful). Uploaded in this frame's command buffer when they changed; the
    //    accumulated image and the radiance cache are outdated then (the uniforms don't show it).
    buildPunctualLightList();
    if (framePunctualLights.size() != uploadedPunctualLights.size() ||
        std::memcmp(framePunctualLights.data(), uploadedPunctualLights.data(),
                    framePunctualLights.size() * sizeof(GpuPunctualLight)) != 0) {
        uploadedPunctualLights = framePunctualLights;
        punctualLightsUploadPending = true;
        resetAccumulation = true;
        cacheClearPending = true;
        VkDeviceSize size = sizeof(GpuPunctualLight) * uploadedPunctualLights.size();
        if (size > punctualLightBuffer.capacity) {
            vkp.deviceWaitIdle(device);  // the buffer is replaced, no frame may use it
            ensureStreamedCapacity(punctualLightBuffer, size);
            if (rtDescriptorSet != VK_NULL_HANDLE) {
                updateRayTracingDescriptorSet();
            }
        }
    }

    // 5b: portals, the same way: what the rays see changed
    buildPortalList();
    if (framePortals.size() != uploadedPortals.size() ||
        std::memcmp(framePortals.data(), uploadedPortals.data(), framePortals.size() * sizeof(GpuPortal)) != 0) {
        uploadedPortals = framePortals;
        portalsUploadPending = true;
        resetAccumulation = true;
        cacheClearPending = true;
        VkDeviceSize size = sizeof(GpuPortal) * uploadedPortals.size();
        if (size > portalBuffer.capacity) {
            vkp.deviceWaitIdle(device);  // the buffer is replaced, no frame may use it
            ensureStreamedCapacity(portalBuffer, size);
            if (rtDescriptorSet != VK_NULL_HANDLE) {
                updateRayTracingDescriptorSet();
            }
        }
    }

    // 6: glowing triangles moved, appeared, or a material's emission changed -> new light list.
    if (tlasDirty || gpuAssets.materialsDirty) {
        buildLightList();  // uploaded in this frame's command buffer
        VkDeviceSize trianglesSize = sizeof(EmissiveTriangle) * emissiveTriangles.size();
        VkDeviceSize offsetsSize = sizeof(int32_t) * instanceFirstLight.size();
        VkDeviceSize treeSize = sizeof(LightTreeNode) * lightTree.size();
        if (trianglesSize > emissiveTriangleBuffer.capacity || offsetsSize > instanceLightBuffer.capacity ||
            treeSize > lightTreeBuffer.capacity) {
            vkp.deviceWaitIdle(device);  // the buffers are replaced, no frame may use them
            ensureStreamedCapacity(emissiveTriangleBuffer, trianglesSize);
            ensureStreamedCapacity(instanceLightBuffer, offsetsSize);
            ensureStreamedCapacity(lightTreeBuffer, treeSize);
            if (rtDescriptorSet != VK_NULL_HANDLE) {
                updateRayTracingDescriptorSet();
            }
        }
    }
}

// Packs all meshes into one vertex buffer and one index buffer. Indices stay relative to their mesh;
// MeshInfo remembers where each mesh starts.
void Renderer::uploadSceneGeometry() {
    cacheClearPending = true;  // the radiance cache's cells belong to the old geometry
    std::vector<SceneVertex> vertices;
    std::vector<uint32_t> indices;
    // glTF gives distances inside a material (absorption) in the file's world units, after the node transforms;
    // the shader measures them in mesh units. The node's scale converts. A mesh used by several nodes takes the
    // first one's scale.
    const std::vector<Mesh>& meshes = assets.meshes();
    std::vector<float> nodeScales(meshes.size(), 0.0f);
    assets.forEachModel([&](const SceneModel& model) {
        for (const MeshInstance& part : model.parts) {
            if (nodeScales[part.meshIndex] == 0.0f) {
                nodeScales[part.meshIndex] = std::cbrt(std::abs(glm::determinant(glm::mat3(part.transform))));
            }
        }
    });

    gpuAssets.meshInfos.clear();
    for (size_t m = 0; m < meshes.size(); m++) {
        const Mesh& mesh = meshes[m];
        MeshInfo info{};
        info.unitScale = nodeScales[m] > 0.0f ? nodeScales[m] : 1.0f;
        info.materialIndex = mesh.materialIndex;
        info.vertexOffset = static_cast<uint32_t>(vertices.size());
        info.indexOffset = static_cast<uint32_t>(indices.size());
        info.vertexCount = static_cast<uint32_t>(mesh.vertices.size());
        info.indexCount = static_cast<uint32_t>(mesh.indices.size());
        info.hasNormals = mesh.hasNormals ? 1 : 0;
        info.hasTangents = mesh.hasTangents ? 1 : 0;
        gpuAssets.meshInfos.push_back(info);

        vertices.insert(vertices.end(), mesh.vertices.begin(), mesh.vertices.end());
        indices.insert(indices.end(), mesh.indices.begin(), mesh.indices.end());
    }

    // Acceleration structure builds read their inputs through device addresses, the shader reads them as storage buffers.
    VkBufferUsageFlags usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                               VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    createDeviceLocalBuffer(vertices.data(), sizeof(vertices[0]) * vertices.size(), usage, gpuAssets.vertexBuffer, gpuAssets.vertexBufferMemory);
    createDeviceLocalBuffer(indices.data(), sizeof(indices[0]) * indices.size(), usage, gpuAssets.indexBuffer, gpuAssets.indexBufferMemory);
    createDeviceLocalBuffer(gpuAssets.meshInfos.data(), sizeof(gpuAssets.meshInfos[0]) * gpuAssets.meshInfos.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                            gpuAssets.meshInfoBuffer, gpuAssets.meshInfoBufferMemory);
}

// The packed buffers; the BLASes stay (destroyBottomLevelAccelerationStructures)
void Renderer::destroySceneGeometry() {
    vkDestroyBuffer(device, gpuAssets.meshInfoBuffer, nullptr);
    vkFreeMemory(device, gpuAssets.meshInfoBufferMemory, nullptr);
    vkDestroyBuffer(device, gpuAssets.indexBuffer, nullptr);
    vkFreeMemory(device, gpuAssets.indexBufferMemory, nullptr);
    vkDestroyBuffer(device, gpuAssets.vertexBuffer, nullptr);
    vkFreeMemory(device, gpuAssets.vertexBufferMemory, nullptr);
    gpuAssets.geometryUploaded = false;
}

// The asset store unloaded what this belonged to. Nothing reads it after this frame's commands (updateScene waits
// for the GPU before changing assets anyway), but the queue keeps that true if the wait goes away.
void Renderer::releaseBlas(const AccelerationStructure& blas) {
    deferredDeletion.defer(frameNumber, [this, copy = blas]() mutable { destroyAccelerationStructure(copy); });
}

void Renderer::releaseTexture(const GpuTexture& texture) {
    deferredDeletion.defer(frameNumber, [this, texture] {
        vkDestroyImageView(device, texture.view, nullptr);
        vkDestroyImage(device, texture.image, nullptr);
        vkFreeMemory(device, texture.memory, nullptr);  // the sampler is shared: it stays
    });
}
