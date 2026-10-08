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
    glm::vec3 direction = glm::normalize(target - position);
    pitch = glm::degrees(std::asin(direction.y));
    yaw = glm::degrees(std::atan2(direction.x, -direction.z));
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
