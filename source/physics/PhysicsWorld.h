//
// Physics as the game sees it (PHYSICS_PLAN.md): colliders and a character (the player), behind an interface so the
// implementation can be swapped (our own SimplePhysics now; mesh collision or a library later) without touching the
// game. It knows nothing of the scene: the game hands it shapes and transforms and reads back where the character went.
//
// Colliders are static or moved by hand each step (kinematic): nothing here moves them, and they don't collide with
// each other. The character is a capsule standing on its feet: moveCharacter walks it, with gravity, sliding along
// what it hits and stepping onto low ledges.
//

#ifndef VULKANDEMO_PHYSICSWORLD_H
#define VULKANDEMO_PHYSICSWORLD_H

#include <memory>
#include "physics/PhysicsTypes.h"

namespace physics {

struct CharacterSettings {
    CapsuleShape shape{0.3f, 0.6f};  // 1.8 m tall
    float maxSlopeDegrees = 45.0f;   // steeper surfaces are walls
    float stepHeight = 0.3f;         // ledges up to this high are stepped onto (and down from, walking)
    float airAcceleration = 10.0f;   // m/s^2 towards the wanted velocity while in the air (on the ground: at once)
};

struct CharacterState {
    glm::vec3 feet = glm::vec3(0.0f);  // the capsule's lowest point
    glm::vec3 velocity = glm::vec3(0.0f);
    bool onGround = false;
    glm::vec3 groundNormal = glm::vec3(0.0f, 1.0f, 0.0f);  // (when on the ground)
};

class PhysicsWorld {
public:
    virtual ~PhysicsWorld() = default;

    // Colliders
    virtual BodyId addBox(const BoxShape& shape, const Transform& transform) = 0;
    virtual void setTransform(BodyId body, const Transform& transform) = 0;
    virtual void setEnabled(BodyId body, bool enabled) = 0;  // a disabled body is passed through
    virtual void remove(BodyId body) = 0;
    [[nodiscard]] virtual size_t bodyCount() const = 0;

    // The character
    virtual CharacterId addCharacter(const CharacterSettings& settings, const glm::vec3& feet) = 0;
    virtual void removeCharacter(CharacterId character) = 0;
    [[nodiscard]] virtual CharacterState character(CharacterId character) const = 0;
    // A jump to another place (a respawn, a portal), going on at the given velocity
    virtual void teleport(CharacterId character, const glm::vec3& feet, const glm::vec3& velocity) = 0;
    // One step of the given length: walks at the wanted horizontal velocity (y ignored), jumps when jumpSpeed > 0
    // and it stands on the ground, falls by gravity; collides and slides. Returns where it is now.
    virtual CharacterState moveCharacter(CharacterId character, const glm::vec3& wantedVelocity, float jumpSpeed,
                                         const glm::vec3& gravity, float seconds) = 0;

    // Whether a capsule (its segment along the transform's Y, centered on its position) overlaps an enabled body
    [[nodiscard]] virtual bool overlapsCapsule(const CapsuleShape& shape, const Transform& transform) const = 0;

    virtual void clear() = 0;  // all bodies and characters
};

// Our own implementation: boxes and one capsule character (physics/SimplePhysics.h)
std::unique_ptr<PhysicsWorld> makeSimplePhysicsWorld();

}  // namespace physics

#endif //VULKANDEMO_PHYSICSWORLD_H
