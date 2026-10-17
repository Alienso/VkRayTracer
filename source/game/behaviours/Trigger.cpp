//
// trigger: a box that shows or hides another entity while the camera is inside. See game/Behaviour.h for what a type
// is made of.
//

#include <cmath>
#include <memory>
#include "game/Behaviour.h"

namespace {

struct Trigger {
    static constexpr const char* type = "trigger";
    static constexpr const char* help =
        "A box (the entity's transform: a unit cube around its origin, scaled and turned with it) that shows the\n"
        "entity target (an id) while the camera is inside, and hides it outside (show: false: the other way round).";
    static constexpr const char* defaults = R"({"target": 0, "show": true})";
    static constexpr bool removedWithEntity = true;
    static constexpr bool onSpawnedCopies = true;

    EntityId entity, target;
    bool show;
    int inside = -1;  // -1: not known yet (the first step sets the target either way)

    static std::optional<Trigger> read(const ReadContext& context, GameProperties& properties) {
        EntityId target = context.existing(properties, "target", true);
        bool show = properties.flag("show", true);
        if (target == 0) {
            return std::nullopt;
        }
        return Trigger{context.entity.id, target, show};
    }

    void update(const GameStep& step, GameWorld& world) {
        Scene& scene = step.scene;
        Entity* shown = scene.find(target);
        if (shown == nullptr || scene.find(entity) == nullptr) {
            return;
        }
        glm::vec3 local = glm::vec3(glm::inverse(scene.worldTransform(entity)) * glm::vec4(world.player, 1.0f));
        int now = std::abs(local.x) <= 0.5f && std::abs(local.y) <= 0.5f && std::abs(local.z) <= 0.5f ? 1 : 0;
        if (now != inside) {  // (only on a change: something else may show or hide it meanwhile)
            inside = now;
            shown->visible = (now == 1) == show;
        }
    }
};

}  // namespace

std::unique_ptr<BehaviourListBase> makeTriggerList() {
    return std::make_unique<BehaviourList<Trigger>>();
}
