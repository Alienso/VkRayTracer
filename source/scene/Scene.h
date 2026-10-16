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
// Model files load in the background: the entity is there at once, its model shows up when it arrives (update).
//
// Render settings: the scene's fields of the program's RenderSettings (SettingsScope::Scene, RenderSettingsFile.h:
// exposure, bounces, ReSTIR, ...) are this scene's. A scene file holds the ones that differ from the defaults; opening
// one sets them (the others to their defaults), a new scene resets them. The program's fields (render mode, ...) it
// never touches. These are the scene's own values: what settings volumes change on top while the camera is inside
// them (later) is the renderer's per frame, not saved.
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
#include "renderer/RenderSettings.h"

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
    // These two can be changed directly (the editor, game logic): the render list finds the change in the next frame
    Transform local;                  // relative to the parent
    bool visible = true;              // hidden hides the children too
    bool uniformScale = true;         // UI only: edit all three scale axes together
};

// A model shown at the entity
struct ModelComponent {
    ModelHandle model;              // invalid when the model couldn't be loaded (missingFile)
    bool originalSize = false;      // skip the model's fit: keep the file's own size and position (glTF: meters)
    // Its model is still loading (AssetStore::isLoading): nothing is shown yet. loadingEntry is how a scene file
    // names it (hidden parts included), applied when it arrives and saved as it is meanwhile.
    bool loading = false;
    std::string loadingEntry;
    // Per part of the model; missing entries count as visible. Changed through Scene::setPartHidden (models have up
    // to millions of parts: the render list doesn't compare them every frame).
    std::vector<bool> hiddenParts;
    // A scene file named a model that couldn't be loaded: its name, why, and the file's entry, saved back as it was
    // (so saving doesn't lose it). Nothing is shown for it.
    std::string missingFile;
    std::string missingReason;
    std::string missingEntry;
};

// The game's data on an entity (GAME_PLAN.md): the scene stores, copies and saves it, the game gives it meaning when
// Play starts. An entity has a list of them, in the order the editor shows them.
struct GameComponent {
    std::string type;        // what the game makes of it ("spin", "door", ...; "": nothing yet)
    std::string properties;  // a JSON object as text, compact ("" is no properties): what that type reads

    bool operator==(const GameComponent& other) const {
        return type == other.type && properties == other.properties;
    }
};

// What happened while models and maps loaded (Scene::takeMessages): for the editor's log
struct SceneMessage {
    enum class Level { Info, Warning, Error } level;
    std::string text;
};

class Scene {
public:
    static constexpr float objectSpacing = 2.4f;  // new objects are lined up along X this far apart

    Scene(AssetStore& assets, RenderSettings& settings);

    // The starting scene: the built-in floor, the camera looking at where the first object goes
    void createDefault();
    // A new scene: everything removed (models released), the default camera, environment and scene settings, the
    // floor; edits to the models' materials and lights undone (AssetStore::resetEdits)
    void clear();

    // Scene files (SceneFile.cpp, the format is described there). save writes through a temporary file, so a failed
    // write leaves the old file; throws on errors.
    void save(const std::string& path) const;
    // Replaces the scene with the file's. Throws when the file can't be read or isn't a scene file (the scene is then
    // unchanged); returns what was skipped: built-in models that don't exist (their entities are kept, see
    // ModelComponent::missingFile), edits that don't fit the model any more. Models and maps not loaded yet arrive
    // later (update), with their hidden parts and edits; what goes wrong then comes as messages.
    std::vector<std::string> load(const std::string& path);
    // The same with the file's text (name: for the errors). With text(), the scene as its file would have it, camera
    // included: Play keeps it and Stop loads it back (GAME_PLAN.md).
    std::vector<std::string> loadText(const std::string& text, const std::string& name);
    [[nodiscard]] std::string text() const;
    // The scene as its file would have it, without the camera (moving around isn't a change to save): when this
    // differs from the text at the last save or load, there is something to save
    [[nodiscard]] std::string contentText() const;

    // A new root entity showing that model file (loading it in the background if it isn't loaded yet). Fitted to the
    // standard size and lined up next to the others, or (originalSize) in the file's own size and place, hiding the
    // floor (built-in models: at the middle, their own size). A file that can't be loaded leaves the entity without
    // its model (missingFile), reported by update.
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
    // matched exactly; under a parent scaled to 0, or scaled to 0 itself, not at all: it keeps its local transform);
    // else it keeps its local transform and moves with the new parent. False (nothing changed)
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
    // The entity's game components, or nullptr when it has none. Changed only through setGame (a copy edited and set
    // back): the pointer is valid until then, or until entities are removed.
    [[nodiscard]] const std::vector<GameComponent>* game(EntityId id) const;
    // Replaces the entity's game components; an empty list removes them
    void setGame(EntityId id, std::vector<GameComponent> components);
    // Shows or hides one part of a model entity
    void setPartHidden(EntityId id, size_t part, bool hidden);
    // After editing a portal end's shape, size or rim: the same for its other end
    void syncPortal(EntityId end);
    [[nodiscard]] ModelHandle portalRimModel() const { return rimModel; }  // valid while there are pairs

    // Once per frame, after AssetStore::update: models that arrived are shown (with the scene file's hidden parts and
    // edits), the ones that failed become missing (ModelComponent::missingFile); the environment map the same way
    void update();
    // What update (and loading) had to say since the last call
    std::vector<SceneMessage> takeMessages() { return std::move(messages); }
    // Some model or map of this scene is still loading
    [[nodiscard]] bool loading() const;

