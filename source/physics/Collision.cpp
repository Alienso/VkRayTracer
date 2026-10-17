//
// Overlap tests between shapes. See Collision.h.
//

#include <algorithm>
#include <cmath>
#include "physics/Collision.h"

namespace physics {

namespace {

float distanceToBox(const glm::vec3& p, const glm::vec3& halfExtents) {
    return glm::length(p - glm::clamp(p, -halfExtents, halfExtents));
}

}  // namespace

std::optional<Contact> capsuleBoxContact(const glm::vec3& a, const glm::vec3& b, float radius, const Transform& box,
                                         const glm::vec3& halfExtents) {
    glm::quat toBox = glm::conjugate(box.rotation);
    glm::vec3 la = toBox * (a - box.position);
    glm::vec3 lb = toBox * (b - box.position);

    // The point of the segment nearest the box: the distance along the segment is convex, so a ternary search finds
    // it (the lower end of the stretch inside the box, when it goes in)
    float lo = 0.0f;
    float hi = 1.0f;
    for (int i = 0; i < 48; i++) {
        float m1 = lo + (hi - lo) * 0.382f;
        float m2 = lo + (hi - lo) * 0.618f;
        if (distanceToBox(glm::mix(la, lb, m1), halfExtents) <= distanceToBox(glm::mix(la, lb, m2), halfExtents)) {
            hi = m2;
        } else {
            lo = m1;
        }
    }
    glm::vec3 s = glm::mix(la, lb, 0.5f * (lo + hi));
    glm::vec3 q = glm::clamp(s, -halfExtents, halfExtents);
    float distance = glm::length(s - q);
    if (distance >= radius) {
        return std::nullopt;
    }

    glm::vec3 normal;
    float depth;
    if (distance > 1e-5f) {
        // Outside: away from the nearest point (a face, an edge or a corner: edges and corners round off)
        normal = (s - q) / distance;
        depth = radius - distance;
    } else {
        // The segment goes into the box: out through the face that needs the shortest push for the whole capsule
        depth = INFINITY;
        for (int axis = 0; axis < 3; axis++) {
            float low = std::min(la[axis], lb[axis]);
            float high = std::max(la[axis], lb[axis]);
            float up = halfExtents[axis] - low + radius;     // past the + face
            float down = high + halfExtents[axis] + radius;  // past the - face
            if (up < depth) {
                depth = up;
                normal = glm::vec3(0.0f);
                normal[axis] = 1.0f;
            }
            if (down < depth) {
                depth = down;
                normal = glm::vec3(0.0f);
                normal[axis] = -1.0f;
            }
        }
        for (int axis = 0; axis < 3; axis++) {
            if (normal[axis] != 0.0f) {
                q[axis] = normal[axis] * halfExtents[axis];  // (on that face)
            }
        }
    }
    return Contact{box.rotation * normal, depth, box.rotation * q + box.position};
}

void boxBounds(const Transform& box, const glm::vec3& halfExtents, glm::vec3& min, glm::vec3& max) {
    glm::mat3 r = glm::mat3_cast(box.rotation);
    glm::vec3 extent(0.0f);
    for (int column = 0; column < 3; column++) {
        extent += glm::abs(r[column]) * halfExtents[column];
    }
    min = box.position - extent;
    max = box.position + extent;
}

}  // namespace physics
