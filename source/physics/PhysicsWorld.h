//
// Physics as the game sees it (PHYSICS_PLAN.md): colliders and a character (the player), behind an interface so the
// implementation can be swapped (our own SimplePhysics now; mesh collision or a library later) without touching the
// game. It knows nothing of the scene: the game hands it shapes and transforms and reads back where the character went.
//
// Colliders are static or moved by hand each step (kinematic): nothing here moves them on its own. Placed with
// setTransform they go anywhere (through other colliders); moved with moveBodies they stop at other colliders. The
// character is a capsule standing on its feet: moveCharacter walks it, with gravity, sliding along what it hits and
// stepping onto low ledges.
//
// Portals (PORTAL_PHYSICS_PLAN.md): given the openings (setPortals), a shape part way through one is in both places
// (its pieces, portals/PortalPieces.h): the character touches what is beyond the exit with its part that went in, and
// not what is behind the entrance in the opening (a wall with a portal in it has a hole there by itself). The
// character goes through when its anchor (the player's eyes) does: moveCharacter carries it to the other end.
//

#ifndef VULKANDEMO_PHYSICSWORLD_H
#define VULKANDEMO_PHYSICSWORLD_H

#include <memory>
#include <vector>
#include "physics/PhysicsTypes.h"
#include "portals/PortalPieces.h"

namespace physics {

struct CharacterSettings {
    CapsuleShape shape{0.3f, 0.6f};  // 1.8 m tall
    float maxSlopeDegrees = 45.0f;   // steeper surfaces are walls
    float stepHeight = 0.3f;         // ledges up to this high are stepped onto (and down from, walking)
    float airAcceleration = 10.0f;   // m/s^2 towards the wanted velocity while in the air (on the ground: at once)
    // The point this high above the feet goes through portals for the character (the player's eyes): when it goes
    // through an opening, the character comes out of the other end; until then the character is on its side
    float anchorHeight = 1.65f;
};

// Where a group of bodies moved with moveBodies got to
struct GroupMove {
    Transform reached;     // the group's transform where it stopped (the target when nothing blocked it)
    bool blocked = false;
};

struct CharacterState {
    glm::vec3 feet = glm::vec3(0.0f);  // the capsule's lowest point
    glm::vec3 velocity = glm::vec3(0.0f);
    bool onGround = false;
    glm::vec3 groundNormal = glm::vec3(0.0f, 1.0f, 0.0f);  // (when on the ground)
    // The last moveCharacter took it through portals: what that did to points (feet, velocity and ground are
    // already through)
    bool crossedPortal = false;
    glm::mat4 portalCrossing = glm::mat4(1.0f);
};

class PhysicsWorld {
public:
    virtual ~PhysicsWorld() = default;

    // The portal openings (pairs: 2k and 2k + 1 are pair k's ends) and how many crossings a shape's pieces go
    // through. None: plain collision, as without portals.
    virtual void setPortals(const std::vector<portals::PortalOpening>& openings, int maxDepth) = 0;

    // Colliders
    virtual BodyId addBox(const BoxShape& shape, const Transform& transform) = 0;
    virtual void setTransform(BodyId body, const Transform& transform) = 0;
    virtual void setEnabled(BodyId body, bool enabled) = 0;  // a disabled body is passed through
    virtual void remove(BodyId body) = 0;
    [[nodiscard]] virtual size_t bodyCount() const = 0;
    // Moves the bodies together as one rigid thing whose transform goes from `from` (where they are now) to `to`
    // (positions in a line, the turn the short way), as far as it can go without overlapping other enabled bodies or
    // squeezing a character (pushing one is fine: its next move pushes it out; it is squeezed when that push can't
    // free it), through portals too (and into their own parts out of another end). Blocked, the straight part of what is left slides along what stopped it (twice at most); a turn only
    // stops. What overlaps them where they start doesn't block them, so things already in each other can part.
    virtual GroupMove moveBodies(const std::vector<BodyId>& group, const Transform& from, const Transform& to) = 0;

    // The character
    virtual CharacterId addCharacter(const CharacterSettings& settings, const glm::vec3& feet) = 0;
    virtual void removeCharacter(CharacterId character) = 0;
    [[nodiscard]] virtual CharacterState character(CharacterId character) const = 0;
    // A jump to another place (a respawn, a portal), going on at the given velocity
    virtual void teleport(CharacterId character, const glm::vec3& feet, const glm::vec3& velocity) = 0;
    // One step of the given length: walks at the wanted horizontal velocity (y ignored), jumps when jumpSpeed > 0
    // and it stands on the ground, falls by gravity; collides and slides; goes through portals (crossedPortal).
    // Returns where it is now.
    virtual CharacterState moveCharacter(CharacterId character, const glm::vec3& wantedVelocity, float jumpSpeed,
                                         const glm::vec3& gravity, float seconds) = 0;

    // Whether a capsule (its segment along the transform's Y, centered on its position) overlaps an enabled body
    [[nodiscard]] virtual bool overlapsCapsule(const CapsuleShape& shape, const Transform& transform) const = 0;

    virtual void clear() = 0;  // all bodies and characters (the openings stay)
};

// Our own implementation: boxes and one capsule character (physics/SimplePhysics.h)
std::unique_ptr<PhysicsWorld> makeSimplePhysicsWorld();

}  // namespace physics

#endif //VULKANDEMO_PHYSICSWORLD_H
