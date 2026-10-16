//
// The editor's Outliner and Details panels: adding models, groups and portals (the Add menu), the tree of entities
// (drag one onto another to move it under it; right click for more), and the selected entity: its transform, model
// (parts, materials, lights), portal end and game components. It changes the Scene and the assets' materials and lights; the renderer
// picks the changes up through the render list and the asset store's revisions.
//

#include <algorithm>
#include <cstring>
#include <iostream>
#include <map>
#include <tuple>
#include <imgui/imgui.h>
#include <nlohmann/json.hpp>
#include "editor/Editor.h"
#include "assets/BuiltInModels.h"

void Editor::init(const std::string& error) {
    const std::vector<EntityId>& roots = scene.roots();
    selected = roots.size() > 1 ? roots.back() : 0;  // the default model, not the floor
    if (!error.empty()) {
        log(LogLevel::Error, error);
    }
    savedText = scene.contentText();  // the starting scene counts as saved: nothing to ask about yet
}

// Adds an object for the UI and selects it. Its model may still be loading: the log says when it arrives (or why
// it couldn't be loaded).
void Editor::addObject(const std::string& fileName) {
    try {
        selected = scene.addObject(fileName, addAtOriginalSize);
        const ModelComponent* component = scene.model(selected);
        log(LogLevel::Info, (component != nullptr && component->loading ? "Loading " : "Added ") + fileName);
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        log(LogLevel::Error, e.what());
    }
}

// The Add menu (in the menu bar and the Outliner): a model from the models folder, a group, a portal pair
void Editor::buildAddMenu() {
    if (ImGui::BeginMenu("Model")) {
        if (ImGui::IsWindowAppearing()) {
            assets.scanModelFiles();  // files put there while the program runs
        }
        ImGui::PushItemFlag(ImGuiItemFlags_AutoClosePopups, false);
        ImGui::MenuItem("Original size", nullptr, &addAtOriginalSize);
        ImGui::PopItemFlag();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Off: new models are scaled to %.0f units and stood on the floor.\n"
                              "On: they keep the file's own size and position (glTF uses meters),\n"
                              "for whole scenes like Sponza. The built-in floor is then hidden.",
                              AssetStore::fittedModelSize);
        }
        ImGui::Separator();
        const std::vector<std::string>& files = assets.modelFiles();
        if (files.empty()) {
            ImGui::TextDisabled("none: put .glb/.gltf files (or folders) into resource/models");
        }
        for (const std::string& file : files) {
            if (ImGui::MenuItem(file.c_str())) {
                addObject(file);
            }
        }
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Group")) {
        selected = scene.addEmpty("Group", 0);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("An entity with nothing to show: drag others onto it in the Outliner to move them together.");
    }
    if (ImGui::MenuItem("Portal pair")) {
        selected = scene.addPortalPair();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Two openings that are one window: what goes into one comes out of the other,\n"
                          "front to front and back to back. An end faces its +Z (rotation 0: towards +Z).\n"
                          "Added as a group with its two ends, which can also be moved on their own.\n"
                          "Light from the sun and lamps doesn't pass through them yet, only bounced light.");
    }
}

void Editor::buildOutliner() {
    if (ImGui::Button("+ Add")) {
        ImGui::OpenPopup("addMenu");
    }
    if (ImGui::BeginPopup("addMenu")) {
        buildAddMenu();
        ImGui::EndPopup();
    }
    ImGui::Separator();
    buildEntityTree();
}

void Editor::buildDetails() {
    if (scene.find(selected) == nullptr) {
        selected = 0;
        ImGui::TextDisabled("Nothing selected: click an entity in the Outliner.");
        return;
    }
    buildEntityUi(selected);
}

