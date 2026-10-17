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

// A turned box as its center, axes and half sizes
struct Box {
    glm::vec3 center;
    glm::vec3 axis[3];
    glm::vec3 half;
};

Box boxOf(const Transform& transform, const glm::vec3& halfExtents) {
    glm::mat3 r = glm::mat3_cast(transform.rotation);
    return Box{transform.position, {r[0], r[1], r[2]}, halfExtents};
}

// The face of reference across `axis` whose outward normal is `normal` (towards incident), against the face of
// incident most turned against it: the incident face's corners clipped to the reference face's sides, kept where they
// are behind the reference face (with how deep). onReference: the points moved onto the reference face.
void clipFaces(const Box& reference, int axis, const glm::vec3& normal, const Box& incident, bool onReference,
               std::vector<glm::vec3>& points, std::vector<float>& depths) {
    int m = 0;
    for (int i = 1; i < 3; i++) {
        if (std::abs(glm::dot(incident.axis[i], normal)) > std::abs(glm::dot(incident.axis[m], normal))) {
            m = i;
        }
    }
    float side = glm::dot(incident.axis[m], normal) > 0.0f ? -1.0f : 1.0f;  // (the face looking at the reference)
    glm::vec3 center = incident.center + incident.axis[m] * (side * incident.half[m]);
    glm::vec3 du = incident.axis[(m + 1) % 3] * incident.half[(m + 1) % 3];
    glm::vec3 dv = incident.axis[(m + 2) % 3] * incident.half[(m + 2) % 3];
    std::vector<glm::vec3> polygon = {center - du - dv, center + du - dv, center + du + dv, center - du + dv};
    for (int other = 1; other < 3; other++) {
        int k = (axis + other) % 3;
        for (float sign : {1.0f, -1.0f}) {
            // Kept: dot(n, p) <= limit (inside this side of the reference face)
            glm::vec3 n = reference.axis[k] * sign;
            float limit = glm::dot(n, reference.center) + reference.half[k];
            std::vector<glm::vec3> clipped;
            for (size_t i = 0; i < polygon.size(); i++) {
                const glm::vec3& p = polygon[i];
                const glm::vec3& q = polygon[(i + 1) % polygon.size()];
                float dp = glm::dot(n, p) - limit;
                float dq = glm::dot(n, q) - limit;
                if (dp <= 0.0f) {
                    clipped.push_back(p);
                }
                if ((dp < 0.0f && dq > 0.0f) || (dp > 0.0f && dq < 0.0f)) {
                    clipped.push_back(p + (q - p) * (dp / (dp - dq)));
                }
            }
            polygon = std::move(clipped);
            if (polygon.empty()) {
                return;
            }
        }
    }
    glm::vec3 face = reference.center + normal * reference.half[axis];
    for (const glm::vec3& p : polygon) {
        float depth = glm::dot(face - p, normal);
        if (depth >= 0.0f) {
            points.push_back(onReference ? p + normal * depth : p);
            depths.push_back(depth);
        }
    }
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

std::optional<Contact> boxBoxContact(const Transform& a, const glm::vec3& halfA, const Transform& b,
                                     const glm::vec3& halfB, std::vector<glm::vec3>* points) {
    Box boxA = boxOf(a, halfA);
    Box boxB = boxOf(b, halfB);
    glm::vec3 apart = boxA.center - boxB.center;

    // The axis that parts them least (none parts them: they overlap). 0-2: A's faces, 3-5: B's, then the edges' crosses
    float best = INFINITY;
    glm::vec3 normal(0.0f, 1.0f, 0.0f);
    int kind = 0;
    auto overlaps = [&](glm::vec3 axis, int which) {
        float length = glm::length(axis);
        if (length < 1e-6f) {
            return true;  // (parallel edges: the face axes cover them)
        }
        axis /= length;
        float ra = 0.0f;
        float rb = 0.0f;
        for (int i = 0; i < 3; i++) {
            ra += boxA.half[i] * std::abs(glm::dot(boxA.axis[i], axis));
            rb += boxB.half[i] * std::abs(glm::dot(boxB.axis[i], axis));
        }
        float along = glm::dot(apart, axis);
        float overlap = ra + rb - std::abs(along);
        if (overlap <= 0.0f) {
            return false;
        }
        // An edge pair only when clearly shallower than the faces: face contacts are steadier
        if (which < 6 ? overlap < best : overlap * 1.05f + 1e-4f < best) {
            best = overlap;
            normal = along < 0.0f ? -axis : axis;
            kind = which;
        }
        return true;
    };
    for (int i = 0; i < 3; i++) {
        if (!overlaps(boxA.axis[i], i)) {
            return std::nullopt;
        }
    }
    for (int i = 0; i < 3; i++) {
        if (!overlaps(boxB.axis[i], 3 + i)) {
            return std::nullopt;
        }
    }
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            if (!overlaps(glm::cross(boxA.axis[i], boxB.axis[j]), 6 + i * 3 + j)) {
                return std::nullopt;
            }
        }
    }

    // The contact's points, on B's surface
    std::vector<glm::vec3> found;
    std::vector<float> depths;
    if (kind < 3) {
        clipFaces(boxA, kind, -normal, boxB, false, found, depths);  // A's face against B: B's corners in it
    } else if (kind < 6) {
        clipFaces(boxB, kind - 3, normal, boxA, true, found, depths);  // B's face against A: A's corners in it
    } else {
        // A's edge along axis i nearest B, B's edge along axis j nearest A: the nearest point of B's to A's
        int i = (kind - 6) / 3;
        int j = (kind - 6) % 3;
        glm::vec3 pa = boxA.center;
        glm::vec3 pb = boxB.center;
        for (int k = 0; k < 3; k++) {
            if (k != i) {
                pa += boxA.axis[k] * (glm::dot(boxA.axis[k], normal) > 0.0f ? -boxA.half[k] : boxA.half[k]);
            }
            if (k != j) {
                pb += boxB.axis[k] * (glm::dot(boxB.axis[k], normal) > 0.0f ? boxB.half[k] : -boxB.half[k]);
            }
        }
        const glm::vec3& ua = boxA.axis[i];
        const glm::vec3& ub = boxB.axis[j];
        glm::vec3 r = pa - pb;
        float c = glm::dot(ua, ub);
        float e = glm::dot(ua, r);
        float f = glm::dot(ub, r);
        float denominator = 1.0f - c * c;
        float s = denominator > 1e-8f ? glm::clamp((c * f - e) / denominator, -boxA.half[i], boxA.half[i]) : 0.0f;
        float t = glm::clamp(f + s * c, -boxB.half[j], boxB.half[j]);
        found.push_back(pb + ub * t);
        depths.push_back(best);
    }
    if (found.empty()) {
        // (Clipping missed it numerically) B's surface nearest A's center
        glm::vec3 q = boxB.center;
        for (int k = 0; k < 3; k++) {
            q += boxB.axis[k] * glm::clamp(glm::dot(apart, boxB.axis[k]), -boxB.half[k], boxB.half[k]);
        }
        found.push_back(q);
        depths.push_back(best);
    }
    size_t deepest = std::max_element(depths.begin(), depths.end()) - depths.begin();
    Contact contact{normal, best, found[deepest]};
    if (points != nullptr) {
        *points = std::move(found);
    }
    return contact;
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
