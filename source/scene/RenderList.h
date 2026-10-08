//
// Render list: what one frame draws, made by the Scene (Scene::buildRenderList) and read by the renderer. No
// Vulkan: mesh indices into AssetStore::meshes(), world matrices, motion, lights, the camera and the environment.
// The renderer turns the instances into TLAS instances one to one (instance i = TLAS instance i), and only when the
// list's revision moved on.
//
// Big scenes (vegetation) have millions of instances, too many to remake every frame: the Scene remakes them only
// when an object changed, or something moved last time (its motion is then only right for that one frame).
// Punctual lights are few and remade every frame.
//

#ifndef VULKANDEMO_RENDERLIST_H
#define VULKANDEMO_RENDERLIST_H

#include <glm/glm.hpp>
#include <cstdint>
#include <vector>
#include "scene/Camera.h"
#include "core/Handles.h"
#include "assets/ModelLoader.h"
#include "scene/SceneEnvironment.h"

// One mesh placed in the world: a visible part of a visible object
struct RenderInstance {
    uint32_t mesh;       // index into AssetStore::meshes()
    uint32_t motion;     // index into RenderList::motions (the object's)
    glm::mat4 world;     // object * fit * part
};

// A punctual light of a visible object, in world space but still in the file's units (the renderer converts it)
struct RenderLight {
    LightType type;
    glm::vec3 color;
    float intensity;
    float range;
    float innerConeAngle;
    float outerConeAngle;
    glm::mat4 world;  // object * fit * the light's node
    float scale;      // the object's (and fit's) scale: distances in the file grow by this much
};

class RenderList {
public:
    std::vector<RenderInstance> instances;
    // Per visible object: this frame's world position -> last frame's (identity when it didn't move). Shared by all
    // its instances: a part's world matrix is object * fit * part, and only the object's transform changes.
    std::vector<glm::mat4> motions;
    std::vector<RenderLight> lights;
    Camera camera;                 // copied every frame
    SceneEnvironment environment;  // copied every frame (its map: AssetStore::environmentMap)

    // Goes up whenever instances or motions were remade. The renderer compares it with what it built from.
    uint64_t instanceRevision() const { return revision; }

private:
    friend class Scene;
    uint64_t revision = 0;
    bool hasMotion = false;  // some motion isn't identity: remake next frame, when it is out of date

    // What the instances were made from: compared every frame, so only actual changes remake them
    struct BuiltObject {
        ModelHandle model;
        bool visible;
        bool originalSize;
        glm::mat4 transform;
        std::vector<bool> hiddenParts;
        bool operator==(const BuiltObject& o) const {
            return model == o.model && visible == o.visible && originalSize == o.originalSize &&
                   transform == o.transform && hiddenParts == o.hiddenParts;
        }
    };
    std::vector<BuiltObject> builtObjects;
};

#endif //VULKANDEMO_RENDERLIST_H
