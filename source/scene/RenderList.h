//
// Render list: what one frame draws, made by the Scene (Scene::buildRenderList) and read by the renderer. No
// Vulkan: mesh indices into AssetStore::meshes(), world matrices, motion, lights, the camera and the environment.
// The renderer turns the instances into TLAS instances one to one (instance i = TLAS instance i).
//
// Big scenes (vegetation) have millions of instances, too many to touch every frame. Each model entity owns a range
// of the list (its visible parts, in order): when it moves, only its range's matrices are rewritten and named in
// changedInstances(), and its motion. The whole list is remade (the revision goes up) only when its layout changes:
// entities added, removed, re-parented, shown or hidden, parts shown or hidden, a model loaded or unloaded.
// Punctual lights and portals are few and remade every frame.
//
// Objects part way through a portal (portals/PortalPieces.h): an instance can be cut (clip: a region of
// clipRegions, which keeps only its part on this side of the openings), and its pieces at the other ends are ghost
// instances after the portal rims, made every frame (few). A ghost appearing or going changes only the list's end:
// remadeInstances() names it, the revision stays.
//

#ifndef VULKANDEMO_RENDERLIST_H
#define VULKANDEMO_RENDERLIST_H

#include <glm/glm.hpp>
#include <cstdint>
#include <utility>
#include <vector>
#include "scene/Camera.h"
#include "core/Handles.h"
#include "assets/ModelLoader.h"
#include "scene/Portal.h"
#include "scene/SceneEnvironment.h"
#include "portals/PortalPieces.h"

// One mesh placed in the world: a visible part of a visible object
struct RenderInstance {
    uint32_t mesh;       // index into AssetStore::meshes()
    uint32_t motion;     // index into RenderList::motions (the object's)
    glm::mat4 world;     // entity (world) * fit * part
    uint32_t clip = 0;   // 0: whole; else 1 + its region in RenderList::clipRegions (cut by portal openings)
};

// What a cut instance keeps (a portal piece's cuts, portals::pieceKeeps): the rest of it isn't there
struct ClipRegion {
    uint32_t cutCount = 0;
    portals::PortalCut cuts[portals::PortalPiece::maxCuts];
};

// A punctual light of a visible object, in world space but still in the file's units (the renderer converts it)
struct RenderLight {
    LightType type;
    glm::vec3 color;
    float intensity;
    float range;
    float innerConeAngle;
    float outerConeAngle;
    glm::mat4 world;  // entity (world) * fit * the light's node
    float scale;      // the object's (and fit's) scale: distances in the file grow by this much
};

// One end of an enabled portal pair. The pairs' ends follow each other: entries 2k and 2k + 1 are a pair.
struct RenderPortal {
    glm::mat4 world;      // the end entity's world placement without scale: the opening in its XY plane, facing +Z
    glm::vec2 halfSize;   // half the width and height
    PortalShape shape;
};

// Instances first, first + 1, ..., first + count - 1
struct InstanceRange {
    uint32_t first;
    uint32_t count;
};

// A box in the world: [min, max] in its own space, placed by transform (the editor's selection: where its outline
// can be on the image)
struct WorldBox {
    glm::mat4 transform;
    glm::vec3 min;
    glm::vec3 max;
};

class RenderList {
public:
    std::vector<RenderInstance> instances;
    // Per visible model entity: this frame's world position -> last frame's (identity when it didn't move). Shared by
    // all its instances: a part's world matrix is entity * fit * part, and only the entity's transform changes.
    std::vector<glm::mat4> motions;
    std::vector<RenderLight> lights;
    std::vector<RenderPortal> portals;
    std::vector<ClipRegion> clipRegions;  // made every frame
    Camera camera;                 // copied every frame
    // Where the camera went through portals since the last build: takes points from where it was to where it is now
    // (identity when it didn't). The same view continues, so last frame's image is still this one's history.
    glm::mat4 cameraTeleport = glm::mat4(1.0f);
    SceneEnvironment environment;  // copied every frame (its map: AssetStore::environmentMap)

