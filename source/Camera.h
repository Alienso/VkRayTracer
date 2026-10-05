//
// Fly camera: WASD to move, Space/Left Shift up/down, Left Ctrl to go faster, hold right mouse button to look around.
//

#ifndef VULKANDEMO_CAMERA_H
#define VULKANDEMO_CAMERA_H

#include <GLFW/glfw3.h>
#include <glm/glm.hpp>

class Camera {
public:
    glm::vec3 position = {0.0f, 0.0f, 0.0f};
    float yaw = 0.0f;    // degrees, 0 = looking down -Z, positive turns right
    float pitch = 0.0f;  // degrees, positive looks up, clamped to +-89
    float fov = 60.0f;   // vertical, degrees
    float moveSpeed = 2.0f;          // units per second
    float mouseSensitivity = 0.15f;  // degrees per pixel

    glm::vec3 forward() const;
    glm::vec3 right() const;
    glm::vec3 up() const;

    void lookAt(const glm::vec3& target);

    // Matrices matching the rays the path tracer shoots (Vulkan clip space: y down, depth 0..1), without jitter.
    // Rays don't need them; motion vectors and DLSS do.
    glm::mat4 viewMatrix() const;
    glm::mat4 projectionMatrix(float aspectRatio, float nearPlane, float farPlane) const;

    // Applies keyboard/mouse input. Pass false for a device ImGui is currently using.
    void update(GLFWwindow* window, float deltaTime, bool keyboardAvailable, bool mouseAvailable);

private:
    bool looking = false;
    double lastCursorX = 0.0;
    double lastCursorY = 0.0;
};

#endif //VULKANDEMO_CAMERA_H
