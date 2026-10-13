//
// Scene -> GPU: the loaded assets become geometry buffers and BLASes, the scene's render list TLAS instances,
// every frame before drawing (updateScene). The renderer's side of the scene; the scene itself is Vulkan-free (Scene.h,
// AssetStore.h).
//
// Models are loaded once (meshes + one BLAS per mesh). Objects only add a transform, so moving, adding or
// removing objects just rebuilds the small TLAS. Loading a model uploads only its own meshes; unloading frees theirs.
//

#include <vulkan/vulkan.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <unordered_map>
#include "renderer/Renderer.h"

// Can a ray pass through parts of this material? Then the shader has to look at every hit on it:
// for holes (alpha), so shadow rays can pass through glass, tinted, and for the surfaces of fog volumes.
static bool needsHitTest(const Material& material) {
    return material.alphaMode != AlphaMode::Opaque || material.transmission > 0.0f || material.fogDensity > 0.0f ||
           material.invisibleSurface;
}

// A unit vector folded onto the octahedron, then flat: as encodeDirection in the shader. (0, 0) for a zero vector
// (decodes to +Z; meshes without normals or tangents have those).
static glm::vec2 octahedral(glm::vec3 n) {
    float sum = std::abs(n.x) + std::abs(n.y) + std::abs(n.z);
    if (!(sum > 0.0f)) {
        return glm::vec2(0.0f);
    }
    n /= sum;
    if (n.z >= 0.0f) {
        return glm::vec2(n.x, n.y);
    }
    return (1.0f - glm::abs(glm::vec2(n.y, n.x))) * glm::vec2(n.x >= 0.0f ? 1.0f : -1.0f, n.y >= 0.0f ? 1.0f : -1.0f);
}

// -1..1 as a signed integer of `bits` bits, in the low bits of the result
static uint32_t snorm(float value, int bits) {
    float scale = static_cast<float>((1 << (bits - 1)) - 1);
    int32_t quantized = static_cast<int32_t>(std::lround(std::clamp(value, -1.0f, 1.0f) * scale));
    return static_cast<uint32_t>(quantized) & ((1u << bits) - 1u);
}

