//
// The physics' own small types (physics/PhysicsWorld.h). Plain values and glm: nothing of the scene, the assets or the
// game, so another implementation (or library) can take them as they are. Metres, seconds, +Y up.
//

#ifndef VULKANDEMO_PHYSICSTYPES_H
#define VULKANDEMO_PHYSICSTYPES_H

#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace physics {

// A place and a turn (no scale: shapes are given at their size)
struct Transform {
    glm::vec3 position = glm::vec3(0.0f);
    glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
};

// A box around its transform's position, turned with it
struct BoxShape {
    glm::vec3 halfExtents = glm::vec3(0.5f);
};

// A segment along Y with a radius around it: 2 * (halfHeight + radius) tall
struct CapsuleShape {
    float radius = 0.3f;
    float halfHeight = 0.6f;
};

// Handles (0: none). An id is used again after its body or character is removed.
struct BodyId {
    uint32_t value = 0;
    [[nodiscard]] bool valid() const { return value != 0; }
};
struct CharacterId {
    uint32_t value = 0;
    [[nodiscard]] bool valid() const { return value != 0; }
};

// Where two shapes overlap: push the first one along normal by depth to part them
struct Contact {
    glm::vec3 normal = glm::vec3(0.0f, 1.0f, 0.0f);  // unit length, away from the other shape
    float depth = 0.0f;
    glm::vec3 point = glm::vec3(0.0f);               // on the other shape's surface
};

}  // namespace physics

#endif //VULKANDEMO_PHYSICSTYPES_H
