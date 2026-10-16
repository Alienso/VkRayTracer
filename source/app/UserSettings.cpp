//
// The program's settings between runs. See UserSettings.h.
//

#include "app/UserSettings.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <nlohmann/json.hpp>
#include "renderer/RenderSettings.h"
#include "renderer/RenderSettingsFile.h"

static const char* formatName = "vulkanDemo user settings";
static constexpr int fileVersion = 1;

void UserSettings::addRecentScene(const std::string& path) {
    recentScenes.erase(std::remove(recentScenes.begin(), recentScenes.end(), path), recentScenes.end());
    recentScenes.insert(recentScenes.begin(), path);
    if (recentScenes.size() > maxRecentScenes) {
        recentScenes.resize(maxRecentScenes);
    }
}

std::string userSettingsText(const UserSettings& user, const RenderSettings& settings) {
    nlohmann::ordered_json json;
    json["format"] = formatName;
    json["version"] = fileVersion;
    json["settings"] = renderSettingsToJson(settings, SettingsScope::Program, true);
    json["window"] = {{"width", user.windowWidth}, {"height", user.windowHeight}, {"maximized", user.maximized},
                      {"fullscreen", user.fullscreen}};
    json["recentScenes"] = user.recentScenes;
    json["lastScene"] = user.lastScene;
    return json.dump(2) + "\n";
}

std::vector<std::string> loadUserSettings(const std::string& path, UserSettings& user, RenderSettings& settings) {
    std::vector<std::string> problems;
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return problems;  // the first run
    }
    nlohmann::json json;
    try {
        json = nlohmann::json::parse(file);
    } catch (const nlohmann::json::exception& e) {
        problems.push_back(std::string("isn't valid JSON (") + e.what() + "): the defaults are used");
        return problems;
    }
    if (!json.is_object() || json.value("format", std::string()) != formatName) {
        problems.emplace_back("isn't a user settings file: the defaults are used");
        return problems;
    }
    if (!json.contains("version") || !json["version"].is_number_integer() || json["version"].get<int>() > fileVersion) {
        problems.emplace_back("is of another version: the defaults are used");
        return problems;
    }

    if (auto s = json.find("settings"); s != json.end()) {
        for (const std::string& problem : applyRenderSettingsJson(settings, *s, SettingsScope::Program)) {
            problems.push_back("settings: " + problem);
        }
    }
    auto readInt = [&](const nlohmann::json& object, const char* key, int& value) {
        if (auto it = object.find(key); it != object.end()) {
            if (it->is_number_integer() && it->get<int>() >= 0) {
                value = it->get<int>();
            } else {
                problems.push_back(std::string("window: \"") + key + "\": expected a whole number");
            }
        }
    };
    auto readBool = [&](const nlohmann::json& object, const char* key, bool& value) {
        if (auto it = object.find(key); it != object.end()) {
            if (it->is_boolean()) {
                value = it->get<bool>();
            } else {
                problems.push_back(std::string("window: \"") + key + "\": expected true or false");
            }
        }
    };
    if (auto w = json.find("window"); w != json.end() && w->is_object()) {
        readInt(*w, "width", user.windowWidth);
        readInt(*w, "height", user.windowHeight);
        readBool(*w, "maximized", user.maximized);
        readBool(*w, "fullscreen", user.fullscreen);
    }
    if (auto recent = json.find("recentScenes"); recent != json.end() && recent->is_array()) {
        user.recentScenes.clear();
        for (const nlohmann::json& entry : *recent) {
            if (entry.is_string() && user.recentScenes.size() < UserSettings::maxRecentScenes) {
                user.recentScenes.push_back(entry.get<std::string>());
            }
        }
    }
    if (auto last = json.find("lastScene"); last != json.end() && last->is_string()) {
        user.lastScene = last->get<std::string>();
    }
    return problems;
}

void saveUserSettings(const std::string& path, const std::string& text) {
    std::filesystem::path target(path);
    std::filesystem::path temporary = target;
    temporary += ".saving";
    {
        std::ofstream file(temporary, std::ios::binary);
        file << text;
        file.close();
        if (!file) {
            throw std::runtime_error("can't write " + temporary.string());
        }
    }
    std::error_code error;
    std::filesystem::rename(temporary, target, error);  // replaces the old file in one step
    if (error) {
        std::filesystem::remove(temporary);
        throw std::runtime_error("can't write " + path + ": " + error.message());
    }
}
