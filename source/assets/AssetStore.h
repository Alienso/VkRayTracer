//
// Asset store: what has been loaded. Models (from files or built in), their meshes, materials and textures, and
// the list of model files there are to load; HDR environment maps the same way. No Vulkan: the renderer makes its
// GPU copies from these lists.
//
// Meshes, materials and textures of all models sit in one list each, in loading order; a model knows where its
// own start (its parts' mesh indices, firstMaterial, texture indices in its materials are into these lists). The
// renderer mirrors the lists one to one (mesh i = MeshInfo i = BLAS i, material i = GPU material i, ...).
//
// Loading: files load in the background, one at a time, on a worker thread (reading, decoding, mip levels: seconds
// for a big scene), so the window keeps going meanwhile. requestModel gives a handle at once; the model arrives in a
// later update() (once per frame, on the main thread), which puts it into the lists. Until then isLoading says so;
// when it couldn't be loaded, loadError says why. Built-in models are made at once. The same file asked for again
// while it loads is loaded once. Quitting during a load waits for it (the loader can't stop midway).
//
// Lifetime: objects hold references to their model (acquire / release, also while it loads). A model nobody
// references any more stays loaded ("unused"), so showing it again is instant (no file read, no GPU upload), until
// the unused models together take more than a memory budget: then the least recently used ones are unloaded.
// Unloading takes its meshes, materials and textures out of the lists, and the ones after them move down (the other
// models' indices are adjusted). The renderer notices by revision() and matches its GPU copies to the lists by
// meshIds() / textureIds(), so it frees exactly what went and keeps the rest. The floor is pinned: never unloaded.
//

#ifndef VULKANDEMO_ASSETSTORE_H
#define VULKANDEMO_ASSETSTORE_H

