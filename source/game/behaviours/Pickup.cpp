//
// pickup: taken when the camera comes near, counted in the overlay. See game/Behaviour.h for what a type is made of.
//

#include <memory>
#include "game/Behaviour.h"

namespace {

struct Pickup {
    static constexpr const char* type = "pickup";
    static constexpr const char* help =
        "Taken when the camera comes within radius: hidden (or removed, with remove: true); counted in the overlay\n"
        "(\"Pickups: 2 / 5\"). Only while it is shown: one hidden at Play (a spawner's target) counts once something\n"
        "shows it.";
    static constexpr const char* defaults = R"({"radius": 1, "remove": false})";
    static constexpr bool removedWithEntity = false;  // the count keeps the ones taken
    static constexpr bool onSpawnedCopies = true;

    EntityId entity;
    float radius;
    bool remove;
    bool taken = false;
    bool counted = false;  // shown at some point: one of the "of N"

    static std::optional<Pickup> read(const ReadContext& context, GameProperties& properties) {
        Pickup pickup{context.entity.id, properties.number("radius", 1.0f), properties.flag("remove", false)};
        if (context.scene.visibleInWorld(context.entity.id)) {
            pickup.counted = true;
            context.world.pickupsCounted++;
        }
        return pickup;
    }

    void update(const GameStep& step, GameWorld& world) {
        Entity* e = step.scene.find(entity);
        if (e == nullptr || taken || !step.scene.visibleInWorld(entity)) {
            return;
        }
        if (!counted) {
            counted = true;
            world.pickupsCounted++;
        }
        if (glm::distance(world.player, worldPosition(step.scene, entity)) <= radius) {
            taken = true;
            world.pickupsTaken++;
            if (remove) {
                world.removals.push_back(entity);
            } else {
                e->visible = false;
            }
        }
    }

    // Deleted (or its properties changed) while paused: one not taken yet isn't one of the "of N" any more
    void removed(GameWorld& world) const {
        if (counted && !taken) {
            world.pickupsCounted--;
        }
    }
};

}  // namespace

std::unique_ptr<BehaviourListBase> makePickupList() {
    return std::make_unique<BehaviourList<Pickup>>();
}
