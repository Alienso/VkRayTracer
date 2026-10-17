//
// bob: moves the entity up and down from where it was at Play. See game/Behaviour.h for what a type is made of.
//

#include <cmath>
#include <memory>
#include <glm/gtc/constants.hpp>
#include "game/Behaviour.h"

namespace {

struct Bob {
    static constexpr const char* type = "bob";
    static constexpr const char* help = "Moves the entity up and down from where it was at Play.\n"
                                        "height: units; period: seconds for one up and down.";
    static constexpr const char* defaults = R"({"height": 0.25, "period": 2})";
    static constexpr bool removedWithEntity = true;
    static constexpr bool onSpawnedCopies = true;

    EntityId entity;
    float height;
    float period;
    glm::vec3 start;   // the position at Play (or when spawned)
    double startTime;

    static std::optional<Bob> read(const ReadContext& context, GameProperties& properties) {
        float period = properties.number("period", 2.0f);
        if (!(period > 0.0f)) {
            properties.warn("\"period\" must be more than 0; 2 is used");
            period = 2.0f;
        }
        return Bob{context.entity.id, properties.number("height", 0.25f), period, context.entity.local.position,
                   context.time};
    }

    // From the time since it started (as spin)
    void update(const GameStep& step, GameWorld&) const {
        if (Entity* e = step.scene.find(entity)) {
            double phase = (step.time - startTime) / static_cast<double>(period) * 2.0 * glm::pi<double>();
            e->local.position = start + glm::vec3(0.0f, height * static_cast<float>(std::sin(phase)), 0.0f);
        }
    }

    // Put somewhere else by hand while paused: it bobs around there
    void moved(const EntityMove& move, GameWorld&) { start += move.newLocal.position - move.oldLocal.position; }
};

}  // namespace

std::unique_ptr<BehaviourListBase> makeBobList() {
    return std::make_unique<BehaviourList<Bob>>();
}
