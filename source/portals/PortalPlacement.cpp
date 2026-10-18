//
// Where a portal end goes when a shot hits a surface. See PortalPlacement.h.
//

#include <algorithm>
#include <cmath>
#include "portals/PortalPlacement.h"

namespace portals {

namespace {

// A rectangle in the surface's plane: its center and its two half axes (in the surface's 2D coordinates)
struct Footprint {
    glm::vec2 center;
    glm::vec2 halfAxes[2];
};

// Whether two rectangles overlap (more than touching): no separating axis among their edges' normals
bool overlap(const Footprint& a, const Footprint& b) {
    const Footprint* both[2] = {&a, &b};
    for (const Footprint* f : both) {
        for (const glm::vec2& halfAxis : f->halfAxes) {
            float length = glm::length(halfAxis);
            if (length < 1e-6f) {
                continue;
            }
            glm::vec2 axis = halfAxis / length;
            float reachA = std::abs(glm::dot(a.halfAxes[0], axis)) + std::abs(glm::dot(a.halfAxes[1], axis));
            float reachB = std::abs(glm::dot(b.halfAxes[0], axis)) + std::abs(glm::dot(b.halfAxes[1], axis));
            if (std::abs(glm::dot(a.center - b.center, axis)) >= reachA + reachB - 1e-4f) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

std::optional<glm::mat4> placePortalEnd(const PortalSurface& surface, const glm::vec3& point, const glm::vec3& shot,
                                        const glm::vec2& halfSize, const glm::mat4* other,
                                        const PlacementOptions& options) {
    glm::vec3 n = glm::normalize(surface.normal);
    // Its top: up on a wall; on a floor or a ceiling, the way the shot went (or the face's axis nearest that)
    bool wall = std::abs(n.y) < 0.7f;
    glm::vec3 up;
    if (wall) {
        up = glm::vec3(0.0f, 1.0f, 0.0f) - n * n.y;
    } else {
        up = shot - n * glm::dot(shot, n);
        if (glm::length(up) < 1e-4f) {
            up = surface.axes[1];  // (shot straight at it)
        }
        if (options.topAlongFaceAxis) {
            const glm::vec3* axes = surface.axes;
            int nearest = std::abs(glm::dot(up, axes[0])) > std::abs(glm::dot(up, axes[1])) ? 0 : 1;
            up = axes[nearest] * (glm::dot(up, axes[nearest]) < 0.0f ? -1.0f : 1.0f);
        }
    }
    up = glm::normalize(up);
    glm::vec3 right = glm::normalize(glm::cross(up, n));

    // On a wall it hangs down from the hit, its top edge there; with too little room below, it rests on what is
    // there (tried down the middle and near both sides, a little in front of the wall)
    glm::vec3 center = point;
    if (wall) {
        float height = 2.0f * halfSize.y;
        float room = height;
        if (options.freeDistance) {
            for (float side : {-0.9f, 0.0f, 0.9f}) {
                glm::vec3 from = point + n * (2.0f * surfaceOffset) + right * (side * halfSize.x);
                room = std::min(room, options.freeDistance(from, -up, height));
            }
        }
        center = point - up * (std::max(room, 0.0f) - halfSize.y);
    }

    // Its rectangle on the surface, slid inward until it fits
    const glm::vec3* axes = surface.axes;
    Footprint mine;
    mine.halfAxes[0] = glm::vec2(glm::dot(right, axes[0]), glm::dot(right, axes[1])) * halfSize.x;
    mine.halfAxes[1] = glm::vec2(glm::dot(up, axes[0]), glm::dot(up, axes[1])) * halfSize.y;
    glm::vec2 reach = glm::abs(mine.halfAxes[0]) + glm::abs(mine.halfAxes[1]);
    if (reach.x > surface.half.x + 1e-4f || reach.y > surface.half.y + 1e-4f) {
        return std::nullopt;
    }
    glm::vec2 room = glm::max(surface.half - reach, glm::vec2(0.0f));
    auto inside = [&](const glm::vec2& c) { return glm::clamp(c, -room, room); };
    glm::vec3 offset = center - surface.center;
    mine.center = inside(glm::vec2(glm::dot(offset, axes[0]), glm::dot(offset, axes[1])));

    // Another end's rectangle on the surface, when it lies on it (facing the same way, as far in front)
    auto footprintOf = [&](const glm::mat4& end, const glm::vec2& half, Footprint& footprint) {
        glm::vec3 endOffset = glm::vec3(end[3]) - surface.center;
        if (glm::dot(glm::vec3(end[2]), n) <= 0.99f || std::abs(glm::dot(endOffset, n) - surfaceOffset) >= 0.02f) {
            return false;
        }
        footprint.center = glm::vec2(glm::dot(endOffset, axes[0]), glm::dot(endOffset, axes[1]));
        for (int i = 0; i < 2; i++) {
            glm::vec3 axis = glm::vec3(end[i]) * half[i];
            footprint.halfAxes[i] = glm::vec2(glm::dot(axis, axes[0]), glm::dot(axis, axes[1]));
        }
        return true;
    };

    // Off the other end when it is on the same surface: pushed straight away from it
    Footprint theirs;
    if (other != nullptr && footprintOf(*other, halfSize, theirs) && overlap(mine, theirs)) {
        glm::vec2 away = mine.center - theirs.center;
        away = glm::length(away) > 1e-4f ? glm::normalize(away) : glm::normalize(mine.halfAxes[0]);
        float low = 0.0f;
        float high = 4.0f * (halfSize.x + halfSize.y);  // (surely clear)
        Footprint moved = mine;
        for (int i = 0; i < 24; i++) {
            float middle = 0.5f * (low + high);
            moved.center = mine.center + away * middle;
            (overlap(moved, theirs) ? low : high) = middle;
        }
        moved.center = inside(mine.center + away * high);
        if (overlap(moved, theirs)) {
            return std::nullopt;  // (no room beside it)
        }
        mine = moved;
    }
    // Other pairs' ends on the surface aren't moved around: on one of them, it isn't placed
    for (const auto& [end, half] : options.keepClearOf) {
        Footprint footprint;
        if (footprintOf(end, half, footprint) && overlap(mine, footprint)) {
            return std::nullopt;
        }
    }

    glm::mat4 end(1.0f);
    end[0] = glm::vec4(right, 0.0f);
    end[1] = glm::vec4(up, 0.0f);
    end[2] = glm::vec4(n, 0.0f);
    end[3] = glm::vec4(surface.center + axes[0] * mine.center.x + axes[1] * mine.center.y + n * surfaceOffset, 1.0f);
    return end;
}

}  // namespace portals
