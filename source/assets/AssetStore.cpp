//
// Asset store: loading models (in the background) into the shared mesh / material / texture lists. See AssetStore.h.
//

#include "assets/AssetStore.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <glm/gtc/matrix_transform.hpp>
#include "assets/BuiltInModels.h"
#include "assets/EditableFields.h"
#include <stb_image.h>

AssetStore::AssetStore(std::string modelsDirectory, std::string environmentsDirectory)
        : directory(std::move(modelsDirectory)), environmentDirectory(std::move(environmentsDirectory)) {}

AssetStore::~AssetStore() {
    if (running.valid()) {
        running.wait();  // the worker reads nothing of the store, but its result has to land somewhere
    }
}

AssetStore::State AssetStore::state(ModelHandle handle) const {
    return handle.index < slots.size() && slots[handle.index].generation == handle.generation
           ? slots[handle.index].state : State::Free;
}

AssetStore::State AssetStore::state(EnvironmentHandle handle) const {
    return handle.index < environmentSlots.size() && environmentSlots[handle.index].generation == handle.generation
           ? environmentSlots[handle.index].state : State::Free;
}

const std::string& AssetStore::loadError(ModelHandle handle) const {
    static const std::string none;
    return state(handle) == State::Failed ? slots[handle.index].error : none;
}

const std::string& AssetStore::loadError(EnvironmentHandle handle) const {
    static const std::string none;
    return state(handle) == State::Failed ? environmentSlots[handle.index].error : none;
}

// The first free slot (its generation already moved on when it was freed), or a new one
uint32_t AssetStore::freeSlot() {
    uint32_t index = 0;
    while (index < slots.size() && slots[index].state != State::Free) {
        index++;
    }
    if (index == slots.size()) {
        slots.emplace_back();
    }
    return index;
}

uint32_t AssetStore::freeEnvironmentSlot() {
    uint32_t index = 0;
    while (index < environmentSlots.size() && environmentSlots[index].state != State::Free) {
        index++;
    }
    if (index == environmentSlots.size()) {
        environmentSlots.emplace_back();
    }
    return index;
}

void AssetStore::scanModelFiles() {
    // Built-in test scenes first, then the files
    files = builtInModelNames();
    std::vector<std::string> found;
    std::error_code error;
    // Subfolders too: a .gltf usually comes with its .bin and images, so it gets a folder of its own.
    for (const auto& entry : std::filesystem::recursive_directory_iterator(directory, error)) {
        std::string extension = entry.path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
        if (entry.is_regular_file() && (extension == ".glb" || extension == ".gltf")) {
            found.push_back(entry.path().lexically_relative(directory).generic_string());  // "Sponza/Sponza.gltf"
        }
    }
    std::sort(found.begin(), found.end());
    files.insert(files.end(), found.begin(), found.end());
    if (found.empty()) {
        std::cerr << "no .glb/.gltf models found in '" << directory << "'" << std::endl;
    }
}

// A large floor quad, so models have something to stand on and cast shadows onto.
static Mesh createFloorMesh() {
    Mesh floor;
    floor.name = "floor";
    const float half = 6.0f;
    glm::vec3 up = {0.0f, 1.0f, 0.0f};
    floor.vertices = {
            {{-half, 0.0f, -half}, up, {0.0f, 0.0f}},
            {{ half, 0.0f, -half}, up, {1.0f, 0.0f}},
            {{ half, 0.0f,  half}, up, {1.0f, 1.0f}},
            {{-half, 0.0f,  half}, up, {0.0f, 1.0f}},
    };
    floor.indices = {0, 1, 2, 0, 2, 3};
    floor.materialIndex = 0;
    floor.hasNormals = true;
    return floor;
}

ModelHandle AssetStore::addFloor() {
    Model contents;
    Material floorMaterial;
    floorMaterial.name = "Floor";
    floorMaterial.baseColor = {0.75f, 0.75f, 0.75f, 1.0f};
    contents.materials.push_back(floorMaterial);
    contents.meshes.push_back(createFloorMesh());
    contents.instances.push_back({0, glm::mat4(1.0f)});

    SceneModel sceneModel;
    sceneModel.name = "Floor";
    sceneModel.pinned = true;
    uint32_t slot = freeSlot();
    add(slot, sceneModel, contents);
    floorModel = {slot, slots[slot].generation};
    return floorModel;
}

