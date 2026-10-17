//
// The game: its behaviour lists, reading the components into them, and the queue. See Game.h.
//

#include <algorithm>
#include <set>
#include "game/Behaviours.h"
#include "game/Game.h"
#include "game/GameProperties.h"

Game::Game() : lists(makeBehaviourLists()), physics(physics::makeSimplePhysicsWorld()) {
    world.physics = physics.get();
}

Game::~Game() = default;

std::vector<GameComponentType> Game::types() {
    std::vector<GameComponentType> types;
    for (const std::unique_ptr<BehaviourListBase>& list : makeBehaviourLists()) {
        types.push_back({list->type(), list->help(), list->defaults()});
    }
    return types;
}

std::vector<std::string> Game::start(const Scene& scene) {
    stop();
    isRunning = true;
    std::vector<std::string> warnings;
    for (const Entity& entity : scene.entities()) {
        addEntity(scene, entity, warnings, false, 0.0);
    }
    return warnings;
}

void Game::addEntity(const Scene& scene, const Entity& entity, std::vector<std::string>& warnings, bool spawned,
                     double time) {
    const std::vector<GameComponent>* components = scene.game(entity.id);
    if (components == nullptr) {
        return;
    }
    std::string where = "entity " + std::to_string(entity.id) + " (" + entity.name + ")";
    std::set<std::string> seen;  // the types on this entity
    ReadContext context{scene, entity, spawned, time, world};
    for (const GameComponent& component : *components) {
        if (component.type.empty()) {
            warnings.push_back(where + ": a game component without a type; ignored");
            continue;
        }
        GameProperties properties(component.properties, where + ": " + component.type, warnings);
        if (!seen.insert(component.type).second) {
            properties.warn("twice on one entity; only the first is used");
            continue;
        }
        auto list = std::find_if(lists.begin(), lists.end(), [&](const std::unique_ptr<BehaviourListBase>& l) {
            return component.type == l->type();
        });
        if (list == lists.end()) {
            warnings.push_back(where + ": unknown game type \"" + component.type + "\"; ignored");
            continue;
        }
        if (spawned && !(*list)->onSpawnedCopies()) {
            continue;
        }
        (*list)->read(context, properties);
        properties.finish();
    }
    if (spawned) {
        for (EntityId child : entity.children) {  // a spawned copy's children were copied with it
            addEntity(scene, *scene.find(child), warnings, true, time);
        }
    }
}

size_t Game::behaviours() const {
    size_t count = 0;
    for (const std::unique_ptr<BehaviourListBase>& list : lists) {
        count += list->size();
    }
    return count;
}

std::string Game::status() const {
    if (world.pickupsCounted == 0) {
        return "";
    }
    return "Pickups: " + std::to_string(world.pickupsTaken) + " / " + std::to_string(world.pickupsCounted);
}

void Game::update(const GameStep& step) {
    if (isPaused) {
        return;
    }
    time = step.time;
    world.player = step.scene.camera().position;
    for (const std::unique_ptr<BehaviourListBase>& list : lists) {
        list->update(step, world);
    }
    applyQueue(step);
    if (snapCameraNext) {
        if (world.controlsCamera) {
            step.snapCamera();  // (not blended from where the editor's camera was)
        }
        snapCameraNext = false;
    }
}

void Game::pause(const Scene& scene) {
    if (!isRunning || isPaused) {
        return;
    }
    isPaused = true;
    pausedEntities.clear();
    for (const Entity& entity : scene.entities()) {
        const std::vector<GameComponent>* components = scene.game(entity.id);
        pausedEntities[entity.id] = {entity.local, scene.worldTransform(entity.id),
                                     components != nullptr ? *components : std::vector<GameComponent>()};
    }
}

std::vector<std::string> Game::resume(const Scene& scene) {
    std::vector<std::string> warnings;
    if (!isPaused) {
        return warnings;
    }
    isPaused = false;
    snapCameraNext = true;
    auto dropBehaviours = [&](EntityId id) {
        for (const std::unique_ptr<BehaviourListBase>& list : lists) {
            list->removeEntity(id, world);
        }
    };
    for (const auto& [id, before] : pausedEntities) {
        if (scene.find(id) == nullptr) {
            dropBehaviours(id);
        }
    }
    auto same = [](const Transform& a, const Transform& b) {
        return a.position == b.position && a.rotation == b.rotation && a.scale == b.scale;
    };
    for (const Entity& entity : scene.entities()) {
        const std::vector<GameComponent>* found = scene.game(entity.id);
        std::vector<GameComponent> components = found != nullptr ? *found : std::vector<GameComponent>();
        auto before = pausedEntities.find(entity.id);
        if (before == pausedEntities.end()) {
            addEntity(scene, entity, warnings, false, time);  // added while paused
            continue;
        }
        if (components != before->second.components) {
            dropBehaviours(entity.id);  // read anew: the old state may not fit the new properties
            addEntity(scene, entity, warnings, false, time);
            continue;
        }
        glm::mat4 worldTransform = scene.worldTransform(entity.id);
        if (!same(entity.local, before->second.local) || worldTransform != before->second.world) {
            EntityMove move{entity.id, before->second.local, entity.local,
                            worldTransform * glm::inverse(before->second.world)};
            for (const std::unique_ptr<BehaviourListBase>& list : lists) {
                list->moved(move, world);
            }
        }
    }
    pausedEntities.clear();
    return warnings;
}

void Game::applyQueue(const GameStep& step) {
    Scene& scene = step.scene;
    // (Taken first: reading a copy's components can add to the lists, not to the spawns)
    std::vector<SpawnRequest> spawns = std::move(world.spawns);
    world.spawns.clear();
    for (const SpawnRequest& spawn : spawns) {
        if (scene.find(spawn.target) == nullptr || scene.find(spawn.at) == nullptr) {
            continue;
        }
        glm::vec3 at = worldPosition(scene, spawn.at);
        EntityId copy = scene.duplicate(spawn.target);  // its subtree and components; models acquired again
        scene.setParent(copy, 0, false);
        Entity& entity = *scene.find(copy);
        entity.local.position = at;
        entity.visible = true;
        std::vector<std::string> ignored;  // (said for the target at Play)
        addEntity(scene, entity, ignored, true, step.time);
        spawn.made(copy, step.time);
        step.snap(copy);
    }
    if (world.removals.empty()) {
        return;
    }
    for (EntityId id : world.removals) {
        if (scene.find(id) != nullptr) {
            scene.remove(id);  // with everything under it
        }
    }
    world.removals.clear();
    for (const std::unique_ptr<BehaviourListBase>& list : lists) {
        list->removeGone(scene, world);  // behaviours of what is gone
    }
}

void Game::stop() {
    isRunning = false;
    isPaused = false;
    snapCameraNext = false;
    time = 0.0;
    pausedEntities.clear();
    for (const std::unique_ptr<BehaviourListBase>& list : lists) {
        list->clear();
    }
    physics->clear();
    world = GameWorld();
    world.physics = physics.get();
}