// The entities as a tree. Dragging one onto another moves it under that one (it stays where it is in the world);
// onto the empty space below the tree, out of its parent. Right click: add a child, duplicate, unparent, delete;
// Delete deletes the selected one. The changes wait until the tree is drawn (they change the children lists).
void Editor::buildEntityTree() {
    TreeEdit edit;
    for (EntityId root : scene.roots()) {
        buildEntityNode(root, edit);
    }
    // The rest of the panel: drop here to move to the top, click to select nothing
    ImVec2 space = ImGui::GetContentRegionAvail();
    ImGui::Dummy(ImVec2(std::max(space.x, 1.0f), std::max(space.y, 2.0f * ImGui::GetTextLineHeightWithSpacing())));
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ENTITY")) {
            edit = {TreeEdit::Move, *static_cast<const EntityId*>(payload->Data), 0};
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::IsItemClicked()) {
        selected = 0;
    }
    if (selected != 0 && ImGui::IsWindowFocused() && !ImGui::GetIO().WantTextInput &&
        ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
        edit = {TreeEdit::Delete, selected, 0};
    }

    switch (edit.kind) {
        case TreeEdit::Move:
            if (scene.setParent(edit.entity, edit.parent)) {
                selected = edit.entity;
            }
            break;
        case TreeEdit::AddChild:
            selected = scene.addEmpty("Group", edit.entity);
            break;
        case TreeEdit::Duplicate:
            selected = scene.duplicate(edit.entity);
            break;
        case TreeEdit::Delete: {
            EntityId parent = scene.find(edit.entity)->parent;
            scene.remove(edit.entity);  // with everything under it
            if (scene.find(selected) == nullptr) {
                selected = parent;
            }
            break;
        }
        case TreeEdit::None:
            break;
    }
}

void Editor::buildEntityNode(EntityId id, TreeEdit& edit) {
    const Entity& entity = *scene.find(id);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
                               ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen;
    if (entity.children.empty()) {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }
    if (id == selected) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }
    const ModelComponent* component = scene.model(id);
    const char* state = component == nullptr     ? ""
                        : component->loading     ? "  (loading)"
                        : !component->model.valid() ? "  (missing)"
                                                    : "";
    // The id keeps entities with the same name apart
    bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<uintptr_t>(id)), flags, "%s%s%s",
                                  entity.name.c_str(), entity.visible ? "" : "  (hidden)", state);
    if ((ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right)) &&
        !ImGui::IsItemToggledOpen()) {
        selected = id;
    }
    if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("Add child")) {
            edit = {TreeEdit::AddChild, id, 0};
        }
        if (ImGui::MenuItem("Duplicate")) {
            edit = {TreeEdit::Duplicate, id, 0};
        }
        if (ImGui::MenuItem("Add game component")) {
            addGameComponent(id);  // (adds no entity: done now)
            selected = id;
        }
        if (entity.parent != 0 && ImGui::MenuItem("Unparent")) {
            edit = {TreeEdit::Move, id, 0};
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Delete", "Del")) {
            edit = {TreeEdit::Delete, id, 0};
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload("ENTITY", &id, sizeof(id));
        ImGui::Text("%s", entity.name.c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ENTITY")) {
            edit = {TreeEdit::Move, *static_cast<const EntityId*>(payload->Data), id};
        }
        ImGui::EndDragDropTarget();
    }
    if (open && !entity.children.empty()) {
        for (EntityId child : entity.children) {
            buildEntityNode(child, edit);
        }
        ImGui::TreePop();
    }
}

