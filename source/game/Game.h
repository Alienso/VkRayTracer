//
// The game (GAME_PLAN.md): what the scene's game components do while playing. At Play it reads them once into its
// behaviours' own lists (typed values, no JSON); each fixed step (GameLoop) it updates them, moving entities through
// the scene like the editor does (Entity::local, visible: the render list finds the changes). It knows nothing of
// the renderer or the window; Stop puts the scene back (the editor), the game only drops its state.
//
// Behaviours (a component's "type", its properties with their defaults):
//   spin  turns the entity around an axis of its own:  "speed": 90 (degrees per second), "axis": [0, 1, 0]
//   bob   moves the entity up and down from where it was at Play:  "height": 0.25 (units), "period": 2 (seconds)
// Several on one entity work together when they move different things (spin turns, bob moves); the same type
// twice on one entity is a warning (only the first is used).
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
};

class Game {
public:
    // The scene's game components into the behaviours; returns what couldn't be read (an unknown type, a property
    // of the wrong kind, an unknown key: the rest is used), for the editor's log
    std::vector<std::string> start(const Scene& scene);
    void update(const GameStep& step);
    void stop();  // drops the game's state (the scene is put back by the editor)

    [[nodiscard]] bool running() const { return isRunning; }
    [[nodiscard]] size_t behaviours() const { return spins.size() + bobs.size(); }

private:
    struct Spin {
        EntityId entity;
        float speed;       // degrees per second
        glm::vec3 axis;    // unit length, in the entity's own space
        glm::quat start;   // the rotation at Play
    };
    struct Bob {
        EntityId entity;
        float height;
        float period;
        glm::vec3 start;   // the position at Play
    };
    bool isRunning = false;
    std::vector<Spin> spins;
    std::vector<Bob> bobs;
};

#endif //VULKANDEMO_GAME_H
