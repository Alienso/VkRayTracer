//
// The editor's scene files: New, Open, Save, Save As (in the scenes folder next to the executable), the unsaved
// changes mark (in the Scene tab and the window title) and the question before New / Open / closing the window
// throws them away. The files themselves: Scene::save / load (SceneFile.cpp).
//

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <imgui/imgui.h>
#include "editor/Editor.h"

void Editor::scanSceneFiles() {
    std::string current = sceneToOpen < static_cast<int>(sceneFiles.size()) ? sceneFiles[sceneToOpen] : "";
    sceneFiles.clear();
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(scenesDirectory, error)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            sceneFiles.push_back(entry.path().filename().string());
        }
    }
    std::sort(sceneFiles.begin(), sceneFiles.end());
    auto it = std::find(sceneFiles.begin(), sceneFiles.end(), current);
    sceneToOpen = it != sceneFiles.end() ? static_cast<int>(it - sceneFiles.begin()) : 0;
}

bool Editor::hasUnsavedChanges() {
    unsaved = scene.contentText() != savedText;
    unsavedCheckTime = ImGui::GetTime();
    return unsaved;
}

bool Editor::allowClose() {
    if (!hasUnsavedChanges()) {
        return true;
    }
    request(FileAction::Exit);
    return false;
}

void Editor::request(FileAction action, const std::string& path) {
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
            savedText = scene.contentText();
            selected = 0;
            fileMessage.clear();
            fileWarnings.clear();
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

void Editor::openSceneFile(const std::string& path) {
    try {
        fileWarnings = scene.load(path);
        scenePath = path;
        savedText = scene.contentText();
        unsaved = false;
        selected = 0;
        fileMessage.clear();
        for (const std::string& warning : fileWarnings) {
            std::cerr << path << ": " << warning << std::endl;
        }
    } catch (const std::exception& e) {
        fileMessage = e.what();  // the scene is unchanged
        std::cerr << fileMessage << std::endl;
    }
}

bool Editor::saveTo(const std::string& path) {
    try {
        scene.save(path);
        scenePath = path;
        savedText = scene.contentText();
        unsaved = false;
        fileMessage = "saved " + path;
        scanSceneFiles();
        return true;
    } catch (const std::exception& e) {
        fileMessage = e.what();
        std::cerr << fileMessage << std::endl;
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
    if (title != shownTitle) {
        window.setTitle(title);
        shownTitle = title;
    }
}

// At the top of the Scene tab: which scene this is, and New / Open / Save / Save As
void Editor::buildFileUi() {
    if (ImGui::GetTime() - unsavedCheckTime > 1.0) {
        hasUnsavedChanges();
    }
    std::string name = scenePath.empty() ? "new scene" : std::filesystem::path(scenePath).filename().string();
    ImGui::Text("Scene: %s", name.c_str());
    if (unsaved) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "(unsaved changes)");
    }
    if (ImGui::Button("New")) {
        request(FileAction::New);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("An empty scene: the floor, the default camera, sun and sky.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Save") ) {
        if (!scenePath.empty()) {
            saveTo(scenePath);
        } else if (std::string path = scenePathFor(scenesDirectory, saveAsName); !path.empty()) {
            saveTo(path);
        } else {
            fileMessage = "not saved yet: type a name under \"Save as\"";
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", scenePath.empty() ? "Not saved yet: saves under the name typed below."
                                                  : ("Into " + scenePath).c_str());
    }

    const char* preview = sceneFiles.empty() ? "(no scenes yet)" : sceneFiles[sceneToOpen].c_str();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    if (ImGui::BeginCombo("##sceneToOpen", preview)) {
        for (int i = 0; i < static_cast<int>(sceneFiles.size()); i++) {
            if (ImGui::Selectable(sceneFiles[i].c_str(), i == sceneToOpen)) {
                sceneToOpen = i;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Open") && !sceneFiles.empty()) {
        request(FileAction::Open, std::string(scenesDirectory) + "/" + sceneFiles[sceneToOpen]);
    }
    ImGui::SameLine();
    if (ImGui::Button("Rescan##scenes")) {
        scanSceneFiles();
    }

    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    ImGui::InputTextWithHint("##saveAsName", "name", saveAsName, sizeof(saveAsName));
    ImGui::SameLine();
    std::string saveAsPath = scenePathFor(scenesDirectory, saveAsName);
    if (ImGui::Button("Save as") && !saveAsPath.empty()) {
        saveTo(saveAsPath);
    }
    if (ImGui::IsItemHovered() && !saveAsPath.empty()) {
        ImGui::SetTooltip("Into %s%s", saveAsPath.c_str(),
                          std::filesystem::exists(saveAsPath) ? "\n(replaces the scene there)" : "");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Scenes are saved in the scenes folder next to the program. Builds copy the scenes from\n"
                          "the project's scenes/ folder there, but never over a scene already there: to keep one in\n"
                          "git, copy it back into scenes/.");
    }

    if (!fileMessage.empty()) {
        bool error = fileMessage.rfind("saved ", 0) != 0;
        ImGui::TextColored(error ? ImVec4(1.0f, 0.4f, 0.4f, 1.0f) : ImVec4(0.6f, 0.9f, 0.6f, 1.0f), "%s",
                           fileMessage.c_str());
    }
    if (!fileWarnings.empty() &&
        ImGui::TreeNode("skipped", "Opening skipped %zu thing(s)", fileWarnings.size())) {
        for (const std::string& warning : fileWarnings) {
            ImGui::TextWrapped("%s", warning.c_str());
        }
        ImGui::TreePop();
    }
    ImGui::Separator();
}

// The question before unsaved changes are thrown away (New, Open, closing the window)
void Editor::buildUnsavedQuestion() {
    if (askAboutUnsaved) {
        ImGui::OpenPopup("Unsaved changes");
        askAboutUnsaved = false;
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
    if (!fileMessage.empty() && fileMessage.rfind("saved ", 0) != 0) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", fileMessage.c_str());
    }
    ImGui::EndPopup();
}