ModelHandle AssetStore::requestModel(const std::string& fileName) {
    for (size_t i = 0; i < slots.size(); i++) {
        if ((slots[i].state == State::Loaded || slots[i].state == State::Loading) && slots[i].model.name == fileName) {
            return {static_cast<uint32_t>(i), slots[i].generation};
        }
    }
    uint32_t index = freeSlot();
    SceneModel sceneModel;
    sceneModel.name = fileName;
    if (isBuiltInModel(fileName)) {
        Model contents = createBuiltInModel(fileName);  // already in world units: no fitting; made at once
        add(index, sceneModel, contents);
    } else {
        slots[index].model = std::move(sceneModel);
        slots[index].state = State::Loading;
        slots[index].error.clear();
        jobs.push_back({false, index, fileName});
        startNextLoad();
    }
    return {index, slots[index].generation};
}

EnvironmentHandle AssetStore::requestEnvironment(const std::string& fileName) {
    for (size_t i = 0; i < environmentSlots.size(); i++) {
        const EnvironmentSlot& slot = environmentSlots[i];
        if ((slot.state == State::Loaded || slot.state == State::Loading) && slot.map.name == fileName) {
            return {static_cast<uint32_t>(i), slot.generation};
        }
    }
    uint32_t index = freeEnvironmentSlot();
    environmentSlots[index].map = EnvironmentMap{};
    environmentSlots[index].map.name = fileName;
    environmentSlots[index].state = State::Loading;
    environmentSlots[index].error.clear();
    jobs.push_back({true, index, fileName});
    startNextLoad();
    return {index, environmentSlots[index].generation};
}

void AssetStore::startNextLoad() {
    if (running.valid() || jobs.empty()) {
        return;
    }
    runningJob = jobs.front();
    jobs.pop_front();
    running = std::async(std::launch::async, &AssetStore::load, runningJob, directory, environmentDirectory);
}

bool AssetStore::update() {
    bool arrived = false;
    if (running.valid() && running.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        finishLoad(running.get());
        runningJob = LoadJob{};
        arrived = true;
    }
    startNextLoad();
    return arrived;
}

// On the worker thread
AssetStore::LoadResult AssetStore::load(const LoadJob& job, const std::string& modelsDirectory,
                                        const std::string& environmentsDirectory) {
    LoadResult result;
    auto start = std::chrono::steady_clock::now();
    try {
        if (job.environment) {
            std::string path = environmentsDirectory + "/" + job.name;
            int width, height, channels;
            float* pixels = stbi_loadf(path.c_str(), &width, &height, &channels, 4);
            if (pixels == nullptr) {
                throw std::runtime_error("could not load " + path + ": " + stbi_failure_reason());
            }
            EnvironmentMap& map = result.map;
            map.width = static_cast<uint32_t>(width);
            map.height = static_cast<uint32_t>(height);
            map.pixels.assign(pixels, pixels + static_cast<size_t>(width) * height * 4);
            stbi_image_free(pixels);
            for (float& value : map.pixels) {
                if (!(value >= 0.0f) || std::isinf(value)) {
                    value = 0.0f;  // NaN, negative or infinite values would end up in every pixel's average
                }
            }
        } else {
            Model& contents = result.model;
            contents = loadGltfModel(modelsDirectory + "/" + job.name);
            // Models come in any size and position (millimeters, centimeters, ...). Scale the largest side to
            // fittedModelSize and put the bottom center at the origin, so the object's position is where it stands.
            glm::vec3 extent = contents.boundsMax - contents.boundsMin;
            float scale = fittedModelSize / std::max(std::max(extent.x, extent.y), std::max(extent.z, 1e-6f));
            glm::vec3 bottomCenter = {(contents.boundsMin.x + contents.boundsMax.x) * 0.5f, contents.boundsMin.y,
                                      (contents.boundsMin.z + contents.boundsMax.z) * 0.5f};
            result.fit = glm::scale(glm::mat4(1.0f), glm::vec3(scale)) *
                         glm::translate(glm::mat4(1.0f), -bottomCenter);
        }
    } catch (const std::exception& e) {
        result.error = e.what();
    }
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::cerr << (result.error.empty() ? "loaded " : "failed to load ") << job.name << " in " << seconds << " s"
              << std::endl;
    return result;
}

