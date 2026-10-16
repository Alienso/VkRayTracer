//
// Where the editor's panels go: a dock space over the window whose middle stays empty (the image shows through it,
// and the mouse there moves the camera), the panels docked around it as on the first run (or after View > Reset
// layout), the menu bar, the numbers over the image, and the Statistics, Log, Rendering and Debug panels. ImGui saves
// the layout in imgui.ini next to the executable; which panels are open is saved there too ([Editor][Panels]).
//

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>  // DockBuilder (the starting layout), the ini settings handler
#include "editor/Editor.h"

static ImGuiID dockSpaceId() {
    return ImHashStr("EditorDockSpace");
}

void Editor::initUi() {
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // imgui.ini keeps where the panels are; this keeps which are open
    ImGuiSettingsHandler handler;
    handler.TypeName = "Editor";
    handler.TypeHash = ImHashStr("Editor");
    handler.UserData = this;
    handler.ReadOpenFn = [](ImGuiContext*, ImGuiSettingsHandler*, const char*) -> void* {
        return reinterpret_cast<void*>(1);  // one entry, [Editor][Panels]
    };
    handler.ReadLineFn = [](ImGuiContext*, ImGuiSettingsHandler* settings, void*, const char* line) {
        Editor& editor = *static_cast<Editor*>(settings->UserData);
        char name[64];
        int open = 0;
        if (std::sscanf(line, "%63[^=]=%d", name, &open) != 2) {
            return;
        }
        if (std::strcmp(name, "Overlay") == 0) {
            editor.showOverlay = open != 0;
        }
        for (Panel& panel : editor.panels) {
            if (std::strcmp(panel.name, name) == 0) {
                panel.open = open != 0;
            }
        }
    };
    handler.WriteAllFn = [](ImGuiContext*, ImGuiSettingsHandler* settings, ImGuiTextBuffer* buffer) {
        const Editor& editor = *static_cast<const Editor*>(settings->UserData);
        buffer->appendf("[%s][Panels]\n", settings->TypeName);
        for (const Panel& panel : editor.panels) {
            buffer->appendf("%s=%d\n", panel.name, panel.open ? 1 : 0);
        }
        buffer->appendf("Overlay=%d\n\n", editor.showOverlay ? 1 : 0);
    };
    ImGui::AddSettingsHandler(&handler);
}

void Editor::log(LogLevel level, const std::string& text) {
    logEntries.push_back({level, text});
    if (logEntries.size() > 1000) {
        logEntries.erase(logEntries.begin(), logEntries.begin() + 100);
    }
    if (level != LogLevel::Info) {
        for (Panel& panel : panels) {
            if (std::strcmp(panel.name, "Log") == 0) {
                panel.open = true;
            }
        }
        showLogNow = true;
    }
}

// Over the whole window below the menu bar. Its middle (the central node) is see-through and lets the mouse through.
void Editor::buildDockSpace() {
    ImGuiID dockSpace = dockSpaceId();
    if (resetLayout || ImGui::DockBuilderGetNode(dockSpace) == nullptr) {  // nothing in imgui.ini yet
        buildDefaultLayout(dockSpace);
        resetLayout = false;
    }
    ImGui::DockSpaceOverViewport(dockSpace, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);
}

// Outliner left with Statistics / Log below it, Details right with the settings panels below it, the image between
void Editor::buildDefaultLayout(ImGuiID dockSpace) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(dockSpace);
    ImGui::DockBuilderAddNode(dockSpace, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockSpace, viewport->WorkSize);
    ImGuiID middle = dockSpace;
    ImGuiID left = ImGui::DockBuilderSplitNode(middle, ImGuiDir_Left, 0.18f, nullptr, &middle);
    ImGuiID right = ImGui::DockBuilderSplitNode(middle, ImGuiDir_Right, 0.32f, nullptr, &middle);
    ImGuiID leftBottom = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.35f, nullptr, &left);
    ImGuiID rightBottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.55f, nullptr, &right);
    ImGui::DockBuilderDockWindow("Outliner", left);
    ImGui::DockBuilderDockWindow("Statistics", leftBottom);
    ImGui::DockBuilderDockWindow("Log", leftBottom);
    ImGui::DockBuilderDockWindow("Details", right);
    ImGui::DockBuilderDockWindow("Rendering", rightBottom);
    ImGui::DockBuilderDockWindow("Lighting", rightBottom);
    ImGui::DockBuilderDockWindow("Camera", rightBottom);
    ImGui::DockBuilderDockWindow("Debug", rightBottom);
    ImGui::DockBuilderFinish(dockSpace);
}

