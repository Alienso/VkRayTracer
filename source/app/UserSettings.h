//
// What the program keeps between runs, in user_settings.json in the folder it runs in (next to imgui.ini): the
// program's render settings (RenderSettingsFile.h: render mode, DLSS model, image size, ...), the window's size and
// state, the scenes opened lately and the last one (opened again at start). Written a second after a change and on
// exit (Application.cpp). Benches neither read nor write it: they start from the defaults.
//
// {"format": "vulkanDemo user settings", "version": 1,
//  "settings": {"renderMode": "DlssDlaa"},   only what differs from the defaults
//  "window": {"width": 1600, "height": 900, "maximized": false, "fullscreen": false},
//  "recentScenes": ["scenes/sponza.json", ...],   newest first
//  "lastScene": "scenes/sponza.json"}             "" or missing: the default scene
//

#ifndef VULKANDEMO_USERSETTINGS_H
#define VULKANDEMO_USERSETTINGS_H

#include <string>
#include <vector>

struct RenderSettings;

struct UserSettings {
    static constexpr size_t maxRecentScenes = 10;

    int windowWidth = 0;   // the window's size when it isn't maximized or fullscreen; 0: the default
    int windowHeight = 0;
    bool maximized = false;
    bool fullscreen = false;
    std::vector<std::string> recentScenes;  // newest first
    std::string lastScene;                  // "": the default scene

    // To the front of the list (taken out further down if it was there); the oldest beyond maxRecentScenes go
    void addRecentScene(const std::string& path);
};

// The file's text (the program's render settings: only the ones that differ from the defaults)
std::string userSettingsText(const UserSettings& user, const RenderSettings& settings);
// Reads the file into both (of the render settings, the program's only). No file: nothing changes, no problems. What
// can't be read is skipped and named in the result.
std::vector<std::string> loadUserSettings(const std::string& path, UserSettings& user, RenderSettings& settings);
// Through a temporary file, so a failed write keeps the old one; throws when it can't be written
void saveUserSettings(const std::string& path, const std::string& text);

#endif //VULKANDEMO_USERSETTINGS_H
