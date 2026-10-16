//
// The editor's scene files: the File menu's New, Open, Open recent, Save, Save As (in the scenes folder next to the
// executable), the unsaved changes mark (in the menu bar and the window title) and the question before New / Open /
// closing the window throws them away; Play / Stop, which keep the scene and put it back. The files themselves: Scene::save / load (SceneFile.cpp). The recent scenes
// and the last one (opened again at the next start) go into the user settings (app/UserSettings.h).
//

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <imgui/imgui.h>
#include "editor/Editor.h"

void Editor::scanSceneFiles() {
    sceneFiles.clear();
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(scenesDirectory, error)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            sceneFiles.push_back(entry.path().filename().string());
        }
    }
    std::sort(sceneFiles.begin(), sceneFiles.end());
}

bool Editor::hasUnsavedChanges() {
    if (!playing) {  // (while playing: as it was at Play, which is what Stop puts back)
        unsaved = scene.contentText() != savedText;
    }
    unsavedCheckTime = ImGui::GetTime();
    return unsaved;
}

bool Editor::allowClose() {
    stop();
    if (!hasUnsavedChanges()) {
        return true;
    }
    request(FileAction::Exit);
    return false;
}

void Editor::request(FileAction action, const std::string& path) {
    stop();  // (the menu has these off while playing; Exit stops)
    if (hasUnsavedChanges()) {
        pendingAction = action;
        pendingPath = path;
        askAboutUnsaved = true;  // opened in build(), at the top of ImGui's stack
    } else {
        perform(action, path);
    }
}

void Editor::perform(FileAction action, const std::string& path) {
    switch (action) {
        case FileAction::New:
            scene.clear();
            scenePath.clear();
            userSettings.lastScene.clear();  // the default scene at the next start
            savedText = scene.contentText();
            selected = 0;
            log(LogLevel::Info, "New scene");
            break;
        case FileAction::Open:
            openSceneFile(path);
            break;
        case FileAction::Exit:
            window.requestClose();  // doesn't ask again
            break;
        case FileAction::None:
            break;
    }
    unsaved = false;
}

bool Editor::openSceneFile(const std::string& path) {
    try {
        std::vector<std::string> warnings = scene.load(path);
        scenePath = path;
        userSettings.addRecentScene(path);
        userSettings.lastScene = path;
        savedText = scene.contentText();
        unsaved = false;
        selected = 0;
        log(LogLevel::Info, "Opened " + path);
        for (const std::string& warning : warnings) {  // what opening skipped
            std::cerr << path << ": " << warning << std::endl;
            log(LogLevel::Warning, path + ": " + warning);
        }
        return true;
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;  // the scene is unchanged
        log(LogLevel::Error, e.what());
        return false;
    }
}

bool Editor::saveTo(const std::string& path) {
    try {
        scene.save(path);
        scenePath = path;
        userSettings.addRecentScene(path);
        userSettings.lastScene = path;
        savedText = scene.contentText();
        unsaved = false;
        saveError.clear();
        log(LogLevel::Info, "Saved " + path);
        return true;
    } catch (const std::exception& e) {
        saveError = e.what();
        std::cerr << saveError << std::endl;
        log(LogLevel::Error, saveError);
        return false;
    }
}

// "scenes/<name>.json" for a name typed in (".json" added when missing); empty for an empty name
static std::string scenePathFor(const char* scenesDirectory, const char* name) {
    std::string file = name;
    file.erase(0, file.find_first_not_of(' '));
    file.erase(file.find_last_not_of(' ') + 1);
    if (file.empty()) {
        return "";
    }
    if (std::filesystem::path(file).extension() != ".json") {
        file += ".json";
    }
    return std::string(scenesDirectory) + "/" + file;
}

void Editor::updateTitle() {
    std::string title = "Vulkan - " + (scenePath.empty() ? std::string("new scene")
                                                         : std::filesystem::path(scenePath).filename().string());
    if (unsaved) {
        title += " *";
    }
    if (playing) {
        title += "  [playing]";
    }
    if (title != shownTitle) {
        window.setTitle(title);
        shownTitle = title;
    }
}

