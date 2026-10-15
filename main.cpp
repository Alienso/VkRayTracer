#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/vec4.hpp>
#include <glm/mat4x4.hpp>

#include <cstring>
#include <filesystem>
#include <iostream>
#include "app/Application.h"
#include "core/Configuration.h"

int main(int argc, char** argv) {
    // --resources first: the asset store takes its folders when the application is made
    for (int i = 1; i + 1 < argc; i++) {
        if (std::strcmp(argv[i], "--resources") == 0) {
            Configuration::resourceDirectory = argv[i + 1];  // models/ and environments/ (default: the project's resource/)
        }
    }
    Application app;
    std::string benchFile, benchOutput;
    for (int i = 1; i < argc; i++) {
        bool hasValue = i + 1 < argc;
        if (std::strcmp(argv[i], "--resources") == 0 && hasValue) {
            i++;  // (read above)
        } else if (std::strcmp(argv[i], "--no-dlss") == 0) {
            app.setDlssDisabled(true);  // don't load Streamline at all (no driver vsync, no per-frame cost)
        } else if (std::strcmp(argv[i], "--dlss-cnn") == 0) {
            app.setDlssCnn(true);  // load the older DLSS RR version that still has the CNN models
        } else if (std::strcmp(argv[i], "--validation") == 0) {
            Configuration::enableValidationLayers = true;  // the Vulkan validation layer (default: Debug builds only)
        } else if (std::strcmp(argv[i], "--no-validation") == 0) {
            Configuration::enableValidationLayers = false;
        } else if (std::strcmp(argv[i], "--sync-validation") == 0) {
            Configuration::enableValidationLayers = true;  // the layer, with synchronization validation (slow)
            Configuration::enableSyncValidation = true;
        } else if (std::strcmp(argv[i], "--shader-dir") == 0 && hasValue) {
            Configuration::shaderDirectory = argv[++i];  // the .spv files (and their pipeline cache) from there
        } else if (std::strcmp(argv[i], "--bench") == 0 && hasValue) {
            benchFile = argv[++i];  // runs the bench, writes its results, exits (app/Bench.h)
        } else if (std::strcmp(argv[i], "--bench-out") == 0 && hasValue) {
            benchOutput = argv[++i];  // where (default: bench_results/<bench name>)
        } else if (argv[i][0] != '-') {
            app.setStartScene(argv[i]);  // a scene file to open
        } else {
            std::cerr << "unknown option " << argv[i]
                      << " (known: --no-dlss, --dlss-cnn, --validation, --no-validation, --sync-validation, --resources <dir>, "
                         "--shader-dir <dir>, --bench <file.json>, --bench-out <dir>, a scene file)"
                      << std::endl;
        }
    }

    try {
        if (!benchFile.empty()) {
            if (benchOutput.empty()) {
                benchOutput = (std::filesystem::path("bench_results") / std::filesystem::path(benchFile).stem()).string();
            }
            app.setBench(std::make_unique<Bench>(benchFile, benchOutput));
        }
        return app.run();
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return EXIT_FAILURE;
    }
}
