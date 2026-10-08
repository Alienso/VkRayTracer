//
// Asset store: what has been loaded. Models (from files or built in), their meshes, materials and textures, and
// the list of model files there are to load; HDR environment maps the same way. No Vulkan: the renderer makes its
// GPU copies from these lists.
//
// Meshes, materials and textures of all models sit in one list each, in loading order; a model knows where its
// own start (its parts' mesh indices, firstMaterial, texture indices in its materials are into these lists). The
// renderer mirrors the lists one to one (mesh i = MeshInfo i = BLAS i, material i = GPU material i, ...).
//
// Lifetime: objects hold references to their model (acquire / release). A model nobody references any more is
// unloaded right away: its meshes, materials and textures leave the lists, and the ones after them move down (the
// other models' indices are adjusted). The renderer notices by revision() and matches its GPU copies to the lists
// by meshIds() / textureIds(), so it frees exactly what went and keeps the rest. The floor is pinned: never unloaded.
//

#ifndef VULKANDEMO_ASSETSTORE_H
#define VULKANDEMO_ASSETSTORE_H

#include <glm/glm.hpp>
#include <cstdint>
#include <string>
#include <vector>
#include "core/Handles.h"
#include "assets/ModelLoader.h"

// A material, with UI switches to ignore its textures and look at the constants alone
struct SceneMaterial {
    Material material;
    bool useBaseColorTexture = true;
    bool useMetallicRoughnessTexture = true;
    bool useTransmissionTexture = true;
    bool useEmissiveTexture = true;
    bool useNormalTexture = true;
    bool useDiffuseTransmissionTexture = true;
    bool useDiffuseTransmissionColorTexture = true;
};

// A model file, loaded once. Any number of objects can show it.
struct SceneModel {
    std::string name;                 // file name, or "Floor" for the built-in floor
    std::vector<MeshInstance> parts;  // meshIndex into AssetStore::meshes(), transform from the file's node tree
    std::vector<PunctualLight> lights;  // KHR_lights_punctual, placed like the parts; edited in the UI
    glm::mat4 fit = glm::mat4(1.0f);  // scales the model to a standard size and stands it on y = 0 (unless originalSize)
    // The model's meshes, materials and textures are these ranges of the store's lists (they move down when an
    // earlier model is unloaded)
    uint32_t firstMesh = 0;
    uint32_t meshCount = 0;
    uint32_t firstMaterial = 0;
    uint32_t materialCount = 0;
    uint32_t firstTexture = 0;
    uint32_t textureCount = 0;
    uint32_t references = 0;  // objects showing it (AssetStore::acquire / release)
    bool pinned = false;      // never unloaded (the floor)
};

// An HDR environment map (equirectangular), from the environments directory
struct EnvironmentMap {
    std::string name;  // file name
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<float> pixels;  // RGBA32F, rows top to bottom; the renderer frees them once it has its copy
    uint64_t id = 0;            // different for every map loaded (the renderer compares it with what it uploaded)
    uint32_t references = 0;    // scenes using it (AssetStore::acquire / release)
};

class AssetStore {
public:
    static constexpr float fittedModelSize = 2.0f;  // loaded models are scaled so their largest side is this long

    AssetStore(std::string modelsDirectory, std::string environmentsDirectory);

    // The model files found in the models directory (subfolders too), built-in test scenes first
    void scanModelFiles();
    [[nodiscard]] const std::vector<std::string>& modelFiles() const { return files; }

    // The built-in floor: model 0, always loaded, so there is always at least one mesh and material
    ModelHandle addFloor();
    [[nodiscard]] ModelHandle floor() const { return floorModel; }

    // The model of that file (or built-in name), loading it first if it isn't yet. Throws if it can't be loaded.
    // Takes no reference: the caller acquires it (a model never acquired stays loaded).
    ModelHandle findOrLoad(const std::string& fileName);

    // One more / one less object showing the model. The last release unloads it (unless pinned); its handle is
    // invalid from then on (isLoaded says false, even once the slot holds another model).
    void acquire(ModelHandle model);
    void release(ModelHandle model);
    [[nodiscard]] bool isLoaded(ModelHandle handle) const {
        return handle.index < slots.size() && slots[handle.index].loaded &&
               slots[handle.index].generation == handle.generation;
    }

