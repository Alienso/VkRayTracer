//
// The scene on the GPU: the loaded assets as geometry buffers, BLASes, materials and textures; the render list as TLAS
// instances; the lights (glowing triangles with their light tree, punctual lights), the portals, the environment (sky
// or HDR map and its sampling table) and the fog volumes' textures. The renderer (Renderer.h) owns one, updates it
// before each frame from the render list (update), and records its uploads and the TLAS build at the start of the
// frame (recordUploads). The passes read its buffers (descriptors) and counts (uniforms).
//
// Changes the rest of the renderer has to know about (the accumulated image is outdated, the radiance cache, DLSS's
// history, a buffer replaced: the descriptors) are collected in `changes`; the renderer applies and clears them.
//
// The data is public for now: the trace pass and the UI read it directly (ENGINE_PLAN.md item 9, later steps).
//

#ifndef VULKANDEMO_GPUSCENE_H
#define VULKANDEMO_GPUSCENE_H

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <functional>
#include <vector>
#include "assets/AssetStore.h"
#include "core/Configuration.h"
#include "renderer/GpuAssets.h"
#include "renderer/GpuContext.h"
#include "renderer/RendererTypes.h"
#include "scene/RenderList.h"

class GpuScene {
public:
    static constexpr uint32_t maxTextures = 1024;  // size of the texture descriptor array
    static constexpr uint32_t maxVolumeTextures = 256;  // size of the 3D texture descriptor array (Volumes.cpp)

    // settings: the renderer's (light picking, sky sampling, punctual light scale, ...)
    GpuScene(GpuContext& gpu, AssetStore& assets, const RenderSettings& settings)
        : gpu(gpu), assets(assets), settings(settings) {}

    // Waits until the GPU has finished every frame (buffers are about to be replaced). The renderer's: with DLSS it
    // goes through Streamline.
    std::function<void()> waitIdle;

    // What the rest of the renderer must do after changes here
    struct Changes {
        bool resetAccumulation = false;  // the accumulated image is outdated (the uniforms may not show it)
        bool clearCache = false;         // the radiance cache's light is
        bool resetDlss = false;          // DLSS's history is useless (a different scene)
        bool descriptors = false;        // a buffer or image was replaced: write the descriptor set again
    };
    Changes changes;  // since the renderer last applied them

    // The buffers and textures that exist from the start (empty, or placeholders), and all of it at the end
    void create();
    void destroy();
    // At the start of a frame, once the frame slot's fence has passed: what it used can go, its upload ring is free.
    // frameNumber: frames submitted before this one (what releases now are tagged with, see DeferredDeletion).
    void beginFrame(uint32_t frameSlot, uint64_t frameNumber);
    // Before a frame: the render list's changes (new models, instances, lights, the environment) on the GPU
    void update(const RenderList& list, uint64_t frameNumber);
    // In the frame's command buffer, before the passes: the TLAS build and the uploads of what changed
    void recordUploads(VkCommandBuffer commandBuffer);

    GpuContext& gpu;
    AssetStore& assets;
    const RenderSettings& settings;
    const RenderList* renderList = nullptr;  // the one update was last given
    uint32_t currentFrame = 0;  // the frame slot (beginFrame)
    uint64_t frameNumber = 0;   // frames submitted before the one being made (update, beginFrame)

    // The copies of the assets (GpuAssets.h), and GPU objects waiting until no frame in flight uses them
    GpuAssets gpuAssets;
    DeferredDeletion deferredDeletion{static_cast<uint64_t>(Configuration::maxFramesInFlight)};