void Editor::buildMenuBar() {
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }
    if (ImGui::BeginMenu("File")) {
        buildFileMenu();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Add")) {
        buildAddMenu();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        for (Panel& panel : panels) {
            if (ImGui::MenuItem(panel.name, nullptr, &panel.open)) {
                ImGui::MarkIniSettingsDirty();
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Numbers over the image", nullptr, &showOverlay)) {
            ImGui::MarkIniSettingsDirty();
        }
        ImGui::MenuItem("Hide the UI", "F1", &uiHidden);
        ImGui::MenuItem("Blend game steps", nullptr, &blendSteps);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("While playing: the game moves things 60 times a second. On, they are shown\n"
                              "between its steps every frame (smooth, up to a step behind); off, as each\n"
                              "step left them (they hold still in some frames and jump in others).");
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Reset layout")) {
            resetLayout = true;
            for (Panel& panel : panels) {
                panel.open = true;
            }
            ImGui::MarkIniSettingsDirty();
        }
        ImGui::EndMenu();
    }
    bool wasPlaying = playing;  // (clicking changes playing: the color is pushed and popped by what it was)
    if (wasPlaying) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 1.0f, 0.5f, 1.0f));
    }
    bool clicked = ImGui::MenuItem(wasPlaying ? "Stop" : "Play", "F5");
    if (wasPlaying) {
        ImGui::PopStyleColor();
    }
    if (clicked) {
        wasPlaying ? stop() : play();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(wasPlaying ? "Puts the scene back as it was at Play." :
                                    "Runs the game on the scene. Stop puts the scene back as it was:\n"
                                    "what changes while playing, also by hand, isn't kept.");
    }

    // Right-aligned: what is loading, the open scene, and whether it has unsaved changes
    std::string name = scenePath.empty() ? "new scene" : std::filesystem::path(scenePath).filename().string();
    if (playing) {
        name = "PLAYING (changes are undone by Stop)    " + name;
    }
    if (unsaved) {
        name += "  (unsaved changes)";
    }
    std::string loading;
    if (!assets.currentLoad().empty()) {
        loading = "loading " + assets.currentLoad();
        if (assets.waitingLoads() > 0) {
            loading += " (" + std::to_string(assets.waitingLoads()) + " more)";
        }
        loading += "    ";
    }
    float width = ImGui::CalcTextSize((loading + name).c_str()).x + ImGui::GetStyle().ItemSpacing.x * 2.0f;
    ImGui::SameLine(ImGui::GetWindowWidth() - width);
    if (!loading.empty()) {
        ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "%s", loading.c_str());
        ImGui::SameLine(0.0f, 0.0f);
    }
    if (unsaved) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "%s", name.c_str());
    } else {
        ImGui::TextDisabled("%s", name.c_str());
    }
    ImGui::EndMainMenuBar();
}

// Frame time, CPU phases and GPU passes in the image's top left corner; the mouse goes through it to the camera
void Editor::buildOverlay() {
    const ImGuiDockNode* middle = ImGui::DockBuilderGetCentralNode(dockSpaceId());
    ImVec2 corner = middle != nullptr ? middle->Pos : ImGui::GetMainViewport()->WorkPos;
    ImGui::SetNextWindowPos(ImVec2(corner.x + 10.0f, corner.y + 10.0f));
    ImGui::SetNextWindowBgAlpha(0.35f);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin("##overlay", nullptr, flags)) {
        ImGui::Text("%.1f FPS (%.2f ms)", ImGui::GetIO().Framerate, 1000.0f / ImGui::GetIO().Framerate);
        ImGui::Text("GPU: cache %.2f ms, ReSTIR %.2f, trace %.2f, DLSS %.2f, exposure %.2f, display %.2f",
                    stats.gpuPassMs[0], stats.gpuPassMs[1], stats.gpuPassMs[2], stats.gpuPassMs[3],
                    stats.gpuPassMs[4], stats.gpuPassMs[5]);
        // CPU work without the waits for the GPU and the display (those fill the frame up to the slower of them)
        if (playing) {
            ImGui::Text("CPU: %.2f ms (update %.2f, game %.2f, UI %.2f, render %.2f), waiting %.2f",
                        cpuTimes.workMs(), cpuTimes.ms[CpuFrameTimes::Update], cpuTimes.ms[CpuFrameTimes::Game],
                        cpuTimes.ms[CpuFrameTimes::Ui], cpuTimes.ms[CpuFrameTimes::Render],
                        cpuTimes.ms[CpuFrameTimes::Wait]);
        } else {
            ImGui::Text("CPU: %.2f ms (update %.2f, UI %.2f, render %.2f), waiting %.2f", cpuTimes.workMs(),
                        cpuTimes.ms[CpuFrameTimes::Update], cpuTimes.ms[CpuFrameTimes::Ui],
                        cpuTimes.ms[CpuFrameTimes::Render], cpuTimes.ms[CpuFrameTimes::Wait]);
        }
        if (playing && !gameStatus.empty()) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f), "%s", gameStatus.c_str());
        }
        if (stats.accumulating) {
            ImGui::Text("%u frames accumulated", stats.accumulatedFrames);
        }
        if (stats.debugView) {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "Debug view on (Debug panel)");
        }
    }
    ImGui::End();
}