    // Goes up whenever the instances and motions were remade: other instances, or in another order
    [[nodiscard]] uint64_t instanceRevision() const { return revision; }
    // Counts the builds (Scene::buildRenderList), one per frame. What changed is what changed since the build before:
    // a reader that missed a build has to take the whole list.
    [[nodiscard]] uint64_t build() const { return builds; }
    // The instances whose matrices this build rewrote (same meshes, same order), when the revision stayed the same
    [[nodiscard]] const std::vector<InstanceRange>& changedInstances() const { return changed; }
    // The instances this build made anew (other meshes, or cut otherwise: the ghosts, entities going into or out of
    // an opening), when the revision stayed the same. The list may have grown or shrunk at its end (the ghosts).
    [[nodiscard]] const std::vector<InstanceRange>& remadeInstances() const { return remade; }
    // Some motion changed in this build (or the revision went up)
    [[nodiscard]] bool motionsChanged() const { return motionChanged; }

    // The model entity whose parts instance i is (0: none, e.g. a portal rim's bar); a ghost's is its entity's
    [[nodiscard]] uint64_t entityOfInstance(uint32_t instance) const {
        for (const BuiltEntity& entity : builtEntities) {
            if (instance >= entity.range.first && instance - entity.range.first < entity.range.count) {
                return entity.id;
            }
        }
        for (const BuiltGhost& ghost : builtGhosts) {
            if (instance >= ghost.range.first && instance - ghost.range.first < ghost.range.count) {
                return ghost.id;
            }
        }
        return 0;
    }
    // The instances of the entities for which selected(id) is true, as ranges (merged where they meet: an entity
    // and the ones under it are next to each other, in tree order)
    template <typename Selected> [[nodiscard]] std::vector<InstanceRange> instanceRanges(Selected&& selected) const {
        std::vector<InstanceRange> ranges;
        for (const BuiltEntity& entity : builtEntities) {
            if (entity.range.count == 0 || !selected(entity.id)) {
                continue;
            }
            if (!ranges.empty() && ranges.back().first + ranges.back().count == entity.range.first) {
                ranges.back().count += entity.range.count;
            } else {
                ranges.push_back(entity.range);
            }
        }
        for (const BuiltGhost& ghost : builtGhosts) {  // (their ghosts too)
            if (ghost.range.count > 0 && selected(ghost.id)) {
                ranges.push_back(ghost.range);
            }
        }
        return ranges;
    }

    // The ghosts of the entities for which selected(id) is true: (entity, the piece's transform) for each (the
    // selection's outline shows them too)
    template <typename Selected>
    [[nodiscard]] std::vector<std::pair<uint64_t, glm::mat4>> ghostsOf(Selected&& selected) const {
        std::vector<std::pair<uint64_t, glm::mat4>> ghosts;
        for (const BuiltGhost& ghost : builtGhosts) {
            if (selected(ghost.id)) {
                ghosts.emplace_back(ghost.id, ghost.transform);
            }
        }
        return ghosts;
    }

private:
    friend class Scene;
    uint64_t revision = 0;
    uint64_t builds = 0;
    std::vector<InstanceRange> changed;
    std::vector<InstanceRange> remade;
    bool motionChanged = false;

    // What the instances were made from: per model entity in tree order, its range of the instances and its motion.
    // Compared with the entities every frame: a different layout remakes the list, a new world transform rewrites
    // the entity's range. Also last frame's world transforms, for the motions (Scene::buildRenderList).
    struct BuiltEntity {
        uint64_t id;  // the entity's id
        ModelHandle model;
        bool visible;  // in the world (its parents too)
        bool originalSize;
        glm::mat4 world;  // the entity's world transform
        InstanceRange range;  // its visible parts (none when hidden)
        uint32_t motion;      // its entry in motions (when visible)
        uint32_t clip = 0;    // its instances' (RenderInstance::clip)
    };
    std::vector<BuiltEntity> builtEntities;
    uint64_t builtLayout = 0;  // Scene's layout revision and the AssetStore's revision the list was made with
    uint64_t builtAssets = 0;
    // The portal rims' bars (mesh, world matrix), after the entities' instances from firstRim on
    std::vector<std::pair<uint32_t, glm::mat4>> builtRims;
    uint32_t firstRim = 0;
    // The ghosts (an entity's piece through portals), after the rims, each with a motion after the others' from
    // firstGhostMotion on. world: the piece's transform * the entity's world transform, for next frame's motion.
    struct BuiltGhost {
        uint64_t id;     // its entity
        uint32_t piece;  // which of the entity's pieces (1: its first ghost)
        glm::mat4 transform;  // the piece's (portals::PortalPiece::transform)
        glm::mat4 world;
        InstanceRange range;
    };
    std::vector<BuiltGhost> builtGhosts;
    uint32_t firstGhostMotion = 0;
};

#endif //VULKANDEMO_RENDERLIST_H