// A load is done: the model (or map) joins the lists, unless it failed. Nobody wanting it any more (all released
// while it loaded): a model is kept unused, as if just released; a map goes, as maps do.
void AssetStore::finishLoad(LoadResult result) {
    if (runningJob.environment) {
        EnvironmentSlot& slot = environmentSlots[runningJob.slot];
        if (slot.map.references == 0) {
            slot.map = EnvironmentMap{};
            slot.state = State::Free;
            slot.generation++;
        } else if (!result.error.empty()) {
            slot.state = State::Failed;
            slot.error = result.error;
        } else {
            result.map.name = slot.map.name;
            result.map.references = slot.map.references;
            result.map.id = nextId++;
            slot.map = std::move(result.map);
            slot.state = State::Loaded;
        }
        return;
    }

    Slot& slot = slots[runningJob.slot];
    if (result.error.empty()) {
        SceneModel sceneModel;
        sceneModel.name = slot.model.name;
        sceneModel.references = slot.model.references;
        sceneModel.fit = result.fit;
        try {
            add(runningJob.slot, std::move(sceneModel), result.model);  // (may unload unused models: slot stays put)
        } catch (const std::exception& e) {
            result.error = e.what();
        }
    }
    Slot& landed = slots[runningJob.slot];
    if (!result.error.empty()) {
        if (landed.model.references == 0) {
            landed.model = SceneModel{};
            landed.state = State::Free;
            landed.generation++;
        } else {
            landed.state = State::Failed;
            landed.error = result.error;
        }
    } else if (landed.model.references == 0) {
        landed.model.lastUsed = ++useClock;  // arrived unused
        trimUnused(unusedBudget);
    }
}

// Appends the model's meshes, materials and textures to the shared lists, its parts and ranges into the slot. The
// model's indices are relative to its own lists; they are shifted to where its data lands.
void AssetStore::add(uint32_t index, SceneModel sceneModel, Model& contents) {
    // Unused models make room first (unloading moves the lists, so before the ranges below are taken)
    while (textureList.size() + contents.textures.size() > textureLimit && unloadOldestUnused()) {
    }
    if (textureList.size() + contents.textures.size() > textureLimit) {
        throw std::runtime_error("too many textures (limit " + std::to_string(textureLimit) + ")");
    }
    uint32_t firstMesh = static_cast<uint32_t>(meshList.size());
    uint32_t firstMaterial = static_cast<uint32_t>(materialList.size());
    int firstTexture = static_cast<int>(textureList.size());

    // What it will take on the GPU: the packed vertices and indices, the textures with their mip levels (made by the
    // loader; a third more for one that has none)
    for (const Mesh& mesh : contents.meshes) {
        sceneModel.memorySize += mesh.vertices.size() * sizeof(SceneVertex) + mesh.indices.size() * sizeof(uint32_t);
    }
    for (const TextureData& texture : contents.textures) {
        sceneModel.memorySize += texture.pixels.size() + (texture.mipLevels == 1 ? texture.pixels.size() / 3 : 0);
    }

    for (Mesh& mesh : contents.meshes) {
        mesh.materialIndex += firstMaterial;
        meshList.push_back(std::move(mesh));
        meshIdList.push_back(nextId++);
    }
    for (Material& material : contents.materials) {
        for (int* texture : {&material.baseColorTexture, &material.metallicRoughnessTexture,
                             &material.transmissionTexture, &material.emissiveTexture, &material.normalTexture,
                             &material.normalVarianceTexture,
                             &material.diffuseTransmissionTexture, &material.diffuseTransmissionColorTexture}) {
            if (*texture >= 0) {
                *texture += firstTexture;
            }
        }
        materialList.push_back({material});
    }
    for (TextureData& texture : contents.textures) {
        textureList.push_back(std::move(texture));
        textureIdList.push_back(nextId++);
    }
    sceneModel.lights = contents.lights;
    sceneModel.fileMaterials.assign(materialList.begin() + firstMaterial, materialList.end());
    sceneModel.fileLights = contents.lights;
    sceneModel.firstMesh = firstMesh;
    sceneModel.meshCount = static_cast<uint32_t>(contents.meshes.size());
    sceneModel.firstMaterial = firstMaterial;
    sceneModel.materialCount = static_cast<uint32_t>(contents.materials.size());
    sceneModel.firstTexture = static_cast<uint32_t>(firstTexture);
    sceneModel.textureCount = static_cast<uint32_t>(contents.textures.size());
    for (const MeshInstance& instance : contents.instances) {
        MeshInstance part = instance;
        part.meshIndex += firstMesh;
        sceneModel.parts.push_back(part);
    }
    slots[index].model = std::move(sceneModel);
    slots[index].state = State::Loaded;
    slots[index].error.clear();
    loadedModels++;
    contentRevision++;
}

