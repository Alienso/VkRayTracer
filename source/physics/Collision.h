//
// Overlap tests between shapes (the narrow phase), used by SimplePhysics. Option B adds capsule against triangle here,
// giving the same Contact.
//

#ifndef VULKANDEMO_COLLISION_H
#define VULKANDEMO_COLLISION_H

#include <optional>
#include "physics/PhysicsTypes.h"

namespace physics {

// A capsule (the segment a..b with a radius) against a turned box: how to push the capsule out, or nothing when they
// don't overlap. The test runs in the box's own space, where it is an axis-aligned box.
std::optional<Contact> capsuleBoxContact(const glm::vec3& a, const glm::vec3& b, float radius, const Transform& box,
                                         const glm::vec3& halfExtents);

// The axis-aligned box around a turned box
void boxBounds(const Transform& box, const glm::vec3& halfExtents, glm::vec3& min, glm::vec3& max);

}  // namespace physics

#endif //VULKANDEMO_COLLISION_H
