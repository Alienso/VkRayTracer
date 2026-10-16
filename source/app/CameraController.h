//
// Fly camera controls, while the right mouse button is held (as in Unity and Unreal: the letters are the editor's
// otherwise): the mouse looks around, WASD moves, Space/Left Shift up/down, Left Ctrl faster. Moves a Camera (the
// scene's); the program owns the controller, since it owns the window's input.
//

#ifndef VULKANDEMO_CAMERACONTROLLER_H
#define VULKANDEMO_CAMERACONTROLLER_H

#include <GLFW/glfw3.h>
#include "scene/Camera.h"

class CameraController {
public:
    float moveSpeed = 2.0f;          // units per second
    float mouseSensitivity = 0.15f;  // degrees per pixel

    // Applies keyboard/mouse input to camera. Pass false for a device ImGui is currently using.
    void update(Camera& camera, GLFWwindow* window, float deltaTime, bool keyboardAvailable, bool mouseAvailable);

private:
    bool looking = false;
    double lastCursorX = 0.0;
    double lastCursorY = 0.0;
};

#endif //VULKANDEMO_CAMERACONTROLLER_H
