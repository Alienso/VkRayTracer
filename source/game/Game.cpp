//
// The game's behaviours. See Game.h.
//

#include <algorithm>
#include <cmath>
#include <set>
#include <glm/gtc/constants.hpp>
#include "game/Game.h"
#include "game/GameProperties.h"

static glm::vec3 worldPosition(const Scene& scene, EntityId id) {
    return glm::vec3(scene.worldTransform(id)[3]);
}

// The point a distance along the path (around and around when it loops; held at the end when it doesn't)
static glm::vec3 pointAlong(const std::vector<glm::vec3>& points, bool loop, double distance) {
    size_t segments = loop ? points.size() : points.size() - 1;
    double length = 0.0;
    for (size_t s = 0; s < segments; s++) {
        length += glm::distance(points[s], points[(s + 1) % points.size()]);
    }
    if (length <= 0.0) {
        return points[0];
    }
    distance = loop ? std::fmod(distance, length) : std::min(distance, length);
    for (size_t s = 0; s < segments; s++) {
        const glm::vec3& a = points[s];
        const glm::vec3& b = points[(s + 1) % points.size()];
        double piece = glm::distance(a, b);
        if (distance <= piece || s + 1 == segments) {
            return piece > 0.0 ? glm::mix(a, b, static_cast<float>(std::min(distance / piece, 1.0))) : b;
        }
        distance -= piece;
    }
    return points.back();
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
    // An id property that must name an entity there is (0: not given, or not an id: warned already)
    auto existing = [&](GameProperties& properties, const char* key, bool required) -> EntityId {
        EntityId id = properties.entity(key);
        if (id != 0 && scene.find(id) == nullptr) {
            properties.warn(std::string("\"") + key + "\": there is no entity " + std::to_string(id));
            return 0;
        }
        if (id == 0 && required) {
            properties.warn(std::string("needs \"") + key + "\": an entity's id; ignored");
        }
        return id;
    };
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
        const std::string& type = component.type;
        if (type == "spin") {
            glm::vec3 axis = properties.vec3("axis", glm::vec3(0.0f, 1.0f, 0.0f));
            if (glm::length(axis) < 1e-6f) {
                properties.warn("\"axis\" is [0, 0, 0]; [0, 1, 0] is used");
                axis = glm::vec3(0.0f, 1.0f, 0.0f);
            }
            spins.push_back({entity.id, properties.number("speed", 90.0f), glm::normalize(axis),
                             entity.local.rotation, time});
        } else if (type == "bob") {
            float period = properties.number("period", 2.0f);
            if (!(period > 0.0f)) {
                properties.warn("\"period\" must be more than 0; 2 is used");
                period = 2.0f;
            }
            bobs.push_back({entity.id, properties.number("height", 0.25f), period, entity.local.position, time});
        } else if (type == "door") {
            glm::vec3 axis = properties.vec3("axis", glm::vec3(0.0f, 1.0f, 0.0f));
            if (glm::length(axis) < 1e-6f) {
                properties.warn("\"axis\" is [0, 0, 0]; [0, 1, 0] is used");
                axis = glm::vec3(0.0f, 1.0f, 0.0f);
            }
            Door door{entity.id, properties.number("openAngle", 90.0f), properties.number("seconds", 1.0f),
                      properties.number("radius", 2.0f), glm::normalize(axis), properties.flag("locked", false),
                      entity.local.rotation};
            doors.push_back(door);
        } else if (type == "pickup") {
            Pickup pickup{entity.id, properties.number("radius", 1.0f), properties.flag("remove", false)};
            pickup.counted = scene.visibleInWorld(entity.id);
            pickups.push_back(pickup);
        } else if (type == "trigger") {
            EntityId target = existing(properties, "target", true);
            bool show = properties.flag("show", true);
            if (target != 0) {
                triggers.push_back({entity.id, target, show});
            }
        } else if (type == "spawner") {
            if (spawned) {
                continue;  // (a copy that spawns copies: not)
            }
            EntityId target = existing(properties, "target", true);
            if (target != 0 && scene.isUnder(entity.id, target)) {
                properties.warn("\"target\" is the spawner itself or above it; ignored");
                target = 0;
            }
            float interval = properties.number("interval", 2.0f);
            if (!(interval > 0.0f)) {
                properties.warn("\"interval\" must be more than 0; 2 is used");
                interval = 2.0f;
            }
            float lifetime = properties.number("lifetime", 5.0f);
            int max = static_cast<int>(properties.number("max", 10.0f));
            if (target != 0) {
                spawners.push_back({entity.id, target, interval, lifetime, std::max(max, 0), interval, {}});
            }
        } else if (type == "cameraPath") {
            float speed = properties.number("speed", 2.0f);
            bool loop = properties.flag("loop", true);
            EntityId lookAt = existing(properties, "lookAt", false);
            std::vector<glm::vec3> points;
            for (EntityId child : entity.children) {
                points.push_back(worldPosition(scene, child));
            }
            if (!cameraPaths.empty()) {
                properties.warn("the game already has a camera path; ignored");
            } else if (points.size() < 2) {
                properties.warn("needs at least 2 children (the points of the path, in their order); ignored");
            } else if (!spawned) {
                cameraPaths.push_back({entity.id, lookAt, points, speed, loop});
            }
        } else {
            warnings.push_back(where + ": unknown game type \"" + type + "\"; ignored");
            continue;
        }
        properties.finish();
    }
    if (spawned) {
        for (EntityId child : entity.children) {  // a spawned copy's children were copied with it
            addEntity(scene, *scene.find(child), warnings, true, time);
        }
    }
}

