//
// Portals: pairs of flat openings, two-way windows into each other. A ray that goes into one end comes out of the
// other: into the front of one, out of the front of the other; into the back, out of the back. The renderer traces
// them (see "Portals" in shaders/raytrace.comp); the scene only places them.
//
// An end's opening lies in its local XY plane, centered on its position, X along its width, Y along its height,
// facing +Z (the side it "looks at"). Both ends of a pair have the same shape and size.
//
// The rim is ordinary geometry around the opening (outside it, so it never covers the view): the Scene stretches
// the built-in rim model's cube into bars, rimDepth thick, centered on the opening's plane.
//

#ifndef VULKANDEMO_PORTAL_H
#define VULKANDEMO_PORTAL_H

#include <glm/glm.hpp>
#include <string>

enum class PortalShape { Rectangle = 0, Ellipse = 1 };  // values as in the shader

struct PortalEnd {
    glm::vec3 position = glm::vec3(0.0f);
    glm::vec3 rotation = glm::vec3(0.0f);  // degrees, as SceneObject::rotation (yaw, then pitch, then roll)

    glm::mat4 transform() const;  // translate * rotate: no scale, the size is the pair's
};

struct PortalPair {
    std::string name;
    bool enabled = true;
    PortalShape shape = PortalShape::Rectangle;
    glm::vec2 size = glm::vec2(1.0f, 2.0f);  // width, height of both ends
    PortalEnd ends[2];                        // A and B
    bool rim = true;
    float rimWidth = 0.06f;   // how far the frame reaches out from the opening's edge
    float rimDepth = 0.04f;   // its thickness across the opening's plane
};

#endif //VULKANDEMO_PORTAL_H
