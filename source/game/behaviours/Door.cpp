//
// door: E near it opens it, E again closes it. See game/Behaviour.h for what a type is made of.
//

#include <algorithm>
#include <memory>
#include "game/Behaviour.h"

namespace {

struct Door {
    static constexpr const char* type = "door";
    static constexpr const char* help =
        "E within radius of it opens it, E again closes it: turns openAngle degrees around axis (its own, through\n"
        "its origin: put the door's model under it to place the hinge), taking seconds. A locked door doesn't open.";
    static constexpr const char* defaults =
        R"({"openAngle": 90, "seconds": 1, "radius": 2, "axis": [0, 1, 0], "locked": false})";
    static constexpr bool removedWithEntity = true;
    static constexpr bool onSpawnedCopies = true;

    EntityId entity;
    float openAngle, seconds, radius;
    glm::vec3 axis;
    bool locked;
    glm::quat closed;  // the rotation at Play
    bool opening = false;
    float open = 0.0f;  // 0 closed .. 1 open

    static std::optional<Door> read(const ReadContext& context, GameProperties& properties) {
        glm::vec3 axis = properties.vec3("axis", glm::vec3(0.0f, 1.0f, 0.0f));
        if (glm::length(axis) < 1e-6f) {
            properties.warn("\"axis\" is [0, 0, 0]; [0, 1, 0] is used");
            axis = glm::vec3(0.0f, 1.0f, 0.0f);
        }
        return Door{context.entity.id, properties.number("openAngle", 90.0f), properties.number("seconds", 1.0f),
                    properties.number("radius", 2.0f), glm::normalize(axis), properties.flag("locked", false),
                    context.entity.local.rotation};
    }

    void update(const GameStep& step, GameWorld& world) {
        Entity* e = step.scene.find(entity);
        if (e == nullptr) {
            return;
        }
        if (step.input.wasPressed(Key::E) && !locked &&
            glm::distance(world.player, worldPosition(step.scene, entity)) <= radius) {
            opening = !opening;
        }
        float target = opening ? 1.0f : 0.0f;
        float change = seconds > 0.0f ? static_cast<float>(step.seconds) / seconds : 1.0f;
        open = open < target ? std::min(open + change, target) : std::max(open - change, target);
        e->local.rotation = closed * glm::angleAxis(glm::radians(openAngle * open), axis);
    }

    // Turned by hand while paused: it closes to the turn it was given
    void moved(const EntityMove& move, GameWorld&) { closed = followRotation(move, closed); }
};

}  // namespace

std::unique_ptr<BehaviourListBase> makeDoorList() {
    return std::make_unique<BehaviourList<Door>>();
}
