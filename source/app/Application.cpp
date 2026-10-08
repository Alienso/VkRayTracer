//
// The program: window, systems, main loop. See Application.h.
//

#include <algorithm>
#include <iostream>
#include <imgui/imgui.h>
#include <imgui/backends/imgui_impl_glfw.h>
#include "app/Application.h"
#include "core/Configuration.h"

static const char* defaultModel = "DamagedHelmet.glb";

void Application::run() {
    window.create(Configuration::wWidth, Configuration::wHeight, "Vulkan");
    createDefaultScene();
    scene.buildRenderList(renderList);
    // The UI: ImGui's context and its input from the window are the program's; the renderer adds the Vulkan side
    ImGui::CreateContext();
    ImGui_ImplGlfw_InitForVulkan(window.handle(), true);  // true: install input callbacks, chaining the window's own
    renderer.init(window.handle(), renderList);
    mainLoop();
    renderer.shutdown();

    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    window.destroy();
}

// The model and environment files there are, the floor, and the default model on it
void Application::createDefaultScene() {
    assets.scanModelFiles();
    assets.scanEnvironmentFiles();
    assets.setTextureLimit(Renderer::maxTextures);
    scene.createDefault();
    std::string error;
    const std::vector<std::string>& files = assets.modelFiles();
    if (std::find(files.begin(), files.end(), defaultModel) != files.end()) {
        try {
            scene.addObject(defaultModel, false);
        } catch (const std::exception& e) {
            error = e.what();
            std::cerr << error << std::endl;
        }
    }
    editor.init(defaultModel, error);
}

void Application::mainLoop() {
    double lastTime = glfwGetTime();
    while (!window.shouldClose()) {
        glfwPollEvents();
        bool fullscreenKey = glfwGetKey(window.handle(), GLFW_KEY_F11) == GLFW_PRESS;
        if (fullscreenKey && !fullscreenKeyDown) {
            window.setFullscreen(!window.isFullscreen());
        }
        fullscreenKeyDown = fullscreenKey;

        double now = glfwGetTime();
        float deltaTime = static_cast<float>(now - lastTime);
        lastTime = now;

        renderer.beginFrame(deltaTime);
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        editor.build();  // may change the scene
        ImGui::Render();
        // Leave mouse/keyboard to ImGui while it is using them (dragging a slider, typing in a field, ...)
        ImGuiIO& io = ImGui::GetIO();
        cameraController.update(scene.camera(), window.handle(), deltaTime, !io.WantCaptureKeyboard, !io.WantCaptureMouse);
        scene.buildRenderList(renderList);
        if (window.takeResized()) {  // dragged, maximized, fullscreen (F11 or the editor)
            renderer.resize();
        }
        renderer.render(renderList);
        if (renderer.quitRequested()) {
            window.requestClose();
        }
    }
}
