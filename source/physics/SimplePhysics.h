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
// - moveBodies tries the group's move at points along it no further apart than its thinnest body's half size (so it
//   doesn't pass through thin bodies), then halves the stretch between the last free point and the first blocked one
//   8 times. Overlaps shallower than contactSkin don't block (boxes resting on each other). A character touching a
//   moved body is pushed out on trial (as its next move would); left in something deeper than squeezeTolerance, it
//   is squeezed and the move is blocked.
//

#ifndef VULKANDEMO_SIMPLEPHYSICS_H
#define VULKANDEMO_SIMPLEPHYSICS_H

#include <vector>
#include "physics/PhysicsWorld.h"

namespace physics {

class SimplePhysicsWorld final : public PhysicsWorld {
public:
    static constexpr float minHalfThickness = 0.05f;
    static constexpr float contactSkin = 1e-3f;
    static constexpr float squeezeTolerance = 0.01f;

    BodyId addBox(const BoxShape& shape, const Transform& transform) override;
    void setTransform(BodyId body, const Transform& transform) override;
    void setEnabled(BodyId body, bool enabled) override;
    void remove(BodyId body) override;
    [[nodiscard]] size_t bodyCount() const override { return liveBodies; }
    GroupMove moveBodies(const std::vector<BodyId>& group, const Transform& from, const Transform& to) override;

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

    // A body of a group moved by moveBodies: its transform in the group's
    struct Mover {
        Body* body;
        Transform relative;
    };
    // What a group's move doesn't test against: its own bodies and what overlapped it at the start (by index)
    struct MoveSkips {
        std::vector<char> bodies;
        std::vector<char> characters;
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
    void resolve(const CharacterSettings& settings, Moving& moving, int rounds = 4) const;
    // Moved by delta in substeps, resolving after each
    Moving sweep(const CharacterSettings& settings, Moving moving, const glm::vec3& delta) const;
    // The deepest overlap of a character at feet with an enabled body (0: none)
    float deepestOverlap(const CharacterSettings& settings, const glm::vec3& feet) const;
    // Whether the character touches one of the movers and a trial push can't free it from everything
    bool squeezed(const Character& character, const std::vector<Mover>& movers) const;
    // The movers placed with the group at pivot: whether they overlap a body or squeeze a character (normal: the
    // deepest blocking body contact's, pushing the movers out; zero for a character)
    bool blockedAt(const std::vector<Mover>& movers, const Transform& pivot, const MoveSkips& skips,
                   glm::vec3& normal);
    // From a (free) towards b: the furthest free fraction of the way; blocked and normal as blockedAt's
    float sweepGroup(const std::vector<Mover>& movers, const Transform& a, const Transform& b, const MoveSkips& skips,
                     bool& blocked, glm::vec3& normal);
};

}  // namespace physics

#endif //VULKANDEMO_SIMPLEPHYSICS_H
