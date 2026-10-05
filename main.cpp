#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/vec4.hpp>
#include <glm/mat4x4.hpp>

#include <cstring>
#include <iostream>
#include "source/Application.h"

int main(int argc, char** argv) {
    Application app;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--no-dlss") == 0) {
            app.setDlssDisabled(true);  // don't load Streamline at all (no driver vsync, no per-frame cost)
        } else if (std::strcmp(argv[i], "--dlss-cnn") == 0) {
            app.setDlssCnn(true);  // load the older DLSS RR version that still has the CNN models
        } else {
            std::cerr << "unknown option " << argv[i] << " (known: --no-dlss, --dlss-cnn)" << std::endl;
        }
    }

    try {
        app.run();
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}