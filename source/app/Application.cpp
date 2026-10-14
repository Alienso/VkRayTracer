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
    window.setCloseCheck([this]() { return editor.allowClose(); });  // asks about unsaved changes
    createDefaultScene();
    scene.buildRenderList(renderList);
    // The UI: ImGui's context and its input from the window are the program's; the renderer adds the Vulkan side
    ImGui::CreateContext();
    editor.initUi();
    ImGui_ImplGlfw_InitForVulkan(window.handle(), true);  // true: install input callbacks, chaining the window's own
    renderer.init(window.handle(), renderList);
    mainLoop();
    renderer.shutdown();

    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    window.destroy();
}

// The model and environment files there are, the floor, and the default model on it; or the scene file given on the
// command line
void Application::createDefaultScene() {
    assets.scanModelFiles();
    assets.scanEnvironmentFiles();
    assets.setTextureLimit(Renderer::maxTextures);
    scene.createDefault();
    std::string error;
    const std::vector<std::string>& files = assets.modelFiles();
    if (startScene.empty() && std::find(files.begin(), files.end(), defaultModel) != files.end()) {
        try {
            scene.addObject(defaultModel, false);
        } catch (const std::exception& e) {
            error = e.what();
            std::cerr << error << std::endl;
        }
    }
    editor.init(error);
    if (!startScene.empty()) {
        editor.openSceneFile(startScene);  // on failure the error shows in the Log panel, over the default scene
    }
}

// Unused models are only a cache (adding one again is instant): they may keep what GPU memory is free beyond a reserve
// for the frame and for loading (render images, staging, BLAS builds), up to their default budget, and give memory
// back once the rest of the program needs it. Without the driver's numbers: the default budget.
static uint64_t unusedModelBudget(const GpuMemory& memory, uint64_t unusedMemory) {
    if (memory.budget == 0) {
        return AssetStore::defaultUnusedBudget;
    }
    int64_t reserve = static_cast<int64_t>(memory.budget / 8);
    int64_t spare = static_cast<int64_t>(memory.budget) - static_cast<int64_t>(memory.used) - reserve;  // < 0: short
    int64_t allowed = static_cast<int64_t>(unusedMemory) + spare;
    return static_cast<uint64_t>(std::clamp<int64_t>(allowed, 0, static_cast<int64_t>(AssetStore::defaultUnusedBudget)));
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
        assets.setUnusedBudget(unusedModelBudget(renderer.gpuMemory(), assets.unusedMemory()));
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
