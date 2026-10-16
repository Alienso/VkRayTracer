//
// Fly camera controls. See CameraController.h.
//

#include <algorithm>
#include "app/CameraController.h"

static const glm::vec3 worldUp = {0.0f, 1.0f, 0.0f};

void CameraController::update(Camera& camera, GLFWwindow* window, float deltaTime, bool keyboardAvailable,
                              bool mouseAvailable) {
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
        camera.yaw += (float) (x - lastCursorX) * mouseSensitivity;
        camera.pitch -= (float) (y - lastCursorY) * mouseSensitivity;
        camera.pitch = std::clamp(camera.pitch, -89.0f, 89.0f);
        lastCursorX = x;
        lastCursorY = y;
    }

    // Only while looking around; then the keyboard belongs to the camera even if ImGui has focus
    if (!looking) {
        return;
    }

    glm::vec3 movement(0.0f);
    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) movement += camera.forward();
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) movement -= camera.forward();
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) movement += camera.right();
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) movement -= camera.right();
    if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS) movement += worldUp;
    if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) movement -= worldUp;

    if (glm::length(movement) > 0.0f) {
        float speed = moveSpeed;
        if (glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS) {
            speed *= 4.0f;
        }
        camera.position += glm::normalize(movement) * speed * deltaTime;
    }
}
