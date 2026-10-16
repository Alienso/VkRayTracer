//
// Game components (Scene.h, GAME_PLAN.md): the scene keeps them with their entities, copies and removes them, and
// saves them as they are; what it can't read is a warning, not a refused file.
//

#include <catch_amalgamated.hpp>
#include <fstream>
#include "TestHelpers.h"
#include "scene/Scene.h"

namespace {  // (each test file has its own TestScene)

struct TestScene : TestAssets {
    Scene scene{assets, settings};
    TestScene() { scene.createDefault(); }

    void write(const std::string& name, const std::string& text) const {
        std::ofstream file(folder.file(name), std::ios::binary);
        file << text;
    }
};

}  // namespace

static const char* header = R"({"format": "vulkanDemo scene", "version": 1, )";

static std::vector<GameComponent> components(const Scene& scene, EntityId id) {
    const std::vector<GameComponent>* list = scene.game(id);
    return list != nullptr ? *list : std::vector<GameComponent>();
}

TEST_CASE("Game components: set, replaced, removed by an empty list", "[GameComponent]") {
    TestScene t;
    EntityId id = t.scene.addEmpty("Door", 0);
    CHECK(t.scene.game(id) == nullptr);
    t.scene.setGame(id, {{"door", R"({"speed":2})"}, {"spin", ""}});
    REQUIRE(t.scene.game(id) != nullptr);
    CHECK(t.scene.game(id)->size() == 2);
    CHECK((*t.scene.game(id))[1].type == "spin");
    t.scene.setGame(id, {{"bob", ""}});
    CHECK(components(t.scene, id) == std::vector<GameComponent>{{"bob", ""}});
    t.scene.setGame(id, {});
    CHECK(t.scene.game(id) == nullptr);
}

TEST_CASE("Game components: saved and read back in their order, properties as written", "[GameComponent][SceneFile]") {
    TestScene t;
    EntityId door = t.scene.addEmpty("Door", 0);
    EntityId empty = t.scene.addEmpty("Nothing", 0);
    // Keys in the order typed, not sorted; a fraction that must read back exactly
    std::vector<GameComponent> list = {{"door", R"({"speed":2,"openAngle":95.5,"axis":[0,1,0]})"},
                                       {"", ""},
                                       {"spin", R"({"speed":0.3})"}};
    t.scene.setGame(door, list);
    std::string path = t.folder.file("game.json");
    t.scene.save(path);

    Scene loaded(t.assets, t.settings);
    std::vector<std::string> warnings = loaded.load(path);
    CHECK(warnings.empty());
    CHECK(components(loaded, door) == list);
    CHECK(loaded.game(empty) == nullptr);
    CHECK(loaded.contentText() == t.scene.contentText());

    std::string again = t.folder.file("again.json");
    loaded.save(again);
    std::ifstream a(path, std::ios::binary), b(again, std::ios::binary);
    CHECK(std::string(std::istreambuf_iterator<char>(a), {}) == std::string(std::istreambuf_iterator<char>(b), {}));
}

TEST_CASE("Game components: an entity without any has no \"game\" in the file", "[GameComponent][SceneFile]") {
    TestScene t;
    t.scene.addEmpty("Nothing", 0);
    std::string path = t.folder.file("none.json");
    t.scene.save(path);
    std::ifstream file(path, std::ios::binary);
    CHECK(std::string(std::istreambuf_iterator<char>(file), {}).find("\"game\"") == std::string::npos);
}

TEST_CASE("Game components: written by hand, made compact; {} is no properties", "[GameComponent][SceneFile]") {
    TestScene t;
    t.write("hand.json", std::string(header) + R"("entities": [{"id": 1, "game": [
        {"type": "door", "properties": { "speed" : 2,  "locked": true }},
        {"type": "spin", "properties": {}},
        {"properties": {"a": 1}}]}]})");
    std::vector<std::string> warnings = t.scene.load(t.folder.file("hand.json"));
    CHECK(warnings.empty());
    CHECK(components(t.scene, 1) == std::vector<GameComponent>{{"door", R"({"speed":2,"locked":true})"},
                                                               {"spin", ""},
                                                               {"", R"({"a":1})"}});
}

