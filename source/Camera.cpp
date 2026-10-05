//
// Fly camera: WASD to move, Space/Left Shift up/down, Left Ctrl to go faster, hold right mouse button to look around.
//

#include <cmath>
#include <algorithm>
#include <glm/gtc/matrix_transform.hpp>
#include "Camera.h"

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

void Camera::update(GLFWwindow* window, float deltaTime, bool keyboardAvailable, bool mouseAvailable) {
    // Mouse look while the right button is held. The cursor is hidden and locked during that time.
    bool rightButtonDown = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
    if (rightButtonDown && !looking && mouseAvailable) {
        looking = true;
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        glfwGetCursorPos(window, &lastCursorX, &lastCursorY);
    } else if (!rightButtonDown && looking) {
        looking = false;
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    }

    if (looking) {
        double x, y;
        glfwGetCursorPos(window, &x, &y);
        yaw += (float) (x - lastCursorX) * mouseSensitivity;
        pitch -= (float) (y - lastCursorY) * mouseSensitivity;
        pitch = std::clamp(pitch, -89.0f, 89.0f);
        lastCursorX = x;
        lastCursorY = y;
    }

    // While looking around, the keyboard belongs to the camera even if ImGui has focus.
    if (!keyboardAvailable && !looking) {
        return;
    }

    glm::vec3 movement(0.0f);
    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) movement += forward();
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) movement -= forward();
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) movement += right();
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) movement -= right();
    if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS) movement += worldUp;
    if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) movement -= worldUp;

    if (glm::length(movement) > 0.0f) {
        float speed = moveSpeed;
        if (glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS) {
            speed *= 4.0f;
        }
        position += glm::normalize(movement) * speed * deltaTime;
    }
}
