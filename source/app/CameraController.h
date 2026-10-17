//
// Fly camera controls, while the right mouse button is held (as in Unity and Unreal: the letters are the editor's
// otherwise): the mouse looks around, WASD moves, Space/Left Shift up/down, Left Ctrl faster. Moves a Camera (the
// scene's); the program owns the controller, since it owns the window's input.
//
// Captured (the game runs and has the mouse, but nothing in it drives the camera): as in a game, the mouse always
// looks, WASD moves, Space/Left Ctrl up/down, Left Shift faster; the cursor is the window's (Window::setCursorCaptured).
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
    void update(Camera& camera, GLFWwindow* window, float deltaTime, bool keyboardAvailable, bool mouseAvailable,
                bool captured = false);

private:
    bool looking = false;
    bool capturedRead = false;  // the cursor's position is known since the capture began
    double lastCursorX = 0.0;
    double lastCursorY = 0.0;
};

#endif //VULKANDEMO_CAMERACONTROLLER_H