// The selected entity: name, visibility, parent, transform, its components, and what can be done with it
void Editor::buildEntityUi(EntityId id) {
    ImGui::PushID("selectedEntity");  // "Position" etc. also exist in the camera section
    Entity& entity = *scene.find(id);

    char name[128];
    std::strncpy(name, entity.name.c_str(), sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    if (ImGui::InputText("Name", name, sizeof(name))) {
        entity.name = name;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("id %llu", static_cast<unsigned long long>(id));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Game components name other entities by this (\"target\": %llu).",
                          static_cast<unsigned long long>(id));
    }
    ImGui::Checkbox("Visible", &entity.visible);
    if (const Entity* parent = scene.find(entity.parent)) {
        ImGui::SameLine();
        ImGui::TextDisabled("under %s", parent->name.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Unparent")) {
            scene.setParent(id, 0);  // (entity stays valid: no entity added or removed)
        }
    }
    buildTransformUi(entity);
    if (scene.model(id) != nullptr) {
        buildModelUi(id);
    }
    if (scene.portal(id) != nullptr) {
        buildPortalUi(id);
    }
    if (scene.game(id) != nullptr) {
        buildGameUi(id);
    }

    ImGui::Separator();
    if (ImGui::Button("Reset transform")) {
        entity.local.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        entity.local.scale = glm::vec3(1.0f);
        entity.local.position.y = 0.0f;
    }
    ImGui::SameLine();
    if (ImGui::Button("+ Game")) {
        addGameComponent(id);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Adds a game component: a type and its properties (JSON),\n"
                          "which the game reads when Play starts.");
    }
    // The ones below add or remove entities: entity is no longer valid after them
    ImGui::SameLine();
    if (ImGui::Button("Add child")) {
        selected = scene.addEmpty("Group", id);
    }
    ImGui::SameLine();
    if (ImGui::Button("Duplicate")) {
        selected = scene.duplicate(id);
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete")) {
        EntityId parent = scene.find(id)->parent;
        scene.remove(id);  // with everything under it
        selected = parent;
    }
    ImGui::PopID();
}

void Editor::buildTransformUi(Entity& entity) {
    Transform& local = entity.local;
    ImGui::DragFloat3("Position", &local.position.x, 0.01f);

    if (eulerEntity != entity.id || local.rotation != eulerRotation) {
        eulerEntity = entity.id;
        eulerRotation = local.rotation;
        eulerDegrees = quatToEuler(local.rotation);
    }
    if (ImGui::DragFloat3("Rotation", &eulerDegrees.x, 0.5f, 0.0f, 0.0f, "%.1f")) {
        local.rotation = eulerToQuat(eulerDegrees);
        eulerRotation = local.rotation;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Degrees around X, Y, Z.\nApplied as yaw (Y), then pitch (X), then roll (Z).\n"
                          "Position, rotation and scale are relative to the parent.");
    }

    if (entity.uniformScale) {
        float scale = local.scale.x;
        if (ImGui::DragFloat("Scale", &scale, 0.01f, 0.01f, 100.0f)) {
            local.scale = glm::vec3(scale);
        }
    } else {
        ImGui::DragFloat3("Scale", &local.scale.x, 0.01f, 0.01f, 100.0f);
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Uniform", &entity.uniformScale) && entity.uniformScale) {
        local.scale = glm::vec3(local.scale.x);
    }
}

// The entity's model: original size, its parts, materials and lights (the last two are the model's: shared by all
// entities showing it)
void Editor::buildModelUi(EntityId id) {
    ModelComponent& component = *scene.model(id);
    ImGui::SeparatorText("Model");
    if (component.loading) {
        ImGui::TextDisabled("%s: loading ...", assets.modelName(component.model).c_str());
        return;
    }
    if (!component.model.valid()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s: not loaded", component.missingFile.c_str());
        ImGui::TextWrapped("%s", component.missingReason.c_str());
        ImGui::TextDisabled("Kept as the scene file has it: saving writes it back.");
        return;
    }
    SceneModel& model = assets.model(component.model);
    ImGui::TextDisabled("%s, %zu parts", model.name.c_str(), model.parts.size());
    if (component.model != assets.floor() && !isBuiltInModel(model.name)) {
        ImGui::Checkbox("Original size", &component.originalSize);
        ImGui::SameLine();
        ImGui::TextDisabled("(file units, not fitted to %.0f)", AssetStore::fittedModelSize);
    }

    if (model.parts.size() > 1 && ImGui::TreeNode("parts", "Parts (%zu)", model.parts.size())) {
        component.hiddenParts.resize(model.parts.size(), false);
        // Only the rows in view are drawn: scenes with vegetation have millions of parts
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(model.parts.size()));
        while (clipper.Step()) {
            for (int p = clipper.DisplayStart; p < clipper.DisplayEnd; p++) {
                ImGui::PushID(p);
                bool visible = !component.hiddenParts[p];
                if (ImGui::Checkbox(assets.meshes()[model.parts[p].meshIndex].name.c_str(), &visible)) {
                    scene.setPartHidden(id, p, !visible);
                }
                ImGui::PopID();
            }
        }
        ImGui::TreePop();
    }
    buildMaterialUi(model);
    buildPunctualLightUi(model);
}

