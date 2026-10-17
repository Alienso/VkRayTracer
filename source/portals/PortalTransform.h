//
// What going through a portal end does to points (scene/Portal.h has the ends' convention). Here, in the portals
// library (glm only), so the scene, the renderer and the portal pieces share it.
//

#ifndef VULKANDEMO_PORTALTRANSFORM_H
#define VULKANDEMO_PORTALTRANSFORM_H

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

// What going through an end does (end and partner: world placements without scale): a point or direction comes out
// with the same place relative to the partner as it had to the end, turned half around the vertical (in at the
// front, out at the front). Rigid, so distances add up; the partner's is the inverse.
inline glm::mat4 portalToPartner(const glm::mat4& end, const glm::mat4& partner) {
    const glm::mat4 halfTurn = glm::rotate(glm::mat4(1.0f), glm::pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f));
    return partner * halfTurn * glm::inverse(end);
}

#endif //VULKANDEMO_PORTALTRANSFORM_H