TEST_CASE("Game components: what can't be read is a warning, the rest is kept", "[GameComponent][SceneFile]") {
    TestScene t;
    t.write("bad.json", std::string(header) + R"("entities": [
        {"id": 1, "game": {"type": "door"}},
        {"id": 2, "game": [5, {"type": "spin", "properties": [1, 2]}, {"type": 7, "properties": {"a": 1}}]},
        {"id": 3, "game": []}]})");
    std::vector<std::string> warnings = t.scene.load(t.folder.file("bad.json"));
    CHECK(warnings.size() == 4);  // not a list; not an object; properties not an object; type not text
    CHECK(t.scene.find(1) != nullptr);
    CHECK(t.scene.game(1) == nullptr);
    CHECK(components(t.scene, 2) == std::vector<GameComponent>{{"spin", ""}, {"", R"({"a":1})"}});
    CHECK(t.scene.game(3) == nullptr);  // an empty list is none
}

TEST_CASE("Game components: duplicated with their entities, the whole subtree", "[GameComponent]") {
    TestScene t;
    EntityId group = t.scene.addEmpty("Group", 0);
    EntityId child = t.scene.addEmpty("Child", group);
    t.scene.setGame(group, {{"bob", R"({"height":0.2})"}});
    t.scene.setGame(child, {{"spin", ""}, {"pickup", R"({"target":12})"}});
    EntityId copy = t.scene.duplicate(group);
    CHECK(components(t.scene, copy) == components(t.scene, group));
    EntityId childCopy = t.scene.find(copy)->children.at(0);
    CHECK(components(t.scene, childCopy) == components(t.scene, child));  // (the id in properties isn't remapped)

    // A copy of its own: changing one leaves the other
    t.scene.setGame(childCopy, {{"spin", R"({"speed":1})"}});
    CHECK(components(t.scene, child).size() == 2);
}

TEST_CASE("Game components: removed with their entities", "[GameComponent]") {
    TestScene t;
    EntityId group = t.scene.addEmpty("Group", 0);
    EntityId child = t.scene.addEmpty("Child", group);
    EntityId other = t.scene.addEmpty("Other", 0);
    t.scene.setGame(child, {{"spin", ""}});
    t.scene.setGame(other, {{"bob", ""}});
    t.scene.remove(group);
    CHECK(t.scene.game(child) == nullptr);
    CHECK(t.scene.game(other) != nullptr);

    t.scene.clear();
    CHECK(t.scene.game(other) == nullptr);
}

TEST_CASE("Game components: a change is a change to save (the unsaved mark)", "[GameComponent][SceneFile]") {
    TestScene t;
    EntityId id = t.scene.addEmpty("Door", 0);
    std::string before = t.scene.contentText();
    t.scene.setGame(id, {{"", ""}});
    std::string added = t.scene.contentText();
    CHECK(added != before);
    t.scene.setGame(id, {{"door", ""}});
    std::string typed = t.scene.contentText();
    CHECK(typed != added);
    t.scene.setGame(id, {{"door", R"({"speed":2})"}});
    CHECK(t.scene.contentText() != typed);
    t.scene.setGame(id, {});
    CHECK(t.scene.contentText() == before);
}

TEST_CASE("Game components: opening another scene drops the old one's", "[GameComponent][SceneFile]") {
    TestScene t;
    EntityId id = t.scene.addEmpty("Door", 0);
    t.scene.setGame(id, {{"door", ""}});
    t.write("other.json", std::string(header) + R"("entities": [{"id": 1}]})");
    t.scene.load(t.folder.file("other.json"));
    CHECK(t.scene.game(1) == nullptr);
    CHECK(t.scene.game(id) == nullptr);
}
