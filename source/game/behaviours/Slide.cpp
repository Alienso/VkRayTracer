//
// slide: moves the entity out along an axis and back, from where it was at Play. See game/Behaviour.h for what a type
// is made of.
//

#include <cmath>
#include <memory>
#include <glm/gtc/constants.hpp>
#include "game/Behaviour.h"

namespace {

struct Slide {
    static constexpr const char* type = "slide";
    static constexpr const char* help =
        "Moves the entity out along an axis and back, from where it was at Play (bob moves up and down).\n"
        "axis: in the entity's own space (turned with it); distance: units out; period: seconds out and back.";
    static constexpr const char* defaults = R"({"axis": [1, 0, 0], "distance": 2, "period": 4})";
    static constexpr bool removedWithEntity = true;
    static constexpr bool onSpawnedCopies = true;

    EntityId entity;
    glm::vec3 direction;  // the axis in the parent's space (the entity's rotation at Play applied), unit length
    float distance;
    float period;
    glm::vec3 start;      // the position at Play (or when spawned)
    double startTime;

    static std::optional<Slide> read(const ReadContext& context, GameProperties& properties) {
        glm::vec3 axis = properties.vec3("axis", glm::vec3(1.0f, 0.0f, 0.0f));
        if (glm::length(axis) < 1e-6f) {
            properties.warn("\"axis\" can't be [0, 0, 0]; [1, 0, 0] is used");
            axis = glm::vec3(1.0f, 0.0f, 0.0f);
        }
        float distance = properties.number("distance", 2.0f);
        float period = properties.number("period", 4.0f);
        if (!(period > 0.0f)) {
            properties.warn("\"period\" must be more than 0; 4 is used");
            period = 4.0f;
        }
        const Transform& local = context.entity.local;
        return Slide{context.entity.id, glm::normalize(local.rotation * axis), distance, period, local.position,
                     context.time};
    }

    // From the time since it started (as bob): out to distance and back, easing at both ends
    void update(const GameStep& step, GameWorld&) const {
        if (Entity* e = step.scene.find(entity)) {
            double phase = (step.time - startTime) / static_cast<double>(period) * 2.0 * glm::pi<double>();
            float out = distance * 0.5f * static_cast<float>(1.0 - std::cos(phase));
            e->local.position = start + direction * out;
        }
    }

    // Put somewhere else (by hand while paused, or through a portal): it slides from there, along its turned axis
    void moved(const EntityMove& move, GameWorld&) {
        glm::quat turn = move.newLocal.rotation * glm::inverse(move.oldLocal.rotation);
        start = move.newLocal.position + turn * (start - move.oldLocal.position);
        direction = glm::normalize(turn * direction);
    }
};

}  // namespace

std::unique_ptr<BehaviourListBase> makeSlideList() {
    return std::make_unique<BehaviourList<Slide>>();
}
