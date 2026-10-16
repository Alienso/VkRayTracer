//
// The program's settings between runs (app/UserSettings.h): written and read back, the program's render settings
// only, recent scenes, files that aren't right.
//

#include <catch_amalgamated.hpp>
#include <fstream>
#include "TestHelpers.h"
#include "app/UserSettings.h"

static void write(const std::string& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary);
    file << text;
}

TEST_CASE("User settings: written and read back", "[UserSettings]") {
    TempFolder folder;
    UserSettings user;
    user.windowWidth = 1280;
    user.windowHeight = 720;
    user.maximized = true;
    user.recentScenes = {"scenes/a.json", "scenes/b.json"};
    user.lastScene = "scenes/a.json";
    RenderSettings settings;
    settings.renderMode = static_cast<int>(RenderMode::DlssDlaa);
    settings.exposure = 3.0f;  // the scene's: not kept here
    std::string path = folder.file("user_settings.json");
    saveUserSettings(path, userSettingsText(user, settings));

    UserSettings readUser;
    RenderSettings readSettings;
    CHECK(loadUserSettings(path, readUser, readSettings).empty());
    CHECK(readUser.windowWidth == 1280);
    CHECK(readUser.windowHeight == 720);
    CHECK(readUser.maximized);
    CHECK_FALSE(readUser.fullscreen);
    CHECK(readUser.recentScenes == user.recentScenes);
    CHECK(readUser.lastScene == "scenes/a.json");
    CHECK(readSettings.renderMode == static_cast<int>(RenderMode::DlssDlaa));
    CHECK(readSettings.exposure == RenderSettings{}.exposure);
    CHECK(userSettingsText(readUser, readSettings) == userSettingsText(user, readSettings));
}

TEST_CASE("User settings: no file is the first run: nothing changes, nothing to report", "[UserSettings]") {
    TempFolder folder;
    UserSettings user;
    RenderSettings settings;
    CHECK(loadUserSettings(folder.file("none.json"), user, settings).empty());
    CHECK(user.windowWidth == 0);
}

TEST_CASE("User settings: a broken file gives the defaults and says so", "[UserSettings]") {
    TempFolder folder;
    std::string path = folder.file("user_settings.json");
    UserSettings user;
    RenderSettings settings;
    SECTION("not JSON") { write(path, "{ nope"); }
    SECTION("another file") { write(path, R"({"format": "vulkanDemo scene", "version": 1})"); }
    SECTION("a newer version") { write(path, R"({"format": "vulkanDemo user settings", "version": 7})"); }
    CHECK(loadUserSettings(path, user, settings).size() == 1);
    CHECK(user.windowWidth == 0);
    CHECK(settings.renderMode == RenderSettings{}.renderMode);
}

TEST_CASE("User settings: what can't be read is skipped, the rest is kept", "[UserSettings]") {
    TempFolder folder;
    std::string path = folder.file("user_settings.json");
    write(path, R"({"format": "vulkanDemo user settings", "version": 1,
                    "settings": {"renderMode": "DlssDlaa", "exposure": 2},
                    "window": {"width": "wide", "height": 700}, "lastScene": "scenes/x.json"})");
    UserSettings user;
    RenderSettings settings;
    std::vector<std::string> problems = loadUserSettings(path, user, settings);
    CHECK(problems.size() == 2);  // the scene's setting, the width
    CHECK(settings.renderMode == static_cast<int>(RenderMode::DlssDlaa));
    CHECK(settings.exposure == RenderSettings{}.exposure);
    CHECK(user.windowWidth == 0);
    CHECK(user.windowHeight == 700);
    CHECK(user.lastScene == "scenes/x.json");
}

TEST_CASE("User settings: recent scenes, newest first, each once, at most ten", "[UserSettings]") {
    UserSettings user;
    user.addRecentScene("a");
    user.addRecentScene("b");
    user.addRecentScene("a");
    CHECK(user.recentScenes == std::vector<std::string>{"a", "b"});
    for (int i = 0; i < 20; i++) {
        user.addRecentScene(std::to_string(i));
    }
    CHECK(user.recentScenes.size() == UserSettings::maxRecentScenes);
    CHECK(user.recentScenes.front() == "19");
}

TEST_CASE("User settings: saving replaces the file", "[UserSettings]") {
    TempFolder folder;
    std::string path = folder.file("user_settings.json");
    UserSettings user;
    RenderSettings settings;
    saveUserSettings(path, userSettingsText(user, settings));
    user.lastScene = "scenes/y.json";
    saveUserSettings(path, userSettingsText(user, settings));
    UserSettings read;
    CHECK(loadUserSettings(path, read, settings).empty());
    CHECK(read.lastScene == "scenes/y.json");
    CHECK_FALSE(std::filesystem::exists(path + ".saving"));
}