#include <glm/glm.hpp>
#include <cstdint>
#include <deque>
#include <future>
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
    // Its materials and lights as the file has them (texture indices aside: those are the live list's), for what a
    // scene file saves (the edits) and AssetStore::resetEdits
    std::vector<SceneMaterial> fileMaterials;
    std::vector<PunctualLight> fileLights;
    glm::mat4 fit = glm::mat4(1.0f);  // scales the model to a standard size and stands it on y = 0 (unless originalSize)
    glm::vec3 boundsMin = glm::vec3(0.0f);  // around all its parts, in the file's space (before fit)
    glm::vec3 boundsMax = glm::vec3(0.0f);
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
    uint64_t memorySize = 0;  // about what its meshes and textures take on the GPU, in bytes (the unused budget)
    uint64_t lastUsed = 0;    // when its last object went (AssetStore's clock): unused models go oldest first
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
    ~AssetStore();  // waits for a load still running

    // The model files found in the models directory (subfolders too), built-in test scenes first
    void scanModelFiles();
    [[nodiscard]] const std::vector<std::string>& modelFiles() const { return files; }

    // The built-in floor: model 0, always loaded, so there is always at least one mesh and material
    ModelHandle addFloor();
    [[nodiscard]] ModelHandle floor() const { return floorModel; }

    // The model of that file (or built-in name): the loaded one, the one loading, or a new load (see above). Built-in
    // models are made at once; any other name is taken for a file (one that doesn't exist fails in the background).
    // Throws only when a built-in model's textures don't fit (setTextureLimit). Takes no reference: the caller
    // acquires it.
    ModelHandle requestModel(const std::string& fileName);
    // Once per frame on the main thread: a finished load joins the lists (or fails), the next one starts. True when
    // something arrived or failed.
    bool update();
    // The file loading now ("" when none), and how many more wait
    [[nodiscard]] const std::string& currentLoad() const { return runningJob.name; }
    [[nodiscard]] size_t waitingLoads() const { return jobs.size(); }

    // One more / one less object showing the model. After the last release it stays loaded but unused, until the
    // unused budget or the texture limit needs its room; once unloaded its handle is invalid (isLoaded says false,
    // even once the slot holds another model). A model released while it loads arrives unused; a failed one goes.
    void acquire(ModelHandle model);
    void release(ModelHandle model);

    // Unused models are kept while they take at most this much GPU memory together (SceneModel::memorySize); the
    // program lowers it while GPU memory is short (unusedModelBudget in Application.cpp)
    static constexpr uint64_t defaultUnusedBudget = 1ull << 30;  // 1 GiB
    void setUnusedBudget(uint64_t bytes);
    // How many loaded models no object shows, and their memory
    [[nodiscard]] size_t unusedModelCount() const;
    [[nodiscard]] uint64_t unusedMemory() const;
    [[nodiscard]] bool isLoaded(ModelHandle handle) const { return state(handle) == State::Loaded; }
    [[nodiscard]] bool isLoading(ModelHandle handle) const { return state(handle) == State::Loading; }
    // Why the model couldn't be loaded ("" unless it failed)
    [[nodiscard]] const std::string& loadError(ModelHandle handle) const;
    // The file (or built-in) name of a model in any state
    [[nodiscard]] const std::string& modelName(ModelHandle handle) const { return slots[handle.index].model.name; }

    // A loaded model
    SceneModel& model(ModelHandle handle) { return slots[handle.index].model; }
    [[nodiscard]] const SceneModel& model(ModelHandle handle) const { return slots[handle.index].model; }
    // Calls f(const SceneModel&) for every loaded model
    template <typename F> void forEachModel(F f) const {
        for (const Slot& slot : slots) {
            if (slot.state == State::Loaded) {
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
    // Every loaded model's materials and lights back to the file's (the values in EditableFields.h): a scene that is
    // opened or started anew applies its own edits on top
    void resetEdits();

    // Environment maps: the .hdr files there are, and the loaded ones. Loaded in the background like models (the
    // same queue); the last release unloads the map (the renderer keeps its GPU copy until it gets another one).
    void scanEnvironmentFiles();
    [[nodiscard]] const std::vector<std::string>& environmentFiles() const { return environmentFileList; }
    EnvironmentHandle requestEnvironment(const std::string& fileName);
    void acquire(EnvironmentHandle map);
    void release(EnvironmentHandle map);
    [[nodiscard]] bool isLoaded(EnvironmentHandle handle) const { return state(handle) == State::Loaded; }
    [[nodiscard]] bool isLoading(EnvironmentHandle handle) const { return state(handle) == State::Loading; }
    [[nodiscard]] const std::string& loadError(EnvironmentHandle handle) const;
    // A map in any state (only a loaded one has pixels)
    EnvironmentMap& environmentMap(EnvironmentHandle handle) { return environmentSlots[handle.index].map; }
    [[nodiscard]] const EnvironmentMap& environmentMap(EnvironmentHandle handle) const { return environmentSlots[handle.index].map; }

    // At most this many textures in total (the renderer's descriptor array); a model that would pass it fails
    void setTextureLimit(uint32_t limit) { textureLimit = limit; }

private:
    // Free: nothing (a handle to it is stale); Loading: asked for, on the queue or the worker; Failed: kept with its
    // error until the last reference goes
    enum class State { Free, Loading, Loaded, Failed };

    // A model's place. Freeing it moves the generation on (stale handles tell).
    struct Slot {
        SceneModel model;
        uint32_t generation = 0;
        State state = State::Free;
        std::string error;
    };

    struct EnvironmentSlot {
        EnvironmentMap map;
        uint32_t generation = 0;
        State state = State::Free;
        std::string error;
    };

    // A file to load, for the slot that waits for it
    struct LoadJob {
        bool environment = false;
        uint32_t slot = 0;
        std::string name;
    };
    // What the worker made of it
    struct LoadResult {
        Model model;
        glm::mat4 fit = glm::mat4(1.0f);
        EnvironmentMap map;
        std::string error;  // empty: it worked
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
    uint64_t unusedBudget = defaultUnusedBudget;
    uint64_t useClock = 0;  // counts releases, for SceneModel::lastUsed
    std::deque<LoadJob> jobs;          // waiting
    LoadJob runningJob;                // on the worker (name "" when none)
    std::future<LoadResult> running;

    [[nodiscard]] State state(ModelHandle handle) const;  // Free for an invalid or stale handle
    [[nodiscard]] State state(EnvironmentHandle handle) const;
    uint32_t freeSlot();
    uint32_t freeEnvironmentSlot();
    // Puts the model's contents into the lists, into that slot. Throws if there is no room for its textures.
    void add(uint32_t slot, SceneModel model, Model& contents);
    // Loads the file (on the worker thread: touches nothing of the store but its arguments)
    static LoadResult load(const LoadJob& job, const std::string& modelsDirectory,
                           const std::string& environmentsDirectory);
    void startNextLoad();
    void finishLoad(LoadResult result);
    void unload(uint32_t slot);
    // Unloads the unused model released longest ago; false if there is none
    bool unloadOldestUnused();
    // Unloads unused models, oldest first, until they take at most `budget` bytes
    void trimUnused(uint64_t budget);
};

#endif //VULKANDEMO_ASSETSTORE_H