    // TLAS: built (or updated) on the GPU, inside the frame's command buffer, whenever an object moves.
    // It has room for tlasCapacity instances and is only recreated when more are needed.
    AccelerationStructure tlas;
    uint32_t tlasCapacity = 0;
    VkBuffer tlasScratchBuffer;
    VmaAllocation tlasScratchMemory;
    VkDeviceAddress tlasScratchAddress = 0;
    std::vector<VkAccelerationStructureInstanceKHR> frameInstances;  // this frame's instances, renderList's one to one
    uint64_t primitiveCount = 0;  // triangles over all instances (billions with vegetation)
    // frameInstances on the GPU, what the TLAS is built from. In device memory: the build reads all of it (from
    // system memory over the bus, 2.9 million instances took ~100 ms of a build).
    StreamedBuffer tlasInstanceData;
    std::vector<GpuInstance> frameGpuInstances;      // per instance: its matrix and motion for the shader
    StreamedBuffer instanceData;                     // frameGpuInstances on the GPU, room for tlasCapacity
    std::vector<glm::mat4> frameMotions;             // the render list's motions (one per object)
    StreamedBuffer motionData;                       // frameMotions on the GPU, binding 34
    // The streamed buffers' changes go through these: one per frame in flight (the other frame may still be copying
    // from its own), filled from the start each frame (uploadRingUsed, reset in beginFrame). More than fits (all of
    // the 2.9 million instances changed) is copied right away instead, see uploadStreamedNow: that stalls the frame,
    // so the ring holds what may change every frame (a moving glowing object re-sends the light list and tree, ~144 B
    // per glowing triangle: ~230k of them fit).
    static constexpr VkDeviceSize uploadRingSize = 32 * 1024 * 1024;
    std::vector<UploadRing> uploadRings;
    VkDeviceSize uploadRingUsed = 0;
    // The instances that changed since a buffer was last written: all of them, or these ranges. Only the changes
    // are copied: big scenes have millions of instances, and usually only a few of them move.
    struct PendingInstances {
        bool all = true;
        std::vector<InstanceRange> ranges;
        void add(const InstanceRange& range) {
            if (!all) {
                ranges.push_back(range);
                all = ranges.size() > 4096;  // many small moves: copying everything is simpler
            }
        }
        void addAll() { all = true; ranges.clear(); }
        void clear() { all = false; ranges.clear(); }
    };
    PendingInstances tlasInstancePending;  // frameInstances -> tlasInstanceData
    PendingInstances instanceDataPending;  // frameGpuInstances -> instanceData
    bool motionUploadPending = false;                     // frameMotions -> motionData
    bool tlasDirty = true;
    // The TLAS is updated in place while only transforms change (recordTopLevelBuild), built anew when instances are
    // added, removed or changed otherwise (tlasRebuildNeeded), once nothing moves any more after updates (then only
    // to tidy the tree: tlasTidyOnly, the image stays), or after this many updates in a row
    static constexpr uint32_t maxTlasUpdates = 1000;
    bool tlasRebuildNeeded = true;
    bool tlasTidyOnly = false;
    uint32_t tlasUpdates = 0;  // since it was last built
    uint64_t builtRenderListRevision = 0;  // renderList.instanceRevision() the frame* lists above were made from
    uint64_t seenRenderListBuild = 0;      // renderList.build() whose changes are in them

    // Light list for sampling emissive surfaces. Rebuilt on the CPU when instances or materials change, or glowing
    // instances move.
    std::vector<EmissiveTriangle> emissiveTriangles;
    size_t sampledEmissiveTriangles = 0;      // of those, how many light sampling can pick (probability > 0)
    std::vector<int32_t> instanceFirstLight;  // per TLAS instance: index of its first triangle in the list, or -1
    std::vector<uint32_t> emissiveInstances;  // the instances with a glowing material, in order
    bool instanceLightsUploadPending = false;  // instanceFirstLight changed (it only does when instances or materials do)
    StreamedBuffer emissiveTriangleBuffer;
    StreamedBuffer instanceLightBuffer;
    std::vector<LightTreeNode> lightTree;
    uint32_t lightTreeDepth = 0;  // levels below the root, for the UI
    int lightListPicking = -1;    // settings.lightPicking the list was built for: the tree only exists for Tree
    // What the light indices name: per glowing instance its mesh's id and firstLight. While it stays the same (lights
    // only moved or changed brightness), index i is still the same triangle, so ReSTIR keeps its history.
    std::vector<uint64_t> lightLayout;
    bool lightIndicesChanged = false;  // the last rebuild changed lightLayout (cleared when ReSTIR has been told)
    StreamedBuffer lightTreeBuffer;
    bool lightsUploadPending = false;
    // Punctual lights (Lights.cpp): this frame's, and what the GPU has (uploaded only when they differ)
    std::vector<GpuPunctualLight> framePunctualLights;
    std::vector<GpuPunctualLight> uploadedPunctualLights;
    StreamedBuffer punctualLightBuffer;
    bool punctualLightsUploadPending = false;
    // Portals (Portals.cpp): as the punctual lights
    std::vector<GpuPortal> framePortals;
    std::vector<GpuPortal> uploadedPortals;
    StreamedBuffer portalBuffer;
    bool portalsUploadPending = false;
    // Clip regions of instances cut by portal openings (Portals.cpp): as the portals
    std::vector<GpuClipRegion> frameClipRegions;
    std::vector<GpuClipRegion> uploadedClipRegions;
    StreamedBuffer clipRegionBuffer;
    bool clipRegionsUploadPending = false;

    // Environment (Environment.cpp): the HDR map, and the table the shader picks sky directions from
    uint64_t uploadedEnvironmentId = 0;  // EnvironmentMap::id of what environmentTexture holds (0: none yet)
    GpuTexture environmentTexture;     // RGBA32F; a black 1x1 placeholder until a map is loaded
    VkSampler environmentSampler = VK_NULL_HANDLE;
    glm::uvec2 environmentMapSize = glm::uvec2(0);
    std::vector<float> environmentMapLuminance;  // the map's brightness in cells (see buildEnvironmentDistribution)
    glm::uvec2 environmentMapCells = glm::uvec2(0);
    std::vector<float> environmentDistribution;  // what binding 22 holds: row CDF, then each row's column CDF
    glm::uvec2 environmentCells = glm::uvec2(0);  // columns, rows of environmentDistribution (0 = no sampling)
    std::vector<float> environmentDistributionKey;  // the settings it was built for
    StreamedBuffer environmentDistributionBuffer;
    bool environmentUploadPending = false;

