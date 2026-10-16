//
// The game's behaviours. See Game.h.
//

#include <cmath>
#include <set>
#include <glm/gtc/constants.hpp>
#include "game/Game.h"
#include "game/GameProperties.h"

std::vector<std::string> Game::start(const Scene& scene) {
    stop();
    isRunning = true;
    std::vector<std::string> warnings;
    for (const Entity& entity : scene.entities()) {
        const std::vector<GameComponent>* components = scene.game(entity.id);
        if (components == nullptr) {
            continue;
        }
        std::string where = "entity " + std::to_string(entity.id) + " (" + entity.name + ")";
        std::set<std::string> seen;  // the types on this entity
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
            if (component.type == "spin") {
                glm::vec3 axis = properties.vec3("axis", glm::vec3(0.0f, 1.0f, 0.0f));
                if (glm::length(axis) < 1e-6f) {
                    properties.warn("\"axis\" is [0, 0, 0]; [0, 1, 0] is used");
                    axis = glm::vec3(0.0f, 1.0f, 0.0f);
                }
                spins.push_back({entity.id, properties.number("speed", 90.0f), glm::normalize(axis),
                                 entity.local.rotation});
            } else if (component.type == "bob") {
                float period = properties.number("period", 2.0f);
                if (!(period > 0.0f)) {
                    properties.warn("\"period\" must be more than 0; 2 is used");
                    period = 2.0f;
                }
                bobs.push_back({entity.id, properties.number("height", 0.25f), period, entity.local.position});
            } else {
                warnings.push_back(where + ": unknown game type \"" + component.type + "\"; ignored");
                continue;
            }
            properties.finish();
        }
    }
    return warnings;
}

void Game::update(const GameStep& step) {
    // From the time since Play, not added up step by step: no drift, and the same at any step
    for (const Spin& spin : spins) {
        if (Entity* entity = step.scene.find(spin.entity)) {  // (gone: removed while playing)
            float angle = static_cast<float>(glm::radians(static_cast<double>(spin.speed) * step.time));
            entity->local.rotation = spin.start * glm::angleAxis(angle, spin.axis);
        }
    }
    for (const Bob& bob : bobs) {
        if (Entity* entity = step.scene.find(bob.entity)) {
            double phase = step.time / static_cast<double>(bob.period) * 2.0 * glm::pi<double>();
            entity->local.position = bob.start + glm::vec3(0.0f, bob.height * static_cast<float>(std::sin(phase)), 0.0f);
        }
    }
}

void Game::stop() {
    isRunning = false;
    spins.clear();
    bobs.clear();
}
