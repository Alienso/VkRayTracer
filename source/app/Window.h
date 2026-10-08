//
// The program's window (GLFW): creating it, fullscreen, and noticing when its framebuffer changes size. The renderer
// draws into it (its surface and size); the editor's Camera tab switches fullscreen.
//

#ifndef VULKANDEMO_WINDOW_H
#define VULKANDEMO_WINDOW_H

#include <GLFW/glfw3.h>

class Window {
public:
    // GLFW and the window (no OpenGL context: Vulkan draws into it)
    void create(int width, int height, const char* title);
    void destroy();

    GLFWwindow* handle() const { return window; }
    bool shouldClose() const { return glfwWindowShouldClose(window); }
    void requestClose() { glfwSetWindowShouldClose(window, GLFW_TRUE); }

    // Borderless fullscreen on the monitor it is on; off: the windowed position and size come back
    void setFullscreen(bool on);
    bool isFullscreen() const { return fullscreen; }

    // True once after the framebuffer changed size (dragging, maximizing, fullscreen), then false until it changes again
    bool takeResized();

private:
    GLFWwindow* window = nullptr;
    bool resized = false;
    bool fullscreen = false;
    int windowedX = 0, windowedY = 0, windowedWidth = 0, windowedHeight = 0;
    bool windowedMaximized = false;

    static void framebufferSizeCallback(GLFWwindow* window, int width, int height);
};

#endif //VULKANDEMO_WINDOW_H
