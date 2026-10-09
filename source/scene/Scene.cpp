//
// The scene's objects and portals: adding, duplicating and removing them. See Scene.h.
//

#include "scene/Scene.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <glm/gtc/matrix_transform.hpp>
#include "assets/BuiltInModels.h"
#include "core/Configuration.h"

// Read right to left: rotate, then move into place. Rotation in degrees: yaw (Y), then pitch (X), then roll (Z).
static glm::mat4 placement(const glm::vec3& position, const glm::vec3& rotation) {
    glm::mat4 m = glm::translate(glm::mat4(1.0f), position);
    m = glm::rotate(m, glm::radians(rotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
    m = glm::rotate(m, glm::radians(rotation.x), glm::vec3(1.0f, 0.0f, 0.0f));
    return glm::rotate(m, glm::radians(rotation.z), glm::vec3(0.0f, 0.0f, 1.0f));
}

glm::mat4 SceneObject::transform() const {
    return glm::scale(placement(position, rotation), scale);  // scaled first
}

glm::mat4 PortalEnd::transform() const {
    return placement(position, rotation);
}

Scene::Scene(AssetStore& assets) : assets(assets) {}

void Scene::createDefault() {
    sceneCamera.position = {0.0f, 1.6f, 4.0f};
    sceneCamera.fov = Configuration::fov;
    sceneCamera.lookAt({0.0f, 0.9f, 0.0f});

    SceneObject floorObject;
    floorObject.name = "Floor";
    floorObject.model = assets.addFloor();
    assets.acquire(floorObject.model);
    objectList.push_back(floorObject);
}

int Scene::addObject(const std::string& fileName, bool originalSize) {
    SceneObject object;
    object.model = assets.findOrLoad(fileName);
    object.name = std::filesystem::path(fileName).stem().string();
    object.originalSize = originalSize && !isBuiltInModel(fileName);

    const SceneModel& model = assets.model(object.model);
    for (const MeshInstance& part : model.parts) {
        object.hiddenParts.push_back(part.hiddenByDefault);
    }

    // Line objects up: 0, +1, -1, +2, -2, ... spacings from the center, so they don't overlap.
    // Built-in rooms and original-size scenes go to the center instead, around whatever already stands there.
    if (object.originalSize) {
        // A whole scene brings its own ground; the built-in floor would cut through it.
        for (SceneObject& other : objectList) {
            if (other.model == assets.floor()) {
                other.visible = false;
            }
        }
    } else if (!isBuiltInModel(fileName)) {
        int existing = static_cast<int>(std::count_if(objectList.begin(), objectList.end(), [&](const SceneObject& o) {
            return o.model != assets.floor() && !isBuiltInModel(o.name);
        }));
        int slot = (existing + 1) / 2 * (existing % 2 == 1 ? 1 : -1);
        object.position.x = slot * objectSpacing;
    }

    assets.acquire(object.model);
    objectList.push_back(object);
    return static_cast<int>(objectList.size() - 1);
}

int Scene::duplicateObject(int index) {
    SceneObject copy = objectList[index];  // same model: no loading, just one more set of TLAS instances
    copy.position.x += objectSpacing;
    assets.acquire(copy.model);
    objectList.push_back(copy);
    return static_cast<int>(objectList.size() - 1);
}

void Scene::removeObject(int index) {
    ModelHandle model = objectList[index].model;
    objectList.erase(objectList.begin() + index);
    assets.release(model);  // the last object showing it: the model is unloaded
}

int Scene::addPortalPair() {
    // 3 units ahead of the camera (level), the ends 1.5 to either side, turned to face it
    glm::vec3 forward = sceneCamera.forward();
    forward.y = 0.0f;
    forward = glm::length(forward) > 1e-4f ? glm::normalize(forward) : glm::vec3(0.0f, 0.0f, -1.0f);
    glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
    glm::vec3 center = sceneCamera.position + forward * 3.0f;
    float yaw = glm::degrees(std::atan2(-forward.x, -forward.z)) + 0.0f;  // +Z towards the camera (+ 0: no -0)

    PortalPair pair;
    pair.name = "Portal " + std::to_string(++portalPairsMade);
    pair.ends[0].position = center - right * 1.5f;
    pair.ends[1].position = center + right * 1.5f;
    pair.ends[0].rotation = pair.ends[1].rotation = glm::vec3(0.0f, yaw, 0.0f);
    portalList.push_back(pair);
    return static_cast<int>(portalList.size()) - 1;
}

void Scene::removePortalPair(int index) {
    portalList.erase(portalList.begin() + index);
}

void Scene::buildRenderList(RenderList& list) {
    std::vector<RenderList::BuiltObject> current;
    current.reserve(objectList.size());
    for (const SceneObject& object : objectList) {
        current.push_back({object.model, object.visible, object.originalSize, object.transform(), object.hiddenParts});
    }
    list.camera = sceneCamera;
    list.environment = sceneEnvironment;

    bool rebuild = list.hasMotion || !(current == list.builtObjects);
    list.hasMotion = false;
    if (rebuild) {
        list.instances.clear();
        list.motions.clear();
        list.revision++;
    }
    list.lights.clear();
    list.portals.clear();
    for (const PortalPair& pair : portalList) {
        if (pair.enabled) {
            for (const PortalEnd& end : pair.ends) {
                list.portals.push_back({end.transform(), pair.size * 0.5f, pair.shape});
            }
        }
    }

    for (size_t o = 0; o < objectList.size(); o++) {
        SceneObject& object = objectList[o];
        const glm::mat4& objectTransform = current[o].transform;
        // Motion vectors need where each point was last frame:
        // last frame's position = previous object transform * inverse(current) * this frame's position.
        // Exactly identity when it didn't move (the product can be off by rounding, which would count as motion).
        glm::mat4 motion = object.hasPreviousTransform && object.previousTransform != objectTransform
                                   ? object.previousTransform * glm::inverse(objectTransform)
                                   : glm::mat4(1.0f);
        object.previousTransform = objectTransform;
        object.hasPreviousTransform = true;
        if (!object.visible) {
            continue;
        }

        const SceneModel& model = assets.model(object.model);
        glm::mat4 placed = objectTransform * (object.originalSize ? glm::mat4(1.0f) : model.fit);
        if (rebuild) {
            if (motion != glm::mat4(1.0f)) {
                list.hasMotion = true;
            }
            uint32_t motionIndex = static_cast<uint32_t>(list.motions.size());
            list.motions.push_back(motion);
            for (size_t p = 0; p < model.parts.size(); p++) {
                if (p < object.hiddenParts.size() && object.hiddenParts[p]) {
                    continue;
                }
                const MeshInstance& part = model.parts[p];
                list.instances.push_back({part.meshIndex, motionIndex, placed * part.transform});
            }
        }

        // The file's distances (falloff, range) are scaled with the object. The file's own node scales don't
        // count (glTF: they don't affect lights).
        float scale = std::cbrt(std::abs(glm::determinant(glm::mat3(placed))));
        for (const PunctualLight& light : model.lights) {
            if (light.enabled) {
                list.lights.push_back({light.type, light.color, light.intensity, light.range, light.innerConeAngle,
                                       light.outerConeAngle, placed * light.transform, scale});
            }
        }
    }
    if (rebuild) {
        list.builtObjects = std::move(current);
    }
}

void Scene::setEnvironmentMap(const std::string& fileName) {
    EnvironmentHandle map = assets.findOrLoadEnvironment(fileName);
    if (map == sceneEnvironment.map) {
        return;
    }
    assets.acquire(map);
    assets.release(sceneEnvironment.map);  // the last user of the old map: it is unloaded
    sceneEnvironment.map = map;
}
