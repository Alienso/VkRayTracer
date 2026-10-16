//
// Scene files: the scene as JSON (nlohmann/json, include/nlohmann, MIT license). See Scene::save / load.
//
// {
//   "format": "vulkanDemo scene", "version": 1,
//   "nextEntityId": 12,               ids go on from here (never reused)
//   "portalPairsMade": 2,             for the next pair's name
//   "camera": {"position": [x, y, z], "yaw": .., "pitch": .., "fov": ..},
//   "environment": {"sunEnabled": .., ..., "background": "sky", "map": "file.hdr" or null, ...},
//   "entities": [                     parents before their children, children in their order
//     {"id": 3, "name": "Helmet", "parent": 0, "position": [..], "rotation": [x, y, z, w], "scale": [..],
//      "visible": false (only when hidden), "uniformScale": false (only when off),
//      "model": {"file": "DamagedHelmet.glb" or "builtIn": "Floor", "originalSize": true (only when on),
//                "hide": [parts hidden that the model shows], "show": [parts shown that the model hides]},
//      "portal": {"other": 7, "endA": true, "shape": "rectangle", "size": [w, h], "rim": true, "rimWidth": ..,
//                 "rimDepth": ..}},
//     ...],
//   "models": [                       edits to models' materials and lights: only what differs from the file
//     {"file": "Bistro/BistroExterior.gltf", "materials": [{"index": 12, "name": "Glass", "roughness": 0.1}],
//      "lights": [{"index": 0, "name": "Lamp", "intensity": 3}]}]
// }
//
// Reading: unknown keys are ignored and missing ones keep their defaults, so files from older versions keep
// loading as fields are added. "version" goes up only for changes an old reader would misread; this reader refuses
// files of a newer version. Positions in model files are relative to the models folder, map files to the
// environments folder, with '/'.
//
// Written with the top level spread over lines and every entity, model edit, camera and environment on one line of
// its own, so a diff of two versions shows what changed. Numbers are the shortest that read back the same (0.3,
// not 0.30000001192092896).
//

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <nlohmann/json.hpp>
#include "scene/Scene.h"
#include "assets/BuiltInModels.h"
#include "assets/EditableFields.h"

using Json = nlohmann::ordered_json;

static const char* formatName = "vulkanDemo scene";
static constexpr int fileVersion = 1;

// ---------------------------------------------------------------------------------------------
// Values

// The shortest decimal that reads back as the same float
static Json number(float value) {
    char text[32];
    std::to_chars_result result = std::to_chars(text, text + sizeof(text) - 1, value);
    *result.ptr = '\0';
    return std::strtod(text, nullptr);
}

static Json toJson(float value) { return number(value); }
static Json toJson(int value) { return value; }
static Json toJson(bool value) { return value; }
static Json toJson(const glm::vec2& v) { return Json::array({number(v.x), number(v.y)}); }
static Json toJson(const glm::vec3& v) { return Json::array({number(v.x), number(v.y), number(v.z)}); }
static Json toJson(const glm::vec4& v) { return Json::array({number(v.x), number(v.y), number(v.z), number(v.w)}); }
static Json toJson(const glm::quat& q) { return Json::array({number(q.x), number(q.y), number(q.z), number(q.w)}); }

static const char* alphaModeNames[] = {"opaque", "mask", "blend"};
static const char* shapeNames[] = {"rectangle", "ellipse"};
static const char* backgroundNames[] = {"sky", "map", "black", "white"};

static Json toJson(AlphaMode mode) { return alphaModeNames[static_cast<int>(mode)]; }

// Readers: throw std::runtime_error naming what was expected (the caller adds where)
static void fromJson(const Json& j, float& value) {
    if (!j.is_number()) {
        throw std::runtime_error("expected a number, found " + j.dump());
    }
    value = j.get<float>();
}

static void fromJson(const Json& j, int& value) {
    if (!j.is_number_integer()) {
        throw std::runtime_error("expected a whole number, found " + j.dump());
    }
    value = j.get<int>();
}

static void fromJson(const Json& j, bool& value) {
    if (!j.is_boolean()) {
        throw std::runtime_error("expected true or false, found " + j.dump());
    }
    value = j.get<bool>();
}