void AssetStore::acquire(ModelHandle handle) {
    if (state(handle) != State::Free) {
        slots[handle.index].model.references++;
    }
}

void AssetStore::release(ModelHandle handle) {
    State current = state(handle);
    if (current == State::Free) {
        return;
    }
    Slot& slot = slots[handle.index];
    if (slot.model.references > 0) {
        slot.model.references--;
    }
    if (slot.model.references > 0) {
        return;
    }
    if (current == State::Failed) {
        slot.model = SceneModel{};
        slot.state = State::Free;
        slot.generation++;
    } else if (current == State::Loaded && !slot.model.pinned) {
        slot.model.lastUsed = ++useClock;  // kept, unless the unused ones now take too much
        trimUnused(unusedBudget);
    }
    // (Loading: it arrives unused, see finishLoad)
}

void AssetStore::resetEdits() {
    for (Slot& slot : slots) {
        if (slot.state != State::Loaded) {
            continue;
        }
        SceneModel& model = slot.model;
        for (uint32_t i = 0; i < model.materialCount && i < model.fileMaterials.size(); i++) {
            SceneMaterial& material = materialList[model.firstMaterial + i];
            const SceneMaterial& file = model.fileMaterials[i];
#define RESET_FIELD(name) material.material.name = file.material.name;
#define RESET_SWITCH(name) material.name = file.name;
            MATERIAL_FIELDS(RESET_FIELD)
            MATERIAL_SWITCHES(RESET_SWITCH)
#undef RESET_FIELD
#undef RESET_SWITCH
        }
        for (size_t i = 0; i < model.lights.size() && i < model.fileLights.size(); i++) {
#define RESET_LIGHT(name) model.lights[i].name = model.fileLights[i].name;
            LIGHT_FIELDS(RESET_LIGHT)
#undef RESET_LIGHT
        }
    }
    materialsChanged();
}

void AssetStore::setUnusedBudget(uint64_t bytes) {
    unusedBudget = bytes;
    trimUnused(unusedBudget);
}

size_t AssetStore::unusedModelCount() const {
    size_t count = 0;
    forEachModel([&](const SceneModel& model) { count += model.references == 0 && !model.pinned ? 1 : 0; });
    return count;
}

uint64_t AssetStore::unusedMemory() const {
    uint64_t bytes = 0;
    forEachModel([&](const SceneModel& model) { bytes += model.references == 0 && !model.pinned ? model.memorySize : 0; });
    return bytes;
}

