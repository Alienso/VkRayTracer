//
// Overlap tests between shapes (the narrow phase), used by SimplePhysics. Option B adds capsule and box against
// triangle here, giving the same Contact.
//

#ifndef VULKANDEMO_COLLISION_H
#define VULKANDEMO_COLLISION_H

#include <optional>
#include <vector>
#include "physics/PhysicsTypes.h"

namespace physics {

// A capsule (the segment a..b with a radius) against a turned box: how to push the capsule out, or nothing when they
// don't overlap. The test runs in the box's own space, where it is an axis-aligned box.
std::optional<Contact> capsuleBoxContact(const glm::vec3& a, const glm::vec3& b, float radius, const Transform& box,
                                         const glm::vec3& halfExtents);

// Two turned boxes (the separating-axis test: their 3 + 3 face normals and the 9 crosses of their edges): how to push
// the first one out of the second, or nothing when they don't overlap. The contact's point is the deepest of its
// points, on the second box's surface; points, when given, gets them all (the faces' overlap: the first face's corners
// clipped to the other face and kept where they go in; for two edges, the nearest point of the second's edge).
std::optional<Contact> boxBoxContact(const Transform& a, const glm::vec3& halfA, const Transform& b,
                                     const glm::vec3& halfB, std::vector<glm::vec3>* points = nullptr);

// The axis-aligned box around a turned box
void boxBounds(const Transform& box, const glm::vec3& halfExtents, glm::vec3& min, glm::vec3& max);

}  // namespace physics

#endif //VULKANDEMO_COLLISION_H