static void readFloats(const Json& j, float* out, size_t count) {
    if (!j.is_array() || j.size() != count) {
        throw std::runtime_error("expected " + std::to_string(count) + " numbers, found " + j.dump());
    }
    for (size_t i = 0; i < count; i++) {
        fromJson(j[i], out[i]);
    }
}

static void fromJson(const Json& j, glm::vec2& v) { readFloats(j, &v.x, 2); }
static void fromJson(const Json& j, glm::vec3& v) { readFloats(j, &v.x, 3); }
static void fromJson(const Json& j, glm::vec4& v) { readFloats(j, &v.x, 4); }

static void fromJson(const Json& j, glm::quat& q) {
    float xyzw[4];
    readFloats(j, xyzw, 4);
    q = glm::quat(xyzw[3], xyzw[0], xyzw[1], xyzw[2]);
    // Normalized only when clearly off (typed in by hand): a saved rotation reads back exactly as it was
    float length = glm::length(q);
    if (std::abs(length - 1.0f) > 1e-5f) {
        q = length > 0.0f ? q / length : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    }
}

static void fromJson(const Json& j, std::string& value) {
    if (!j.is_string()) {
        throw std::runtime_error("expected a string, found " + j.dump());
    }
    value = j.get<std::string>();
}

// A name from a list (the enums' names above) -> its position
template <size_t N> static int nameIndex(const Json& j, const char* (&names)[N]) {
    std::string name;
    fromJson(j, name);
    for (size_t i = 0; i < N; i++) {
        if (name == names[i]) {
            return static_cast<int>(i);
        }
    }
    throw std::runtime_error("unknown value \"" + name + "\"");
}

static void fromJson(const Json& j, AlphaMode& mode) { mode = static_cast<AlphaMode>(nameIndex(j, alphaModeNames)); }