size_t Game::behaviours() const {
    return spins.size() + bobs.size() + doors.size() + pickups.size() + triggers.size() + spawners.size() +
           cameraPaths.size();
}

std::string Game::status() const {
    size_t counted = std::count_if(pickups.begin(), pickups.end(), [](const Pickup& p) { return p.counted; });
    if (counted == 0) {
        return "";
    }
    return "Pickups: " + std::to_string(pickupsTaken) + " / " + std::to_string(counted);
}

void Game::update(const GameStep& step) {
    Scene& scene = step.scene;
    glm::vec3 player = scene.camera().position;
    // Spin and bob from the time since they started, not added up step by step: no drift, and the same at any step
    for (const Spin& spin : spins) {
        if (Entity* entity = scene.find(spin.entity)) {  // (gone: removed while playing)
            double seconds = step.time - spin.startTime;
            float angle = static_cast<float>(glm::radians(static_cast<double>(spin.speed) * seconds));
            entity->local.rotation = spin.start * glm::angleAxis(angle, spin.axis);
        }
    }
    for (const Bob& bob : bobs) {
        if (Entity* entity = scene.find(bob.entity)) {
            double phase = (step.time - bob.startTime) / static_cast<double>(bob.period) * 2.0 * glm::pi<double>();
            entity->local.position = bob.start + glm::vec3(0.0f, bob.height * static_cast<float>(std::sin(phase)), 0.0f);
        }
    }
    for (Door& door : doors) {
        Entity* entity = scene.find(door.entity);
        if (entity == nullptr) {
            continue;
        }
        if (step.input.wasPressed(Key::E) && !door.locked &&
            glm::distance(player, worldPosition(scene, door.entity)) <= door.radius) {
            door.opening = !door.opening;
        }
        float target = door.opening ? 1.0f : 0.0f;
        float change = door.seconds > 0.0f ? static_cast<float>(step.seconds) / door.seconds : 1.0f;
        door.open = door.open < target ? std::min(door.open + change, target) : std::max(door.open - change, target);
        entity->local.rotation = door.closed * glm::angleAxis(glm::radians(door.openAngle * door.open), door.axis);
    }
    for (Pickup& pickup : pickups) {
        Entity* entity = scene.find(pickup.entity);
        if (entity == nullptr || pickup.taken || !scene.visibleInWorld(pickup.entity)) {
            continue;
        }
        pickup.counted = true;
        if (glm::distance(player, worldPosition(scene, pickup.entity)) <= pickup.radius) {
            pickup.taken = true;
            pickupsTaken++;
            if (pickup.remove) {
                removals.push_back(pickup.entity);
            } else {
                entity->visible = false;
            }
        }
    }
    for (Trigger& trigger : triggers) {
        Entity* target = scene.find(trigger.target);
        if (target == nullptr || scene.find(trigger.entity) == nullptr) {
            continue;
        }
        glm::vec3 local = glm::vec3(glm::inverse(scene.worldTransform(trigger.entity)) * glm::vec4(player, 1.0f));
        int inside = std::abs(local.x) <= 0.5f && std::abs(local.y) <= 0.5f && std::abs(local.z) <= 0.5f ? 1 : 0;
        if (inside != trigger.inside) {  // (only on a change: something else may show or hide it meanwhile)
            trigger.inside = inside;
            target->visible = (inside == 1) == trigger.show;
        }
    }
    for (size_t s = 0; s < spawners.size(); s++) {
        Spawner& spawner = spawners[s];
        if (scene.find(spawner.entity) == nullptr) {
            continue;
        }
        for (auto it = spawner.spawned.begin(); it != spawner.spawned.end();) {
            if (scene.find(it->first) == nullptr) {  // (taken as a pickup, say)
                it = spawner.spawned.erase(it);
            } else if (spawner.lifetime > 0.0f && step.time - it->second >= spawner.lifetime) {
                removals.push_back(it->first);
                it = spawner.spawned.erase(it);
            } else {
                ++it;
            }
        }
        if (step.time >= spawner.nextTime) {
            spawner.nextTime += spawner.interval;
            if (static_cast<int>(spawner.spawned.size()) < spawner.max && scene.find(spawner.target) != nullptr) {
                spawns.push_back({s});
            }
        }
    }
    if (!cameraPaths.empty()) {
        const CameraPath& path = cameraPaths[0];
        Camera& camera = scene.camera();
        double distance = static_cast<double>(path.speed) * step.time;
        camera.position = pointAlong(path.points, path.loop, distance);
        glm::vec3 look = path.lookAt != 0 && scene.find(path.lookAt) != nullptr
                         ? worldPosition(scene, path.lookAt)
                         : pointAlong(path.points, path.loop, distance + 1.0);  // a unit ahead
        if (glm::distance(look, camera.position) > 1e-4f) {
            camera.lookAt(look);
        }
    }
    applyQueue(step);
}

