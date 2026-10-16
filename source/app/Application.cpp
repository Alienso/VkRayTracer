//
// The program: window, systems, main loop. See Application.h.
//

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <imgui/imgui.h>
#include <imgui/backends/imgui_impl_glfw.h>
#include "app/Application.h"
#include "core/Configuration.h"

static const char* defaultModel = "DamagedHelmet.glb";

int Application::run() {
    std::vector<std::string> userSettingsProblems;
    if (bench) {
        startScene = bench->sceneFile();  // (and the defaults: no user settings)
    } else {
        userSettingsProblems = loadUserSettings(userSettingsFile, userSettings, renderSettings);
        savedUserSettings = userSettingsText(userSettings, renderSettings);
        savedRenderMode = renderSettings.renderMode;
        savedDlssModel = renderSettings.dlssModel;
        // The last scene again, unless the command line names one (and it is still there: else the default scene)
        if (startScene.empty() && !userSettings.lastScene.empty() && std::filesystem::exists(userSettings.lastScene)) {
            startScene = userSettings.lastScene;
        }
    }
    window.create(userSettings.windowWidth > 0 ? userSettings.windowWidth : Configuration::wWidth,
                  userSettings.windowHeight > 0 ? userSettings.windowHeight : Configuration::wHeight, "Vulkan");
    if (userSettings.maximized) {
        window.maximize();
    }
    if (userSettings.fullscreen) {
        window.setFullscreen(true);
    }
    window.setCloseCheck([this]() { return editor.allowClose(); });  // asks about unsaved changes
    createDefaultScene();
    for (const std::string& problem : userSettingsProblems) {
        editor.warn(std::string(userSettingsFile) + ": " + problem);
    }
    scene.buildRenderList(renderList);
    // The UI: ImGui's context and its input from the window are the program's; the renderer adds the Vulkan side
    ImGui::CreateContext();
    editor.initUi();
    ImGui_ImplGlfw_InitForVulkan(window.handle(), true);  // true: install input callbacks, chaining the window's own
    renderer.init(window.handle(), renderList);
    if (bench) {
        bench->start(renderSettings);
    }
    mainLoop();
    if (!bench) {
        saveUserSettingsIfChanged();
    }
    renderer.shutdown();

    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    window.destroy();
    return bench && (bench->failed() || !benchDone) ? 1 : 0;
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
        // On failure the error shows in the Log panel, over the default scene
        if (!editor.openSceneFile(startScene) && bench) {
            bench->fail("cannot open the scene " + startScene);
        }
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

void Application::saveUserSettingsIfChanged() {
    userSettings.fullscreen = window.isFullscreen();
    userSettings.maximized = window.isMaximized();
    if (!window.isFullscreen() && !userSettings.maximized) {  // (else the size it comes back to is kept)
        window.windowedSize(userSettings.windowWidth, userSettings.windowHeight);
    }
    RenderSettings kept = renderSettings;
    if (!renderer.stats().dlssSupported) {
        kept.renderMode = savedRenderMode;
    }
    if (!renderer.dlssModelAvailable(savedDlssModel)) {
        kept.dlssModel = savedDlssModel;
    }
    savedRenderMode = kept.renderMode;
    savedDlssModel = kept.dlssModel;
    std::string text = userSettingsText(userSettings, kept);
    if (text == savedUserSettings) {
        return;
    }
    try {
        saveUserSettings(userSettingsFile, text);
        savedUserSettings = text;
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        savedUserSettings = text;  // (tried once per change, not every second)
    }
}

void Application::mainLoop() {
    using Clock = std::chrono::steady_clock;
    auto msBetween = [](Clock::time_point from, Clock::time_point to) {
        return std::chrono::duration<float, std::milli>(to - from).count();
    };
    double lastTime = glfwGetTime();
    Clock::time_point frameStart = Clock::now();  // the last frame's render returned (all of the loop is in a phase)
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
        if (bench) {
            deltaTime = bench->deltaTime(deltaTime);
        }

        renderer.beginFrame(deltaTime);
        assets.setUnusedBudget(unusedModelBudget(renderer.gpuMemory(), assets.unusedMemory()));
        assets.update();  // a model or map loaded in the background arrives
        scene.update();   // and shows up
        Clock::time_point uiStart = Clock::now();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        editor.build();  // may change the scene
        ImGui::Render();
        Clock::time_point uiEnd = Clock::now();
        // Leave mouse/keyboard to ImGui while it is using them (dragging a slider, typing in a field, ...)
        ImGuiIO& io = ImGui::GetIO();
        glm::vec3 cameraFrom = scene.camera().position;
        if (bench) {
            bench->beforeRender(scene, renderer, assets);  // the camera is the bench's
        } else {
            cameraController.update(scene.camera(), window.handle(), deltaTime, !io.WantCaptureKeyboard, !io.WantCaptureMouse);
        }
        scene.moveCameraThroughPortals(cameraFrom);
        scene.buildRenderList(renderList);
        if (window.takeResized()) {  // dragged, maximized, fullscreen (F11 or the editor)
            renderer.resize();
        }
        Clock::time_point renderStart = Clock::now();
        renderer.render(renderList);
        Clock::time_point renderEnd = Clock::now();
        float waitMs = renderer.lastWaitMs();
        float phases[CpuFrameTimes::PhaseCount];
        phases[CpuFrameTimes::Update] = msBetween(frameStart, uiStart) + msBetween(uiEnd, renderStart);
        phases[CpuFrameTimes::Ui] = msBetween(uiStart, uiEnd);
        phases[CpuFrameTimes::Render] = std::max(0.0f, msBetween(renderStart, renderEnd) - waitMs);
        phases[CpuFrameTimes::Wait] = waitMs;
        cpuTimes.add(phases);
        frameStart = renderEnd;
        if (bench && !benchDone && bench->afterRender(renderer, cpuTimes)) {
            benchDone = true;
            window.requestClose();
        }
        if (!bench && now - userSettingsCheckTime >= 1.0) {
            userSettingsCheckTime = now;
            saveUserSettingsIfChanged();
        }
    }
}
