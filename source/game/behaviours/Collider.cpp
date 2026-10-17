//
// collider: a solid box the player can't walk through (PHYSICS_PLAN.md), following its entity. See game/Behaviour.h
// for what a type is made of.
//

#include <cmath>
#include <memory>
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

struct Collider {
    static constexpr const char* type = "collider";
    static constexpr const char* help =
        "A solid box: the player can't walk through it and stands on it. size [0, 0, 0]: around the entity's model\n"
        "(a unit cube without one); else that size, centered at offset, in the entity's own space (scaled with it).\n"
        "It moves with the entity (a door, a platform); while the entity is hidden it isn't solid.";
    static constexpr const char* defaults = R"({"size": [0, 0, 0], "offset": [0, 0, 0]})";
    static constexpr bool removedWithEntity = true;
    static constexpr bool onSpawnedCopies = true;

    EntityId entity;
    glm::vec3 center;      // the box in the entity's own space
    glm::vec3 halfSize;
    physics::BodyId body;
    glm::mat4 placedBy = glm::mat4(0.0f);  // the entity's world transform the body was placed by
    glm::vec3 placedHalf = glm::vec3(0.0f);
    bool enabled = true;

    // The body's box and transform for the entity's world transform (its scale goes into the box's size)
    void boxFor(const glm::mat4& world, physics::BoxShape& shape, physics::Transform& transform) const {
        glm::vec3 scale(glm::length(glm::vec3(world[0])), glm::length(glm::vec3(world[1])),
                        glm::length(glm::vec3(world[2])));
        glm::mat3 turn(1.0f);
        for (int axis = 0; axis < 3; axis++) {
            if (scale[axis] > 1e-12f) {
                turn[axis] = glm::vec3(world[axis]) / scale[axis];
            }
        }
        shape.halfExtents = halfSize * scale;
        transform.position = glm::vec3(world * glm::vec4(center, 1.0f));
        transform.rotation = glm::normalize(glm::quat_cast(turn));
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
    }

    static std::optional<Collider> read(const ReadContext& context, GameProperties& properties) {
        Collider collider{context.entity.id};
        colliderBox(context.scene, context.entity.id, properties, collider.center, collider.halfSize);
        if (context.world.physics != nullptr) {
            collider.enabled = context.scene.visibleInWorld(context.entity.id);
            collider.place(context.scene, *context.world.physics);
        }
        return collider;
    }

    // Follows the entity (moved by the game, or by hand while paused), solid while it is shown
    void update(const GameStep& step, GameWorld& world) {
        if (world.physics == nullptr || step.scene.find(entity) == nullptr) {
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