bool AssetStore::unloadOldestUnused() {
    int oldest = -1;
    for (size_t i = 0; i < slots.size(); i++) {
        const SceneModel& model = slots[i].model;
        if (slots[i].state == State::Loaded && model.references == 0 && !model.pinned &&
            (oldest < 0 || model.lastUsed < slots[oldest].model.lastUsed)) {
            oldest = static_cast<int>(i);
        }
    }
    if (oldest < 0) {
        return false;
    }
    unload(static_cast<uint32_t>(oldest));
    return true;
}

void AssetStore::trimUnused(uint64_t budget) {
    while (unusedMemory() > budget && unloadOldestUnused()) {
    }
}

// Takes the model's meshes, materials and textures out of the lists. Everything after them moves down, so the
// indices pointing there (other models' parts and ranges, meshes' materials, materials' textures) move down too.
void AssetStore::unload(uint32_t index) {
    SceneModel& gone = slots[index].model;
    uint32_t meshEnd = gone.firstMesh + gone.meshCount;
    uint32_t materialEnd = gone.firstMaterial + gone.materialCount;
    uint32_t textureEnd = gone.firstTexture + gone.textureCount;

    meshList.erase(meshList.begin() + gone.firstMesh, meshList.begin() + meshEnd);
    meshIdList.erase(meshIdList.begin() + gone.firstMesh, meshIdList.begin() + meshEnd);
    materialList.erase(materialList.begin() + gone.firstMaterial, materialList.begin() + materialEnd);
    textureList.erase(textureList.begin() + gone.firstTexture, textureList.begin() + textureEnd);
    textureIdList.erase(textureIdList.begin() + gone.firstTexture, textureIdList.begin() + textureEnd);

    for (Slot& slot : slots) {
        if (slot.state != State::Loaded || &slot.model == &gone) {
            continue;
        }
        SceneModel& model = slot.model;
        if (model.firstMesh >= meshEnd) {
            model.firstMesh -= gone.meshCount;
            for (MeshInstance& part : model.parts) {
                part.meshIndex -= gone.meshCount;
            }
        }
        if (model.firstMaterial >= materialEnd) {
            model.firstMaterial -= gone.materialCount;
        }
        if (model.firstTexture >= textureEnd) {
            model.firstTexture -= gone.textureCount;
        }
    }
    for (Mesh& mesh : meshList) {
        if (mesh.materialIndex >= materialEnd) {
            mesh.materialIndex -= gone.materialCount;
        }
    }
    for (SceneMaterial& sceneMaterial : materialList) {
        Material& material = sceneMaterial.material;
        for (int* texture : {&material.baseColorTexture, &material.metallicRoughnessTexture,
                             &material.transmissionTexture, &material.emissiveTexture, &material.normalTexture,
                             &material.normalVarianceTexture,
                             &material.diffuseTransmissionTexture, &material.diffuseTransmissionColorTexture}) {
            if (*texture >= static_cast<int>(textureEnd)) {
                *texture -= static_cast<int>(gone.textureCount);
            }
        }
    }

    slots[index].model = SceneModel{};
    slots[index].state = State::Free;
    slots[index].generation++;  // handles to it are stale now
    loadedModels--;
    contentRevision++;
}

void AssetStore::scanEnvironmentFiles() {
    environmentFileList.clear();
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(environmentDirectory, error)) {
        std::string extension = entry.path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
        if (entry.is_regular_file() && extension == ".hdr") {
            environmentFileList.push_back(entry.path().filename().string());
        }
    }
    std::sort(environmentFileList.begin(), environmentFileList.end());
}

void AssetStore::acquire(EnvironmentHandle handle) {
    if (state(handle) != State::Free) {
        environmentSlots[handle.index].map.references++;
    }
}

void AssetStore::release(EnvironmentHandle handle) {
    State current = state(handle);
    if (current == State::Free) {
        return;
    }
    EnvironmentSlot& slot = environmentSlots[handle.index];
    if (slot.map.references > 0) {
        slot.map.references--;
    }
    if (slot.map.references == 0 && current != State::Loading) {  // (a loading one goes when it arrives)
        slot.map = EnvironmentMap{};
        slot.state = State::Free;
        slot.error.clear();
        slot.generation++;
    }
}