// A portal end: the pair's shape, size and rim (the same on both ends), and the other end
void Editor::buildPortalUi(EntityId id) {
    PortalComponent& portal = *scene.portal(id);
    ImGui::SeparatorText(portal.endA ? "Portal end A" : "Portal end B");
    bool changed = false;
    static const char* shapeNames[] = {"Rectangle", "Ellipse"};
    int shape = static_cast<int>(portal.shape);
    if (ImGui::Combo("Shape", &shape, shapeNames, IM_ARRAYSIZE(shapeNames))) {
        portal.shape = static_cast<PortalShape>(shape);
        changed = true;
    }
    changed |= ImGui::DragFloat2("Size", &portal.size.x, 0.01f, 0.01f, 100.0f);
    ImGui::SameLine();
    ImGui::TextDisabled("width, height");
    changed |= ImGui::Checkbox("Rim", &portal.rim);
    if (portal.rim) {
        changed |= ImGui::DragFloat("Rim width", &portal.rimWidth, 0.002f, 0.001f, 1.0f, "%.3f");
        changed |= ImGui::DragFloat("Rim depth", &portal.rimDepth, 0.002f, 0.001f, 1.0f, "%.3f");
    }
    if (changed) {
        scene.syncPortal(id);
    }
    if (const Entity* other = scene.find(portal.other)) {
        ImGui::TextDisabled("Other end: %s", other->name.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Select")) {
            selected = portal.other;
        }
        if (!scene.visibleInWorld(id) || !scene.visibleInWorld(portal.other)) {
            ImGui::TextDisabled("Off: both ends have to be visible");
        }
    }
    buildMaterialUi(assets.model(scene.portalRimModel()));  // rim A and rim B, shared by all pairs
}

