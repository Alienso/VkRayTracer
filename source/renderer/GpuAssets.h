//
// GPU assets: the renderer's copies of what the AssetStore has loaded. Slot i here is asset i there (mesh i = MeshInfo
// i = BLAS i, material i = GPU material i, texture i = texture array entry i), filled by updateScene when the store's
// revision moved on (SceneGpu.cpp, Materials.cpp, RayTracer.cpp do the uploads; the structs here only hold them).
//
// BLASes and textures are matched to the store's by id (AssetStore::meshIds / textureIds): when the store changes,
// the ones still there are kept (only their index may change), new ones are built / uploaded, and the ones that went
// are released (Renderer::releaseBlas / releaseTexture -> DeferredDeletion). Each mesh's vertices and indices keep
// their place in the shared buffers the same way (meshGeometry, by id): only new meshes are copied, and the ranges
// of the ones that went are reused. The mesh infos and the material buffer are small and simply remade.
//

#ifndef VULKANDEMO_GPUASSETS_H
#define VULKANDEMO_GPUASSETS_H

#include <vulkan/vulkan.h>
#include <vma/vk_mem_alloc.h>
#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>
#include <glm/glm.hpp>

struct AccelerationStructure {
    VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation memory = VK_NULL_HANDLE;
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
static_assert(sizeof(MeshInfo) == 60, "MeshInfo must match MeshInfo in shaders/raytrace.comp");

// A vertex as the shader reads it (binding 4): 28 bytes, SceneVertex has 48. Must match Vertex in shaders/raytrace.comp
// (scalar layout). The BLAS builds read the position (first). Made by packVertex (SceneGpu.cpp).
struct GpuVertex {
    glm::vec3 position;
    uint32_t normal;   // octahedral (Cigolle et al. 2014), 2 x 16-bit snorm: decodeDirection in the shader
    uint32_t tangent;  // octahedral, 16 + 15-bit snorm; bit 31 set: tangent.w < 0 (decodeTangent in the shader)
    glm::vec2 uv;
};
static_assert(sizeof(GpuVertex) == 28, "GpuVertex must match Vertex in shaders/raytrace.comp");

// The free parts of a buffer of `capacity` elements: a range is taken from the first free part it fits in (first
// fit), and a freed range joins its free neighbors, so space left by unloaded meshes is reused.
class RangeAllocator {
public:
    [[nodiscard]] uint32_t capacity() const { return total; }

    // The buffer is now newCapacity long: the added end is free
    void grow(uint32_t newCapacity) {
        if (newCapacity > total) {
            release(total, newCapacity - total);
            total = newCapacity;
        }
    }

    // false: no free part is long enough (grow first). An empty range always fits (offset 0).
    bool allocate(uint32_t count, uint32_t& offset) {
        offset = 0;
        if (count == 0) {
            return true;
        }
        for (auto it = freeRanges.begin(); it != freeRanges.end(); ++it) {
            if (it->second >= count) {
                offset = it->first;
                uint32_t left = it->second - count;
                freeRanges.erase(it);
                if (left > 0) {
                    freeRanges[offset + count] = left;
                }
                return true;
            }
        }
        return false;
    }

    void release(uint32_t offset, uint32_t count) {
        if (count == 0) {
            return;
        }
        auto next = freeRanges.lower_bound(offset);
        if (next != freeRanges.end() && next->first == offset + count) {  // joins the free part after it
            count += next->second;
            next = freeRanges.erase(next);
        }
        if (next != freeRanges.begin()) {
            auto previous = std::prev(next);
            if (previous->first + previous->second == offset) {  // and the one before
                previous->second += count;
                return;
            }
        }
        freeRanges[offset] = count;
    }

    void clear() {
        freeRanges.clear();
        total = 0;
    }

private:
    std::map<uint32_t, uint32_t> freeRanges;  // offset -> length
    uint32_t total = 0;
};

// Where one mesh's data sits in the shared vertex and index buffers, and its box (from its vertices)
struct MeshGeometry {
    uint32_t vertexOffset = 0;
    uint32_t vertexCount = 0;
    uint32_t indexOffset = 0;
    uint32_t indexCount = 0;
    glm::vec3 boundsMin = glm::vec3(0.0f);
    glm::vec3 boundsMax = glm::vec3(0.0f);
};

// A texture on the GPU. Samplers are shared between textures with the same settings.
struct GpuTexture {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
};

struct GpuAssets {
    // What the copies were made from: AssetStore::revision() and materialRevision()
    uint64_t uploadedRevision = 0;
    uint64_t uploadedMaterialRevision = 0;

    // Geometry: every mesh's vertices/indices in one shared buffer each (a range per mesh, kept while it is loaded;
    // the buffers grow when full), one BLAS per mesh, and the mesh infos that tell the shader where each mesh is.
    bool geometryUploaded = false;
    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VmaAllocation vertexBufferMemory = VK_NULL_HANDLE;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VmaAllocation indexBufferMemory = VK_NULL_HANDLE;
    RangeAllocator vertexRanges;  // in vertices
    RangeAllocator indexRanges;   // in indices
    std::unordered_map<uint64_t, MeshGeometry> meshGeometry;  // key: AssetStore::meshIds()
    VkBuffer meshInfoBuffer = VK_NULL_HANDLE;
    VmaAllocation meshInfoBufferMemory = VK_NULL_HANDLE;
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
    VmaAllocation materialBufferMemory;
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
