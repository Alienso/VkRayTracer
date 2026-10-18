//
// The game (README: Game logic): what the scene's game components do while playing. At Play it reads them once into its
// behaviours' own lists (typed values, no JSON); each fixed step (GameLoop) it updates them, moving entities through
// the scene like the editor does (Entity::local, visible: the render list finds the changes). It knows nothing of
// the renderer or the window; Stop puts the scene back (the editor), the game only drops its state.
//
// Distances (doors, pickups, triggers) are to the camera's position: the player's eyes when there is a player, a
// body walking among colliders (physics/PhysicsWorld.h, owned here); else the fly camera.
//
// Behaviours: spin, bob, slide, door, pickup, trigger, spawner, collider, player, portalGun, cameraPath, one file each in game/behaviours/ (what each
// does and reads: its help and defaults there; how a type is made: game/Behaviour.h). Several on one entity work
// together when they move different things (spin turns, bob moves); two that turn it (spin and door) fight: the later
// one in the update order (game/Behaviours.cpp) wins. The same type twice on one entity is a warning (only the first
// is used). Entities added or removed while playing are applied after the step (the queue).
//
// Portals: any entity the step moved whose origin went through a portal's opening comes out of the other end (the
// topmost one that moved: what is under it comes along), its behaviours told (the moved hook), shown there at once.
// The player goes through on its own (its eyes, with the camera).
//
// Pause (the editor's F6): no steps; the editor may change the scene meanwhile. Resume compares the scene with how it
// was at the pause: behaviours of removed entities go, new entities' components and changed components are read
// (starting then), and entities moved by hand tell their behaviours (the moved hook), so they go on from there.
//

#ifndef VULKANDEMO_GAME_H
#define VULKANDEMO_GAME_H

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
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
    // The scene as it is now is what resume compares with (call with the stepped transforms, not blended ones)
    void pause(const Scene& scene);
    // The editor's changes since pause taken in; returns what couldn't be read in components new or changed
    std::vector<std::string> resume(const Scene& scene);

    [[nodiscard]] bool running() const { return isRunning; }
    [[nodiscard]] bool paused() const { return isPaused; }
    [[nodiscard]] size_t behaviours() const;
    // A camera path drives the camera: the fly camera is off while playing
    [[nodiscard]] bool controlsCamera() const { return world.controlsCamera; }
    // The game wants the mouse captured while it runs (the view turned by moving it): the player's, or the
    // game-style fly camera when nothing drives the camera (not under a camera path)
    [[nodiscard]] bool capturesMouse() const { return world.mouseLook || !world.controlsCamera; }
    // A crosshair at the view's center: something aims with it (the portal gun)
    [[nodiscard]] bool wantsCrosshair() const { return world.portalGun; }
    // The colliders and the player's body (our own SimplePhysics; another implementation goes in Game.cpp)
    [[nodiscard]] const physics::PhysicsWorld& physicsWorld() const { return *physics; }
    // What the overlay shows while playing ("" : nothing)
    [[nodiscard]] std::string status() const;

private:
    bool isRunning = false;
    bool isPaused = false;
    bool snapCameraNext = false;  // the first step after resume takes the camera back at once
    double time = 0.0;            // the last step's (since Play)
    std::vector<std::unique_ptr<BehaviourListBase>> lists;  // one per type, in update order
    std::unique_ptr<physics::PhysicsWorld> physics;
    GameWorld world;
    // The entities at pause, to compare with at resume
    struct PausedEntity {
        Transform local;
        glm::mat4 world;
        std::vector<GameComponent> components;
    };
    std::map<EntityId, PausedEntity> pausedEntities;
    // The entity's components into behaviours. spawned: a copy made while playing (no spawners, starting now).
    void addEntity(const Scene& scene, const Entity& entity, std::vector<std::string>& warnings, bool spawned,
                   double time);
    void applyQueue(const GameStep& step);
    // The entities that went through a portal in this step (before: their world origins at its start) carried
    // through
    void carryThroughPortals(const GameStep& step, const std::unordered_map<EntityId, glm::vec3>& before);
};

#endif //VULKANDEMO_GAME_H