// Whether a mesh is closed: every edge shared by exactly two triangles (corners at the same place count as one, a
// mesh has a corner several times where its normals or UVs differ). Fog fills a mesh by the surfaces a ray crosses,
// so an open one has fog leaking out of it, or inside out. Kept by mesh id: a mesh doesn't change while loaded.
static bool meshIsClosed(const AssetStore& assets, uint32_t meshIndex) {
    static std::map<uint64_t, bool> known;
    uint64_t id = assets.meshIds()[meshIndex];
    auto it = known.find(id);
    if (it != known.end()) {
        return it->second;
    }
    const Mesh& mesh = assets.meshes()[meshIndex];
    auto key = [](const glm::vec3& p) { return std::make_tuple(p.x, p.y, p.z); };
    std::map<std::tuple<float, float, float>, uint32_t> corners;
    std::vector<uint32_t> corner(mesh.vertices.size());
    for (size_t v = 0; v < mesh.vertices.size(); v++) {
        corner[v] = corners.emplace(key(mesh.vertices[v].position), static_cast<uint32_t>(corners.size())).first->second;
    }
    std::map<std::pair<uint32_t, uint32_t>, int> edges;
    for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
        for (int e = 0; e < 3; e++) {
            uint32_t a = corner[mesh.indices[t + e]], b = corner[mesh.indices[t + (e + 1) % 3]];
            if (a != b) {
                edges[{std::min(a, b), std::max(a, b)}]++;
            }
        }
    }
    bool closed = !edges.empty() && std::all_of(edges.begin(), edges.end(), [](const auto& edge) { return edge.second == 2; });
    known[id] = closed;
    return closed;
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
            bool fog = m.fogDensity > 0.0f;
            if (ImGui::Checkbox("Fog inside", &fog)) {
                m.fogDensity = fog ? 0.3f : 0.0f;
                changed = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Fills the mesh with fog (it must be closed). With an invisible surface: haze, smoke,\n"
                                  "a cloud. With solid glass: murky water, milky glass.");
            }
            if (fog) {
                changed |= ImGui::SliderFloat("Thickness##fog", &m.fogDensity, 0.001f, 10.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("How much fog there is per unit of distance: light goes about 1 / thickness units\n"
                                      "before it scatters.");
                }
                changed |= ImGui::ColorEdit3("Fog color", &m.fogColor.x, ImGuiColorEditFlags_Float);
                changed |= ImGui::SliderFloat("Forward scattering", &m.fogForward, -0.9f, 0.9f, "%.2f");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("0: light scatters every way alike. Higher: mostly onwards, so beams glow brightest\n"
                                      "looking towards the light (real haze: 0.3 - 0.8). Negative: back towards the light.");
                }
                changed |= ImGui::Checkbox("Patchy (noise)", &m.fogNoise);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Fog in blobs and wisps instead of the same everywhere. The thickness is then the\n"
                                      "most there is. The noise belongs to the object: it moves, turns and scales with it.");
                }
                if (m.fogNoise) {
                    changed |= ImGui::SliderFloat("Blob size", &m.fogNoiseSize, 0.01f, 100.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("About how big the blobs are, in the mesh's own units (before the object's scale)");
                    }
                    changed |= ImGui::SliderFloat("Coverage", &m.fogCoverage, 0.0f, 1.0f, "%.2f");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("About the share of the volume that has fog: low = a few wisps, 1 = everywhere, thinner\n"
                                          "in places");
                    }
                    changed |= ImGui::SliderInt("Variation", &m.fogNoiseSeed, 0, 99);
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Another part of the noise: the same kind of fog, other blobs");
                    }
                    changed |= ImGui::DragFloat3("Drift", &m.fogDrift.x, 0.01f, -10.0f, 10.0f, "%.2f");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Wind: the blobs move this far per second (mesh units, along the mesh's own X, Y, Z).\n"
                                          "They pass through the mesh, which stays where it is. 0 = still.\n"
                                          "While fog drifts, Accumulation restarts every frame (the image changes).");
                    }
                }
                changed |= ImGui::SliderFloat("Edge softness", &m.fogEdge, 0.0f, 10.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Fades the fog out over this distance towards the mesh's surface (mesh units), instead\n"
                                      "of ending sharply there. 0: sharp.");
                }
                if (m.fogNoise || m.fogEdge > 0.0f) {
                    ImGui::TextDisabled("Varying fog: slower than even fog, the more so the thinner it is in places");
                }
                for (uint32_t mesh = model.firstMesh; mesh < model.firstMesh + model.meshCount; mesh++) {
                    if (assets.meshes()[mesh].materialIndex == index && !meshIsClosed(assets, mesh)) {
                        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Mesh \"%s\" isn't closed: fog may leak or turn inside out",
                                           assets.meshes()[mesh].name.c_str());
                    }
                }
            }
            changed |= ImGui::Checkbox("Invisible surface", &m.invisibleSurface);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Rays pass the surface unchanged: only the fog inside shows");
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


// ImGui text fields for a std::string (the buffer grows as text is typed)
static int resizeString(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* text = static_cast<std::string*>(data->UserData);
        text->resize(data->BufTextLen);
        data->Buf = text->data();
    }
    return 0;
}

static bool inputText(const char* label, std::string& text) {
    return ImGui::InputText(label, text.data(), text.capacity() + 1, ImGuiInputTextFlags_CallbackResize, resizeString,
                            &text);
}

static bool inputTextMultiline(const char* label, std::string& text, const ImVec2& size) {
    return ImGui::InputTextMultiline(label, text.data(), text.capacity() + 1, size, ImGuiInputTextFlags_CallbackResize,
                                     resizeString, &text);
}

// A component's properties as the editor shows them: over lines, indented ("" stays "")
static std::string prettyProperties(const std::string& properties) {
    if (properties.empty()) {
        return "";
    }
    try {
        return nlohmann::ordered_json::parse(properties).dump(2);
    } catch (const std::exception&) {
        return properties;  // (the scene only holds valid JSON: not expected)
    }
}