    // AccelerationStructures.cpp
    AccelerationStructure createAccelerationStructure(VkAccelerationStructureTypeKHR type, VkDeviceSize size,
                                                      VmaPool pool = VK_NULL_HANDLE, bool ownMemory = false);
    VkDeviceAddress createScratchBuffer(VkDeviceSize size, VkBuffer& buffer, VmaAllocation& memory, VmaPool pool = VK_NULL_HANDLE,
                                        bool ownMemory = false);
    void destroyAccelerationStructure(AccelerationStructure& as);
    void syncBottomLevelAccelerationStructures();
    void createBottomLevelAccelerationStructures(const std::vector<uint32_t>& meshes);
    void destroyBottomLevelAccelerationStructures();
    void createTopLevelAccelerationStructure(uint32_t capacity);
    void destroyTopLevelAccelerationStructure();
    void recordTopLevelBuild(VkCommandBuffer commandBuffer);
    void recordInstanceUpload(VkCommandBuffer commandBuffer);

    // SceneGpu.cpp
    void updateScene();
    // The TLAS instance for a render list instance (its mesh's BLAS, flags and mask from its material)
    VkAccelerationStructureInstanceKHR makeTlasInstance(const RenderInstance& renderInstance) const;
    void syncSceneGeometry();
    void growGeometryBuffer(VkBuffer& buffer, VmaAllocation& memory, VkDeviceSize oldSize, VkDeviceSize newSize);
    void destroySceneGeometry();
    // A mesh's BLAS / a texture whose model was unloaded: destroyed through deferredDeletion
    void releaseBlas(const AccelerationStructure& blas);
    void releaseTexture(const GpuTexture& texture);

    // Lights.cpp
    void createUploadRings();
    void destroyUploadRings();
    void createStreamedBuffer(StreamedBuffer& streamed, VkDeviceSize capacity,
                              VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                              VkPipelineStageFlags readers = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    void destroyStreamedBuffer(StreamedBuffer& streamed);
    bool ensureStreamedCapacity(StreamedBuffer& streamed, VkDeviceSize size);
    void recordStreamedUpload(VkCommandBuffer commandBuffer, StreamedBuffer& streamed, const void* data, VkDeviceSize size);
    void recordStreamedCopies(VkCommandBuffer commandBuffer, StreamedBuffer& streamed,
                              const std::vector<StreamedPiece>& pieces);
    void uploadStreamedNow(StreamedBuffer& streamed, const std::vector<StreamedPiece>& pieces);
    // Only these ranges of an array of elementSize-byte elements (the rest of the buffer stays as it is)
    void recordStreamedUpload(VkCommandBuffer commandBuffer, StreamedBuffer& streamed, const void* data,
                              VkDeviceSize elementSize, const std::vector<InstanceRange>& ranges);
    void createLightBuffers();
    void destroyLightBuffers();
    // instancesChanged: other instances or materials (which instances glow is looked up again); else only glowing
    // instances moved
    void buildLightList(bool instancesChanged);
    void buildLightTree();
    void recordLightUpload(VkCommandBuffer commandBuffer);
    void buildPunctualLightList();
    void recordPunctualLightUpload(VkCommandBuffer commandBuffer);

    // Portals.cpp
    void createPortalBuffer();
    void destroyPortalBuffer();
    void buildPortalList();
    void recordPortalUpload(VkCommandBuffer commandBuffer);
    void buildClipRegionList();
    void recordClipRegionUpload(VkCommandBuffer commandBuffer);

    // Environment.cpp
    void createEnvironment();
    void destroyEnvironment();
    void uploadEnvironmentImage(uint32_t width, uint32_t height, const float* rgba);
    void destroyEnvironmentImage();
    void uploadEnvironmentMap(EnvironmentMap& map);
    bool environmentMapReady() const;
    void updateEnvironment();
    void recordEnvironmentUpload(VkCommandBuffer commandBuffer);


    // Materials.cpp
    VkSampler getSampler(const TextureData& texture);
    void syncTextures();
    VkDeviceSize uploadTexture(TextureData& texture, GpuTexture& uploaded);
    void destroyTextures();
    std::vector<GpuMaterial> packMaterials() const;
    void createMaterialBuffer();
    void destroyMaterialBuffer();
    void recordMaterialUpdate(VkCommandBuffer commandBuffer);
    // Volumes.cpp: the fog's 3D textures
    void createVolumeTextures();
    void destroyVolumeTextures();
    bool syncDistanceFields();
    GpuTexture uploadVolumeTexture(uint32_t width, uint32_t height, uint32_t depth, VkFormat format, const void* data,
                                   size_t size, VkSampler sampler);
    VkSampler volumeRepeatSampler = VK_NULL_HANDLE;  // the noise: tiles
    VkSampler volumeClampSampler = VK_NULL_HANDLE;   // distance fields
};

#endif //VULKANDEMO_GPUSCENE_H
