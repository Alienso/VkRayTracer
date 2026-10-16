//
// A game component's properties into typed values. See GameProperties.h.
//

#include <sstream>
#include "game/GameProperties.h"

GameProperties::GameProperties(const std::string& text, std::string where, std::vector<std::string>& warnings)
    : where(std::move(where)), warnings(warnings) {
    if (text.empty()) {
        return;
    }
    try {
        nlohmann::json parsed = nlohmann::json::parse(text);
        if (parsed.is_object()) {
            json = std::move(parsed);
        } else {
            warn("the properties aren't an object; the defaults are used");
        }
    } catch (const nlohmann::json::exception&) {
        warn("the properties aren't valid JSON; the defaults are used");  // (the scene only keeps valid ones)
    }
}

const nlohmann::json* GameProperties::find(const char* key) {
    asked.insert(key);
    auto it = json.find(key);
    return it == json.end() ? nullptr : &*it;
}

static std::string shown(float value) {
    std::ostringstream text;
    text << value;
    return text.str();
}

float GameProperties::number(const char* key, float fallback) {
    const nlohmann::json* value = find(key);
    if (value == nullptr) {
        return fallback;
    }
    if (!value->is_number()) {
        warn(std::string("\"") + key + "\" isn't a number; " + shown(fallback) + " is used");
        return fallback;
    }
    return value->get<float>();
}

bool GameProperties::flag(const char* key, bool fallback) {
    const nlohmann::json* value = find(key);
    if (value == nullptr) {
        return fallback;
    }
    if (!value->is_boolean()) {
        warn(std::string("\"") + key + "\" isn't true or false; " + (fallback ? "true" : "false") + " is used");
        return fallback;
    }
    return value->get<bool>();
}

glm::vec3 GameProperties::vec3(const char* key, const glm::vec3& fallback) {
    const nlohmann::json* value = find(key);
    if (value == nullptr) {
        return fallback;
    }
    if (!value->is_array() || value->size() != 3 || !(*value)[0].is_number() || !(*value)[1].is_number() ||
        !(*value)[2].is_number()) {
        warn(std::string("\"") + key + "\" isn't three numbers [x, y, z]; [" + shown(fallback.x) + ", " +
             shown(fallback.y) + ", " + shown(fallback.z) + "] is used");
        return fallback;
    }
    return {(*value)[0].get<float>(), (*value)[1].get<float>(), (*value)[2].get<float>()};
}

void GameProperties::finish() {
    for (const auto& [key, value] : json.items()) {
        if (!asked.count(key)) {
            warn("\"" + key + "\" isn't one of its properties; ignored");
        }
    }
}
