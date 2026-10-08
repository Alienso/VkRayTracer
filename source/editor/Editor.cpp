//
// The editor: the settings window over the image (ImGui). Its tabs edit the scene (objects, camera, sun and sky:
// here and EditorScene.cpp) and show the renderer's own settings (Renderer::buildRenderingUi and co.); the numbers
// at the top come from Renderer::stats.
//

#include <iostream>
#include <imgui/imgui.h>
#include "editor/Editor.h"

Editor::Editor(AssetStore& assets, Scene& scene, CameraController& cameraController, Renderer& renderer,
               Window& window)
        : assets(assets), scene(scene), cameraController(cameraController), renderer(renderer), window(window) {}

void Editor::build() {
    stats = renderer.stats();

    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::Begin("Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    // Always visible, whichever tab is open
    ImGui::Text("GPU: %s", stats.deviceName.c_str());
    ImGui::Text("%.1f FPS (%.3f ms)", ImGui::GetIO().Framerate, 1000.0f / ImGui::GetIO().Framerate);
    ImGui::Text("GPU: cache %.2f ms, ReSTIR %.2f, trace %.2f, DLSS %.2f, exposure %.2f, display %.2f",
                stats.gpuPassMs[0], stats.gpuPassMs[1], stats.gpuPassMs[2], stats.gpuPassMs[3], stats.gpuPassMs[4],
                stats.gpuPassMs[5]);
    ImGui::Text("%llu triangles", static_cast<unsigned long long>(stats.triangles));
    if (ImGui::Button("Capture")) {
        renderer.requestCapture();  // the next frame's image, without this window, as a PNG next to the executable
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", stats.captureMessage.empty() ? "PNG of the image, without the UI" : stats.captureMessage.c_str());
    if (stats.accumulating) {
        ImGui::Text("%u frames accumulated", stats.accumulatedFrames);
    }
    if (stats.debugView) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "Debug view on (Debug tab)");
    }

    // The window sizes itself to the open tab
    if (ImGui::BeginTabBar("settingsTabs")) {
        if (ImGui::BeginTabItem("Scene")) {
            buildSceneUi();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Camera")) {
            buildCameraUi();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Lighting")) {
            buildLightingUi();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Rendering")) {
            renderer.buildRenderingUi();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Debug")) {
            renderer.buildDebugUi();
            ImGui::Separator();
            ImGui::Checkbox("Show ImGui demo", &showImGuiDemo);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();

    if (showImGuiDemo) {
        ImGui::ShowDemoWindow(&showImGuiDemo);
    }

}


void Editor::buildCameraUi() {
    ImGui::TextDisabled("WASD move, Space/LShift up/down, LCtrl faster,");
    ImGui::TextDisabled("hold right mouse button to look around");
    bool fullscreenChecked = window.isFullscreen();
    if (ImGui::Checkbox("Fullscreen (F11)", &fullscreenChecked)) {
        window.setFullscreen(fullscreenChecked);
    }
    Camera& camera = scene.camera();
    ImGui::DragFloat3("Position", &camera.position.x, 0.05f);
    ImGui::DragFloat("Yaw", &camera.yaw, 0.5f);
    ImGui::SliderFloat("Pitch", &camera.pitch, -89.0f, 89.0f);
    ImGui::SliderFloat("Field of view", &camera.fov, 20.0f, 120.0f);
    ImGui::SliderFloat("Move speed", &cameraController.moveSpeed, 0.1f, 20.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
}


// ImGui needs unique IDs per widget; "##sun" suffixes are hidden from the labels.
void Editor::buildLightingUi() {
    SceneEnvironment& environment = scene.environment();
    ImGui::SeparatorText("Sun");
    ImGui::Checkbox("Enabled##sun", &environment.sunEnabled);
    ImGui::SliderFloat("Azimuth", &environment.sunAzimuth, -180.0f, 180.0f);
    ImGui::SliderFloat("Elevation", &environment.sunElevation, -10.0f, 90.0f);
    ImGui::SliderFloat("Size (degrees)", &environment.sunAngularRadius, 0.0f, 15.0f, "%.2f");
    ImGui::ColorEdit3("Color##sun", &environment.sunColor.x);
    ImGui::SliderFloat("Intensity##sun", &environment.sunIntensity, 0.0f, 5.0f);
    renderer.buildShadowsUi();

    ImGui::SeparatorText("Sky");
    buildEnvironmentUi();

    ImGui::SeparatorText("Lights from models");
    renderer.buildPunctualLightSettingsUi();
}


// The editor picked a map: the scene loads it (on failure the old one stays, and the error is shown)
void Editor::pickEnvironmentMap(const std::string& fileName) {
    try {
        scene.setEnvironmentMap(fileName);
        environmentError.clear();
    } catch (const std::exception& e) {
        environmentError = e.what();
        std::cerr << environmentError << std::endl;
    }
}


// The "Sky" part of the Lighting tab: the scene's background, and how the renderer samples it (its own widgets). ImGui needs unique
// IDs per widget; "##sky"/"##map" suffixes are hidden.
void Editor::buildEnvironmentUi() {
    SceneEnvironment& environment = scene.environment();
    const std::vector<std::string>& files = assets.environmentFiles();
    if (ImGui::Combo("Environment", &environment.background,
                     "Sky gradient\0HDR map\0Black (only emissive surfaces light the scene)\0White (furnace test)\0") &&
        environment.background == static_cast<int>(Background::Map) && !assets.isLoaded(environment.map) &&
        !files.empty()) {
        pickEnvironmentMap(files[0]);  // the first map, until another is picked
    }
    Background mode = static_cast<Background>(environment.background);
    if (mode == Background::White) {
        ImGui::TextDisabled("turn the sun off; white materials should vanish into the background");
    }
    if (mode == Background::Sky) {
        ImGui::ColorEdit3("Zenith", &environment.skyZenithColor.x);
        ImGui::ColorEdit3("Horizon", &environment.skyHorizonColor.x);
        ImGui::ColorEdit3("Ground", &environment.groundColor.x);
        ImGui::SliderFloat("Intensity##sky", &environment.skyIntensity, 0.0f, 3.0f);
    } else if (mode == Background::Map) {
        if (files.empty()) {
            ImGui::TextDisabled("no .hdr files in the environments folder (resource/environments)");
        } else {
            const char* current = assets.isLoaded(environment.map) ? assets.environmentMap(environment.map).name.c_str()
                                                                   : "(none)";
            if (ImGui::BeginCombo("Map", current)) {
                for (const std::string& file : files) {
                    if (ImGui::Selectable(file.c_str(), file == current)) {
                        pickEnvironmentMap(file);
                    }
                }
                ImGui::EndCombo();
            }
            if (stats.environmentMapSize.x > 0) {
                ImGui::SameLine();
                ImGui::TextDisabled("%u x %u", stats.environmentMapSize.x, stats.environmentMapSize.y);
            }
            ImGui::SliderFloat("Rotation##map", &environment.mapRotation, -180.0f, 180.0f, "%.0f deg");
            // Down to 1e-5: night maps come brightness-normalized like day ones (Poly Haven), while real moonlight
            // or city sky glow is ~1/30000 of daylight
            ImGui::SliderFloat("Intensity##map", &environment.mapIntensity, 0.00001f, 100.0f, "%.2g",
                               ImGuiSliderFlags_Logarithmic);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Multiplies the map. Night maps are stored about as bright as day ones: for a night\n"
                                  "scene lit by street lamps (Bistro), around 0.0001-0.001 (real moonlight is ~0.0001).");
            }
            if (environment.sunEnabled) {
                ImGui::TextDisabled("a map with a sun in it already lights the scene: the Sun above adds a second one");
            }
        }
        if (!environmentError.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", environmentError.c_str());
        }
    }
    if (mode != Background::Black) {
        renderer.buildSkySamplingUi();
    }
}
