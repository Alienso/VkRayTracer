//
// Scene -> GPU: the loaded assets become geometry buffers and BLASes, the scene's render list TLAS instances,
// every frame before drawing (updateScene). The renderer's side of the scene; the scene itself is Vulkan-free (Scene.h,
// AssetStore.h).
//
// Models are loaded once (meshes + one BLAS per mesh). Objects only add a transform, so moving, adding or
// removing objects just rebuilds the TLAS. Loading a model uploads only its own meshes; unloading frees theirs. When
// objects move, only their instances are rewritten and copied (big scenes have millions of instances).
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

VkAccelerationStructureInstanceKHR GpuScene::makeTlasInstance(const RenderInstance& renderInstance) const {
    VkAccelerationStructureInstanceKHR instance{};
    instance.transform = toVkTransform(renderInstance.world);
    instance.instanceCustomIndex = renderInstance.mesh;  // 24 bits for us; the shader uses it to find the mesh
    const Material& material = assets.materials()[gpuAssets.meshInfos[renderInstance.mesh].materialIndex].material;
    // Rays look at everything (0xFF); fog and glass volumes are also in groups of their own, which mediumAt looks at
    // alone: 0x02 even fog, 0x04 varying, 0x08 solid glass (see "Fog" and "Glass volumes" there). The rest: 0x01.
    bool glassVolume = material.transmission > 0.0f && material.solid;
    instance.mask = fogVaries(material) ? 0x04 : material.fogDensity > 0.0f ? 0x02 : glassVolume ? 0x08 : 0x01;
    // (No ray tracing pipeline reads it: it holds 1 + the instance's clip region, see "Portal pieces" in tracing.glsl)
    instance.instanceShaderBindingTableRecordOffset = renderInstance.clip;
    instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    // Materials a ray can pass through (needsHitTest) are "not opaque": rays ask the shader whether a hit on
    // them counts (except in the basic shader without fog, whose rays treat everything as opaque). Everything
    // else stays on the fast path where the hardware decides alone. Switching a material's alpha mode in the UI
    // changes the flags, which rebuilds the TLAS. Instances cut by portal openings too: the shader tells their
    // parts apart.
    instance.flags |= needsHitTest(material) || renderInstance.clip != 0 ? VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR
                                                                         : VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;
    instance.accelerationStructureReference = gpuAssets.blases[renderInstance.mesh].address;
    return instance;
}

void GpuScene::create() {
    createVolumeTextures();
    createUploadRings();  // the staging for the streamed buffers below
    createTopLevelAccelerationStructure(16);  // grows when more instances are needed
    createStreamedBuffer(motionData, sizeof(glm::mat4) * 16);  // and this with the objects (updateScene)
    createLightBuffers();
    createPortalBuffer();
    createEnvironment();
}

void GpuScene::destroy() {
    deferredDeletion.flush();  // the device is idle
    destroyVolumeTextures();
    destroyTopLevelAccelerationStructure();
    destroyBottomLevelAccelerationStructures();
    destroySceneGeometry();
    destroyMaterialBuffer();
    destroyTextures();
    destroyLightBuffers();
    destroyPortalBuffer();
    destroyStreamedBuffer(motionData);
    destroyEnvironment();
    destroyUploadRings();
}

void GpuScene::beginFrame(uint32_t frameSlot, uint64_t frameNumber) {
    currentFrame = frameSlot;
    this->frameNumber = frameNumber;
    deferredDeletion.collect(frameNumber);  // the frame that last used this slot is done: its releases can go
    uploadRingUsed = 0;  // and so is its upload ring
}

void GpuScene::update(const RenderList& list, uint64_t frameNumber) {
    renderList = &list;
    this->frameNumber = frameNumber;
    updateScene();  // the render list's changes on the GPU (new models, instances, lights)
    updateEnvironment();  // a new HDR map, or new sky colors to sample by
}