    // This frame's instances, lights, portals, camera and environment, into list. Call once per frame, with the same
    // list: it holds last frame's entities (motion vectors, what changed). Only what changed is redone (see
    // RenderList.h); the lights and portals every time.
    void buildRenderList(RenderList& list);
    // A check for the above (slow: Debug panel): after each build that only rewrote what changed, the whole list is
    // also made from scratch and compared. A difference (a change the build missed) is counted, and printed.
    bool checkUpdates = false;
    [[nodiscard]] uint64_t updateCheckFailures() const { return checkFailures; }

    Camera& camera() { return sceneCamera; }
    [[nodiscard]] const Camera& camera() const { return sceneCamera; }
    // After moving the camera from `from` to where it is now: if that line goes through a portal's opening, the
    // camera comes out of the other end (also more than once), seeing what it saw through the opening. Rays start
    // at the camera's position, so it is on one side or the other, never half in. Its roll is lost (the camera
    // has none): a pair whose ends lean differently turns the view upright as it comes out.
    void moveCameraThroughPortals(const glm::vec3& from);

    SceneEnvironment& environment() { return sceneEnvironment; }
    [[nodiscard]] const SceneEnvironment& environment() const { return sceneEnvironment; }
    // The background map. One not loaded yet loads in the background: the previous map stays until it arrives, and
    // stays when it can't be loaded (environmentMapError says why).
    void setEnvironmentMap(const std::string& fileName);
    [[nodiscard]] EnvironmentHandle pendingEnvironmentMap() const { return pendingMap; }  // the one loading
    [[nodiscard]] const std::string& environmentMapError() const { return mapError; }

private:
    AssetStore& assets;
    RenderSettings& settings;  // the program's; the scene's fields of it are this scene's (see above)
    std::vector<Entity> entityList;
    std::unordered_map<EntityId, size_t> entityIndex;  // id -> position in entityList
    std::vector<EntityId> rootList;                    // in the order the editor lists them
    std::unordered_map<EntityId, ModelComponent> modelTable;
    std::unordered_map<EntityId, PortalComponent> portalTable;
    std::unordered_map<EntityId, std::vector<GameComponent>> gameTable;  // never an empty list
    EntityId entitiesMade = 0;  // for their ids
    int portalPairsMade = 0;    // for their names
    ModelHandle rimModel;       // the built-in portal rim, while there are pairs
    Camera sceneCamera;
    glm::mat4 cameraTeleport = glm::mat4(1.0f);  // since the last render list (RenderList::cameraTeleport)
    SceneEnvironment sceneEnvironment;
    // Goes up when the render list's layout changes in ways it doesn't find by comparing entities: entities added,
    // removed or re-parented (the tree order), parts shown or hidden
    uint64_t layoutRevision = 1;
    uint64_t checkFailures = 0;
    EnvironmentHandle pendingMap;  // asked for and acquired, still loading (then it becomes sceneEnvironment.map)
    std::string mapError;
    // A scene file's edits ("models" entries) for models that were still loading: (model name, the entry's text),
    // applied when the model arrives, saved as they are meanwhile
    std::vector<std::pair<std::string, std::string>> pendingModelEdits;
    std::vector<SceneMessage> messages;

    Entity& create(const std::string& name, EntityId parent);
    // With an id from a file (entitiesMade is set after loading)
    Entity& createWithId(EntityId id, const std::string& name, EntityId parent);
    // Releases every model, rim and map reference the scene holds (before it is replaced or cleared)
    void releaseAll();
    // This scene becomes other (same AssetStore), which is left empty
    void takeFrom(Scene& other);
    // The file text: with or without the camera (SceneFile.cpp)
    [[nodiscard]] std::string toText(bool withCamera) const;
    // A model that arrived: its hidden parts from the component's loadingEntry, its edits from pendingModelEdits
    // (SceneFile.cpp). Return what didn't fit.
    std::vector<std::string> applyLoadingEntry(ModelComponent& component);
    std::vector<std::string> applyPendingEdits(const std::string& modelName);
    // How a scene file names a model ({"file": ..., "originalSize": ...}), as text
    static std::string modelEntryText(const std::string& fileName, bool builtIn, bool originalSize);
    // The pending map arrived (it becomes the map) or failed
    void updateEnvironmentMap();
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
    // The enabled pairs (both ends visible), in the order of their A ends
    [[nodiscard]] std::vector<PlacedPair> placePairs(const std::vector<Placed>& placed) const;
    // The bars of the pairs' rims: (mesh, world matrix) for each
    [[nodiscard]] std::vector<std::pair<uint32_t, glm::mat4>> buildPortalRims(
            const std::vector<PlacedPair>& pairs) const;
    // A model entity as this frame has it (buildRenderList)
    struct PlacedModel {
        EntityId id;
        const ModelComponent* component;
        bool visible;
        glm::mat4 world;
    };
    // The render list's instances and motions made from scratch / only the moved entities' ranges rewritten (false
    // when the layout turned out different: then nothing useful was done)
    void remakeInstances(RenderList& list, const std::vector<PlacedModel>& models,
                         std::vector<std::pair<uint32_t, glm::mat4>>& rims) const;
    bool updateInstances(RenderList& list, const std::vector<PlacedModel>& models,
                         std::vector<std::pair<uint32_t, glm::mat4>>& rims) const;
    // An entity's parts in world space (entity * fit * part), the hidden ones left out, in order
    template <typename Visit>
    void forEachVisiblePart(const PlacedModel& model, Visit&& visit) const;
};

#endif //VULKANDEMO_SCENE_H
