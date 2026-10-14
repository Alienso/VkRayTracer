//
// The scene: entities placed in the world, the camera, and the environment (sun, sky). No Vulkan: it describes each
// frame in a RenderList, which the renderer turns into TLAS instances (SceneGpu.cpp); the editor UI changes it
// (EditorScene.cpp).
//
// Entities form a tree: each has a parent (or none: a root) and a transform relative to it, so moving a parent moves
// everything under it. What an entity is comes from its components, kept in one table per kind, by entity id:
// a model to show (ModelComponent), a portal end (PortalComponent, Portal.h). An entity without components is a
// group, a point to hang others on.
//
// A model entity holds a reference to its model in the AssetStore (acquired when the component is made, released
// when the entity is removed); a model no entity shows is kept for a while, then unloaded (see AssetStore.h).
//

#ifndef VULKANDEMO_SCENE_H
#define VULKANDEMO_SCENE_H

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <unordered_map>
#include <vector>
#include "assets/AssetStore.h"
#include "scene/Camera.h"
#include "core/Handles.h"
#include "scene/Portal.h"
#include "scene/RenderList.h"
#include "scene/SceneEnvironment.h"

// Unique in the scene and never reused: what the scene's tree, the components and the render list (motion vectors)
// know an entity by. 0 = none.
using EntityId = uint64_t;

// Placement relative to the parent: scaled, then rotated, then moved
struct Transform {
    glm::vec3 position = glm::vec3(0.0f);
    glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    glm::vec3 scale = glm::vec3(1.0f);

    [[nodiscard]] glm::mat4 matrix() const;  // translate * rotate * scale
};

// Rotation as degrees around X, Y, Z (pitch, yaw, roll), applied as yaw, then pitch, then roll: how the editor shows
// a rotation. eulerToQuat(quatToEuler(q)) gives q back (or its equal); the angles can come back different from the
// ones put in (e.g. pitch beyond 90 degrees), the rotation is the same.
glm::quat eulerToQuat(const glm::vec3& degrees);
glm::vec3 quatToEuler(const glm::quat& rotation);

struct Entity {
    EntityId id = 0;
    std::string name;
    EntityId parent = 0;              // 0: a root
    std::vector<EntityId> children;   // in the order the editor lists them
    Transform local;                  // relative to the parent
    bool visible = true;              // hidden hides the children too
    bool uniformScale = true;         // UI only: edit all three scale axes together
};

// A model shown at the entity
struct ModelComponent {
    ModelHandle model;              // invalid when the model couldn't be loaded (missingFile)
    bool originalSize = false;      // skip the model's fit: keep the file's own size and position (glTF: meters)
    std::vector<bool> hiddenParts;  // per part of the model; missing entries count as visible
    // A scene file named a model that couldn't be loaded: its name, why, and the file's entry, saved back as it was
    // (so saving doesn't lose it). Nothing is shown for it.
    std::string missingFile;
    std::string missingReason;
    std::string missingEntry;
};

class Scene {
public:
    static constexpr float objectSpacing = 2.4f;  // new objects are lined up along X this far apart

    explicit Scene(AssetStore& assets);

    // The starting scene: the built-in floor, the camera looking at where the first object goes
    void createDefault();
    // A new scene: everything removed (models released), the default camera and environment, the floor; edits to
    // the models' materials and lights undone (AssetStore::resetEdits)
    void clear();

    // Scene files (SceneFile.cpp, the format is described there). save writes through a temporary file, so a failed
    // write leaves the old file; throws on errors.
    void save(const std::string& path) const;
    // Replaces the scene with the file's. Throws when the file can't be read or isn't a scene file (the scene is then
    // unchanged); returns what was skipped: models or maps that couldn't be loaded (their entities are kept, see
    // ModelComponent::missingFile), edits that don't fit the model any more.
    std::vector<std::string> load(const std::string& path);
    // The scene as its file would have it, without the camera (moving around isn't a change to save): when this
    // differs from the text at the last save or load, there is something to save
    [[nodiscard]] std::string contentText() const;

    // A new root entity showing that model file (loaded if it isn't yet). Fitted to the standard size and lined up
    // next to the others, or (originalSize) in the file's own size and place, hiding the floor. Throws if the model
    // can't be loaded (the scene is then unchanged).
    EntityId addObject(const std::string& fileName, bool originalSize);
    // A new entity without components (a group), under parent (0: a root)
    EntityId addEmpty(const std::string& name, EntityId parent);
    // A new portal pair in front of the camera: a group with the two ends side by side, facing the camera. Each pair
    // holds a reference to the rim model (AssetStore::acquire), so after the last one it is unused. Returns the group.
    EntityId addPortalPair();
    // A copy of the entity and everything under it, next to it (same parent), its copies' models acquired again.
    // A portal end copied without its other end is copied as a plain entity. Returns the copy.
    EntityId duplicate(EntityId id);
    // The entity and everything under it. Removing a portal end removes the other end (and what is under it) too.
    void remove(EntityId id);
    // Moves the entity under parent (0: to the roots), last among its children. keepWorld: its local transform is
    // changed so it stays where it is (as far as a transform can: a rotated non-uniform scale above it can't be
    // matched exactly); else it keeps its local transform and moves with the new parent. False (nothing changed)
    // when parent is the entity itself or under it.
    bool setParent(EntityId id, EntityId parent, bool keepWorld = true);

