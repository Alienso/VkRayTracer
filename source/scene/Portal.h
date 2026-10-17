//
// Portals: pairs of flat openings, two-way windows into each other. A ray that goes into one end comes out of the
// other: into the front of one, out of the front of the other; into the back, out of the back. The renderer traces
// them (see "Portals" in shaders/raytrace/portals.glsl); the scene only places them.
//
// Each end is an entity with a PortalComponent naming the other end, so either end can be placed (and parented)
// on its own. An end's opening lies in its entity's local XY plane, centered on its position, X along its width,
// Y along its height, facing +Z (the side it "looks at"). Scale doesn't apply to ends: the size is the pair's.
// Both ends of a pair have the same shape, size and rim (Scene::syncPortal keeps them equal). A pair is enabled
// while both ends are visible.
//
// The rim is ordinary geometry around the opening (outside it, so it never covers the view): the Scene stretches
// the built-in rim model's cube into bars, rimDepth thick, centered on the opening's plane.
//

#ifndef VULKANDEMO_PORTAL_H
#define VULKANDEMO_PORTAL_H

#include <glm/glm.hpp>
#include <cstdint>
#include "portals/PortalTransform.h"  // portalToPartner

enum class PortalShape { Rectangle = 0, Ellipse = 1 };  // values as in the shader

struct PortalComponent {
    uint64_t other = 0;   // the other end's entity
    bool endA = false;    // the pair's first end (rim A; the render list lists a pair as A, B)
    PortalShape shape = PortalShape::Rectangle;
    glm::vec2 size = glm::vec2(1.0f, 2.0f);  // width, height of both ends
    bool rim = true;
    float rimWidth = 0.06f;   // how far the frame reaches out from the opening's edge
    float rimDepth = 0.04f;   // its thickness across the opening's plane
};

#endif //VULKANDEMO_PORTAL_H
