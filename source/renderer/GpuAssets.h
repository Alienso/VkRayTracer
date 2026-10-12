//
// GPU assets: the renderer's copies of what the AssetStore has loaded. Slot i here is asset i there (mesh i = MeshInfo
// i = BLAS i, material i = GPU material i, texture i = texture array entry i), filled by updateScene when the store's
// revision moved on (SceneGpu.cpp, Materials.cpp, RayTracer.cpp do the uploads; the structs here only hold them).
//
// BLASes and textures are matched to the store's by id (AssetStore::meshIds / textureIds): when the store changes,
// the ones still there are kept (only their index may change), new ones are built / uploaded, and the ones that went
// are released (Renderer::releaseBlas / releaseTexture -> DeferredDeletion). The packed vertex/index buffers and
// the material buffer are cheap to rebuild and simply remade.
//

#ifndef VULKANDEMO_GPUASSETS_H
#define VULKANDEMO_GPUASSETS_H

#include <vulkan/vulkan.h>
#include <cstdint>
#include <functional>
#include <map>
#include <utility>
#include <vector>
#include <glm/glm.hpp>

struct AccelerationStructure {
    VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceAddress address = 0;
};

// Must match MeshInfo in shaders/raytrace.comp (scalar layout). One per mesh, indexed by the
// TLAS instance's custom index. Tells the shader where the mesh's data lives in the shared buffers.
struct MeshInfo {
    uint32_t materialIndex;  // into the material buffer
    uint32_t vertexOffset;  // first vertex in the vertex buffer
    uint32_t indexOffset;   // first index in the index buffer
    uint32_t vertexCount;
    uint32_t indexCount;
    uint32_t hasNormals;
    uint32_t hasTangents;
    float unitScale;  // file units per mesh unit (the glTF node's scale), for absorption distances
    glm::vec3 boundsMin;  // the mesh's box, in its own space
    glm::vec3 boundsMax;
    int32_t distanceField;  // in the volume textures: distance to the surface over that box (Volumes.cpp); -1: none
};

// A texture on the GPU. Samplers are shared between textures with the same settings.
struct GpuTexture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
};

struct GpuAssets {
    // What the copies were made from: AssetStore::revision() and materialRevision()
    uint64_t uploadedRevision = 0;
    uint64_t uploadedMaterialRevision = 0;

    // Geometry: every mesh's vertices/indices packed into one shared buffer each, one BLAS per mesh.
    // Only rebuilt when a new model is loaded.
    bool geometryUploaded = false;
    VkBuffer vertexBuffer;
    VkDeviceMemory vertexBufferMemory;
    VkBuffer indexBuffer;
    VkDeviceMemory indexBufferMemory;
    VkBuffer meshInfoBuffer;
    VkDeviceMemory meshInfoBufferMemory;
    std::vector<MeshInfo> meshInfos;
    std::vector<AccelerationStructure> blases;  // one per mesh
    std::vector<uint64_t> meshIds;              // AssetStore::meshIds() of each BLAS
    std::vector<VkDeviceSize> blasBuildSizes;   // per BLAS: before compaction
    std::vector<VkDeviceSize> blasSizes;        // per BLAS: after compaction
    VkDeviceSize blasSizeBeforeCompaction = 0;  // the sums, for the UI
    VkDeviceSize blasSizeAfterCompaction = 0;

    // Textures (one bindless array in the shader) and materials. Materials are re-uploaded inside the frame when
    // edited.
    std::vector<GpuTexture> textures;
    std::vector<uint64_t> textureIds;         // AssetStore::textureIds() of each texture
    std::vector<VkDeviceSize> textureSizes;   // bytes per texture
    std::map<uint32_t, VkSampler> samplers;  // key: filter and wrap modes, see getSampler; kept until shutdown
    VkDeviceSize textureMemorySize = 0;      // the sum, for the UI
    VkBuffer materialBuffer;
    VkDeviceMemory materialBufferMemory;
    bool materialBufferCreated = false;
    bool materialsDirty = false;  // edited since the last upload: copied in this frame's command buffer

    // 3D textures (one bindless array in the shader, Volumes.cpp): entry 0 the fog noise, then the distance fields of
    // the meshes that have varying fog, baked when one first needs it and kept by mesh id like the BLASes
    GpuTexture fogNoise;
    std::map<uint64_t, GpuTexture> distanceFields;  // key: AssetStore::meshIds()
    std::vector<GpuTexture> volumeTextures;         // as in the descriptor array (MeshInfo::distanceField)
};

// GPU objects to destroy once no frame in flight can use them any more. Something released while preparing frame
// F may still be read by frames F-1 .. F-maxFramesInFlight+1 (already submitted); it goes once frame F's slot comes
// round again, i.e. once frame F + framesInFlight starts (its fence wait means frame F finished).
class DeferredDeletion {
public:
    explicit DeferredDeletion(uint64_t framesInFlight) : framesInFlight(framesInFlight) {}

    // destroy runs at the start of frame `frame + framesInFlight` or later
    void defer(uint64_t frame, std::function<void()> destroy) { pending.emplace_back(frame, std::move(destroy)); }

    // Call at the start of `frame`, after waiting for its slot's fence
    void collect(uint64_t frame) {
        size_t kept = 0;
        for (size_t i = 0; i < pending.size(); i++) {
            if (pending[i].first + framesInFlight <= frame) {
                pending[i].second();
            } else {
                if (kept != i) {
                    pending[kept] = std::move(pending[i]);
                }
                kept++;
            }
        }
        pending.resize(kept);
    }

    // Everything now (the device must be idle: shutdown)
    void flush() {
        for (auto& entry : pending) {
            entry.second();
        }
        pending.clear();
    }

    size_t size() const { return pending.size(); }

private:
    uint64_t framesInFlight;
    std::vector<std::pair<uint64_t, std::function<void()>>> pending;
};

#endif //VULKANDEMO_GPUASSETS_H
