//
// Reading a game component's properties (GameComponent::properties, a JSON object as text) into a behaviour's typed
// fields, once at Play. Nothing in them is an error: a value of the wrong kind is a warning and the default is used,
// and keys no read asked for are warned about by finish() (most likely typos).
//

#ifndef VULKANDEMO_GAMEPROPERTIES_H
#define VULKANDEMO_GAMEPROPERTIES_H

#include <set>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

class GameProperties {
public:
    // where: who they belong to, for the warnings ("entity 5 (Door): spin")
    GameProperties(const std::string& text, std::string where, std::vector<std::string>& warnings);

    float number(const char* key, float fallback);
    bool flag(const char* key, bool fallback);
    glm::vec3 vec3(const char* key, const glm::vec3& fallback);
    // An entity's id (Scene's EntityId), 0 when not given; a value that isn't an id is a warning (and 0)
    uint64_t entity(const char* key);
    // Warns about the keys none of the above asked for
    void finish();

    void warn(const std::string& text) { warnings.push_back(where + ": " + text); }

private:
    nlohmann::json json = nlohmann::json::object();
    std::string where;
    std::vector<std::string>& warnings;
    std::set<std::string> asked;

    const nlohmann::json* find(const char* key);
};

#endif //VULKANDEMO_GAMEPROPERTIES_H