// The field's text as a component's properties: compact, "" for nothing or {}. Returns why it can't be ("": it can).
static std::string compactProperties(const std::string& text, std::string& properties) {
    if (text.find_first_not_of(" \t\r\n") == std::string::npos) {
        properties.clear();
        return "";
    }
    try {
        nlohmann::ordered_json json = nlohmann::ordered_json::parse(text);
        if (!json.is_object()) {
            return "expected an object: { \"name\": value, ... }";
        }
        properties = json.empty() ? "" : json.dump();
        return "";
    } catch (const nlohmann::json::exception& e) {
        std::string message = e.what();  // "[json.exception.parse_error.101] parse error at line 2, ..."
        size_t start = message.find("] ");
        return start == std::string::npos ? message : message.substr(start + 2);
    }
}

void Editor::addGameComponent(EntityId id) {
    const std::vector<GameComponent>* existing = scene.game(id);
    std::vector<GameComponent> components = existing != nullptr ? *existing : std::vector<GameComponent>();
    components.emplace_back();
    scene.setGame(id, std::move(components));
}

// The entity's game components: a type and properties (JSON) each, in their order. The editor doesn't know the
// types: the game reads them when Play starts (game/Game.h). Properties are applied while they are valid JSON.
void Editor::buildGameUi(EntityId id) {
    std::vector<GameComponent> components = *scene.game(id);
    bool fresh = gameEditEntity != id || gameEdits.size() != components.size();
    for (size_t i = 0; !fresh && i < components.size(); i++) {
        fresh = gameEdits[i].applied != components[i].properties;
    }
    if (fresh) {
        gameEditEntity = id;
        gameEdits.clear();
        for (const GameComponent& component : components) {
            gameEdits.push_back({prettyProperties(component.properties), component.properties, ""});
        }
    }

    ImGui::SeparatorText("Game");
    bool changed = false;
    int removed = -1;
    int swapped = -1;  // with the one before it
    for (size_t i = 0; i < components.size(); i++) {
        GameComponent& component = components[i];
        GameEdit& edit = gameEdits[i];
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::TreeNodeEx("component", ImGuiTreeNodeFlags_DefaultOpen, "%s",
                              component.type.empty() ? "(no type)" : component.type.c_str())) {
            changed |= inputText("Type", component.type);
            int lines = static_cast<int>(std::count(edit.properties.begin(), edit.properties.end(), '\n')) + 2;
            float height = static_cast<float>(std::clamp(lines, 3, 16)) * ImGui::GetTextLineHeight() +
                           ImGui::GetStyle().FramePadding.y * 2.0f;
            bool invalid = !edit.error.empty();
            if (invalid) {
                ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.45f, 0.12f, 0.12f, 1.0f));
            }
            if (inputTextMultiline("##properties", edit.properties, ImVec2(-FLT_MIN, height))) {
                std::string properties;
                edit.error = compactProperties(edit.properties, properties);
                if (edit.error.empty() && properties != component.properties) {
                    component.properties = properties;
                    edit.applied = properties;
                    changed = true;
                }
            }
            if (invalid) {
                ImGui::PopStyleColor();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Properties as a JSON object, e.g. { \"speed\": 90, \"axis\": [0, 1, 0] }.\n"
                                  "Applied while the text is valid; the type decides what it reads.");
            }
            if (!edit.error.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                ImGui::TextWrapped("Not applied: %s", edit.error.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::BeginDisabled(i == 0);
            if (ImGui::SmallButton("Up")) {
                swapped = static_cast<int>(i);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(i + 1 == components.size());
            if (ImGui::SmallButton("Down")) {
                swapped = static_cast<int>(i + 1);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove")) {
                removed = static_cast<int>(i);
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (removed >= 0) {
        components.erase(components.begin() + removed);
        gameEdits.erase(gameEdits.begin() + removed);
        changed = true;
    } else if (swapped > 0) {
        std::swap(components[swapped - 1], components[swapped]);
        std::swap(gameEdits[swapped - 1], gameEdits[swapped]);
        changed = true;
    }
    if (changed) {
        scene.setGame(id, std::move(components));
    }
}
