//
// Camera: directions and matrices. See Camera.h.
//

#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include "scene/Camera.h"

static const glm::vec3 worldUp = {0.0f, 1.0f, 0.0f};

glm::vec3 Camera::forward() const {
    float yawRad = glm::radians(yaw);
    float pitchRad = glm::radians(pitch);
    return {std::cos(pitchRad) * std::sin(yawRad),
            std::sin(pitchRad),
            -std::cos(pitchRad) * std::cos(yawRad)};
}

glm::vec3 Camera::right() const {
    return glm::normalize(glm::cross(forward(), worldUp));
}

glm::vec3 Camera::up() const {
    return glm::cross(right(), forward());
}

void Camera::lookAt(const glm::vec3& target) {
    glm::vec3 offset = target - position;
    if (glm::dot(offset, offset) <= 0.0f) {
        return;  // no direction: stays as it is
    }
    glm::vec3 direction = glm::normalize(offset);
    // Within +-89 degrees, as the controls keep it: straight up or down there is no right direction (right() and the
    // view matrix would be NaN)
    pitch = glm::clamp(glm::degrees(std::asin(glm::clamp(direction.y, -1.0f, 1.0f))), -89.0f, 89.0f);
    if (direction.x != 0.0f || direction.z != 0.0f) {
        yaw = glm::degrees(std::atan2(direction.x, -direction.z));  // (straight up: the yaw it had)
    }
}

glm::mat4 Camera::viewMatrix() const {
    // Same basis as right() / up() / forward(); looks down -Z in view space
    return glm::lookAtRH(position, position + forward(), worldUp);
}

glm::mat4 Camera::projectionMatrix(float aspectRatio, float nearPlane, float farPlane) const {
    glm::mat4 projection = glm::perspectiveRH_ZO(glm::radians(fov), aspectRatio, nearPlane, farPlane);
    projection[1][1] *= -1.0f;  // Vulkan: clip-space y points down, like image rows (and like the ray generation)
    return projection;
}
