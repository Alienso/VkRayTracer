//
// The program's window (GLFW): creating it, fullscreen, and noticing when its framebuffer changes size. The renderer
// draws into it (its surface and size); the editor's Camera panel switches fullscreen.
//

#ifndef VULKANDEMO_WINDOW_H
#define VULKANDEMO_WINDOW_H

#include <GLFW/glfw3.h>
#include <functional>
#include <string>

class Window {
public:
    // GLFW and the window (no OpenGL context: Vulkan draws into it)
    void create(int width, int height, const char* title);
    void destroy();

    [[nodiscard]] GLFWwindow* handle() const { return window; }
    [[nodiscard]] bool shouldClose() const { return glfwWindowShouldClose(window); }
    void requestClose() { glfwSetWindowShouldClose(window, GLFW_TRUE); }
    // Asked when the user closes the window (its close button, Alt+F4): false keeps it open (the editor asks about
    // unsaved changes first). requestClose doesn't ask.
    void setCloseCheck(std::function<bool()> check) { closeCheck = std::move(check); }
    void setTitle(const std::string& title) { glfwSetWindowTitle(window, title.c_str()); }

    // Borderless fullscreen on the monitor it is on; off: the windowed position and size come back
    void setFullscreen(bool on);
    [[nodiscard]] bool isFullscreen() const { return fullscreen; }
    void maximize() { glfwMaximizeWindow(window); }
    // Maximized (when fullscreen: whether it was before), and its size when it is neither maximized nor fullscreen
    // (what the user settings keep)
    [[nodiscard]] bool isMaximized() const;
    void windowedSize(int& width, int& height) const;

    // True once after the framebuffer changed size (dragging, maximizing, fullscreen), then false until it changes again
    bool takeResized();

private:
    GLFWwindow* window = nullptr;
    bool resized = false;
    bool fullscreen = false;
    int windowedX = 0, windowedY = 0, windowedWidth = 0, windowedHeight = 0;
    bool windowedMaximized = false;
    std::function<bool()> closeCheck;

    static void framebufferSizeCallback(GLFWwindow* window, int width, int height);
    static void closeCallback(GLFWwindow* window);
};

#endif //VULKANDEMO_WINDOW_H
