//
// Where a portal end goes when a shot hits a surface (the game's portal gun). Plain glm, like the
// rest of the portals library.
//
// The end faces out of the surface, a little off it (so it doesn't flicker against it). On a wall it stands upright
// and hangs down from where the shot hit (its top edge there), so one shot at eye height can be walked into; when what
// is below (the floor) leaves too little room, it rests on that instead. On a floor or a ceiling it is centered on the
// hit, its top pointing the way the shot went (away from the shooter). It is slid inward until the opening fits on the
// surface, and away from the pair's other end when that is on the same surface; when it can't fit, or would cover
// another pair's end, it isn't placed.
//

#ifndef VULKANDEMO_PORTALPLACEMENT_H
#define VULKANDEMO_PORTALPLACEMENT_H

#include <functional>
#include <optional>
#include <utility>
#include <vector>
#include <glm/glm.hpp>

namespace portals {

// A flat rectangle an end can go on (a collider box's face): half[i] along axes[i] (unit length) around center, its
// front facing normal
struct PortalSurface {
    glm::vec3 center = glm::vec3(0.0f);
    glm::vec3 normal = glm::vec3(0.0f, 0.0f, 1.0f);
    glm::vec3 axes[2] = {glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f)};
    glm::vec2 half = glm::vec2(0.0f);
};

constexpr float surfaceOffset = 0.01f;  // how far in front of the surface an end goes

struct PlacementOptions {
    // How far a point can go along a direction before it meets something, up to maxDistance (maxDistance: nothing
    // there). Asked for a wall's end: how far down there is room in front of the wall. None: room everywhere.
    std::function<float(const glm::vec3& from, const glm::vec3& direction, float maxDistance)> freeDistance;
    // Its top along the face's nearest axis (on a floor or a ceiling, where it otherwise points the way the shot
    // went): for an end put under something scaled differently along the face's two axes, which would shear an end
    // turned at an angle to them
    bool topAlongFaceAxis = false;
    // Other portal ends (their placements and half sizes): one on the same surface that it would overlap stops it
    // from being placed there (only the pair's own other end is pushed away from)
    std::vector<std::pair<glm::mat4, glm::vec2>> keepClearOf;
};

// The placement of an end (as PortalOpening::end: X along its width, Y along its height, facing +Z) for a shot going
// along `shot` that hit point on surface; halfSize: the pair's half width and height. other: the pair's other end
// when it is placed (the same size), kept clear of. Nothing when the opening doesn't fit.
std::optional<glm::mat4> placePortalEnd(const PortalSurface& surface, const glm::vec3& point, const glm::vec3& shot,
                                        const glm::vec2& halfSize, const glm::mat4* other,
                                        const PlacementOptions& options = PlacementOptions());

}  // namespace portals

#endif //VULKANDEMO_PORTALPLACEMENT_H
