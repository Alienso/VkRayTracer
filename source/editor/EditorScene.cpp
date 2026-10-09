//
// The editor's Scene tab: adding models, the object list, an object's transform, parts, materials and lights,
// portals, and the actions behind it. It changes the Scene and the assets' materials and lights; the renderer picks the changes
// up through the render list and the asset store's revisions.
//

#include <algorithm>
#include <iostream>
#include <imgui/imgui.h>
#include "editor/Editor.h"
#include "assets/BuiltInModels.h"

void Editor::init(const std::string& defaultModel, const std::string& error) {
    const std::vector<std::string>& files = assets.modelFiles();
    auto it = std::find(files.begin(), files.end(), defaultModel);
    modelToAdd = it != files.end() ? static_cast<int>(it - files.begin()) : 0;
    selectedObject = scene.objects().size() > 1 ? static_cast<int>(scene.objects().size()) - 1 : -1;
    loadError = error;
}

// Adds an object for the UI: selects it, or shows why the model couldn't be loaded
void Editor::addObject(const std::string& fileName) {
    try {
        selectedObject = scene.addObject(fileName, addAtOriginalSize);
        loadError.clear();
    } catch (const std::exception& e) {
        loadError = e.what();
        std::cerr << loadError << std::endl;
    }
}

void Editor::buildSceneUi() {
    // Add a model from the models folder
    const std::vector<std::string>& availableModels = assets.modelFiles();
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
        assets.scanModelFiles();
        assets.scanEnvironmentFiles();
        auto it = std::find(availableModels.begin(), availableModels.end(), current);
        modelToAdd = it != availableModels.end() ? static_cast<int>(it - availableModels.begin()) : 0;
    }
    ImGui::Checkbox("Original size", &addAtOriginalSize);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Off: new models are scaled to %.0f units and stood on the floor.\n"
                          "On: they keep the file's own size and position (glTF uses meters),\n"
                          "for whole scenes like Sponza. The built-in floor is then hidden.", AssetStore::fittedModelSize);
    }
    ImGui::TextDisabled("put .glb/.gltf files (or folders) into resource/models");
    if (!loadError.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", loadError.c_str());
    }

    // Object list
    std::vector<SceneObject>& sceneObjects = scene.objects();
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
        SceneModel& model = assets.model(object.model);

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
        if (object.model != assets.floor() && !isBuiltInModel(model.name)) {
            ImGui::Checkbox("Original size", &object.originalSize);
            ImGui::SameLine();
            ImGui::TextDisabled("(file units, not fitted to %.0f)", AssetStore::fittedModelSize);
        }

        if (model.parts.size() > 1 && ImGui::TreeNode("parts", "Parts (%zu)", model.parts.size())) {
            object.hiddenParts.resize(model.parts.size(), false);
            // Only the rows in view are drawn: scenes with vegetation have millions of parts
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(model.parts.size()));
            while (clipper.Step()) {
                for (int p = clipper.DisplayStart; p < clipper.DisplayEnd; p++) {
                    ImGui::PushID(p);
                    bool visible = !object.hiddenParts[p];
                    if (ImGui::Checkbox(assets.meshes()[model.parts[p].meshIndex].name.c_str(), &visible)) {
                        object.hiddenParts[p] = !visible;
                    }
                    ImGui::PopID();
                }
            }
            ImGui::TreePop();
        }
        buildMaterialUi(model);
        buildPunctualLightUi(model);

        if (ImGui::Button("Reset transform")) {
            object.rotation = glm::vec3(0.0f);
            object.scale = glm::vec3(1.0f);
            object.position.y = 0.0f;
        }
        ImGui::SameLine();
        if (ImGui::Button("Duplicate")) {
            selectedObject = scene.duplicateObject(selectedObject);  // (object is no longer valid: the list grew)
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete")) {
            scene.removeObject(selectedObject);
            selectedObject = std::min(selectedObject, static_cast<int>(sceneObjects.size()) - 1);
        }
        ImGui::PopID();
    }

    buildPortalUi();

    ImGui::Text("%zu models, %zu meshes, %zu instances", assets.modelCount(), stats.meshes, stats.instances);
    ImGui::Text("%zu materials, %zu textures (%.1f MB)", assets.materials().size(), stats.textures,
                stats.textureMemory / (1024.0 * 1024.0));
    ImGui::Text("%zu emissive triangles (%zu bright enough to sample)", stats.emissiveTriangles,
                stats.sampledEmissiveTriangles);
    ImGui::Text("BLAS memory: %.2f MB -> %.2f MB compacted",
                stats.blasBuildSize / (1024.0 * 1024.0), stats.blasSize / (1024.0 * 1024.0));
}

