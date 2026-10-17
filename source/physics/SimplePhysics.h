//
// Our own PhysicsWorld (physics/PhysicsWorld.h): turned boxes as colliders, capsule characters that collide and slide.
//
// - Bodies are kept in a list (no tree): the character tests only those whose bounds meet its own. Fine for the
//   hundreds of hand-placed colliders it is for; many more (mesh colliders, option B) want a tree behind the same
//   interface.
// - A box thinner than minHalfThickness on an axis is grown to it (downwards on its Y: a floor's flat bounds), so
//   the character can stand on it without falling through.
// - The character moves in substeps of at most half its radius (no tunnelling through thin walls at running speed),
//   each pushed out of what it overlaps, deepest first. A walkable surface (within maxSlope of up) pushes it straight
//   up, so it doesn't slide down slopes it stands on; a steeper one only sideways, so it can't climb it. Blocked
//   while walking, it tries the same substep raised by stepHeight and keeps it if it lands on something; walking off
//   a ledge or down a slope it is pulled down by up to stepHeight to stay on the ground.
//

#ifndef VULKANDEMO_SIMPLEPHYSICS_H
#define VULKANDEMO_SIMPLEPHYSICS_H

#include <vector>
#include "physics/PhysicsWorld.h"

namespace physics {

class SimplePhysicsWorld final : public PhysicsWorld {
public:
    static constexpr float minHalfThickness = 0.05f;

    BodyId addBox(const BoxShape& shape, const Transform& transform) override;
    void setTransform(BodyId body, const Transform& transform) override;
    void setEnabled(BodyId body, bool enabled) override;
    void remove(BodyId body) override;
    [[nodiscard]] size_t bodyCount() const override { return liveBodies; }

    CharacterId addCharacter(const CharacterSettings& settings, const glm::vec3& feet) override;
    void removeCharacter(CharacterId character) override;
    [[nodiscard]] CharacterState character(CharacterId character) const override;
    void teleport(CharacterId character, const glm::vec3& feet, const glm::vec3& velocity) override;
    CharacterState moveCharacter(CharacterId character, const glm::vec3& wantedVelocity, float jumpSpeed,
                                 const glm::vec3& gravity, float seconds) override;

    [[nodiscard]] bool overlapsCapsule(const CapsuleShape& shape, const Transform& transform) const override;

    void clear() override;

private:
    struct Body {
        glm::vec3 halfExtents;  // grown to minHalfThickness
        glm::vec3 offset;       // of the grown box's center, in the box's own space
        Transform transform;    // as given
        Transform solid;        // the grown box's
        glm::vec3 min, max;     // its bounds
        bool enabled = true;
        bool alive = false;
    };
    struct Character {
        CharacterSettings settings;
        CharacterState state;
        bool alive = false;
    };
    // A character on its way through one step
    struct Moving {
        glm::vec3 feet;
        glm::vec3 velocity;
        bool ground = false;
        glm::vec3 groundNormal = glm::vec3(0.0f, 1.0f, 0.0f);
        bool wall = false;  // something too steep to stand on stopped it
    };

    std::vector<Body> bodies;
    std::vector<uint32_t> freeBodies;
    size_t liveBodies = 0;
    std::vector<Character> characters;
    std::vector<uint32_t> freeCharacters;

    Body* findBody(BodyId id);
    Character* findCharacter(CharacterId id);
    const Character* findCharacter(CharacterId id) const;
    static void place(Body& body, const Transform& transform);
    // Pushed out of what it overlaps (ground and wall set by what it touched)
    void resolve(const CharacterSettings& settings, Moving& moving) const;
    // Moved by delta in substeps, resolving after each
    Moving sweep(const CharacterSettings& settings, Moving moving, const glm::vec3& delta) const;
};

}  // namespace physics

#endif //VULKANDEMO_SIMPLEPHYSICS_H
