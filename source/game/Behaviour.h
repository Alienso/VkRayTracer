//
// What a behaviour type is made of (game/behaviours/*.cpp). A type is a struct with its typed values per entity and:
//   static constexpr const char* type      its name in the scene file ("spin")
//   static constexpr const char* help      what it does, shown in the editor
//   static constexpr const char* defaults  its properties with their default values, as a JSON object (the editor
//                                          fills a new component with them; tests check they are the keys it reads)
//   static constexpr bool removedWithEntity     dropped when its entity is removed while playing (false: kept, e.g.
//                                               for a count)
//   static constexpr bool onSpawnedCopies       read on a spawner's copies too
//   static std::optional<T> read(const ReadContext&, GameProperties&)   at Play (or when spawned): its values, or
//                                               nothing (warned about through the properties)
//   void update(const GameStep&, GameWorld&)    each step
// BehaviourList<T> keeps one type's behaviours in a list and does the rest (Stop, removed entities, counting); the
// types are listed once, in game/Behaviours.cpp, in update order.
//

#ifndef VULKANDEMO_BEHAVIOUR_H
#define VULKANDEMO_BEHAVIOUR_H

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "core/Input.h"
#include "game/GameProperties.h"
#include "game/StepInterpolation.h"
#include "scene/Scene.h"

// What one step works with
struct GameStep {
    Scene& scene;
    RenderSettings& settings;  // the program's; the scene's fields of it are put back by Stop with the scene
    const InputState& input;   // what happened since the last step (GameLoop)
    double seconds;            // the step's length: GameLoop::stepSeconds
    double time;               // since Play, at the end of this step
    uint64_t index;            // steps before this one
    StepInterpolation* interpolation = nullptr;  // how the steps are shown (none in some tests)

    // The entity's change in this step is a jump (a respawn, a teleport): shown at once, not blended from where it
    // was
    void snap(EntityId id) const {
        if (interpolation != nullptr) {
            interpolation->snap(id);
        }
    }
    void snapCamera() const {
        if (interpolation != nullptr) {
            interpolation->snapCamera();
        }
    }
};

// A copy of target at the entity "at", made after the step's behaviours (Game::applyQueue); made(copy, time) is told
// the copy's id
struct SpawnRequest {
    EntityId at;
    EntityId target;
    std::function<void(EntityId copy, double time)> made;
};

// What the behaviours share while playing: the player, the counts the overlay shows, and the changes that add or
// remove entities (applied after a step's behaviours: they move the scene's entities)
struct GameWorld {
    glm::vec3 player = glm::vec3(0.0f);  // the camera's position at the start of the step
    int pickupsTaken = 0;
    int pickupsCounted = 0;      // pickups shown at some point: the "of N"
    bool controlsCamera = false; // a camera path drives the camera
    std::vector<EntityId> removals;
    std::vector<SpawnRequest> spawns;
};

// Where a behaviour is read from
struct ReadContext {
    const Scene& scene;
    const Entity& entity;
    bool spawned;  // a copy made while playing, starting now
    double time;   // now
    GameWorld& world;

    // An id property that must name an entity there is (0: not given, or not an id: warned already)
    EntityId existing(GameProperties& properties, const char* key, bool required) const {
        EntityId id = properties.entity(key);
        if (id != 0 && scene.find(id) == nullptr) {
            properties.warn(std::string("\"") + key + "\": there is no entity " + std::to_string(id));
            return 0;
        }
        if (id == 0 && required) {
            properties.warn(std::string("needs \"") + key + "\": an entity's id; ignored");
        }
        return id;
    }
};

inline glm::vec3 worldPosition(const Scene& scene, EntityId id) {
    return glm::vec3(scene.worldTransform(id)[3]);
}

class BehaviourListBase {
public:
    virtual ~BehaviourListBase() = default;
    [[nodiscard]] virtual const char* type() const = 0;
    [[nodiscard]] virtual const char* help() const = 0;
    [[nodiscard]] virtual const char* defaults() const = 0;
    [[nodiscard]] virtual bool onSpawnedCopies() const = 0;
    [[nodiscard]] virtual size_t size() const = 0;
    virtual void read(const ReadContext& context, GameProperties& properties) = 0;
    virtual void update(const GameStep& step, GameWorld& world) = 0;
    virtual void removeGone(const Scene& scene) = 0;  // the behaviours of entities removed while playing
    virtual void clear() = 0;
};

template <typename T> class BehaviourList : public BehaviourListBase {
public:
    [[nodiscard]] const char* type() const override { return T::type; }
    [[nodiscard]] const char* help() const override { return T::help; }
    [[nodiscard]] const char* defaults() const override { return T::defaults; }
    [[nodiscard]] bool onSpawnedCopies() const override { return T::onSpawnedCopies; }
    [[nodiscard]] size_t size() const override { return items.size(); }
    void read(const ReadContext& context, GameProperties& properties) override {
        if (std::optional<T> behaviour = T::read(context, properties)) {
            items.push_back(std::move(*behaviour));
        }
    }
    void update(const GameStep& step, GameWorld& world) override {
        for (T& behaviour : items) {
            behaviour.update(step, world);
        }
    }
    void removeGone(const Scene& scene) override {
        if constexpr (T::removedWithEntity) {
            items.erase(std::remove_if(items.begin(), items.end(),
                                       [&](const T& behaviour) { return scene.find(behaviour.entity) == nullptr; }),
                        items.end());
        }
    }
    void clear() override { items.clear(); }

private:
    std::vector<T> items;
};

#endif //VULKANDEMO_BEHAVIOUR_H
