//
// collider: a solid box the player can't walk through (PHYSICS_PLAN.md), following its entity. See game/Behaviour.h
// for what a type is made of.
//
// A "solid" collider also doesn't go through other colliders when the game moves its entity: the colliders under the
// topmost entity that moved this step are moved as one group (PhysicsWorld::moveBodies: a platform and the box on it
// stop together), and a blocked group's entity is held where it got to. The behaviours that moved it aren't told:
// they keep their own state (a door its open amount, a bob its time) and the entity follows them again once free.
//

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>
#include "game/Behaviour.h"
#include "game/Colliders.h"

void colliderBox(const Scene& scene, EntityId entity, GameProperties& properties, glm::vec3& center,
                 glm::vec3& halfSize) {
    glm::vec3 size = properties.vec3("size", glm::vec3(0.0f));
    glm::vec3 offset = properties.vec3("offset", glm::vec3(0.0f));
    center = offset;
    halfSize = glm::abs(size) * 0.5f;
    if (size == glm::vec3(0.0f)) {
        glm::vec3 min, max;
        if (scene.modelBox(entity, min, max)) {
            center = (min + max) * 0.5f;
            halfSize = (max - min) * 0.5f;
        } else {
            center = glm::vec3(0.0f);
            halfSize = glm::vec3(0.5f);
        }
    }
}

std::vector<WorldBox> colliderBoxes(const Scene& scene, EntityId only) {
    std::vector<WorldBox> boxes;
    for (const Entity& entity : scene.entities()) {
        const std::vector<GameComponent>* components = scene.game(entity.id);
        if (components == nullptr || (only != 0 && entity.id != only) ||
            (only == 0 && !scene.visibleInWorld(entity.id))) {
            continue;
        }
        for (const GameComponent& component : *components) {
            if (component.type == "collider") {
                std::vector<std::string> ignored;  // (said at Play)
                GameProperties properties(component.properties, "", ignored);
                glm::vec3 center, halfSize;
                colliderBox(scene, entity.id, properties, center, halfSize);
                boxes.push_back({scene.worldTransform(entity.id), center - halfSize, center + halfSize});
                break;  // (the same type twice: only the first is used)
            }
        }
    }
    return boxes;
}

namespace {

// A world transform without its scale, as the physics takes it
physics::Transform rigidOf(const glm::mat4& world) {
    glm::mat3 turn(1.0f);
    for (int axis = 0; axis < 3; axis++) {
        float scale = glm::length(glm::vec3(world[axis]));
        if (scale > 1e-12f) {
            turn[axis] = glm::vec3(world[axis]) / scale;
        }
    }
    return {glm::vec3(world[3]), glm::normalize(glm::quat_cast(turn))};
}

glm::mat4 matrixOf(const physics::Transform& transform) {
    glm::mat4 matrix = glm::mat4_cast(transform.rotation);
    matrix[3] = glm::vec4(transform.position, 1.0f);
    return matrix;
}

struct Collider {
    static constexpr const char* type = "collider";
    static constexpr const char* help =
        "A solid box: the player can't walk through it and stands on it. size [0, 0, 0]: around the entity's model\n"
        "(a unit cube without one); else that size, centered at offset, in the entity's own space (scaled with it).\n"
        "It moves with the entity (a door, a platform); while the entity is hidden it isn't solid.\n"
        "solid: when the game moves it, it stops at other colliders (and when it would squeeze the player) instead\n"
        "of going through them; pushing the player is fine.";
    static constexpr const char* defaults = R"({"size": [0, 0, 0], "offset": [0, 0, 0], "solid": false})";
    static constexpr bool removedWithEntity = true;
    static constexpr bool onSpawnedCopies = true;

    EntityId entity;
    glm::vec3 center;      // the box in the entity's own space
    glm::vec3 halfSize;
    physics::BodyId body;
    glm::mat4 placedBy = glm::mat4(0.0f);  // the entity's world transform the body was placed by
    glm::vec3 placedHalf = glm::vec3(0.0f);
    bool enabled = true;
    bool solid = false;
    // Solid: the entity and its parents (root first) with the world transforms they had when the body was placed
    std::vector<std::pair<EntityId, glm::mat4>> placedChain;

    // The body's box and transform for the entity's world transform (its scale goes into the box's size)
    void boxFor(const glm::mat4& world, physics::BoxShape& shape, physics::Transform& transform) const {
        glm::vec3 scale(glm::length(glm::vec3(world[0])), glm::length(glm::vec3(world[1])),
                        glm::length(glm::vec3(world[2])));
        shape.halfExtents = halfSize * scale;
        transform.position = glm::vec3(world * glm::vec4(center, 1.0f));
        transform.rotation = rigidOf(world).rotation;
    }