// Reads key into value when the object has it
template <typename T> static void readOptional(const Json& object, const char* key, T& value) {
    auto it = object.find(key);
    if (it != object.end()) {
        try {
            fromJson(*it, value);
        } catch (const std::exception& e) {
            throw std::runtime_error(std::string("\"") + key + "\": " + e.what());
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Writing

// The top level spread over lines; arrays of objects one object per line; everything else on one line
static std::string documentText(const Json& document) {
    auto dump = [](const Json& j) { return j.dump(-1, ' ', false, Json::error_handler_t::replace); };
    std::string text = "{\n";
    size_t i = 0;
    for (auto it = document.begin(); it != document.end(); ++it, ++i) {
        text += "  " + dump(Json(it.key())) + ": ";
        const Json& value = it.value();
        if (value.is_array() && !value.empty() && value[0].is_object()) {
            text += "[\n";
            for (size_t k = 0; k < value.size(); k++) {
                text += "    " + dump(value[k]) + (k + 1 < value.size() ? ",\n" : "\n");
            }
            text += "  ]";
        } else {
            text += dump(value);
        }
        text += i + 1 < document.size() ? ",\n" : "\n";
    }
    return text + "}\n";
}

// How a file names a model: its file (relative to the models folder) or a built-in model's name
static Json modelReference(const std::string& name, bool builtIn) {
    Json reference = Json::object();
    reference[builtIn ? "builtIn" : "file"] = name;
    return reference;
}

static Json modelReference(const AssetStore& assets, ModelHandle handle) {
    const std::string& name = assets.modelName(handle);
    return modelReference(name, handle == assets.floor() || isBuiltInModel(name));
}

std::string Scene::modelEntryText(const std::string& fileName, bool builtIn, bool originalSize) {
    Json entry = modelReference(fileName, builtIn);
    if (originalSize) {
        entry["originalSize"] = true;
    }
    return entry.dump();
}

static Json materialEdits(const SceneMaterial& now, const SceneMaterial& file) {
    Json edits = Json::object();
#define WRITE_FIELD(name) if (!(now.material.name == file.material.name)) edits[#name] = toJson(now.material.name);
#define WRITE_SWITCH(name) if (now.name != file.name) edits[#name] = now.name;
    MATERIAL_FIELDS(WRITE_FIELD)
    MATERIAL_SWITCHES(WRITE_SWITCH)
#undef WRITE_FIELD
#undef WRITE_SWITCH
    return edits;
}

static Json lightEdits(const PunctualLight& now, const PunctualLight& file) {
    Json edits = Json::object();
#define WRITE_LIGHT(name) if (!(now.name == file.name)) edits[#name] = toJson(now.name);
    LIGHT_FIELDS(WRITE_LIGHT)
#undef WRITE_LIGHT
    return edits;
}

std::string Scene::toText(bool withCamera) const {
    Json document;
    document["format"] = formatName;
    document["version"] = fileVersion;
    document["nextEntityId"] = entitiesMade + 1;
    document["portalPairsMade"] = portalPairsMade;
    if (withCamera) {
        document["camera"] = {{"position", toJson(sceneCamera.position)}, {"yaw", number(sceneCamera.yaw)},
                              {"pitch", number(sceneCamera.pitch)}, {"fov", number(sceneCamera.fov)}};
    }

    const SceneEnvironment& e = sceneEnvironment;
    Json environment;
    environment["sunEnabled"] = e.sunEnabled;
    environment["sunAzimuth"] = number(e.sunAzimuth);
    environment["sunElevation"] = number(e.sunElevation);
    environment["sunAngularRadius"] = number(e.sunAngularRadius);
    environment["sunColor"] = toJson(e.sunColor);
    environment["sunIntensity"] = number(e.sunIntensity);
    environment["background"] = backgroundNames[std::clamp(e.background, 0, 3)];
    environment["skyZenithColor"] = toJson(e.skyZenithColor);
    environment["skyHorizonColor"] = toJson(e.skyHorizonColor);
    environment["groundColor"] = toJson(e.groundColor);
    environment["skyIntensity"] = number(e.skyIntensity);
    // (The one picked last, also while it loads)
    EnvironmentHandle map = pendingMap.valid() ? pendingMap : e.map;
    environment["map"] = map.valid() ? Json(assets.environmentMap(map).name) : Json();
    environment["mapRotation"] = number(e.mapRotation);
    environment["mapIntensity"] = number(e.mapIntensity);
    document["environment"] = environment;

    Json entities = Json::array();
    std::vector<ModelHandle> models;  // the models shown, in the order of their first entity
    for (EntityId id : treeOrder()) {
        const Entity& entity = *find(id);
        Json j;
        j["id"] = entity.id;
        j["name"] = entity.name;
        j["parent"] = entity.parent;
        j["position"] = toJson(entity.local.position);
        j["rotation"] = toJson(entity.local.rotation);
        j["scale"] = toJson(entity.local.scale);
        if (!entity.visible) {
            j["visible"] = false;
        }
        if (!entity.uniformScale) {
            j["uniformScale"] = false;
        }
        if (const ModelComponent* component = model(id)) {
            Json m;
            if (!component->model.valid()) {
                m = Json::parse(component->missingEntry);  // as the file had it
            } else if (component->loading) {
                m = Json::parse(component->loadingEntry);  // the same: its parts aren't known yet
                if (std::find(models.begin(), models.end(), component->model) == models.end()) {
                    models.push_back(component->model);
                }
            } else {
                m = modelReference(assets, component->model);
                if (component->originalSize) {
                    m["originalSize"] = true;
                }
                const SceneModel& sceneModel = assets.model(component->model);
                Json hide = Json::array(), show = Json::array();
                for (size_t p = 0; p < sceneModel.parts.size(); p++) {
                    bool hidden = p < component->hiddenParts.size() && component->hiddenParts[p];
                    if (hidden != sceneModel.parts[p].hiddenByDefault) {
                        (hidden ? hide : show).push_back(p);
                    }
                }
                if (!hide.empty()) {
                    m["hide"] = hide;
                }
                if (!show.empty()) {
                    m["show"] = show;
                }
                if (std::find(models.begin(), models.end(), component->model) == models.end()) {
                    models.push_back(component->model);
                }
            }
            j["model"] = m;
        }
        if (const PortalComponent* portal = this->portal(id)) {
            j["portal"] = {{"other", portal->other}, {"endA", portal->endA},
                           {"shape", shapeNames[static_cast<int>(portal->shape)]}, {"size", toJson(portal->size)},
                           {"rim", portal->rim}, {"rimWidth", number(portal->rimWidth)},
                           {"rimDepth", number(portal->rimDepth)}};
        }
        entities.push_back(j);
    }
    document["entities"] = entities;

    Json modelList = Json::array();
    for (ModelHandle handle : models) {
        if (!assets.isLoaded(handle)) {
            // Still loading: the edits the file had for it, as they were
            for (const auto& [name, entry] : pendingModelEdits) {
                if (name == assets.modelName(handle)) {
                    modelList.push_back(Json::parse(entry));
                }
            }
            continue;
        }
        const SceneModel& sceneModel = assets.model(handle);
        Json materials = Json::array(), lights = Json::array();
        for (uint32_t i = 0; i < sceneModel.materialCount && i < sceneModel.fileMaterials.size(); i++) {
            const SceneMaterial& file = sceneModel.fileMaterials[i];
            Json edits = materialEdits(assets.materials()[sceneModel.firstMaterial + i], file);
            if (!edits.empty()) {
                Json entry = {{"index", i}, {"name", file.material.name}};
                entry.update(edits);
                materials.push_back(entry);
            }
        }
        for (size_t i = 0; i < sceneModel.lights.size() && i < sceneModel.fileLights.size(); i++) {
            Json edits = lightEdits(sceneModel.lights[i], sceneModel.fileLights[i]);
            if (!edits.empty()) {
                Json entry = {{"index", i}, {"name", sceneModel.fileLights[i].name}};
                entry.update(edits);
                lights.push_back(entry);
            }
        }
        if (!materials.empty() || !lights.empty()) {
            Json entry = modelReference(assets, handle);
            if (!materials.empty()) {
                entry["materials"] = materials;
            }
            if (!lights.empty()) {
                entry["lights"] = lights;
            }
            modelList.push_back(entry);
        }
    }
    document["models"] = modelList;
    return documentText(document);
}

std::string Scene::contentText() const {
    return toText(false);
}

void Scene::save(const std::string& path) const {
    std::string text = toText(true);
    std::filesystem::path target(path);
    if (target.has_parent_path()) {
        std::filesystem::create_directories(target.parent_path());
    }
    std::filesystem::path temporary = target;
    temporary += ".saving";
    {
        std::ofstream file(temporary, std::ios::binary);
        file << text;
        file.close();
        if (!file) {
            throw std::runtime_error("can't write " + temporary.string());
        }
    }
    std::error_code error;
    std::filesystem::rename(temporary, target, error);  // replaces the old file in one step
    if (error) {
        std::filesystem::remove(temporary);
        throw std::runtime_error("can't write " + path + ": " + error.message());
    }
}

// ---------------------------------------------------------------------------------------------
// Reading

// The model a file's reference names, acquired: loaded, or loading in the background. Throws for a built-in model
// that doesn't exist (or a broken reference).
static ModelHandle loadModel(AssetStore& assets, const Json& reference) {
    ModelHandle handle;
    if (auto it = reference.find("builtIn"); it != reference.end()) {
        std::string name;
        fromJson(*it, name);
        if (name == "Floor") {
            handle = assets.isLoaded(assets.floor()) ? assets.floor() : assets.addFloor();  // as createDefault
        } else if (isBuiltInModel(name)) {
            handle = assets.requestModel(name);
        } else {
            // (Checked before asking the store: it would take the name for a model file and start loading it)
            throw std::runtime_error("no built-in model \"" + name + "\"");
        }
    } else if (auto file = reference.find("file"); file != reference.end()) {
        std::string name;
        fromJson(*file, name);
        handle = assets.requestModel(name);
    } else {
        throw std::runtime_error("a model needs \"file\" or \"builtIn\"");
    }
    assets.acquire(handle);
    return handle;
}

// A model entry's "hide" and "show" lists into the component's hidden parts (which start as the model's). Parts the
// model doesn't have are warned about; a list that isn't one throws.
static void readParts(const SceneModel& model, ModelComponent& component, const Json& m,
                      std::vector<std::string>& warnings) {
    for (const char* key : {"hide", "show"}) {
        auto list = m.find(key);
        if (list == m.end()) {
            continue;
        }
        if (!list->is_array()) {
            throw std::runtime_error(std::string("\"") + key + "\": expected a list of parts");
        }
        for (const Json& part : *list) {
            if (!part.is_number_unsigned() || part.get<size_t>() >= model.parts.size()) {
                warnings.push_back("no part " + part.dump() + " in " + model.name);
                continue;
            }
            component.hiddenParts[part.get<size_t>()] = std::string(key) == "hide";
        }
    }
}

std::vector<std::string> Scene::applyLoadingEntry(ModelComponent& component) {
    std::vector<std::string> warnings;
    try {
        readParts(assets.model(component.model), component, Json::parse(component.loadingEntry), warnings);
    } catch (const std::exception& e) {
        warnings.push_back(e.what());
    }
    return warnings;
}

static std::string referenceName(const Json& reference) {
    std::string name;
    if (auto it = reference.find("file"); it != reference.end() && it->is_string()) {
        name = it->get<std::string>();
    } else if (auto builtIn = reference.find("builtIn"); builtIn != reference.end() && builtIn->is_string()) {
        name = builtIn->get<std::string>();
    }
    return name;
}

// A "models" entry's edits onto the model's materials and lights. An edit that no longer fits the model (the file
// changed) is skipped, with a warning.
static void applyEdits(AssetStore& assets, SceneModel& model, const Json& entry, std::vector<std::string>& warnings) {
    const std::string& name = model.name;
    auto edit = [&](const char* kind, size_t count, auto&& apply) {
        auto items = entry.find(kind);
        if (items == entry.end() || !items->is_array()) {
            return;
        }
        for (const Json& item : *items) {
            size_t index = item.contains("index") && item["index"].is_number_unsigned()
                           ? item["index"].get<size_t>() : count;
            std::string itemName;
            if (auto n = item.find("name"); n != item.end() && n->is_string()) {
                itemName = n->get<std::string>();
            }
            try {
                apply(index, itemName, item);
            } catch (const std::exception& e) {
                warnings.push_back(name + ": " + kind + " " + std::to_string(index) + " (" + itemName +
                                   "): " + e.what());
            }
        }
    };
    edit("materials", model.materialCount, [&](size_t index, const std::string& itemName, const Json& item) {
        if (index >= model.materialCount || index >= model.fileMaterials.size() ||
            model.fileMaterials[index].material.name != itemName) {
            throw std::runtime_error("not in the model any more: edit skipped");
        }
        SceneMaterial material = assets.materials()[model.firstMaterial + index];  // a copy: all or nothing
#define READ_FIELD(name) readOptional(item, #name, material.material.name);
#define READ_SWITCH(name) readOptional(item, #name, material.name);
        MATERIAL_FIELDS(READ_FIELD)
        MATERIAL_SWITCHES(READ_SWITCH)
#undef READ_FIELD
#undef READ_SWITCH
        assets.materials()[model.firstMaterial + index] = material;
    });
    edit("lights", model.lights.size(), [&](size_t index, const std::string& itemName, const Json& item) {
        if (index >= model.lights.size() || index >= model.fileLights.size() ||
            model.fileLights[index].name != itemName) {
            throw std::runtime_error("not in the model any more: edit skipped");
        }
        PunctualLight light = model.lights[index];
#define READ_LIGHT(name) readOptional(item, #name, light.name);
        LIGHT_FIELDS(READ_LIGHT)
#undef READ_LIGHT
        model.lights[index] = light;
    });
}

std::vector<std::string> Scene::applyPendingEdits(const std::string& modelName) {
    std::vector<std::string> warnings;
    for (auto it = pendingModelEdits.begin(); it != pendingModelEdits.end();) {
        if (it->first != modelName) {
            ++it;
            continue;
        }
        for (const auto& [id, component] : modelTable) {
            if (assets.isLoaded(component.model) && assets.modelName(component.model) == modelName) {
                applyEdits(assets, assets.model(component.model), Json::parse(it->second), warnings);
                break;
            }
        }
        it = pendingModelEdits.erase(it);
    }
    return warnings;
}

std::vector<std::string> Scene::load(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("can't open " + path);
    }
    Json document;
    try {
        document = Json::parse(file);
    } catch (const Json::exception& e) {
        throw std::runtime_error(path + " isn't valid JSON: " + e.what());
    }
    if (!document.is_object() || document.value("format", std::string()) != formatName) {
        throw std::runtime_error(path + " isn't a scene file (no \"format\": \"" + formatName + "\")");
    }
    int version = 0;
    readOptional(document, "version", version);
    if (version < 1 || version > fileVersion) {
        throw std::runtime_error(path + " is version " + std::to_string(version) + ", this program reads up to " +
                                 std::to_string(fileVersion));
    }

    // Built into a scene of its own first: if anything goes wrong, this one stays as it is
    std::vector<std::string> warnings;
    Scene loaded(assets);
    try {
        // Entities: parents come before their children
        EntityId highest = 0;
        const Json entities = document.contains("entities") ? document["entities"] : Json::array();
        if (!entities.is_array()) {
            throw std::runtime_error("\"entities\": expected a list");
        }
        for (size_t i = 0; i < entities.size(); i++) {
            const Json& j = entities[i];
            EntityId id = j.contains("id") && j["id"].is_number_unsigned() ? j["id"].get<EntityId>() : 0;
            if (id == 0 || loaded.find(id) != nullptr) {
                throw std::runtime_error("entity " + std::to_string(i + 1) + ": missing or repeated \"id\"");
            }
            std::string where = "entity " + std::to_string(id);
            try {
                std::string name = "Entity";
                readOptional(j, "name", name);
                EntityId parent = j.contains("parent") && j["parent"].is_number_unsigned()
                                  ? j["parent"].get<EntityId>() : 0;
                if (parent != 0 && loaded.find(parent) == nullptr) {
                    warnings.push_back(where + " (" + name + "): its parent " + std::to_string(parent) +
                                       " isn't listed before it; put at the top");
                    parent = 0;
                }
                Entity& entity = loaded.createWithId(id, name, parent);
                readOptional(j, "position", entity.local.position);
                readOptional(j, "rotation", entity.local.rotation);
                readOptional(j, "scale", entity.local.scale);
                readOptional(j, "visible", entity.visible);
                readOptional(j, "uniformScale", entity.uniformScale);
                highest = std::max(highest, id);

                if (auto m = j.find("model"); m != j.end()) {
                    // In the table from the start: if reading fails further on, releaseAll releases its model
                    ModelComponent& component = loaded.modelTable[id];
                    readOptional(*m, "originalSize", component.originalSize);
                    try {
                        component.model = loadModel(assets, *m);
                    } catch (const std::exception& e) {
                        component.missingFile = referenceName(*m);
                        component.missingReason = e.what();
                        component.missingEntry = m->dump();
                        warnings.push_back(where + " (" + name + "): model " + component.missingFile + " not loaded: " +
                                           e.what());
                    }
                    if (assets.isLoaded(component.model)) {
                        const SceneModel& model = assets.model(component.model);
                        for (const MeshInstance& part : model.parts) {
                            component.hiddenParts.push_back(part.hiddenByDefault);
                        }
                        std::vector<std::string> partWarnings;
                        readParts(model, component, *m, partWarnings);
                        for (const std::string& warning : partWarnings) {
                            warnings.push_back(where + " (" + name + "): " + warning);
                        }
                    } else if (component.model.valid()) {
                        component.loading = true;  // its parts when it arrives (update)
                        component.loadingEntry = m->dump();
                    }
                }

                if (auto p = j.find("portal"); p != j.end()) {
                    PortalComponent portal;
                    portal.other = p->contains("other") && (*p)["other"].is_number_unsigned()
                                   ? (*p)["other"].get<EntityId>() : 0;
                    readOptional(*p, "endA", portal.endA);
                    if (auto shape = p->find("shape"); shape != p->end()) {
                        portal.shape = static_cast<PortalShape>(nameIndex(*shape, shapeNames));
                    }
                    readOptional(*p, "size", portal.size);
                    readOptional(*p, "rim", portal.rim);
                    readOptional(*p, "rimWidth", portal.rimWidth);
                    readOptional(*p, "rimDepth", portal.rimDepth);
                    loaded.portalTable[id] = portal;
                }
            } catch (const std::exception& e) {
                throw std::runtime_error(where + ": " + e.what());
            }
        }

        // Portal ends must name each other, one of them end A: else they are plain entities
        std::vector<EntityId> broken;
        for (const auto& [id, portal] : loaded.portalTable) {
            const PortalComponent* other = loaded.portal(portal.other);
            if (other == nullptr || other->other != id || other->endA == portal.endA) {
                broken.push_back(id);
            }
        }
        for (EntityId id : broken) {
            warnings.push_back("entity " + std::to_string(id) + ": its portal's other end doesn't match; not a portal");
            loaded.portalTable.erase(id);
        }
        for (const auto& [id, portal] : loaded.portalTable) {
            if (portal.endA) {
                loaded.rimModel = assets.requestModel(portalRimModelName());  // built in: made at once
                assets.acquire(loaded.rimModel);  // one reference per pair
            }
        }

        EntityId next = 0;
        if (document.contains("nextEntityId") && document["nextEntityId"].is_number_unsigned()) {
            next = document["nextEntityId"].get<EntityId>();
        }
        loaded.entitiesMade = std::max(highest, next > 0 ? next - 1 : 0);
        readOptional(document, "portalPairsMade", loaded.portalPairsMade);

        if (auto c = document.find("camera"); c != document.end()) {
            try {
                readOptional(*c, "position", loaded.sceneCamera.position);
                readOptional(*c, "yaw", loaded.sceneCamera.yaw);
                readOptional(*c, "pitch", loaded.sceneCamera.pitch);
                readOptional(*c, "fov", loaded.sceneCamera.fov);
            } catch (const std::exception& e) {
                throw std::runtime_error(std::string("camera: ") + e.what());
            }
        }

        if (auto en = document.find("environment"); en != document.end()) {
            SceneEnvironment& e = loaded.sceneEnvironment;
            try {
                readOptional(*en, "sunEnabled", e.sunEnabled);
                readOptional(*en, "sunAzimuth", e.sunAzimuth);
                readOptional(*en, "sunElevation", e.sunElevation);
                readOptional(*en, "sunAngularRadius", e.sunAngularRadius);
                readOptional(*en, "sunColor", e.sunColor);
                readOptional(*en, "sunIntensity", e.sunIntensity);
                if (auto background = en->find("background"); background != en->end()) {
                    e.background = nameIndex(*background, backgroundNames);
                }
                readOptional(*en, "skyZenithColor", e.skyZenithColor);
                readOptional(*en, "skyHorizonColor", e.skyHorizonColor);
                readOptional(*en, "groundColor", e.groundColor);
                readOptional(*en, "skyIntensity", e.skyIntensity);
                readOptional(*en, "mapRotation", e.mapRotation);
                readOptional(*en, "mapIntensity", e.mapIntensity);
            } catch (const std::exception& error) {
                throw std::runtime_error(std::string("environment: ") + error.what());
            }
            if (auto map = en->find("map"); map != en->end() && map->is_string()) {
                loaded.setEnvironmentMap(map->get<std::string>());  // (a failure comes later, as a message)
            }
        }
    } catch (...) {
        loaded.releaseAll();
        throw;
    }

    // Read: this scene becomes the file's. Models both scenes show stay loaded (acquired again above first).
    releaseAll();
    takeFrom(loaded);

    // Model edits, on top of the files' materials and lights. An edit that no longer fits the model (the file
    // changed) is skipped. Models still loading get theirs when they arrive.
    assets.resetEdits();
    if (auto list = document.find("models"); list != document.end() && list->is_array()) {
        for (const Json& entry : *list) {
            std::string name = referenceName(entry);
            ModelHandle handle;
            for (const auto& [id, component] : modelTable) {
                if (component.model.valid() && assets.modelName(component.model) == name) {
                    handle = component.model;
                    break;
                }
            }
            if (!handle.valid()) {
                continue;  // no entity shows it (or it wasn't loaded: warned about above)
            }
            if (assets.isLoaded(handle)) {
                applyEdits(assets, assets.model(handle), entry, warnings);
            } else {
                pendingModelEdits.emplace_back(name, entry.dump());
            }
        }
    }
    assets.materialsChanged();
    return warnings;
}
