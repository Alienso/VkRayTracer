//
// The program's window. See Window.h.
//

#include <algorithm>
#include "app/Window.h"

void Window::create(int width, int height, const char* title) {
    glfwInit();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    // Hidden until the program can draw into it (show): shown at once, it sat there for seconds with nothing drawn
    // (a maximized one showed its first size's blank area in a black window)
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    window = glfwCreateWindow(width, height, title, nullptr, nullptr);
    glfwSetWindowUserPointer(window, this);
    glfwSetFramebufferSizeCallback(window, framebufferSizeCallback);
    glfwSetWindowCloseCallback(window, closeCallback);
}

void Window::destroy() {
    glfwDestroyWindow(window);
    glfwTerminate();
    window = nullptr;
}

void Window::framebufferSizeCallback(GLFWwindow* window, int width, int height) {
    reinterpret_cast<Window*>(glfwGetWindowUserPointer(window))->resized = true;
}

void Window::closeCallback(GLFWwindow* window) {
    Window* self = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
    if (self->closeCheck && !self->closeCheck()) {
        glfwSetWindowShouldClose(window, GLFW_FALSE);
    }
}

bool Window::isMaximized() const {
    return fullscreen ? windowedMaximized : glfwGetWindowAttrib(window, GLFW_MAXIMIZED) == GLFW_TRUE;
}

void Window::windowedSize(int& width, int& height) const {
    if (fullscreen) {
        width = windowedWidth;  // (saved un-maximized when it went fullscreen)
        height = windowedHeight;
    } else {
        glfwGetWindowSize(window, &width, &height);
    }
}

void Window::setCursorCaptured(bool on) {
    cursorCaptured = on;
    glfwSetInputMode(window, GLFW_CURSOR, on ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    if (glfwRawMouseMotionSupported()) {
        glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, on ? GLFW_TRUE : GLFW_FALSE);
    }
}

bool Window::takeResized() {
    bool was = resized;
    resized = false;
    return was;
}

// Borderless fullscreen on the monitor holding most of the window, at the monitor's own resolution and refresh rate:
// the swapchain follows like after any other size change.
void Window::setFullscreen(bool on) {
    if (on == fullscreen) {
        return;
    }
    if (on) {
        windowedMaximized = glfwGetWindowAttrib(window, GLFW_MAXIMIZED) == GLFW_TRUE;
        if (windowedMaximized) {
            glfwRestoreWindow(window);  // remember the size it un-maximizes to
        }
        glfwGetWindowPos(window, &windowedX, &windowedY);
        glfwGetWindowSize(window, &windowedWidth, &windowedHeight);
        int monitorCount = 0;
        GLFWmonitor** monitors = glfwGetMonitors(&monitorCount);
        GLFWmonitor* best = glfwGetPrimaryMonitor();
        int bestOverlap = 0;
        for (int i = 0; i < monitorCount; i++) {
            int x, y;
            glfwGetMonitorPos(monitors[i], &x, &y);
            const GLFWvidmode* mode = glfwGetVideoMode(monitors[i]);
            int overlapX = std::min(windowedX + windowedWidth, x + mode->width) - std::max(windowedX, x);
            int overlapY = std::min(windowedY + windowedHeight, y + mode->height) - std::max(windowedY, y);
            int overlap = std::max(overlapX, 0) * std::max(overlapY, 0);
            if (overlap > bestOverlap) {
                bestOverlap = overlap;
                best = monitors[i];
            }
        }
        const GLFWvidmode* mode = glfwGetVideoMode(best);
        glfwSetWindowMonitor(window, best, 0, 0, mode->width, mode->height, mode->refreshRate);
    } else {
        glfwSetWindowMonitor(window, nullptr, windowedX, windowedY, windowedWidth, windowedHeight, GLFW_DONT_CARE);
        if (windowedMaximized) {
            glfwMaximizeWindow(window);
        }
    }
    fullscreen = on;
    resized = true;
}
