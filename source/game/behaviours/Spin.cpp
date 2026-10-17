//
// spin: turns the entity around an axis of its own. See game/Behaviour.h for what a type is made of.
//

#include <memory>
#include "game/Behaviour.h"

namespace {

struct Spin {
    static constexpr const char* type = "spin";
    static constexpr const char* help = "Turns the entity around an axis of its own.\n"
                                        "speed: degrees per second; axis: in the entity's own space.";
    static constexpr const char* defaults = R"({"speed": 90, "axis": [0, 1, 0]})";
    static constexpr bool removedWithEntity = true;
    static constexpr bool onSpawnedCopies = true;

    EntityId entity;
    float speed;       // degrees per second
    glm::vec3 axis;    // unit length, in the entity's own space
    glm::quat start;   // the rotation at Play (or when spawned)
    double startTime;

    static std::optional<Spin> read(const ReadContext& context, GameProperties& properties) {
        glm::vec3 axis = properties.vec3("axis", glm::vec3(0.0f, 1.0f, 0.0f));
        if (glm::length(axis) < 1e-6f) {
            properties.warn("\"axis\" is [0, 0, 0]; [0, 1, 0] is used");
            axis = glm::vec3(0.0f, 1.0f, 0.0f);
        }
        return Spin{context.entity.id, properties.number("speed", 90.0f), glm::normalize(axis),
                    context.entity.local.rotation, context.time};
    }

    // From the time since it started, not added up step by step: no drift, and the same at any step
    void update(const GameStep& step, GameWorld&) const {
        if (Entity* e = step.scene.find(entity)) {  // (gone: removed while playing)
            double seconds = step.time - startTime;
            float angle = static_cast<float>(glm::radians(static_cast<double>(speed) * seconds));
            e->local.rotation = start * glm::angleAxis(angle, axis);
        }
    }

    // Turned by hand while paused: it turns on from there
    void moved(const EntityMove& move, GameWorld&) { start = followRotation(move, start); }
};

}  // namespace

std::unique_ptr<BehaviourListBase> makeSpinList() {
    return std::make_unique<BehaviourList<Spin>>();
}