void Game::applyQueue(const GameStep& step) {
    Scene& scene = step.scene;
    for (const Spawn& spawn : spawns) {
        Spawner& spawner = spawners[spawn.spawner];
        if (scene.find(spawner.target) == nullptr || scene.find(spawner.entity) == nullptr) {
            continue;
        }
        glm::vec3 at = worldPosition(scene, spawner.entity);
        EntityId copy = scene.duplicate(spawner.target);  // its subtree and components; models acquired again
        scene.setParent(copy, 0, false);
        Entity& entity = *scene.find(copy);
        entity.local.position = at;
        entity.visible = true;
        std::vector<std::string> ignored;  // (said for the target at Play)
        addEntity(scene, entity, ignored, true, step.time);
        spawner.spawned.emplace_back(copy, step.time);
        step.snap(copy);
    }
    spawns.clear();
    if (removals.empty()) {
        return;
    }
    for (EntityId id : removals) {
        if (scene.find(id) != nullptr) {
            scene.remove(id);  // with everything under it
        }
    }
    removals.clear();
    // Behaviours of what is gone
    auto gone = [&](const auto& behaviour) { return scene.find(behaviour.entity) == nullptr; };
    spins.erase(std::remove_if(spins.begin(), spins.end(), gone), spins.end());
    bobs.erase(std::remove_if(bobs.begin(), bobs.end(), gone), bobs.end());
    doors.erase(std::remove_if(doors.begin(), doors.end(), gone), doors.end());
    triggers.erase(std::remove_if(triggers.begin(), triggers.end(), gone), triggers.end());
    // (Pickups stay: the count keeps the ones taken. Spawners stay: their indexes are in spawns)
}

void Game::stop() {
    isRunning = false;
    spins.clear();
    bobs.clear();
    doors.clear();
    pickups.clear();
    triggers.clear();
    spawners.clear();
    cameraPaths.clear();
    pickupsTaken = 0;
    removals.clear();
    spawns.clear();
}