    SceneModel& model(ModelHandle handle) { return slots[handle.index].model; }
    [[nodiscard]] const SceneModel& model(ModelHandle handle) const { return slots[handle.index].model; }
    // Calls f(const SceneModel&) for every loaded model
    template <typename F> void forEachModel(F f) const {
        for (const Slot& slot : slots) {
            if (slot.loaded) {
                f(slot.model);
            }
        }
    }
    [[nodiscard]] size_t modelCount() const { return loadedModels; }

    // All models' meshes, materials and textures. The renderer frees textures' pixels once uploaded, and the
    // editor edits materials in place (then calls materialsChanged).
    std::vector<Mesh>& meshes() { return meshList; }
    [[nodiscard]] const std::vector<Mesh>& meshes() const { return meshList; }
    std::vector<SceneMaterial>& materials() { return materialList; }
    [[nodiscard]] const std::vector<SceneMaterial>& materials() const { return materialList; }
    std::vector<TextureData>& textures() { return textureList; }
    [[nodiscard]] const std::vector<TextureData>& textures() const { return textureList; }
    // A number per mesh / texture (parallel to meshes() / textures()) that stays the same while it is loaded, even
    // when its index changes, and is never given to another one. The renderer keys its GPU copies by these.
    [[nodiscard]] const std::vector<uint64_t>& meshIds() const { return meshIdList; }
    [[nodiscard]] const std::vector<uint64_t>& textureIds() const { return textureIdList; }

    // Goes up whenever meshes, materials or textures are added or removed. The renderer compares it with the
    // revision it last uploaded.
    [[nodiscard]] uint64_t revision() const { return contentRevision; }

    // Materials are edited in place (materials()); call this after an edit so the renderer uploads them. Goes up
    // only for edits: loading and unloading move revision() on, and the renderer re-uploads everything then anyway.
    void materialsChanged() { materialRevisionCounter++; }
    [[nodiscard]] uint64_t materialRevision() const { return materialRevisionCounter; }

    // Environment maps: the .hdr files there are, and the loaded ones. Same lifetime rules as models: the last
    // release unloads the map (the renderer keeps its GPU copy until it gets another one).
    void scanEnvironmentFiles();
    [[nodiscard]] const std::vector<std::string>& environmentFiles() const { return environmentFileList; }
    EnvironmentHandle findOrLoadEnvironment(const std::string& fileName);  // throws if it can't be loaded
    void acquire(EnvironmentHandle map);
    void release(EnvironmentHandle map);
    [[nodiscard]] bool isLoaded(EnvironmentHandle handle) const {
        return handle.index < environmentSlots.size() && environmentSlots[handle.index].loaded &&
               environmentSlots[handle.index].generation == handle.generation;
    }
    EnvironmentMap& environmentMap(EnvironmentHandle handle) { return environmentSlots[handle.index].map; }
    [[nodiscard]] const EnvironmentMap& environmentMap(EnvironmentHandle handle) const { return environmentSlots[handle.index].map; }

    // At most this many textures in total (the renderer's descriptor array); loading past it throws
    void setTextureLimit(uint32_t limit) { textureLimit = limit; }

private:
    // A model's place. Unloading frees it for the next model, with a new generation (stale handles tell).
    struct Slot {
        SceneModel model;
        uint32_t generation = 0;
        bool loaded = false;
    };

    struct EnvironmentSlot {
        EnvironmentMap map;
        uint32_t generation = 0;
        bool loaded = false;
    };

    std::string directory;
    std::string environmentDirectory;
    std::vector<std::string> files;
    std::vector<std::string> environmentFileList;
    std::vector<EnvironmentSlot> environmentSlots;
    std::vector<Slot> slots;
    size_t loadedModels = 0;
    std::vector<Mesh> meshList;
    std::vector<uint64_t> meshIdList;
    std::vector<SceneMaterial> materialList;
    std::vector<TextureData> textureList;
    std::vector<uint64_t> textureIdList;
    uint64_t nextId = 1;
    ModelHandle floorModel;
    uint64_t contentRevision = 0;
    uint64_t materialRevisionCounter = 0;
    uint32_t textureLimit = ~0u;

    ModelHandle add(SceneModel model, Model& contents);
    void unload(uint32_t slot);
};

#endif //VULKANDEMO_ASSETSTORE_H
