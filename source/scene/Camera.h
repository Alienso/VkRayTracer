//
// Camera: where the image is seen from. Plain data, part of the Scene; CameraController.h moves it with the keyboard
// and mouse.
//

#ifndef VULKANDEMO_CAMERA_H
#define VULKANDEMO_CAMERA_H

#include <glm/glm.hpp>

class Camera {
public:
    glm::vec3 position = {0.0f, 0.0f, 0.0f};
    float yaw = 0.0f;    // degrees, 0 = looking down -Z, positive turns right
    float pitch = 0.0f;  // degrees, positive looks up, clamped to +-89
    float fov = 60.0f;   // vertical, degrees

    glm::vec3 forward() const;
    glm::vec3 right() const;
    glm::vec3 up() const;

    void lookAt(const glm::vec3& target);

    // Matrices matching the rays the path tracer shoots (Vulkan clip space: y down, depth 0..1), without jitter.
    // Rays don't need them; motion vectors and DLSS do.
    glm::mat4 viewMatrix() const;
    glm::mat4 projectionMatrix(float aspectRatio, float nearPlane, float farPlane) const;
};

#endif //VULKANDEMO_CAMERA_H