    // nullptr when there is no such entity. Pointers are valid until entities are added or removed.
    Entity* find(EntityId id);
    [[nodiscard]] const Entity* find(EntityId id) const;
    [[nodiscard]] const std::vector<Entity>& entities() const { return entityList; }
    [[nodiscard]] const std::vector<EntityId>& roots() const { return rootList; }
    // Whether id is `ancestor` or somewhere under it
    [[nodiscard]] bool isUnder(EntityId id, EntityId ancestor) const;
    // Where the entity is in the world: its parents' transforms times its own
    [[nodiscard]] glm::mat4 worldTransform(EntityId id) const;
    // Visible, and so are all its parents
    [[nodiscard]] bool visibleInWorld(EntityId id) const;

    // The entity's components, or nullptr. Pointers stay valid while the component exists.
    ModelComponent* model(EntityId id);
    [[nodiscard]] const ModelComponent* model(EntityId id) const;
    PortalComponent* portal(EntityId id);
    [[nodiscard]] const PortalComponent* portal(EntityId id) const;
    // After editing a portal end's shape, size or rim: the same for its other end
    void syncPortal(EntityId end);
    [[nodiscard]] ModelHandle portalRimModel() const { return rimModel; }  // valid while there are pairs

    // This frame's instances, lights, portals, camera and environment, into list. Call once per frame, with the same
    // list: it holds last frame's objects (motion vectors). The instances are only remade when an object changed or
    // something moved last frame (see RenderList.h); the lights every time.
    void buildRenderList(RenderList& list);

    Camera& camera() { return sceneCamera; }
    [[nodiscard]] const Camera& camera() const { return sceneCamera; }

    SceneEnvironment& environment() { return sceneEnvironment; }
    [[nodiscard]] const SceneEnvironment& environment() const { return sceneEnvironment; }
    // The background map: loads that file if it isn't yet (throws if it can't be: nothing changes then) and releases
    // the previous map
    void setEnvironmentMap(const std::string& fileName);

private:
    AssetStore& assets;
    std::vector<Entity> entityList;
    std::unordered_map<EntityId, size_t> entityIndex;  // id -> position in entityList
    std::vector<EntityId> rootList;                    // in the order the editor lists them
    std::unordered_map<EntityId, ModelComponent> modelTable;
    std::unordered_map<EntityId, PortalComponent> portalTable;
    EntityId entitiesMade = 0;  // for their ids
    int portalPairsMade = 0;    // for their names
    ModelHandle rimModel;       // the built-in portal rim, while there are pairs
    Camera sceneCamera;
    SceneEnvironment sceneEnvironment;

    Entity& create(const std::string& name, EntityId parent);
    // With an id from a file (entitiesMade is set after loading)
    Entity& createWithId(EntityId id, const std::string& name, EntityId parent);
    // Releases every model, rim and map reference the scene holds (before it is replaced or cleared)
    void releaseAll();
    // This scene becomes other (same AssetStore), which is left empty
    void takeFrom(Scene& other);
    // The file text: with or without the camera (SceneFile.cpp)
    [[nodiscard]] std::string toText(bool withCamera) const;
    // Every entity in tree order: roots in order, each followed by its children's subtrees
    [[nodiscard]] std::vector<EntityId> treeOrder() const;
    // The children list the entity sits in: its parent's, or the roots
    std::vector<EntityId>& siblings(EntityId parent);
    // The entity's subtree, the entity first, parents before their children
    [[nodiscard]] std::vector<EntityId> subtree(EntityId id) const;
    void reindex();

    // Per entity in tree order (roots in order, each followed by its children's subtrees): its world transform and
    // whether it is visible in the world
    struct Placed {
        const Entity* entity;
        glm::mat4 world;
        bool visible;
    };
    [[nodiscard]] std::vector<Placed> placeAll() const;
    // An enabled portal pair: its ends' world placements (A, B; without scale) and the pair's shape, size and rim
    struct PlacedPair {
        glm::mat4 ends[2];
        const PortalComponent* portal;
    };
    // The bars of the pairs' rims: (mesh, world matrix) for each
    [[nodiscard]] std::vector<std::pair<uint32_t, glm::mat4>> buildPortalRims(
            const std::vector<PlacedPair>& pairs) const;
};

#endif //VULKANDEMO_SCENE_H