// Objects moved (or the scene changed): the TLAS first. It may outdate the accumulated image (changes), which the
// uniforms recorded after this have to know.
void GpuScene::recordUploads(VkCommandBuffer commandBuffer) {
    if (tlasDirty) {
        recordTopLevelBuild(commandBuffer);
    }
    if (gpuAssets.materialsDirty) {
        recordMaterialUpdate(commandBuffer);
    }
    if (lightsUploadPending) {
        recordLightUpload(commandBuffer);
    }
    if (environmentUploadPending) {
        recordEnvironmentUpload(commandBuffer);
    }
    if (punctualLightsUploadPending) {
        recordPunctualLightUpload(commandBuffer);
    }
    if (portalsUploadPending) {
        recordPortalUpload(commandBuffer);
    }
    if (clipRegionsUploadPending) {
        recordClipRegionUpload(commandBuffer);
    }
}

// Called every frame before drawFrame (update): applies UI changes to the GPU-side scene.
void GpuScene::updateScene() {
    // 1: a model was loaded or unloaded (the asset store's revision moved on) -> the new meshes' geometry and BLASes,
    //    uploads of new textures; what went is released. Rare, so simply wait for the GPU.
    // 0: a material was edited -> uploaded in this frame's command buffer (unless step 1 re-creates the buffer)
    if (assets.materialRevision() != gpuAssets.uploadedMaterialRevision) {
        gpuAssets.uploadedMaterialRevision = assets.materialRevision();
        gpuAssets.materialsDirty = true;
    }

    bool geometryReuploaded = false;
    if (assets.revision() != gpuAssets.uploadedRevision) {
        waitIdle();
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
        tlasDirty = true;
        geometryReuploaded = true;  // and the instances remade (new mesh infos), below
        changes.resetDlss = true;        // a different scene: DLSS's history is useless
        changes.descriptors = true;
    }

    // 1b: varying fog needs its mesh's distance field (Volumes.cpp); baked when a mesh first needs it
    if ((geometryReuploaded || gpuAssets.materialsDirty) && syncDistanceFields()) {
        changes.descriptors = true;
    }

    // 2: the scene's render list -> TLAS instances, one per list instance. All of them when the list was remade (or
    //    a build of it was missed: its changes are only the last build's) or the geometry was re-uploaded (BLAS
    //    addresses); else only the ranges the list rewrote (moved entities), and the flags when a material changed.
    bool consecutive = renderList->build() == seenRenderListBuild + 1;
    seenRenderListBuild = renderList->build();
    bool instancesRemade = renderList->instanceRevision() != builtRenderListRevision || !consecutive ||
                           geometryReuploaded;
    bool emissiveMoved = false;  // a glowing instance moved: the light list follows (step 6)
    auto allInstancesChanged = [&]() {
        tlasInstancePending.addAll();
        instanceDataPending.addAll();
        tlasDirty = true;
        tlasRebuildNeeded = true;
        tlasTidyOnly = false;
    };
    if (instancesRemade) {
        builtRenderListRevision = renderList->instanceRevision();
        size_t count = renderList->instances.size();
        std::vector<VkAccelerationStructureInstanceKHR> remade(count);  // zeroed, so memcmp compares only real data
        frameGpuInstances.resize(count);
        primitiveCount = 0;
        for (size_t i = 0; i < count; i++) {
            const RenderInstance& renderInstance = renderList->instances[i];
            remade[i] = makeTlasInstance(renderInstance);
            frameGpuInstances[i] = {glm::mat4x3(renderInstance.world), renderInstance.mesh, renderInstance.motion};
            primitiveCount += gpuAssets.meshInfos[renderInstance.mesh].indexCount / 3;
        }
        // The same instances as before (e.g. a group added, or an entity re-parented where it stands): the TLAS
        // stays, and so does the accumulated image. The motion indices may still have moved.
        bool same = !geometryReuploaded && remade.size() == frameInstances.size() &&
                    std::memcmp(remade.data(), frameInstances.data(),
                                remade.size() * sizeof(VkAccelerationStructureInstanceKHR)) == 0;
        frameInstances = std::move(remade);
        frameMotions = renderList->motions;
        motionUploadPending = true;
        if (same) {
            instanceDataPending.addAll();
        } else {
            allInstancesChanged();
        }
    } else {
        // Ghosts (portal pieces) came, went or are cut otherwise; entities went into or out of an opening: those
        // instances made anew (the list's end may have grown or shrunk), and the TLAS built anew
        size_t count = renderList->instances.size();
        if (count != frameInstances.size() || !renderList->remadeInstances().empty()) {
            frameInstances.resize(count);
            frameGpuInstances.resize(count);
            for (const InstanceRange& range : renderList->remadeInstances()) {
                if (range.count == 0) {
                    continue;
                }
                for (uint32_t i = range.first; i < range.first + range.count; i++) {
                    const RenderInstance& renderInstance = renderList->instances[i];
                    frameInstances[i] = makeTlasInstance(renderInstance);
                    frameGpuInstances[i] = {glm::mat4x3(renderInstance.world), renderInstance.mesh,
                                            renderInstance.motion};
                }
                tlasInstancePending.add(range);
                instanceDataPending.add(range);
            }
            tlasDirty = true;
            tlasRebuildNeeded = true;
            tlasTidyOnly = false;
        }
        // A material can change which rays may pass through an instance (the flags) and which group it is in (the
        // mask). Only when one did is the TLAS rebuilt.
        if (gpuAssets.materialsDirty) {
            bool flagsChanged = false;
            for (size_t i = 0; i < frameInstances.size(); i++) {
                VkAccelerationStructureInstanceKHR instance = makeTlasInstance(renderList->instances[i]);
                if (instance.mask != frameInstances[i].mask || instance.flags != frameInstances[i].flags) {
                    frameInstances[i].mask = instance.mask;
                    frameInstances[i].flags = instance.flags;
                    flagsChanged = true;
                }
            }
            if (flagsChanged) {
                tlasInstancePending.addAll();
                tlasDirty = true;
                tlasRebuildNeeded = true;
                tlasTidyOnly = false;
            }
        }
        for (const InstanceRange& range : renderList->changedInstances()) {
            for (uint32_t i = range.first; i < range.first + range.count; i++) {
                const glm::mat4& world = renderList->instances[i].world;
                frameInstances[i].transform = toVkTransform(world);
                frameGpuInstances[i].objectToWorld = glm::mat4x3(world);
            }
            tlasInstancePending.add(range);
            instanceDataPending.add(range);
            tlasDirty = true;
            tlasTidyOnly = false;
            auto glowing = std::lower_bound(emissiveInstances.begin(), emissiveInstances.end(), range.first);
            emissiveMoved |= glowing != emissiveInstances.end() && *glowing < range.first + range.count;
        }
        if (renderList->motionsChanged()) {
            frameMotions = renderList->motions;
            motionUploadPending = true;
        }
        // Nothing moved after a time of updates: the TLAS's tree is built anew for the instances' new places (rays
        // find the same hits through either: the image stays)
        if (!tlasDirty && tlasUpdates > 0) {
            tlasDirty = true;
            tlasRebuildNeeded = true;
            tlasTidyOnly = true;
        }
    }

    // 3: not enough room in the TLAS -> make a bigger one. Its handle changes, so the descriptor set
    //    must be updated, which is only allowed while no frame uses it.
    if (frameInstances.size() > tlasCapacity) {
        waitIdle();
        uint32_t newCapacity = std::max(static_cast<uint32_t>(frameInstances.size()), tlasCapacity * 2);
        destroyTopLevelAccelerationStructure();
        createTopLevelAccelerationStructure(newCapacity);
        changes.descriptors = true;
    }

    // 3b: the same for the motions (one per object)
    if (sizeof(glm::mat4) * frameMotions.size() > motionData.capacity) {
        waitIdle();  // the buffer is replaced, no frame may use it
        ensureStreamedCapacity(motionData, sizeof(glm::mat4) * frameMotions.size());
        motionUploadPending = true;
        changes.descriptors = true;
    }

    // 5: punctual lights, every frame (a handful). Uploaded in this frame's command buffer when they changed; the
    //    accumulated image and the radiance cache are outdated then (the uniforms don't show it).
    buildPunctualLightList();
    if (framePunctualLights.size() != uploadedPunctualLights.size() ||
        std::memcmp(framePunctualLights.data(), uploadedPunctualLights.data(),
                    framePunctualLights.size() * sizeof(GpuPunctualLight)) != 0) {
        uploadedPunctualLights = framePunctualLights;
        punctualLightsUploadPending = true;
        changes.resetAccumulation = true;
        changes.clearCache = true;
        VkDeviceSize size = sizeof(GpuPunctualLight) * uploadedPunctualLights.size();
        if (size > punctualLightBuffer.capacity) {
            waitIdle();  // the buffer is replaced, no frame may use it
            ensureStreamedCapacity(punctualLightBuffer, size);
            changes.descriptors = true;
        }
    }

    // 5b: portals, the same way: what the rays see changed
    buildPortalList();
    if (framePortals.size() != uploadedPortals.size() ||
        std::memcmp(framePortals.data(), uploadedPortals.data(), framePortals.size() * sizeof(GpuPortal)) != 0) {
        uploadedPortals = framePortals;
        portalsUploadPending = true;
        changes.resetAccumulation = true;
        changes.clearCache = true;
        VkDeviceSize size = sizeof(GpuPortal) * uploadedPortals.size();
        if (size > portalBuffer.capacity) {
            waitIdle();  // the buffer is replaced, no frame may use it
            ensureStreamedCapacity(portalBuffer, size);
            changes.descriptors = true;
        }
    }

    // 5c: the clip regions of instances cut by portal openings, the same way
    buildClipRegionList();
    if (frameClipRegions.size() != uploadedClipRegions.size() ||
        std::memcmp(frameClipRegions.data(), uploadedClipRegions.data(),
                    frameClipRegions.size() * sizeof(GpuClipRegion)) != 0) {
        uploadedClipRegions = frameClipRegions;
        clipRegionsUploadPending = true;
        changes.resetAccumulation = true;
        changes.clearCache = true;
        VkDeviceSize size = sizeof(GpuClipRegion) * uploadedClipRegions.size();
        if (size > clipRegionBuffer.capacity) {
            waitIdle();  // the buffer is replaced, no frame may use it
            ensureStreamedCapacity(clipRegionBuffer, size);
            changes.descriptors = true;
        }
    }

    // 6: glowing triangles moved, appeared, or a material's emission changed -> new light list. Also when light
    // picking changes: the tree is built only for picking with it.
    bool lightsChanged = instancesRemade || gpuAssets.materialsDirty;
    bool ghostsResized = !lightsChanged && instanceFirstLight.size() != renderList->instances.size();
    if (ghostsResized) {
        // The ghosts at the list's end came or went: they aren't lights (LIMITATIONS.md), only the instances' table
        // follows them (none at their end)
        instanceFirstLight.resize(renderList->instances.size(), -1);
        instanceLightsUploadPending = true;
    }
    if (lightsChanged || emissiveMoved || ghostsResized || settings.lightPicking != lightListPicking) {
        buildLightList(lightsChanged);  // uploaded in this frame's command buffer
        VkDeviceSize trianglesSize = sizeof(EmissiveTriangle) * emissiveTriangles.size();
        VkDeviceSize offsetsSize = sizeof(int32_t) * instanceFirstLight.size();
        VkDeviceSize treeSize = sizeof(LightTreeNode) * lightTree.size();
        if (trianglesSize > emissiveTriangleBuffer.capacity || offsetsSize > instanceLightBuffer.capacity ||
            treeSize > lightTreeBuffer.capacity) {
            waitIdle();  // the buffers are replaced, no frame may use them
            ensureStreamedCapacity(emissiveTriangleBuffer, trianglesSize);
            if (ensureStreamedCapacity(instanceLightBuffer, offsetsSize)) {
                instanceLightsUploadPending = true;  // a new, empty buffer
            }
            ensureStreamedCapacity(lightTreeBuffer, treeSize);
            changes.descriptors = true;
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
void GpuScene::syncSceneGeometry() {
    changes.clearCache = true;  // the radiance cache's cells belong to the old geometry
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
        gpu.createBuffer(stagingBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stagingBuffer, stagingMemory);
        uint8_t* mapped;
        vmaMapMemory(gpu.allocator, stagingMemory, reinterpret_cast<void**>(&mapped));
        std::vector<VkBufferCopy> vertexCopies;
        std::vector<VkBufferCopy> indexCopies;
        VkDeviceSize used = 0;
        auto flush = [&]() {
            if (used == 0) {
                return;
            }
            gpu.immediateSubmit([&](VkCommandBuffer cmd) {
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
        vmaUnmapMemory(gpu.allocator, stagingMemory);
        vkDestroyBuffer(gpu.device, stagingBuffer, nullptr);
        vmaFreeMemory(gpu.allocator, stagingMemory);
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
        vkDestroyBuffer(gpu.device, gpuAssets.meshInfoBuffer, nullptr);
        vmaFreeMemory(gpu.allocator, gpuAssets.meshInfoBufferMemory);
    }
    gpu.createDeviceLocalBuffer(gpuAssets.meshInfos.data(), sizeof(gpuAssets.meshInfos[0]) * gpuAssets.meshInfos.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                gpuAssets.meshInfoBuffer, gpuAssets.meshInfoBufferMemory);
}

// A bigger buffer in place of a full one, with its contents (the first oldSize bytes) copied over on the GPU. The GPU
// must be idle: the old buffer goes at once. BLASes keep their own copy of the triangles, so they don't mind.
void GpuScene::growGeometryBuffer(VkBuffer& buffer, VmaAllocation& memory, VkDeviceSize oldSize, VkDeviceSize newSize) {
    VkBuffer bigger;
    VmaAllocation biggerMemory;
    gpu.createBuffer(newSize, geometryUsage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, bigger, biggerMemory, VK_NULL_HANDLE, true);
    if (buffer != VK_NULL_HANDLE) {
        if (oldSize > 0) {
            gpu.copyBuffer(buffer, bigger, oldSize);
        }
        vkDestroyBuffer(gpu.device, buffer, nullptr);
        vmaFreeMemory(gpu.allocator, memory);
    }
    buffer = bigger;
    memory = biggerMemory;
}

// The shared buffers and the mesh infos (at shutdown); the BLASes stay (destroyBottomLevelAccelerationStructures)
void GpuScene::destroySceneGeometry() {
    for (auto [buffer, memory] : {std::pair{&gpuAssets.meshInfoBuffer, &gpuAssets.meshInfoBufferMemory},
                                  std::pair{&gpuAssets.indexBuffer, &gpuAssets.indexBufferMemory},
                                  std::pair{&gpuAssets.vertexBuffer, &gpuAssets.vertexBufferMemory}}) {
        if (*buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(gpu.device, *buffer, nullptr);
            vmaFreeMemory(gpu.allocator, *memory);
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
void GpuScene::releaseBlas(const AccelerationStructure& blas) {
    deferredDeletion.defer(frameNumber, [this, copy = blas]() mutable { destroyAccelerationStructure(copy); });
}

void GpuScene::releaseTexture(const GpuTexture& texture) {
    deferredDeletion.defer(frameNumber, [this, texture] {
        vkDestroyImageView(gpu.device, texture.view, nullptr);
        vkDestroyImage(gpu.device, texture.image, nullptr);
        vmaFreeMemory(gpu.allocator, texture.memory);  // the sampler is shared: it stays
    });
}
