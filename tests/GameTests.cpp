//
// The game (game/): the fixed-step clock and its input, the behaviours spin and bob, the warnings for components it
// can't use, and the types' defaults.
//

#include <catch_amalgamated.hpp>
#include <set>
#include <glm/gtc/constants.hpp>
#include "TestHelpers.h"
#include "game/Behaviours.h"
#include "game/Game.h"
#include "game/GameLoop.h"
#include "game/GameProperties.h"
#include "scene/Scene.h"

namespace {  // (each test file has its own TestScene)

struct TestScene : TestAssets {
    Scene scene{assets, settings};
    Game game;
    GameLoop loop;
    TestScene() { scene.createDefault(); }

    std::vector<std::string> play() {
        loop.reset();
        return game.start(scene);
    }
    // Frames of the given length, the game stepping as the program does
    void run(int frames, double frameSeconds, const InputState& input = InputState()) {
        for (int f = 0; f < frames; f++) {
            loop.advance(frameSeconds, input, [&](const InputState& stepInput, uint64_t index) {
                GameStep step{scene, settings, stepInput, GameLoop::stepSeconds,
                              static_cast<double>(index + 1) * GameLoop::stepSeconds, index};
                game.update(step);
            });
        }
    }
};

bool nearlyEqual(const glm::quat& a, const glm::quat& b, float tolerance = 1e-5f) {
    return std::abs(std::abs(glm::dot(a, b)) - 1.0f) < tolerance;  // (q and -q are the same rotation)
}

bool contains(const std::vector<std::string>& warnings, const std::string& text) {
    for (const std::string& warning : warnings) {
        if (warning.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// The clock

TEST_CASE("Game loop: one step per frame at 60 fps, none owed after", "[Game][GameLoop]") {
    GameLoop loop;
    loop.reset();
    int steps = 0;
    for (int f = 0; f < 600; f++) {
        steps += loop.advance(GameLoop::stepSeconds, InputState(), [](const InputState&, uint64_t) {});
    }
    CHECK(steps == 600);
    CHECK(loop.steps() == 600);
    CHECK(loop.owed() < 1e-9);
}

TEST_CASE("Game loop: the steps follow real time at any frame rate", "[Game][GameLoop]") {
    for (double fps : {30.0, 45.0, 59.0, 90.0, 144.0, 1000.0}) {
        GameLoop loop;
        loop.reset();
        int frames = static_cast<int>(fps * 10.0);  // 10 s
        int most = 0;
        for (int f = 0; f < frames; f++) {
            most = std::max(most, loop.advance(1.0 / fps, InputState(), [](const InputState&, uint64_t) {}));
        }
        INFO("fps " << fps);
        CHECK(std::abs(static_cast<double>(loop.steps()) - 600.0) <= 1.0);  // 10 s of 60 Hz steps
        CHECK(loop.time() <= 10.0 + 1e-9);
        CHECK(most <= (fps < 60.0 ? 2 : 1));
        CHECK(loop.dropped() == 0.0);
    }
}

TEST_CASE("Game loop: a long frame runs at most 5 steps, the rest is dropped", "[Game][GameLoop]") {
    GameLoop loop;
    loop.reset();
    CHECK(loop.advance(0.5, InputState(), [](const InputState&, uint64_t) {}) == GameLoop::maxStepsPerFrame);
    CHECK(loop.owed() < GameLoop::stepSeconds);
    CHECK(loop.dropped() == Catch::Approx(0.5 - 5 * GameLoop::stepSeconds - loop.owed()));
    // The next normal frame isn't owed the dropped time
    CHECK(loop.advance(GameLoop::stepSeconds, InputState(), [](const InputState&, uint64_t) {}) == 1);
    // A negative frame time (a clock that went back) counts as none
    CHECK(loop.advance(-1.0, InputState(), [](const InputState&, uint64_t) {}) == 0);
}

TEST_CASE("Game loop: a press in a frame without a step reaches the next step, once", "[Game][GameLoop]") {
    GameLoop loop;
    loop.reset();
    InputState press;
    press.down[Key::E] = true;
    press.pressed[Key::E] = true;
    press.mouseDelta = {3.0f, -1.0f};
    std::vector<bool> seen;
    std::vector<bool> held;
    float moved = 0.0f;
    auto step = [&](const InputState& input, uint64_t) {
        seen.push_back(input.wasPressed(Key::E));
        held.push_back(input.isDown(Key::E));
        moved += input.mouseDelta.x;
    };
    // At 1000 fps: the press comes in a frame that runs no step
    CHECK(loop.advance(0.001, press, step) == 0);
    InputState hold;
    hold.down[Key::E] = true;
    hold.mouseDelta = {2.0f, 0.0f};
    for (int f = 0; f < 40; f++) {
        loop.advance(0.001, hold, step);
    }
    // Then a long frame: several steps, the press only in the first
    loop.advance(3.0 * GameLoop::stepSeconds, hold, step);
    REQUIRE(seen.size() >= 3);
    CHECK(seen[0]);
    for (size_t i = 1; i < seen.size(); i++) {
        CHECK_FALSE(seen[i]);
    }
    for (bool down : held) {
        CHECK(down);
    }
    CHECK(moved == 3.0f + 41 * 2.0f);  // all the movement, each frame's once
}

// ---------------------------------------------------------------------------------------------
// Behaviours

TEST_CASE("Game: spin turns around its axis at its speed, from the rotation at Play", "[Game]") {
    TestScene t;
    EntityId id = t.scene.addEmpty("Spinner", 0);
    glm::quat start = eulerToQuat({20.0f, 0.0f, 0.0f});
    t.scene.find(id)->local.rotation = start;
    t.scene.setGame(id, {{"spin", R"({"speed": 90, "axis": [0, 0, 2]})"}});
    CHECK(t.play().empty());
    CHECK(t.game.behaviours() == 1);
    t.run(60, GameLoop::stepSeconds);  // 1 s
    glm::quat expected = start * glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    CHECK(nearlyEqual(t.scene.find(id)->local.rotation, expected));
    t.run(60 * 3, GameLoop::stepSeconds);  // 4 s: all the way round
    CHECK(nearlyEqual(t.scene.find(id)->local.rotation, start));
}

TEST_CASE("Game: the same game time gives the same state at any frame rate", "[Game]") {
    auto rotationAfter = [](double fps) {
        TestScene t;
        EntityId id = t.scene.addEmpty("Spinner", 0);
        t.scene.setGame(id, {{"spin", R"({"speed": 37.5})"}, {"bob", R"({"height": 0.5, "period": 1.5})"}});
        t.play();
        while (t.loop.steps() < 300) {
            t.run(1, 1.0 / fps);
        }
        // (A fast frame rate can't overshoot; a slow one may run a step past 300)
        return std::make_pair(t.loop.steps(), t.scene.find(id)->local);
    };
    auto [steps60, at60] = rotationAfter(60.0);
    auto [steps144, at144] = rotationAfter(144.0);
    REQUIRE(steps60 == 300);
    REQUIRE(steps144 == 300);
    CHECK(at60.rotation == at144.rotation);  // exactly: computed from the step's time
    CHECK(at60.position == at144.position);
}

TEST_CASE("Game: bob moves up and down from the position at Play", "[Game]") {
    TestScene t;
    EntityId id = t.scene.addEmpty("Bobber", 0);
    t.scene.find(id)->local.position = {1.0f, 2.0f, 3.0f};
    t.scene.setGame(id, {{"bob", R"({"height": 0.5, "period": 2})"}});
    CHECK(t.play().empty());
    t.run(30, GameLoop::stepSeconds);  // a quarter of the period: at the top
    CHECK(nearlyEqual(t.scene.find(id)->local.position, glm::vec3(1.0f, 2.5f, 3.0f)));
    t.run(60, GameLoop::stepSeconds);  // three quarters: at the bottom
    CHECK(nearlyEqual(t.scene.find(id)->local.position, glm::vec3(1.0f, 1.5f, 3.0f)));
}

TEST_CASE("Game: spin and bob on one entity both apply", "[Game]") {
    TestScene t;
    EntityId id = t.scene.addEmpty("Pickup", 0);
    t.scene.setGame(id, {{"spin", ""}, {"bob", ""}});  // the defaults: 90 degrees/s around Y; 0.25 every 2 s
    CHECK(t.play().empty());
    t.run(30, GameLoop::stepSeconds);
    CHECK(nearlyEqual(t.scene.find(id)->local.rotation, glm::angleAxis(glm::radians(45.0f), glm::vec3(0, 1, 0))));
    CHECK(nearlyEqual(t.scene.find(id)->local.position, glm::vec3(0.0f, 0.25f, 0.0f)));
}

TEST_CASE("Game: what it can't use is a warning, the rest runs", "[Game]") {
    TestScene t;
    EntityId a = t.scene.addEmpty("A", 0);
    EntityId b = t.scene.addEmpty("B", 0);
    t.scene.setGame(a, {{"", ""},                                   // no type
                        {"teleporter", R"({"x": 1})"},              // unknown type
                        {"spin", R"({"speed": "fast", "axis": [1, 2], "sped": 3})"},
                        {"spin", ""}});                             // the same type again
    t.scene.setGame(b, {{"bob", R"({"period": 0, "height": true})"}, {"spin", R"({"axis": [0, 0, 0]})"}});
    std::vector<std::string> warnings = t.play();
    CHECK(contains(warnings, "without a type"));
    CHECK(contains(warnings, "unknown game type \"teleporter\""));
    CHECK(contains(warnings, "\"speed\" isn't a number; 90 is used"));
    CHECK(contains(warnings, "\"axis\" isn't three numbers"));
    CHECK(contains(warnings, "\"sped\" isn't one of its properties"));
    CHECK(contains(warnings, "twice on one entity"));
    CHECK(contains(warnings, "\"period\" must be more than 0"));
    CHECK(contains(warnings, "\"height\" isn't a number"));
    CHECK(contains(warnings, "\"axis\" is [0, 0, 0]"));
    CHECK(warnings.size() == 9);
    CHECK(contains(warnings, "entity " + std::to_string(a) + " (A): spin"));  // says whose
    CHECK(t.game.behaviours() == 3);  // A's first spin, B's bob and spin, with defaults
    t.run(60, GameLoop::stepSeconds);
    CHECK(nearlyEqual(t.scene.find(a)->local.rotation, glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0))));
}

TEST_CASE("Game: an entity removed while playing is skipped", "[Game]") {
    TestScene t;
    EntityId gone = t.scene.addEmpty("Gone", 0);
    EntityId kept = t.scene.addEmpty("Kept", 0);
    t.scene.setGame(gone, {{"spin", ""}});
    t.scene.setGame(kept, {{"bob", ""}});
    t.play();
    t.run(10, GameLoop::stepSeconds);
    t.scene.remove(gone);
    t.run(20, GameLoop::stepSeconds);
    CHECK(nearlyEqual(t.scene.find(kept)->local.position, glm::vec3(0.0f, 0.25f, 0.0f)));
}

TEST_CASE("Game: stop drops the behaviours; Play again reads the scene anew", "[Game]") {
    TestScene t;
    EntityId id = t.scene.addEmpty("Spinner", 0);
    t.scene.setGame(id, {{"spin", ""}});
    t.play();
    CHECK(t.game.running());
    t.game.stop();
    CHECK_FALSE(t.game.running());
    CHECK(t.game.behaviours() == 0);
    t.scene.setGame(id, {{"spin", ""}, {"bob", ""}});
    t.play();
    CHECK(t.game.behaviours() == 2);
}

TEST_CASE("Game: each type's defaults are the properties it reads, and read without warnings", "[Game]") {
    TestScene t;
    EntityId id = t.scene.addEmpty("A", 0);
    t.scene.addEmpty("Point 1", id);  // (a camera path's)
    t.scene.addEmpty("Point 2", id);
    std::vector<std::unique_ptr<BehaviourListBase>> lists = makeBehaviourLists();
    std::vector<GameComponentType> types = Game::types();
    REQUIRE(types.size() == lists.size());
    std::set<std::string> names;
    for (size_t i = 0; i < lists.size(); i++) {
        BehaviourListBase& list = *lists[i];
        INFO(list.type());
        CHECK(types[i].type == list.type());
        CHECK_FALSE(types[i].help.empty());
        CHECK(names.insert(list.type()).second);
        nlohmann::json defaults = nlohmann::json::parse(list.defaults());
        REQUIRE(defaults.is_object());
        std::set<std::string> keys;
        for (const auto& [key, value] : defaults.items()) {
            keys.insert(key);
        }
        std::vector<std::string> warnings;
        GameProperties properties(list.defaults(), list.type(), warnings);
        std::unique_ptr<physics::PhysicsWorld> physicsWorld = physics::makeSimplePhysicsWorld();
        GameWorld world;
        world.physics = physicsWorld.get();
        list.read(ReadContext{t.scene, *t.scene.find(id), false, 0.0, world}, properties);
        properties.finish();
        CHECK(properties.askedKeys() == keys);
        for (const std::string& warning : warnings) {  // only an id it needs (0 in the defaults: not given)
            CHECK(warning.find("needs \"") != std::string::npos);
        }
    }
}