void Editor::buildFileMenu() {
    ImGui::BeginDisabled(playing);
    if (ImGui::MenuItem("New")) {
        request(FileAction::New);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("An empty scene: the floor, the default camera, sun and sky.");
    }
    if (ImGui::BeginMenu("Open")) {
        if (ImGui::IsWindowAppearing()) {
            scanSceneFiles();
        }
        if (sceneFiles.empty()) {
            ImGui::TextDisabled("no scenes in the %s folder yet", scenesDirectory);
        }
        for (const std::string& file : sceneFiles) {
            if (ImGui::MenuItem(file.c_str())) {
                request(FileAction::Open, std::string(scenesDirectory) + "/" + file);
            }
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Open recent", !userSettings.recentScenes.empty())) {
        std::string picked;
        for (const std::string& path : userSettings.recentScenes) {
            bool there = std::filesystem::exists(path);
            if (ImGui::MenuItem(path.c_str(), there ? nullptr : "missing", false, there)) {
                picked = path;
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Clear the list")) {
            userSettings.recentScenes.clear();
        }
        ImGui::EndMenu();
        if (!picked.empty()) {
            request(FileAction::Open, picked);  // (after the loop: opening changes the list)
        }
    }
    if (ImGui::MenuItem("Save", "Ctrl+S")) {
        if (scenePath.empty()) {
            askSaveAs = true;
        } else {
            saveTo(scenePath);
        }
    }
    if (ImGui::IsItemHovered() && !scenePath.empty()) {
        ImGui::SetTooltip("Into %s", scenePath.c_str());
    }
    if (ImGui::MenuItem("Save as...")) {
        askSaveAs = true;
    }
    ImGui::EndDisabled();
    if (playing) {
        ImGui::TextDisabled("(Stop first: the scene is being played)");
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Capture image")) {
        renderer.requestCapture();  // the next frame's image, without the UI, as a PNG next to the executable
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("The next frame as a PNG next to the program, without the UI.");
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Exit")) {
        request(FileAction::Exit);
    }
}

// File > Save As (and Save before the scene has a file): a name for the scene in the scenes folder
void Editor::buildSaveAsQuestion() {
    if (askSaveAs) {
        ImGui::OpenPopup("Save scene as");
        askSaveAs = false;
        saveError.clear();
    }
    if (!ImGui::BeginPopupModal("Save scene as", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    ImGui::Text("A name for the scene (in the %s folder next to the program):", scenesDirectory);
    if (ImGui::IsWindowAppearing()) {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    bool enter = ImGui::InputTextWithHint("##saveAsName", "name", saveAsName, sizeof(saveAsName),
                                          ImGuiInputTextFlags_EnterReturnsTrue);
    std::string path = scenePathFor(scenesDirectory, saveAsName);
    if (!path.empty() && std::filesystem::exists(path)) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "replaces %s", path.c_str());
    }
    ImGui::BeginDisabled(path.empty());
    if ((ImGui::Button("Save") || enter) && !path.empty() && saveTo(path)) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ImGui::CloseCurrentPopup();
    }
    if (!saveError.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", saveError.c_str());
    }
    ImGui::TextDisabled("Builds copy the scenes from the project's scenes/ folder there, but never over a scene\n"
                        "already there: to keep one in git, copy it back into scenes/.");
    ImGui::EndPopup();
}

// The question before unsaved changes are thrown away (New, Open, closing the window)
void Editor::buildUnsavedQuestion() {
    if (askAboutUnsaved) {
        ImGui::OpenPopup("Unsaved changes");
        askAboutUnsaved = false;
        saveError.clear();
    }
    if (!ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    const char* what = pendingAction == FileAction::Exit ? "closing" : pendingAction == FileAction::New
                                                                       ? "starting a new scene" : "opening another";
    ImGui::Text("The scene has changes that aren't saved. Save them before %s?", what);
    std::string path = scenePath;
    if (path.empty()) {
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
        ImGui::InputTextWithHint("##unsavedName", "name", saveAsName, sizeof(saveAsName));
        path = scenePathFor(scenesDirectory, saveAsName);
    }
    if (ImGui::Button("Save") && !path.empty()) {
        if (saveTo(path)) {
            ImGui::CloseCurrentPopup();
            perform(pendingAction, pendingPath);
            pendingAction = FileAction::None;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Don't save")) {
        ImGui::CloseCurrentPopup();
        perform(pendingAction, pendingPath);
        pendingAction = FileAction::None;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        ImGui::CloseCurrentPopup();
        pendingAction = FileAction::None;
    }
    if (!saveError.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", saveError.c_str());
    }
    ImGui::EndPopup();
}

void Editor::play() {
    if (playing) {
        return;
    }
    hasUnsavedChanges();  // the mark as it is now: kept while playing
    playSnapshot = scene.text();
    playing = true;
    log(LogLevel::Info, "Playing: what changes now is undone by Stop");
}

void Editor::stop() {
    if (!playing) {
        return;
    }
    playing = false;
    try {
        // Warnings would be the ones opening the scene gave (the text is the scene's own): not logged again
        scene.loadText(playSnapshot, "the scene at Play");
        log(LogLevel::Info, "Stopped: the scene is back as it was at Play");
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;  // not expected: the text is the scene's own
        log(LogLevel::Error, std::string("Stop couldn't put the scene back: ") + e.what());
    }
    playSnapshot.clear();
    hasUnsavedChanges();
}