    void place(const Scene& scene, physics::PhysicsWorld& physics) {
        glm::mat4 world = scene.worldTransform(entity);
        if (body.valid() && world == placedBy) {
            return;
        }
        physics::BoxShape shape;
        physics::Transform transform;
        boxFor(world, shape, transform);
        if (!body.valid() || glm::any(glm::greaterThan(glm::abs(shape.halfExtents - placedHalf), glm::vec3(1e-6f)))) {
            if (body.valid()) {
                physics.remove(body);  // (scaled: a new box)
            }
            body = physics.addBox(shape, transform);
            physics.setEnabled(body, enabled);
            placedHalf = shape.halfExtents;
        } else {
            physics.setTransform(body, transform);
        }
        placedBy = world;
        if (solid) {
            placedChain.clear();
            for (EntityId id = entity; id != 0; id = scene.find(id)->parent) {
                placedChain.emplace_back(id, scene.worldTransform(id));
            }
            std::reverse(placedChain.begin(), placedChain.end());
        }
    }

    // The topmost of its entity and its parents that moved since the body was placed: the one whose move this is
    // (0: none, or one of them jumped: put there, not moved)
    [[nodiscard]] EntityId mover(const Scene& scene, const GameWorld& world) const {
        for (const auto& [id, placed] : placedChain) {
            if (scene.find(id) == nullptr ||
                std::find(world.jumped.begin(), world.jumped.end(), id) != world.jumped.end()) {
                return 0;
            }
        }
        for (const auto& [id, placed] : placedChain) {
            if (scene.worldTransform(id) != placed) {
                return id;
            }
        }
        return 0;
    }
    [[nodiscard]] glm::mat4 placedWorld(EntityId id) const {
        for (const auto& [chained, placed] : placedChain) {
            if (chained == id) {
                return placed;
            }
        }
        return glm::mat4(1.0f);
    }

    static std::optional<Collider> read(const ReadContext& context, GameProperties& properties) {
        Collider collider;
        collider.entity = context.entity.id;
        colliderBox(context.scene, context.entity.id, properties, collider.center, collider.halfSize);
        collider.solid = properties.flag("solid", false);
        if (context.world.physics != nullptr) {
            collider.enabled = context.scene.visibleInWorld(context.entity.id);
            collider.place(context.scene, *context.world.physics);
        }
        return collider;
    }

    // Each step: the solid ones whose entity moved are moved there as far as they can go, in groups by the topmost
    // entity that moved; then every collider follows its entity
    static void updateAll(std::vector<Collider>& colliders, const GameStep& step, GameWorld& world) {
        if (world.physics == nullptr) {
            return;
        }
        Scene& scene = step.scene;
        std::vector<std::pair<EntityId, std::vector<size_t>>> groups;  // the entity that moved, its solid colliders
        for (size_t i = 0; i < colliders.size(); i++) {
            const Collider& collider = colliders[i];
            if (!collider.solid || !collider.enabled || !collider.body.valid() ||
                scene.find(collider.entity) == nullptr || scene.worldTransform(collider.entity) == collider.placedBy) {
                continue;
            }
            EntityId mover = collider.mover(scene, world);
            if (mover == 0) {
                continue;
            }
            auto group = std::find_if(groups.begin(), groups.end(), [&](const auto& g) { return g.first == mover; });
            if (group == groups.end()) {
                groups.push_back({mover, {i}});
            } else {
                group->second.push_back(i);
            }
        }
        for (const auto& [mover, members] : groups) {
            std::vector<physics::BodyId> bodies;
            for (size_t i : members) {
                bodies.push_back(colliders[i].body);
            }
            physics::Transform to = rigidOf(scene.worldTransform(mover));
            physics::GroupMove move =
                world.physics->moveBodies(bodies, rigidOf(colliders[members[0]].placedWorld(mover)), to);
            if (move.blocked) {
                moveInWorld(scene, mover, matrixOf(move.reached) * glm::inverse(matrixOf(to)));  // held there
            }
        }
        for (Collider& collider : colliders) {
            collider.follow(step, world);
        }
    }

    // Follows the entity (moved by the game, or by hand while paused), solid while it is shown
    void follow(const GameStep& step, GameWorld& world) {
        if (step.scene.find(entity) == nullptr) {
            return;
        }
        place(step.scene, *world.physics);
        bool shown = step.scene.visibleInWorld(entity);
        if (shown != enabled) {
            enabled = shown;
            world.physics->setEnabled(body, enabled);
        }
    }

    void removed(GameWorld& world) const {
        if (world.physics != nullptr) {
            world.physics->remove(body);
        }
    }
};

}  // namespace

std::unique_ptr<BehaviourListBase> makeColliderList() {
    return std::make_unique<BehaviourList<Collider>>();
}
