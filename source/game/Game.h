//
// The game (README: Game logic): what the scene's game components do while playing. At Play it reads them once into its
// behaviours' own lists (typed values, no JSON); each fixed step (GameLoop) it updates them, moving entities through
// the scene like the editor does (Entity::local, visible: the render list finds the changes). It knows nothing of
// the renderer or the window; Stop puts the scene back (the editor), the game only drops its state.
//
// The player is the camera (there is no body or collision yet): distances are to the camera's position.
//
// Behaviours: spin, bob, door, pickup, trigger, spawner, cameraPath, one file each in game/behaviours/ (what each
// does and reads: its help and defaults there; how a type is made: game/Behaviour.h). Several on one entity work
// together when they move different things (spin turns, bob moves); two that turn it (spin and door) fight: the later
// one in the update order (game/Behaviours.cpp) wins. The same type twice on one entity is a warning (only the first
// is used). Entities added or removed while playing are applied after the step (the queue).
//

#ifndef VULKANDEMO_GAME_H
#define VULKANDEMO_GAME_H

#include <memory>
#include <string>
#include <vector>
#include "game/Behaviour.h"
#include "scene/Scene.h"

class Game {
public:
    Game();
    ~Game();

    // The behaviour types, for the editor: name, help, default properties
    [[nodiscard]] static std::vector<GameComponentType> types();

    // The scene's game components into the behaviours; returns what couldn't be read (an unknown type, a property
    // of the wrong kind, an unknown key, an id that names no entity: the rest is used), for the editor's log
    std::vector<std::string> start(const Scene& scene);
    void update(const GameStep& step);
    void stop();  // drops the game's state (the scene is put back by the editor)

    [[nodiscard]] bool running() const { return isRunning; }
    [[nodiscard]] size_t behaviours() const;
    // A camera path drives the camera: the fly camera is off while playing
    [[nodiscard]] bool controlsCamera() const { return world.controlsCamera; }
    // What the overlay shows while playing ("" : nothing)
    [[nodiscard]] std::string status() const;

private:
    bool isRunning = false;
    std::vector<std::unique_ptr<BehaviourListBase>> lists;  // one per type, in update order
    GameWorld world;
    // The entity's components into behaviours. spawned: a copy made while playing (no spawners, starting now).
    void addEntity(const Scene& scene, const Entity& entity, std::vector<std::string>& warnings, bool spawned,
                   double time);
    void applyQueue(const GameStep& step);
};

#endif //VULKANDEMO_GAME_H
