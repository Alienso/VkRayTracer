//
// spawner: copies of an entity that come and go. See game/Behaviour.h for what a type is made of.
//

#include <algorithm>
#include <memory>
#include "game/Behaviour.h"

namespace {

struct Spawner {
    static constexpr const char* type = "spawner";
    static constexpr const char* help =
        "Every interval seconds, a copy of target (an id; usually a hidden one) at the spawner, shown, with its\n"
        "components working (but not spawners); removed after lifetime seconds (0: kept); at most max at a time.";
    static constexpr const char* defaults = R"({"target": 0, "interval": 2, "lifetime": 5, "max": 10})";
    // Kept: a spawn request made this step points at its spawner (and no spawner is added while playing, so the
    // list doesn't move)
    static constexpr bool removedWithEntity = false;
    static constexpr bool onSpawnedCopies = false;  // (a copy that spawns copies: not)

    EntityId entity, target;
    float interval, lifetime;
    int max;
    double nextTime;
    std::vector<std::pair<EntityId, double>> spawned;  // and when

    static std::optional<Spawner> read(const ReadContext& context, GameProperties& properties) {
        const Scene& scene = context.scene;
        EntityId target = context.existing(properties, "target", true);
        if (target != 0 && scene.isUnder(context.entity.id, target)) {
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
        if (target == 0) {
            return std::nullopt;
        }
        return Spawner{context.entity.id, target, interval, lifetime, std::max(max, 0), interval, {}};
    }

    void update(const GameStep& step, GameWorld& world) {
        Scene& scene = step.scene;
        if (scene.find(entity) == nullptr) {
            return;
        }
        for (auto it = spawned.begin(); it != spawned.end();) {
            if (scene.find(it->first) == nullptr) {  // (taken as a pickup, say)
                it = spawned.erase(it);
            } else if (lifetime > 0.0f && step.time - it->second >= lifetime) {
                world.removals.push_back(it->first);
                it = spawned.erase(it);
            } else {
                ++it;
            }
        }
        if (step.time >= nextTime) {
            nextTime += interval;
            if (static_cast<int>(spawned.size()) < max && scene.find(target) != nullptr) {
                world.spawns.push_back({entity, target, [this](EntityId copy, double time) {
                                            spawned.emplace_back(copy, time);
                                        }});
            }
        }
    }
};

}  // namespace

std::unique_ptr<BehaviourListBase> makeSpawnerList() {
    return std::make_unique<BehaviourList<Spawner>>();
}
