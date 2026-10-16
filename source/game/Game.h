//
// The game (README: Game logic): what the scene's game components do while playing. At Play it reads them once into its
// behaviours' own lists (typed values, no JSON); each fixed step (GameLoop) it updates them, moving entities through
// the scene like the editor does (Entity::local, visible: the render list finds the changes). It knows nothing of
// the renderer or the window; Stop puts the scene back (the editor), the game only drops its state.
//
// The player is the camera (there is no body or collision yet): distances are to the camera's position.
//
// Behaviours (a component's "type", its properties with their defaults):
//   spin       turns the entity around an axis of its own:  "speed": 90 (degrees per second), "axis": [0, 1, 0]
//   bob        moves the entity up and down from where it was at Play:  "height": 0.25 (units), "period": 2 (s)
//   door       E within "radius": 2 of it opens it, E again closes it: turns "openAngle": 90 degrees around "axis":
//              [0, 1, 0] (its own, through its origin: put the door's model under it to place the hinge), taking
//              "seconds": 1; "locked": false
//   pickup     taken when the camera comes within "radius": 1: hidden (or "remove": true, removed); counted in the
//              overlay ("Pickups: 2 / 5"). Only while it is shown: one hidden at Play (a spawner's target) counts
//              once something shows it.
//   trigger    a box (the entity's transform: a unit cube around its origin, scaled and turned with it) that shows
//              the entity "target" (an id) while the camera is inside, and hides it outside ("show": false: the
//              other way round)
//   spawner    every "interval": 2 seconds, a copy of "target" (an id; usually a hidden one) at the spawner, shown,
//              with its components working (but not spawners); removed after "lifetime": 5 seconds (0: kept);
//              at most "max": 10 at a time
//   cameraPath the game drives the camera: along the entity's children (in their order, where they are at Play)
//              at "speed": 2 units per second, "loop": true (back to the first), looking ahead along the path or at
//              the entity "lookAt" (an id). One per game.
// Several on one entity work together when they move different things (spin turns, bob moves); two that turn it
// (spin and door) fight: the later one in the update order wins. The same type twice on one entity is a warning
// (only the first is used). Entities added or removed while playing are applied after the step (the queue).
//

#ifndef VULKANDEMO_GAME_H
#define VULKANDEMO_GAME_H

#include <string>
#include <vector>
#include "core/Input.h"
#include "game/StepInterpolation.h"
#include "scene/Scene.h"

// What one step works with
struct GameStep {
    Scene& scene;
    RenderSettings& settings;  // the program's; the scene's fields of it are put back by Stop with the scene
    const InputState& input;   // what happened since the last step (GameLoop)
    double seconds;            // the step's length: GameLoop::stepSeconds
    double time;               // since Play, at the end of this step
    uint64_t index;            // steps before this one
    StepInterpolation* interpolation = nullptr;  // how the steps are shown (none in some tests)

    // The entity's change in this step is a jump (a respawn, a teleport): shown at once, not blended from where it
    // was
    void snap(EntityId id) const {
        if (interpolation != nullptr) {
            interpolation->snap(id);
        }
    }
    void snapCamera() const {
        if (interpolation != nullptr) {
            interpolation->snapCamera();
        }
    }
};

class Game {
public:
    // The scene's game components into the behaviours; returns what couldn't be read (an unknown type, a property
    // of the wrong kind, an unknown key, an id that names no entity: the rest is used), for the editor's log
    std::vector<std::string> start(const Scene& scene);
    void update(const GameStep& step);
    void stop();  // drops the game's state (the scene is put back by the editor)

    [[nodiscard]] bool running() const { return isRunning; }
    [[nodiscard]] size_t behaviours() const;
    // A camera path drives the camera: the fly camera is off while playing
    [[nodiscard]] bool controlsCamera() const { return !cameraPaths.empty(); }
    // What the overlay shows while playing ("" : nothing)
    [[nodiscard]] std::string status() const;

private:
    struct Spin {
        EntityId entity;
        float speed;       // degrees per second
        glm::vec3 axis;    // unit length, in the entity's own space
        glm::quat start;   // the rotation at Play (or when spawned)
        double startTime;
    };
    struct Bob {
        EntityId entity;
        float height;
        float period;
        glm::vec3 start;   // the position at Play (or when spawned)
        double startTime;
    };
    struct Door {
        EntityId entity;
        float openAngle, seconds, radius;
        glm::vec3 axis;
        bool locked;
        glm::quat closed;  // the rotation at Play
        bool opening = false;
        float open = 0.0f;  // 0 closed .. 1 open
    };
    struct Pickup {
        EntityId entity;
        float radius;
        bool remove;
        bool taken = false;
        bool counted = false;  // shown at some point: one of the "of N"
    };
    struct Trigger {
        EntityId entity, target;
        bool show;
        int inside = -1;  // -1: not known yet (the first step sets the target either way)
    };
    struct Spawner {
        EntityId entity, target;
        float interval, lifetime;
        int max;
        double nextTime;
        std::vector<std::pair<EntityId, double>> spawned;  // and when
    };
    struct CameraPath {
        EntityId entity, lookAt;
        std::vector<glm::vec3> points;  // world positions at Play
        float speed;
        bool loop;
    };
    bool isRunning = false;
    std::vector<Spin> spins;
    std::vector<Bob> bobs;
    std::vector<Door> doors;
    std::vector<Pickup> pickups;
    std::vector<Trigger> triggers;
    std::vector<Spawner> spawners;
    std::vector<CameraPath> cameraPaths;
    int pickupsTaken = 0;
    // Changes that add or remove entities, applied after a step's behaviours (they move the scene's entities)
    std::vector<EntityId> removals;
    struct Spawn {
        size_t spawner;
    };
    std::vector<Spawn> spawns;

    // The entity's components into behaviours. spawned: a copy made while playing (no spawners, starting now).
    void addEntity(const Scene& scene, const Entity& entity, std::vector<std::string>& warnings, bool spawned,
                   double time);
    void applyQueue(const GameStep& step);
};

#endif //VULKANDEMO_GAME_H
