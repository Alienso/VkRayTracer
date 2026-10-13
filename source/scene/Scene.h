//
// The scene: objects placing loaded models in the world, the camera, and the environment (sun, sky). No Vulkan: it
// describes each frame in a RenderList, which the renderer turns into TLAS instances (SceneGpu.cpp); the editor UI
// changes it (EditorScene.cpp).
//
// An object holds a reference to its model in the AssetStore (acquired when the object is made, released when it is
// removed); a model no object shows is kept for a while, then unloaded (see AssetStore.h).
//

#ifndef VULKANDEMO_SCENE_H
#define VULKANDEMO_SCENE_H

#include <glm/glm.hpp>
#include <string>
#include <vector>
#include "assets/AssetStore.h"
#include "scene/Camera.h"
#include "core/Handles.h"
#include "scene/Portal.h"
#include "scene/RenderList.h"
#include "scene/SceneEnvironment.h"

// Something placed in the world: a model plus a transform edited in ImGui.
struct SceneObject {
    std::string name;
    ModelHandle model;
    bool visible = true;
    glm::vec3 position = glm::vec3(0.0f);
    glm::vec3 rotation = glm::vec3(0.0f);  // degrees around X, Y, Z; applied in the order Y (yaw), X (pitch), Z (roll)
    glm::vec3 scale = glm::vec3(1.0f);
    bool uniformScale = true;  // UI only: edit all three scale axes together
    bool originalSize = false;  // skip the model's fit: keep the file's own size and position (glTF: meters)
    std::vector<bool> hiddenParts;  // per part of the model; missing entries count as visible

    [[nodiscard]] glm::mat4 transform() const;           // translate * rotate * scale
    // Unique in the scene, never reused (set by the Scene): the render list finds the object's last frame by it
    uint64_t id = 0;
};

class Scene {
public:
    static constexpr float objectSpacing = 2.4f;  // new objects are lined up along X this far apart

    explicit Scene(AssetStore& assets);

    // The starting scene: the built-in floor, the camera looking at where the first object goes
    void createDefault();

    // A new object showing that model file (loaded if it isn't yet). Fitted to the standard size and lined up next
    // to the others, or (originalSize) in the file's own size and place, hiding the floor. Returns its index;
    // throws if the model can't be loaded (the scene is then unchanged).
    int addObject(const std::string& fileName, bool originalSize);
    // A copy next to it (same model: nothing loaded). Returns the copy's index.
    int duplicateObject(int index);
    // Releases its model (AssetStore::release): after the last object of a model it stays loaded, unused.
    void removeObject(int index);

    // A new portal pair in front of the camera: its two ends side by side, facing the camera. Returns its index.
    // Each pair holds a reference to the rim model (AssetStore::acquire), so after the last one it is unused.
    int addPortalPair();
    void removePortalPair(int index);
    [[nodiscard]] ModelHandle portalRimModel() const { return rimModel; }  // valid while there are pairs
    std::vector<PortalPair>& portals() { return portalList; }
    [[nodiscard]] const std::vector<PortalPair>& portals() const { return portalList; }

    // This frame's instances, lights, portals, camera and environment, into list. Call once per frame, with the same
    // list: it holds last frame's objects (motion vectors). The instances are only remade when an object changed or
    // something moved last frame (see RenderList.h); the lights every time.
    void buildRenderList(RenderList& list);

    std::vector<SceneObject>& objects() { return objectList; }
    [[nodiscard]] const std::vector<SceneObject>& objects() const { return objectList; }

    Camera& camera() { return sceneCamera; }
    [[nodiscard]] const Camera& camera() const { return sceneCamera; }

    SceneEnvironment& environment() { return sceneEnvironment; }
    [[nodiscard]] const SceneEnvironment& environment() const { return sceneEnvironment; }
    // The background map: loads that file if it isn't yet (throws if it can't be: nothing changes then) and releases
    // the previous map
    void setEnvironmentMap(const std::string& fileName);

private:
    AssetStore& assets;
    std::vector<SceneObject> objectList;
    std::vector<PortalPair> portalList;
    int portalPairsMade = 0;  // for their names
    uint64_t objectsMade = 0;  // for their ids
    ModelHandle rimModel;     // the built-in portal rim, while there are pairs

    // The bars of the enabled pairs' rims: (mesh, world matrix) for each
    std::vector<std::pair<uint32_t, glm::mat4>> buildPortalRims() const;
    Camera sceneCamera;
    SceneEnvironment sceneEnvironment;
};

#endif //VULKANDEMO_SCENE_H