// Portal pairs: the list, and the selected pair's shape, size and its two ends' places
void Editor::buildPortalUi() {
    std::vector<PortalPair>& pairs = scene.portals();
    if (!ImGui::TreeNode("portals", "Portals (%zu pairs)", pairs.size())) {
        return;
    }
    if (ImGui::Button("Add portal pair")) {
        selectedPortal = scene.addPortalPair();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Two openings that are one window: what goes into one comes out of the other,\n"
                          "front to front and back to back. An end faces its +Z (rotation 0: towards +Z).\n"
                          "Light from the sun and lamps doesn't pass through them yet, only bounced light.");
    }
    if (!pairs.empty() && ImGui::BeginListBox("##portalList", ImVec2(0.0f, 3.0f * ImGui::GetTextLineHeightWithSpacing()))) {
        for (int i = 0; i < static_cast<int>(pairs.size()); i++) {
            ImGui::PushID(i);
            std::string label = pairs[i].name + (pairs[i].enabled ? "" : "  (off)");
            if (ImGui::Selectable(label.c_str(), i == selectedPortal)) {
                selectedPortal = i;
            }
            ImGui::PopID();
        }
        ImGui::EndListBox();
    }

    if (selectedPortal >= 0 && selectedPortal < static_cast<int>(pairs.size())) {
        ImGui::PushID("selectedPortal");
        PortalPair& pair = pairs[selectedPortal];
        ImGui::Checkbox("Enabled", &pair.enabled);
        static const char* shapeNames[] = {"Rectangle", "Ellipse"};
        int shape = static_cast<int>(pair.shape);
        if (ImGui::Combo("Shape", &shape, shapeNames, IM_ARRAYSIZE(shapeNames))) {
            pair.shape = static_cast<PortalShape>(shape);
        }
        ImGui::DragFloat2("Size", &pair.size.x, 0.01f, 0.01f, 100.0f);
        ImGui::SameLine();
        ImGui::TextDisabled("width, height");
        ImGui::Checkbox("Rim", &pair.rim);
        if (pair.rim) {
            ImGui::DragFloat("Rim width", &pair.rimWidth, 0.002f, 0.001f, 1.0f, "%.3f");
            ImGui::DragFloat("Rim depth", &pair.rimDepth, 0.002f, 0.001f, 1.0f, "%.3f");
        }
        for (int e = 0; e < 2; e++) {
            ImGui::PushID(e);
            ImGui::SeparatorText(e == 0 ? "End A" : "End B");
            ImGui::DragFloat3("Position", &pair.ends[e].position.x, 0.01f);
            ImGui::DragFloat3("Rotation", &pair.ends[e].rotation.x, 0.5f, 0.0f, 0.0f, "%.1f");
            ImGui::PopID();
        }
        if (ImGui::Button("Delete portal pair")) {
            scene.removePortalPair(selectedPortal);
            selectedPortal = std::min(selectedPortal, static_cast<int>(pairs.size()) - 1);
        }
        ImGui::PopID();
    }
    if (!pairs.empty()) {
        buildMaterialUi(assets.model(scene.portalRimModel()));  // rim A and rim B, shared by all pairs
    }
    ImGui::TreePop();
}

// Materials belong to the model, so editing one changes every object that shows that model.
void Editor::buildMaterialUi(const SceneModel& model) {
    if (!ImGui::TreeNode("materials", "Materials (%u, shared by all %s objects)", model.materialCount, model.name.c_str())) {
        return;
    }
    static const char* alphaModeNames[] = {"Opaque", "Mask (cutout)", "Blend (see-through)"};

    for (uint32_t i = 0; i < model.materialCount; i++) {
        uint32_t index = model.firstMaterial + i;
        SceneMaterial& sceneMaterial = assets.materials()[index];
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
                assets.materialsChanged();  // the renderer copies them in this frame's command buffer
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    ImGui::TreePop();
}

// Lights belong to the model like materials, so editing one changes every object that shows that model.
void Editor::buildPunctualLightUi(SceneModel& model) {
    if (model.lights.empty() ||
        !ImGui::TreeNode("lights", "Lights (%zu, shared by all %s objects)", model.lights.size(), model.name.c_str())) {
        return;
    }
    static const char* typeNames[] = {"directional", "point", "spot"};
    for (size_t i = 0; i < model.lights.size(); i++) {
        PunctualLight& light = model.lights[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::Checkbox("##enabled", &light.enabled);
        ImGui::SameLine();
        if (ImGui::TreeNode("light", "%s (%s)", light.name.c_str(), typeNames[static_cast<uint32_t>(light.type)])) {
            ImGui::ColorEdit3("Color", &light.color.x);
            ImGui::DragFloat(light.type == LightType::Directional ? "Intensity (lux)" : "Intensity (candela)",
                             &light.intensity, std::max(light.intensity * 0.01f, 0.001f), 0.0f, 1e6f, "%.3f");
            if (light.type != LightType::Directional) {
                ImGui::DragFloat("Range (0 = none)", &light.range, 0.05f, 0.0f, 1e4f);
            }
            if (light.type == LightType::Spot) {
                ImGui::SliderAngle("Inner cone", &light.innerConeAngle, 0.0f, 90.0f);
                ImGui::SliderAngle("Outer cone", &light.outerConeAngle, 0.0f, 90.0f);
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    ImGui::TreePop();
}

