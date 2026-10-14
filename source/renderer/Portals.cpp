//
// Portals: the render list's portal ends -> the shader's portal list (binding 31). The shader does the rest (see
// "Portals" in shaders/raytrace.comp).
//

#include <cstring>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "renderer/Renderer.h"

void Renderer::createPortalBuffer() {
    createStreamedBuffer(portalBuffer, 16 * sizeof(GpuPortal));  // never empty: descriptors can't point at nothing
}

void Renderer::destroyPortalBuffer() {
    destroyStreamedBuffer(portalBuffer);
}

// This frame's portal ends in world space, each with the way to its partner. Cheap, so every frame; uploaded only on
// changes. A ray into one end comes out of the other with the same position and direction relative to it, turned
// half around the vertical: in at the front, out at the front (and back to back).
void Renderer::buildPortalList() {
    framePortals.clear();
    const std::vector<RenderPortal>& portals = renderList->portals;
    for (size_t i = 0; i < portals.size(); i++) {
        const RenderPortal& portal = portals[i];
        const RenderPortal& partner = portals[i ^ 1];
        GpuPortal gpu{};
        gpu.center = glm::vec3(portal.world[3]);
        gpu.normal = glm::normalize(glm::vec3(portal.world[2]));
        gpu.right = glm::normalize(glm::vec3(portal.world[0]));
        gpu.halfWidth = std::max(portal.halfSize.x, 1e-4f);
        gpu.halfHeight = std::max(portal.halfSize.y, 1e-4f);
        gpu.shape = static_cast<uint32_t>(portal.shape);
        gpu.toPartner = glm::mat4x3(portalToPartner(portal.world, partner.world));
        framePortals.push_back(gpu);
    }
}

void Renderer::recordPortalUpload(VkCommandBuffer commandBuffer) {
    recordStreamedUpload(commandBuffer, portalBuffer, uploadedPortals.data(), sizeof(GpuPortal) * uploadedPortals.size());
    portalsUploadPending = false;
}