static GpuVertex packVertex(const SceneVertex& vertex) {
    glm::vec2 normal = octahedral(vertex.normal);
    glm::vec2 tangent = octahedral(glm::vec3(vertex.tangent));
    GpuVertex packed{};
    packed.position = vertex.position;
    packed.normal = snorm(normal.x, 16) | snorm(normal.y, 16) << 16;
    packed.tangent = snorm(tangent.x, 16) | snorm(tangent.y, 15) << 16 | (vertex.tangent.w < 0.0f ? 1u << 31 : 0u);
    packed.uv = vertex.uv;
    return packed;
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
    // 1: a model was loaded or unloaded (the asset store's revision moved on) -> the new meshes' geometry and BLASes,
    //    uploads of new textures; what went is released. Rare, so simply wait for the GPU.
    // 0: a material was edited -> uploaded in this frame's command buffer (unless step 1 re-creates the buffer)
    if (assets.materialRevision() != gpuAssets.uploadedMaterialRevision) {
        gpuAssets.uploadedMaterialRevision = assets.materialRevision();
        gpuAssets.materialsDirty = true;
    }

    bool geometryReuploaded = false;
    if (assets.revision() != gpuAssets.uploadedRevision) {
        vkp.deviceWaitIdle(device);
        if (gpuAssets.materialBufferCreated) {
            destroyMaterialBuffer();
        }
        syncTextures();
        createMaterialBuffer();
        syncSceneGeometry();
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

    // 1b: varying fog needs its mesh's distance field (Volumes.cpp); baked when a mesh first needs it
    if ((geometryReuploaded || gpuAssets.materialsDirty) && syncDistanceFields() && rtDescriptorSet != VK_NULL_HANDLE) {
        updateRayTracingDescriptorSet();
    }

    // 2: the scene's render list -> TLAS instances, one per list instance. Only remade when the list was (an object
    //    changed or moved), a material changed (the opaque flags), or the geometry was re-uploaded (BLAS addresses).
    bool rebuild = renderList->instanceRevision() != builtRenderListRevision || gpuAssets.materialsDirty || geometryReuploaded;
    if (rebuild) {
        builtRenderListRevision = renderList->instanceRevision();
        frameInstances.clear();
        frameGpuInstances.clear();
        frameMotions = renderList->motions;
        primitiveCount = 0;
        instanceUploadPending = true;
        frameInstances.reserve(renderList->instances.size());
        frameGpuInstances.reserve(renderList->instances.size());
        for (const RenderInstance& renderInstance : renderList->instances) {
            const glm::mat4& world = renderInstance.world;
            VkAccelerationStructureInstanceKHR instance{};  // zeroed, so memcmp below compares only real data
            instance.transform = toVkTransform(world);
            instance.instanceCustomIndex = renderInstance.mesh;  // 24 bits for us; the shader uses it to find the mesh
            const Material& material = assets.materials()[gpuAssets.meshInfos[renderInstance.mesh].materialIndex].material;
            // Rays look at everything (0xFF); fog volumes are also in groups of their own, which mediumAt looks at alone:
            // 0x02 even fog, 0x04 varying (see "Fog" there). The rest: 0x01.
            instance.mask = fogVaries(material) ? 0x04 : material.fogDensity > 0.0f ? 0x02 : 0x01;
            instance.instanceShaderBindingTableRecordOffset = 0;
            instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
            // Materials a ray can pass through (needsHitTest) are "not opaque": rays ask the shader whether a hit on
            // them counts (except in the basic shader without fog, whose rays treat everything as opaque). Everything
            // else stays on the fast path where the hardware decides alone. A material change remakes the instances,
            // so switching a material's alpha mode in the UI rebuilds the TLAS (the instances differ).
            instance.flags |= needsHitTest(material) ? VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR
                                                     : VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;
            instance.accelerationStructureReference = gpuAssets.blases[renderInstance.mesh].address;
            frameInstances.push_back(instance);
            frameGpuInstances.push_back({glm::mat4x3(world), renderInstance.mesh, renderInstance.motion});
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

    // 3b: the same for the motions (one per object)
    if (sizeof(glm::mat4) * frameMotions.size() > motionData.capacity) {
        vkp.deviceWaitIdle(device);  // the buffer is replaced, no frame may use it
        ensureStreamedCapacity(motionData, sizeof(glm::mat4) * frameMotions.size());
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

// The buffers the shader reads the vertices (GpuVertex) and indices from, and the BLAS builds their triangles
static constexpr VkBufferUsageFlags geometryUsage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                                    VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
static constexpr uint32_t minGeometryCapacity = 1u << 16;  // vertices / indices: the first buffers, at least

// Matches the shared vertex and index buffers to the store's meshes, by id (like the BLASes): a mesh keeps its range
// while it is loaded, so only new meshes are copied, and the ranges of the ones that went are free for the next.
// Indices stay relative to their mesh; MeshInfo tells the shader where each mesh starts. The GPU must be idle.
void Renderer::syncSceneGeometry() {
    cacheClearPending = true;  // the radiance cache's cells belong to the old geometry
    const std::vector<Mesh>& meshes = assets.meshes();
    const std::vector<uint64_t>& ids = assets.meshIds();

    // 1: the meshes that went free their ranges
    std::unordered_map<uint64_t, size_t> current;
    for (size_t m = 0; m < ids.size(); m++) {
        current[ids[m]] = m;
    }
    for (auto it = gpuAssets.meshGeometry.begin(); it != gpuAssets.meshGeometry.end();) {
        if (current.count(it->first) == 0) {
            gpuAssets.vertexRanges.release(it->second.vertexOffset, it->second.vertexCount);
            gpuAssets.indexRanges.release(it->second.indexOffset, it->second.indexCount);
            it = gpuAssets.meshGeometry.erase(it);
        } else {
            ++it;
        }
    }

    // 2: ranges for the new ones. A full buffer grows (its contents copied on the GPU) at most once per change: by
    //    what the new meshes still need, plus an eighth, so small models added later rarely grow it again. (Doubling
    //    would leave up to half of a big scene's buffer empty: 300 MB in Jungle Ruins.)
    uint64_t verticesNeeded = 0;
    uint64_t indicesNeeded = 0;
    for (size_t m = 0; m < ids.size(); m++) {
        if (gpuAssets.meshGeometry.count(ids[m]) == 0) {
            verticesNeeded += meshes[m].vertices.size();
            indicesNeeded += meshes[m].indices.size();
        }
    }
    auto place = [&](RangeAllocator& ranges, VkBuffer& buffer, VmaAllocation& memory, VkDeviceSize elementSize,
                     uint32_t count, uint64_t& stillNeeded, uint32_t& offset) {
        if (!ranges.allocate(count, offset)) {
            // The added end is free and holds all that is still needed (this mesh included)
            uint64_t old = ranges.capacity();
            uint64_t grown = std::max<uint64_t>(old + stillNeeded + old / 8, minGeometryCapacity);
            if (grown > UINT32_MAX) {
                throw std::runtime_error("too much geometry for one buffer (32-bit offsets)");
            }
            growGeometryBuffer(buffer, memory, old * elementSize, grown * elementSize);
            ranges.grow(static_cast<uint32_t>(grown));
            ranges.allocate(count, offset);
        }
        stillNeeded -= count;
    };
    std::vector<size_t> added;
    for (size_t m = 0; m < ids.size(); m++) {
        if (gpuAssets.meshGeometry.count(ids[m]) != 0) {
            continue;
        }
        const Mesh& mesh = meshes[m];
        MeshGeometry geometry;
        geometry.vertexCount = static_cast<uint32_t>(mesh.vertices.size());
        geometry.indexCount = static_cast<uint32_t>(mesh.indices.size());
        place(gpuAssets.vertexRanges, gpuAssets.vertexBuffer, gpuAssets.vertexBufferMemory, sizeof(GpuVertex),
              geometry.vertexCount, verticesNeeded, geometry.vertexOffset);
        place(gpuAssets.indexRanges, gpuAssets.indexBuffer, gpuAssets.indexBufferMemory, sizeof(uint32_t),
              geometry.indexCount, indicesNeeded, geometry.indexOffset);
        // (from the vertices: not every built-in mesh fills in its bounds)
        geometry.boundsMin = glm::vec3(mesh.vertices.empty() ? 0.0f : 1e30f);
        geometry.boundsMax = glm::vec3(mesh.vertices.empty() ? 0.0f : -1e30f);
        for (const SceneVertex& vertex : mesh.vertices) {
            geometry.boundsMin = glm::min(geometry.boundsMin, vertex.position);
            geometry.boundsMax = glm::max(geometry.boundsMax, vertex.position);
        }
        gpuAssets.meshGeometry[ids[m]] = geometry;
        added.push_back(m);
    }
    // Before the first model (or with empty meshes only): the descriptors still need buffers
    if (gpuAssets.vertexBuffer == VK_NULL_HANDLE) {
        growGeometryBuffer(gpuAssets.vertexBuffer, gpuAssets.vertexBufferMemory, 0, minGeometryCapacity * sizeof(GpuVertex));
        gpuAssets.vertexRanges.grow(minGeometryCapacity);
    }
    if (gpuAssets.indexBuffer == VK_NULL_HANDLE) {
        growGeometryBuffer(gpuAssets.indexBuffer, gpuAssets.indexBufferMemory, 0, minGeometryCapacity * sizeof(uint32_t));
        gpuAssets.indexRanges.grow(minGeometryCapacity);
    }

    // 3: the new meshes' data, each copied to its range through a staging buffer of at most 64 MB: filled, copied,
    //    filled again. (One buffer for the whole load would take as much system memory as the load: 0.7 GB for Jungle
    //    Ruins.) A mesh may be split across fillings.
    VkDeviceSize totalBytes = 0;
    for (size_t m : added) {
        totalBytes += meshes[m].vertices.size() * sizeof(GpuVertex) + meshes[m].indices.size() * sizeof(uint32_t);
    }
    if (totalBytes > 0) {
        const VkDeviceSize stagingBytes = std::min<VkDeviceSize>(totalBytes, 64ull << 20);
        VkBuffer stagingBuffer;
        VmaAllocation stagingMemory;
        createBuffer(stagingBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stagingBuffer, stagingMemory);
        uint8_t* mapped;
        vmaMapMemory(allocator, stagingMemory, reinterpret_cast<void**>(&mapped));
        std::vector<VkBufferCopy> vertexCopies;
        std::vector<VkBufferCopy> indexCopies;
        VkDeviceSize used = 0;
        auto flush = [&]() {
            if (used == 0) {
                return;
            }
            immediateSubmit([&](VkCommandBuffer cmd) {
                if (!vertexCopies.empty()) {
                    vkCmdCopyBuffer(cmd, stagingBuffer, gpuAssets.vertexBuffer, static_cast<uint32_t>(vertexCopies.size()),
                                    vertexCopies.data());
                }
                if (!indexCopies.empty()) {
                    vkCmdCopyBuffer(cmd, stagingBuffer, gpuAssets.indexBuffer, static_cast<uint32_t>(indexCopies.size()),
                                    indexCopies.data());
                }
            });
            vertexCopies.clear();
            indexCopies.clear();
            used = 0;
        };
        // Copies count elements (written into the staging buffer by fill(destination, first, n)) to element offset
        // `offset` of the target buffer, in as many pieces as needed
        auto upload = [&](std::vector<VkBufferCopy>& copies, VkDeviceSize elementSize, size_t count, uint32_t offset,
                          auto&& fill) {
            for (size_t done = 0; done < count;) {
                if (stagingBytes - used < elementSize) {
                    flush();
                }
                size_t n = std::min<size_t>(count - done, (stagingBytes - used) / elementSize);
                fill(mapped + used, done, n);
                copies.push_back({used, (offset + done) * elementSize, n * elementSize});
                used += n * elementSize;
                done += n;
            }
        };
        for (size_t m : added) {
            const Mesh& mesh = meshes[m];
            const MeshGeometry& geometry = gpuAssets.meshGeometry[ids[m]];
            upload(vertexCopies, sizeof(GpuVertex), mesh.vertices.size(), geometry.vertexOffset,
                   [&](uint8_t* destination, size_t first, size_t n) {
                       GpuVertex* packed = reinterpret_cast<GpuVertex*>(destination);
                       for (size_t v = 0; v < n; v++) {
                           packed[v] = packVertex(mesh.vertices[first + v]);
                       }
                   });
            upload(indexCopies, sizeof(uint32_t), mesh.indices.size(), geometry.indexOffset,
                   [&](uint8_t* destination, size_t first, size_t n) {
                       std::memcpy(destination, mesh.indices.data() + first, n * sizeof(uint32_t));
                   });
        }
        flush();
        vmaUnmapMemory(allocator, stagingMemory);
        vkDestroyBuffer(device, stagingBuffer, nullptr);
        vmaFreeMemory(allocator, stagingMemory);
    }

    // 4: the mesh infos, all of them (mesh indices and material indices move when a model is unloaded)
    // glTF gives distances inside a material (absorption) in the file's world units, after the node transforms;
    // the shader measures them in mesh units. The node's scale converts. A mesh used by several nodes takes the
    // first one's scale.
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
        const MeshGeometry& geometry = gpuAssets.meshGeometry[ids[m]];
        info.materialIndex = mesh.materialIndex;
        info.vertexOffset = geometry.vertexOffset;
        info.indexOffset = geometry.indexOffset;
        info.vertexCount = geometry.vertexCount;
        info.indexCount = geometry.indexCount;
        info.hasNormals = mesh.hasNormals ? 1 : 0;
        info.hasTangents = mesh.hasTangents ? 1 : 0;
        info.boundsMin = geometry.boundsMin;
        info.boundsMax = geometry.boundsMax;
        info.distanceField = -1;  // syncDistanceFields
        gpuAssets.meshInfos.push_back(info);
    }
    if (gpuAssets.meshInfoBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, gpuAssets.meshInfoBuffer, nullptr);
        vmaFreeMemory(allocator, gpuAssets.meshInfoBufferMemory);
    }
    createDeviceLocalBuffer(gpuAssets.meshInfos.data(), sizeof(gpuAssets.meshInfos[0]) * gpuAssets.meshInfos.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                            gpuAssets.meshInfoBuffer, gpuAssets.meshInfoBufferMemory);
}

// A bigger buffer in place of a full one, with its contents (the first oldSize bytes) copied over on the GPU. The GPU
// must be idle: the old buffer goes at once. BLASes keep their own copy of the triangles, so they don't mind.
void Renderer::growGeometryBuffer(VkBuffer& buffer, VmaAllocation& memory, VkDeviceSize oldSize, VkDeviceSize newSize) {
    VkBuffer bigger;
    VmaAllocation biggerMemory;
    createBuffer(newSize, geometryUsage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, bigger, biggerMemory, VK_NULL_HANDLE, true);
    if (buffer != VK_NULL_HANDLE) {
        if (oldSize > 0) {
            copyBuffer(buffer, bigger, oldSize);
        }
        vkDestroyBuffer(device, buffer, nullptr);
        vmaFreeMemory(allocator, memory);
    }
    buffer = bigger;
    memory = biggerMemory;
}

// The shared buffers and the mesh infos (at shutdown); the BLASes stay (destroyBottomLevelAccelerationStructures)
void Renderer::destroySceneGeometry() {
    for (auto [buffer, memory] : {std::pair{&gpuAssets.meshInfoBuffer, &gpuAssets.meshInfoBufferMemory},
                                  std::pair{&gpuAssets.indexBuffer, &gpuAssets.indexBufferMemory},
                                  std::pair{&gpuAssets.vertexBuffer, &gpuAssets.vertexBufferMemory}}) {
        if (*buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device, *buffer, nullptr);
            vmaFreeMemory(allocator, *memory);
            *buffer = VK_NULL_HANDLE;
            *memory = VK_NULL_HANDLE;
        }
    }
    gpuAssets.vertexRanges.clear();
    gpuAssets.indexRanges.clear();
    gpuAssets.meshGeometry.clear();
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
        vmaFreeMemory(allocator, texture.memory);  // the sampler is shared: it stays
    });
}