void Editor::buildStatisticsUi() {
    ImGui::Text("GPU: %s", stats.deviceName.c_str());
    if (stats.gpuMemory.budget > 0) {
        ImGui::Text("GPU memory: %.2f of %.2f GB", stats.gpuMemory.used / 1073741824.0,
                    stats.gpuMemory.budget / 1073741824.0);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Video memory this program uses, and how much it can have without slowing down\n"
                              "(the driver's estimate, less what other programs use)\n"
                              "%u buffers and images in %u memory objects (Vulkan promises 4096 objects)",
                              stats.gpuMemory.allocations, stats.gpuMemory.memoryObjects);
        }
    }
    ImGui::Text("%llu triangles", static_cast<unsigned long long>(stats.triangles));
    ImGui::Text("%zu models, %zu meshes, %zu instances", assets.modelCount(), stats.meshes, stats.instances);
    if (size_t unused = assets.unusedModelCount(); unused > 0) {
        ImGui::TextDisabled("(%zu unused, %.0f MB)", unused, assets.unusedMemory() / (1024.0 * 1024.0));
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Models no object shows any more stay loaded, so adding them again is instant.\n"
                              "Beyond %.0f MB together, or when GPU memory runs short, the ones removed longest\n"
                              "ago are unloaded.", AssetStore::defaultUnusedBudget / (1024.0 * 1024.0));
        }
    }
    ImGui::Text("%zu materials, %zu textures (%.1f MB)", assets.materials().size(), stats.textures,
                stats.textureMemory / (1024.0 * 1024.0));
    ImGui::Text("%zu emissive triangles (%zu bright enough to sample)", stats.emissiveTriangles,
                stats.sampledEmissiveTriangles);
    ImGui::Text("BLAS memory: %.2f MB -> %.2f MB compacted",
                stats.blasBuildSize / (1024.0 * 1024.0), stats.blasSize / (1024.0 * 1024.0));
}

// What loading, saving and capturing said, newest at the bottom (warnings and errors bring the panel to the front)
void Editor::buildLogUi() {
    if (ImGui::SmallButton("Clear")) {
        logEntries.clear();
    }
    ImGui::Separator();
    ImGui::BeginChild("logLines");
    for (const LogEntry& entry : logEntries) {
        ImVec4 color = entry.level == LogLevel::Error   ? ImVec4(1.0f, 0.4f, 0.4f, 1.0f)
                     : entry.level == LogLevel::Warning ? ImVec4(1.0f, 0.75f, 0.2f, 1.0f)
                                                        : ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::TextWrapped("%s", entry.text.c_str());
        ImGui::PopStyleColor();
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0f);  // at the bottom: stays there as lines come in
    }
    ImGui::EndChild();
}

void Editor::buildDebugPanel() {
    buildRendererDebugUi();
    ImGui::Separator();
    ImGui::Checkbox("Check scene updates", &scene.checkUpdates);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Slow: every frame something moves, the whole render list is also made from scratch and\n"
                          "compared with the one that only changed what moved. A difference means a change was\n"
                          "missed (printed to the console).");
    }
    if (scene.updateCheckFailures() > 0) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%llu frames differed",
                           static_cast<unsigned long long>(scene.updateCheckFailures()));
    }
    ImGui::Checkbox("Show ImGui demo", &showImGuiDemo);
}
