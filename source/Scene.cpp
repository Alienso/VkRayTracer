//
// Scene management: loaded models, the objects placing them in the world, and the scene part of the UI.
//
// Models are loaded once (meshes + one BLAS per mesh). Objects only add a transform, so moving, adding or
// removing objects just rebuilds the small TLAS. Only loading a new model re-uploads geometry.
//

#include <vulkan/vulkan.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <glm/gtc/matrix_transform.hpp>
#include "../include/imgui/imgui.h"
#include "Application.h"
#include "BuiltInModels.h"

static const char* modelsDirectory = "models";  // copied next to the executable from resource/models
static const char* defaultModel = "DamagedHelmet.glb";
static const float modelSize = 2.0f;  // loaded models are scaled so their largest side is this long
static const float objectSpacing = 2.4f;  // new objects are lined up along X this far apart

// Can a ray pass through parts of this material? Then the shader has to look at every hit on it:
// for holes (alpha), and so shadow rays can pass through glass, tinted.
static bool needsHitTest(const Material& material) {
    return material.alphaMode != AlphaMode::Opaque || material.transmission > 0.0f;
}

glm::mat4 SceneObject::transform() const {
    // Read right to left: scale first, then rotate, then move into place.
    glm::mat4 m = glm::translate(glm::mat4(1.0f), position);
    m = glm::rotate(m, glm::radians(rotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
    m = glm::rotate(m, glm::radians(rotation.x), glm::vec3(1.0f, 0.0f, 0.0f));
    m = glm::rotate(m, glm::radians(rotation.z), glm::vec3(0.0f, 0.0f, 1.0f));
    return glm::scale(m, scale);
}

void Application::findAvailableModels() {
    // Built-in test scenes first, then the files
    availableModels = builtInModelNames();
    std::vector<std::string> files;
    std::error_code error;
    // Subfolders too: a .gltf usually comes with its .bin and images, so it gets a folder of its own.
    for (const auto& entry : std::filesystem::recursive_directory_iterator(modelsDirectory, error)) {
        std::string extension = entry.path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
        if (entry.is_regular_file() && (extension == ".glb" || extension == ".gltf")) {
            files.push_back(entry.path().lexically_relative(modelsDirectory).generic_string());  // "Sponza/Sponza.gltf"
        }
    }
    std::sort(files.begin(), files.end());
    availableModels.insert(availableModels.end(), files.begin(), files.end());
    if (files.empty()) {
        std::cerr << "no .glb/.gltf models found in '" << modelsDirectory << "'" << std::endl;
    }
    auto it = std::find(availableModels.begin(), availableModels.end(), defaultModel);
    modelToAdd = it != availableModels.end() ? static_cast<int>(it - availableModels.begin()) : 0;
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

void Application::createDefaultScene() {
    // Model 0 is the built-in floor. It always stays loaded, so there is always at least one mesh and material.
    Material floorMaterial;
    floorMaterial.name = "Floor";
    floorMaterial.baseColor = {0.75f, 0.75f, 0.75f, 1.0f};
    sceneMaterials.push_back({floorMaterial});
    sceneMeshes.push_back(createFloorMesh());

    SceneModel floorModel;
    floorModel.name = "Floor";
    floorModel.parts.push_back({0, glm::mat4(1.0f)});
    floorModel.firstMaterial = 0;
    floorModel.materialCount = 1;
    sceneModels.push_back(floorModel);

    SceneObject floorObject;
    floorObject.name = "Floor";
    floorObject.modelIndex = 0;
    sceneObjects.push_back(floorObject);
    geometryDirty = true;

    if (std::find(availableModels.begin(), availableModels.end(), defaultModel) != availableModels.end()) {
        addObject(defaultModel);
    }
}

// Returns the index into sceneModels, loading the file first if no object has used it yet. Throws on failure.
int Application::findOrLoadModel(const std::string& fileName) {
    for (size_t i = 0; i < sceneModels.size(); i++) {
        if (sceneModels[i].name == fileName) {
            return static_cast<int>(i);
        }
    }

    SceneModel sceneModel;
    sceneModel.name = fileName;
    Model model;
    if (isBuiltInModel(fileName)) {
        model = createBuiltInModel(fileName);  // already in world units: no fitting
    } else {
        model = loadGltfModel(std::string(modelsDirectory) + "/" + fileName);

        // Models come in any size and position (millimeters, centimeters, ...). Scale the largest side to
        // modelSize and put the bottom center at the origin, so the object's position is where it stands.
        glm::vec3 extent = model.boundsMax - model.boundsMin;
        float scale = modelSize / std::max(std::max(extent.x, extent.y), std::max(extent.z, 1e-6f));
        glm::vec3 bottomCenter = {(model.boundsMin.x + model.boundsMax.x) * 0.5f, model.boundsMin.y,
                                  (model.boundsMin.z + model.boundsMax.z) * 0.5f};
        sceneModel.fit = glm::scale(glm::mat4(1.0f), glm::vec3(scale)) * glm::translate(glm::mat4(1.0f), -bottomCenter);
    }

    // The model's indices are relative to its own lists; shift them to where its data lands in the scene's lists.
    uint32_t firstMesh = static_cast<uint32_t>(sceneMeshes.size());
    uint32_t firstMaterial = static_cast<uint32_t>(sceneMaterials.size());
    int firstTexture = static_cast<int>(sceneTextures.size());
    if (sceneTextures.size() + model.textures.size() > maxTextures) {
        throw std::runtime_error("too many textures (limit " + std::to_string(maxTextures) + ")");
    }

    for (Mesh& mesh : model.meshes) {
        mesh.materialIndex += firstMaterial;
        sceneMeshes.push_back(std::move(mesh));
    }
    for (Material& material : model.materials) {
        for (int* texture : {&material.baseColorTexture, &material.metallicRoughnessTexture,
                             &material.transmissionTexture, &material.emissiveTexture, &material.normalTexture,
                             &material.diffuseTransmissionTexture, &material.diffuseTransmissionColorTexture}) {
            if (*texture >= 0) {
                *texture += firstTexture;
            }
        }
        sceneMaterials.push_back({material});
    }
    for (TextureData& texture : model.textures) {
        sceneTextures.push_back(std::move(texture));
    }
    sceneModel.firstMaterial = firstMaterial;
    sceneModel.materialCount = static_cast<uint32_t>(model.materials.size());
    for (const MeshInstance& instance : model.instances) {
        MeshInstance part = instance;
        part.meshIndex += firstMesh;
        sceneModel.parts.push_back(part);
    }
    sceneModels.push_back(sceneModel);
    geometryDirty = true;  // uploaded in updateScene, before the next frame is recorded
    return static_cast<int>(sceneModels.size() - 1);
}

void Application::addObject(const std::string& fileName) {
    try {
        SceneObject object;
        object.modelIndex = static_cast<uint32_t>(findOrLoadModel(fileName));
        object.name = std::filesystem::path(fileName).stem().string();
        object.originalSize = addAtOriginalSize && !isBuiltInModel(fileName);

        const SceneModel& model = sceneModels[object.modelIndex];
        for (const MeshInstance& part : model.parts) {
            object.hiddenParts.push_back(part.hiddenByDefault);
        }

        // Line objects up: 0, +1, -1, +2, -2, ... spacings from the center, so they don't overlap.
        // Built-in rooms and original-size scenes go to the center instead, around whatever already stands there.
        if (object.originalSize) {
            // A whole scene brings its own ground; the built-in floor would cut through it.
            for (SceneObject& other : sceneObjects) {
                if (other.modelIndex == 0) {
                    other.visible = false;
                }
            }
        } else if (!isBuiltInModel(fileName)) {
            int existing = static_cast<int>(std::count_if(sceneObjects.begin(), sceneObjects.end(), [](const SceneObject& o) {
                return o.modelIndex != 0 && !isBuiltInModel(o.name);
            }));
            int slot = (existing + 1) / 2 * (existing % 2 == 1 ? 1 : -1);
            object.position.x = slot * objectSpacing;
        }

        sceneObjects.push_back(object);
        selectedObject = static_cast<int>(sceneObjects.size() - 1);
        loadError.clear();
    } catch (const std::exception& e) {
        loadError = e.what();
        std::cerr << loadError << std::endl;
    }
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
void Application::updateScene() {
    // 1: a model was loaded -> new geometry buffers and BLASes. Rare, so simply wait for the GPU.
    if (geometryDirty) {
        vkp.deviceWaitIdle(device);
        if (geometryUploaded) {
            destroySceneGeometry();
        }
        if (materialBufferCreated) {
            destroyMaterialBuffer();
        }
        uploadNewTextures();
        createMaterialBuffer();
        uploadSceneGeometry();
        createBottomLevelAccelerationStructures();
        geometryUploaded = true;
        geometryDirty = false;
        builtInstances.clear();  // instances point at the old BLASes: the TLAS must be rebuilt
        dlssReset = true;        // a different scene: DLSS's history is useless
        if (rtDescriptorSet != VK_NULL_HANDLE) {
            updateRayTracingDescriptorSet();
        }
    }

    // 2: objects -> TLAS instances. An object becomes one instance per part of its model.
    frameInstances.clear();
    frameInstanceTransforms.clear();
    frameInstanceMotion.clear();
    frameInstanceMeshes.clear();
    primitiveCount = 0;
    for (SceneObject& object : sceneObjects) {
        glm::mat4 objectTransform = object.transform();
        // Motion vectors need where each point was last frame. A part's world matrix is object * fit * part,
        // and only the object's transform changes, so for all parts alike:
        // last frame's position = previous object transform * inverse(current) * this frame's position.
        glm::mat4 motion = object.hasPreviousTransform ? object.previousTransform * glm::inverse(objectTransform)
                                                       : glm::mat4(1.0f);
        object.previousTransform = objectTransform;
        object.hasPreviousTransform = true;
        if (!object.visible) {
            continue;
        }

        const SceneModel& model = sceneModels[object.modelIndex];
        for (size_t p = 0; p < model.parts.size(); p++) {
            const MeshInstance& part = model.parts[p];
            if (p < object.hiddenParts.size() && object.hiddenParts[p]) {
                continue;
            }
            glm::mat4 world = objectTransform * (object.originalSize ? glm::mat4(1.0f) : model.fit) * part.transform;
            VkAccelerationStructureInstanceKHR instance{};  // zeroed, so memcmp below compares only real data
            instance.transform = toVkTransform(world);
            instance.instanceCustomIndex = part.meshIndex;  // 24 bits for us; the shader uses it to find the mesh
            instance.mask = 0xFF;
            instance.instanceShaderBindingTableRecordOffset = 0;
            instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
            // Cut-out (alpha) materials are "not opaque": every ray asks the shader whether a hit on them counts.
            // Everything else stays on the fast path where the hardware decides alone. Read every frame, so
            // switching a material's alpha mode in the UI rebuilds the TLAS (the instances differ).
            const Material& material = sceneMaterials[meshInfos[part.meshIndex].materialIndex].material;
            instance.flags |= needsHitTest(material) ? VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR
                                                     : VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;
            instance.accelerationStructureReference = blases[part.meshIndex].address;
            frameInstances.push_back(instance);
            frameInstanceTransforms.push_back(world);
            frameInstanceMotion.push_back(motion);
            frameInstanceMeshes.push_back(part.meshIndex);
            primitiveCount += meshInfos[part.meshIndex].indexCount / 3;
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
    if (frameInstances.size() != builtInstances.size() ||
        std::memcmp(frameInstances.data(), builtInstances.data(),
                    frameInstances.size() * sizeof(VkAccelerationStructureInstanceKHR)) != 0) {
        tlasDirty = true;
    }

    // 5: glowing triangles moved, appeared, or a material's emission changed -> new light list.
    if (tlasDirty || materialsDirty) {
        buildLightList();  // uploaded in this frame's command buffer
        VkDeviceSize trianglesSize = sizeof(EmissiveTriangle) * emissiveTriangles.size();
        VkDeviceSize offsetsSize = sizeof(int32_t) * instanceFirstLight.size();
        if (trianglesSize > emissiveTriangleBuffer.capacity || offsetsSize > instanceLightBuffer.capacity) {
            vkp.deviceWaitIdle(device);  // the buffers are replaced, no frame may use them
            ensureStreamedCapacity(emissiveTriangleBuffer, trianglesSize);
            ensureStreamedCapacity(instanceLightBuffer, offsetsSize);
            if (rtDescriptorSet != VK_NULL_HANDLE) {
                updateRayTracingDescriptorSet();
            }
        }
    }
}

// Packs all meshes into one vertex buffer and one index buffer. Indices stay relative to their mesh;
// MeshInfo remembers where each mesh starts.
void Application::uploadSceneGeometry() {
    cacheClearPending = true;  // the radiance cache's cells belong to the old geometry
    std::vector<SceneVertex> vertices;
    std::vector<uint32_t> indices;
    // glTF gives distances inside a material (absorption) in the file's world units, after the node transforms;
    // the shader measures them in mesh units. The node's scale converts. A mesh used by several nodes takes the
    // first one's scale.
    std::vector<float> nodeScales(sceneMeshes.size(), 0.0f);
    for (const SceneModel& model : sceneModels) {
        for (const MeshInstance& part : model.parts) {
            if (nodeScales[part.meshIndex] == 0.0f) {
                nodeScales[part.meshIndex] = std::cbrt(std::abs(glm::determinant(glm::mat3(part.transform))));
            }
        }
    }

    meshInfos.clear();
    for (size_t m = 0; m < sceneMeshes.size(); m++) {
        const Mesh& mesh = sceneMeshes[m];
        MeshInfo info{};
        info.unitScale = nodeScales[m] > 0.0f ? nodeScales[m] : 1.0f;
        info.materialIndex = mesh.materialIndex;
        info.vertexOffset = static_cast<uint32_t>(vertices.size());
        info.indexOffset = static_cast<uint32_t>(indices.size());
        info.vertexCount = static_cast<uint32_t>(mesh.vertices.size());
        info.indexCount = static_cast<uint32_t>(mesh.indices.size());
        info.hasNormals = mesh.hasNormals ? 1 : 0;
        info.hasTangents = mesh.hasTangents ? 1 : 0;
        meshInfos.push_back(info);

        vertices.insert(vertices.end(), mesh.vertices.begin(), mesh.vertices.end());
        indices.insert(indices.end(), mesh.indices.begin(), mesh.indices.end());
    }

    // Acceleration structure builds read their inputs through device addresses, the shader reads them as storage buffers.
    VkBufferUsageFlags usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                               VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    createDeviceLocalBuffer(vertices.data(), sizeof(vertices[0]) * vertices.size(), usage, vertexBuffer, vertexBufferMemory);
    createDeviceLocalBuffer(indices.data(), sizeof(indices[0]) * indices.size(), usage, indexBuffer, indexBufferMemory);
    createDeviceLocalBuffer(meshInfos.data(), sizeof(meshInfos[0]) * meshInfos.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                            meshInfoBuffer, meshInfoBufferMemory);
}

void Application::destroySceneGeometry() {
    for (AccelerationStructure& blas : blases) {
        destroyAccelerationStructure(blas);
    }
    blases.clear();

    vkDestroyBuffer(device, meshInfoBuffer, nullptr);
    vkFreeMemory(device, meshInfoBufferMemory, nullptr);
    vkDestroyBuffer(device, indexBuffer, nullptr);
    vkFreeMemory(device, indexBufferMemory, nullptr);
    vkDestroyBuffer(device, vertexBuffer, nullptr);
    vkFreeMemory(device, vertexBufferMemory, nullptr);
    geometryUploaded = false;
}

void Application::buildSceneUi() {
    if (!ImGui::CollapsingHeader("Scene", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }

    // Add a model from the models folder
    const char* preview = availableModels.empty() ? "(no models found)" : availableModels[modelToAdd].c_str();
    if (ImGui::BeginCombo("##modelToAdd", preview)) {
        for (int i = 0; i < static_cast<int>(availableModels.size()); i++) {
            if (ImGui::Selectable(availableModels[i].c_str(), i == modelToAdd)) {
                modelToAdd = i;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Add") && !availableModels.empty()) {
        addObject(availableModels[modelToAdd]);
    }
    ImGui::SameLine();
    if (ImGui::Button("Rescan")) {
        std::string current = availableModels.empty() ? "" : availableModels[modelToAdd];
        findAvailableModels();
        auto it = std::find(availableModels.begin(), availableModels.end(), current);
        if (it != availableModels.end()) {
            modelToAdd = static_cast<int>(it - availableModels.begin());
        }
    }
    ImGui::Checkbox("Original size", &addAtOriginalSize);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Off: new models are scaled to %.0f units and stood on the floor.\n"
                          "On: they keep the file's own size and position (glTF uses meters),\n"
                          "for whole scenes like Sponza. The built-in floor is then hidden.", modelSize);
    }
    ImGui::TextDisabled("put .glb/.gltf files (or folders) into resource/models");
    if (!loadError.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", loadError.c_str());
    }

    // Object list
    if (ImGui::BeginListBox("Objects", ImVec2(0.0f, 6.0f * ImGui::GetTextLineHeightWithSpacing()))) {
        for (int i = 0; i < static_cast<int>(sceneObjects.size()); i++) {
            ImGui::PushID(i);  // names can repeat; the ID keeps the items apart
            std::string label = sceneObjects[i].name + (sceneObjects[i].visible ? "" : "  (hidden)");
            if (ImGui::Selectable(label.c_str(), i == selectedObject)) {
                selectedObject = i;
            }
            ImGui::PopID();
        }
        ImGui::EndListBox();
    }

    // The selected object's transform
    if (selectedObject >= 0 && selectedObject < static_cast<int>(sceneObjects.size())) {
        ImGui::PushID("selectedObject");  // "Position" etc. also exist in the camera section
        SceneObject& object = sceneObjects[selectedObject];
        const SceneModel& model = sceneModels[object.modelIndex];

        ImGui::Checkbox("Visible", &object.visible);
        ImGui::SameLine();
        ImGui::TextDisabled("%s, %zu parts", model.name.c_str(), model.parts.size());

        ImGui::DragFloat3("Position", &object.position.x, 0.01f);
        ImGui::DragFloat3("Rotation", &object.rotation.x, 0.5f, 0.0f, 0.0f, "%.1f");
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Degrees around X, Y, Z.\nApplied as yaw (Y), then pitch (X), then roll (Z).");
        }

        if (object.uniformScale) {
            float scale = object.scale.x;
            if (ImGui::DragFloat("Scale", &scale, 0.01f, 0.01f, 100.0f)) {
                object.scale = glm::vec3(scale);
            }
        } else {
            ImGui::DragFloat3("Scale", &object.scale.x, 0.01f, 0.01f, 100.0f);
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("Uniform", &object.uniformScale) && object.uniformScale) {
            object.scale = glm::vec3(object.scale.x);
        }
        if (object.modelIndex != 0 && !isBuiltInModel(model.name)) {
            ImGui::Checkbox("Original size", &object.originalSize);
            ImGui::SameLine();
            ImGui::TextDisabled("(file units, not fitted to %.0f)", modelSize);
        }

        if (model.parts.size() > 1 && ImGui::TreeNode("parts", "Parts (%zu)", model.parts.size())) {
            object.hiddenParts.resize(model.parts.size(), false);
            for (size_t p = 0; p < model.parts.size(); p++) {
                ImGui::PushID(static_cast<int>(p));
                bool visible = !object.hiddenParts[p];
                if (ImGui::Checkbox(sceneMeshes[model.parts[p].meshIndex].name.c_str(), &visible)) {
                    object.hiddenParts[p] = !visible;
                }
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        buildMaterialUi(model);

        if (ImGui::Button("Reset transform")) {
            object.rotation = glm::vec3(0.0f);
            object.scale = glm::vec3(1.0f);
            object.position.y = 0.0f;
        }
        ImGui::SameLine();
        if (ImGui::Button("Duplicate")) {
            SceneObject copy = object;  // same model: no loading, just one more set of TLAS instances
            copy.position.x += objectSpacing;
            sceneObjects.push_back(copy);
            selectedObject = static_cast<int>(sceneObjects.size() - 1);
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete")) {
            sceneObjects.erase(sceneObjects.begin() + selectedObject);
            selectedObject = std::min(selectedObject, static_cast<int>(sceneObjects.size()) - 1);
        }
        ImGui::PopID();
    }

    ImGui::Text("%zu models, %zu meshes, %zu instances", sceneModels.size(), meshInfos.size(), frameInstances.size());
    ImGui::Text("%zu materials, %zu textures (%.1f MB)", sceneMaterials.size(), gpuTextures.size(),
                textureMemorySize / (1024.0 * 1024.0));
    ImGui::Text("%zu emissive triangles (%zu bright enough to sample)", emissiveTriangles.size(), sampledEmissiveTriangles);
    ImGui::Text("BLAS memory: %.2f MB -> %.2f MB compacted",
                blasSizeBeforeCompaction / (1024.0 * 1024.0), blasSizeAfterCompaction / (1024.0 * 1024.0));
}

// Materials belong to the model, so editing one changes every object that shows that model.
void Application::buildMaterialUi(const SceneModel& model) {
    if (!ImGui::TreeNode("materials", "Materials (%u, shared by all %s objects)", model.materialCount, model.name.c_str())) {
        return;
    }
    static const char* alphaModeNames[] = {"Opaque", "Mask (cutout)", "Blend (see-through)"};

    for (uint32_t i = 0; i < model.materialCount; i++) {
        uint32_t index = model.firstMaterial + i;
        SceneMaterial& sceneMaterial = sceneMaterials[index];
        Material& m = sceneMaterial.material;

        ImGui::PushID(static_cast<int>(index));
        if (ImGui::TreeNode("material", "%s  (#%u)", m.name.c_str(), index)) {
            bool changed = false;
            // A checkbox per texture: off = look at the constant alone. The constant is multiplied with the texture.
            auto textureToggle = [&](const char* id, int texture, bool& use) {
                if (texture >= 0) {
                    ImGui::SameLine();
                    changed |= ImGui::Checkbox(id, &use);
                }
            };

            changed |= ImGui::ColorEdit4("Base color", &m.baseColor.x, ImGuiColorEditFlags_Float);
            textureToggle("texture##baseColor", m.baseColorTexture, sceneMaterial.useBaseColorTexture);
            changed |= ImGui::SliderFloat("Metallic", &m.metallic, 0.0f, 1.0f);
            textureToggle("texture##metallicRoughness", m.metallicRoughnessTexture, sceneMaterial.useMetallicRoughnessTexture);
            changed |= ImGui::SliderFloat("Roughness", &m.roughness, 0.0f, 1.0f);
            changed |= ImGui::SliderFloat("Transmission", &m.transmission, 0.0f, 1.0f);
            textureToggle("texture##transmission", m.transmissionTexture, sceneMaterial.useTransmissionTexture);
            changed |= ImGui::SliderFloat("IOR", &m.ior, 1.0f, 3.0f);
            changed |= ImGui::SliderFloat("Diffuse transmission", &m.diffuseTransmission, 0.0f, 1.0f);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Thin surfaces that let light through and scatter it (leaves, paper, fabric):\n"
                                  "this share of the matte light leaves on the other side.");
            }
            textureToggle("texture##diffuseTransmission", m.diffuseTransmissionTexture, sceneMaterial.useDiffuseTransmissionTexture);
            if (m.diffuseTransmission > 0.0f) {
                changed |= ImGui::ColorEdit3("Transmitted color", &m.diffuseTransmissionColor.x, ImGuiColorEditFlags_Float);
                textureToggle("texture##diffuseTransmissionColor", m.diffuseTransmissionColorTexture,
                              sceneMaterial.useDiffuseTransmissionColorTexture);
            }
            changed |= ImGui::ColorEdit3("Emissive", &m.emissiveColor.x, ImGuiColorEditFlags_Float);
            textureToggle("texture##emissive", m.emissiveTexture, sceneMaterial.useEmissiveTexture);
            changed |= ImGui::DragFloat("Emissive strength", &m.emissiveStrength, 0.05f, 0.0f, 1000.0f);
            if (m.normalTexture >= 0) {
                changed |= ImGui::SliderFloat("Normal map strength", &m.normalScale, 0.0f, 2.0f);
                textureToggle("texture##normal", m.normalTexture, sceneMaterial.useNormalTexture);
            }
            // Alpha = base color alpha (constant * texture). Mask: holes where it is below the cutoff.
            // Blend: each ray passes through with chance 1 - alpha, which averages to see-through.
            int alphaMode = static_cast<int>(m.alphaMode);
            if (ImGui::Combo("Alpha mode", &alphaMode, alphaModeNames, IM_ARRAYSIZE(alphaModeNames))) {
                m.alphaMode = static_cast<AlphaMode>(alphaMode);
                changed = true;
            }
            if (m.alphaMode == AlphaMode::Mask) {
                changed |= ImGui::SliderFloat("Alpha cutoff", &m.alphaCutoff, 0.0f, 1.0f);
            }
            if (m.transmission > 0.0f) {
                changed |= ImGui::Checkbox("Solid (refracts)", &m.solid);
                ImGui::SameLine();
                ImGui::TextDisabled(m.solid ? "closed glass volume" : "thin, like a window pane");
                if (m.solid) {
                    changed |= ImGui::ColorEdit3("Attenuation color", &m.attenuationColor.x, ImGuiColorEditFlags_Float);
                    changed |= ImGui::DragFloat("Attenuation distance", &m.attenuationDistance, 0.01f, 0.0f, 100.0f, "%.3f");
                    ImGui::TextDisabled("color after this distance inside (model units), 0 = clear");
                }
            }
            if (changed) {
                materialsDirty = true;  // copied to the GPU in this frame's command buffer
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    ImGui::TreePop();
}
