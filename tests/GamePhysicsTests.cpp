//
// The player and colliders (game/behaviours/Player.cpp, Collider.cpp): the player walking, turning, jumping and
// falling among boxes the scene marks as solid, through the game as the program runs it. The physics on its own:
// PhysicsTests.cpp.
//

#include <catch_amalgamated.hpp>
#include "TestHelpers.h"
#include "game/Game.h"
#include "game/GameLoop.h"
#include "game/StepInterpolation.h"
#include "scene/Scene.h"

namespace {  // (each test file has its own TestScene)

struct TestScene : TestAssets {
    Scene scene{assets, settings};
    Game game;
    GameLoop loop;
    StepInterpolation interpolation;
    TestScene() {
        scene.createDefault();
        interpolation.enabled = false;  // (the stepped values, to check them exactly)
    }

    std::vector<std::string> play() {
        loop.reset();
        interpolation.clear();
        return game.start(scene);
    }
    // Steps at 60 fps: one per frame; the first frame gets the input as given, the rest with its keys held
    void run(int steps, const InputState& input = InputState()) {
        for (int i = 0; i < steps; i++) {
            InputState frame = input;
            if (i > 0) {
                frame.pressed.reset();
                frame.released.reset();
                frame.mouseDelta = glm::vec2(0.0f);
            }
            interpolation.restore(scene);
            int ran = loop.advance(GameLoop::stepSeconds, frame, [&](const InputState& stepInput, uint64_t index) {
                interpolation.beforeStep(scene);
                GameStep step{scene, settings, stepInput, GameLoop::stepSeconds,
                              static_cast<double>(index + 1) * GameLoop::stepSeconds, index, &interpolation};
                game.update(step);
            });
            interpolation.blend(scene, ran, static_cast<float>(loop.owed() / GameLoop::stepSeconds));
        }
    }
    void pause() {
        interpolation.restore(scene);
        interpolation.clear();
        game.pause(scene);
    }
    void resume() {
        game.resume(scene);
        loop.dropInput();
        interpolation.clear();
    }
    EntityId entityWith(const std::string& name, std::vector<GameComponent> components, const glm::vec3& position,
                        EntityId parent = 0) {
        EntityId id = scene.addEmpty(name, parent);
        scene.find(id)->local.position = position;
        scene.setGame(id, std::move(components));
        return id;
    }
    // A floor whose top is at y = 0
    EntityId floor() { return entityWith("Floor", {{"collider", R"({"size": [40, 1, 40]})"}}, {0.0f, -0.5f, 0.0f}); }
    EntityId player(const glm::vec3& feet, const std::string& properties = "") {
        return entityWith("Player", {{"player", properties}}, feet);
    }
    glm::vec3 eyes() const { return scene.camera().position; }
};

InputState keys(std::initializer_list<int> held, std::initializer_list<int> pressed = {}) {
    InputState input;
    for (int key : held) {
        input.down[key] = true;
    }
    for (int key : pressed) {
        input.down[key] = true;
        input.pressed[key] = true;
    }
    return input;
}

}  // namespace

TEST_CASE("Player: falls onto a floor collider and stands on it, the camera at its eyes", "[Game][Player]") {
    TestScene t;
    t.floor();
    EntityId player = t.player({0.0f, 2.0f, 0.0f});
    CHECK(t.play().empty());
    CHECK(t.game.controlsCamera());
    CHECK(t.game.capturesMouse());
    t.run(120);
    CHECK(t.eyes().y == Catch::Approx(1.65f).margin(2e-3));
    CHECK(t.scene.find(player)->local.position.y == Catch::Approx(0.0f).margin(2e-3));  // the entity at its feet
}

TEST_CASE("Player: walks where it looks, and a wall stops it", "[Game][Player]") {
    TestScene t;
    t.floor();
    t.entityWith("Wall", {{"collider", R"({"size": [0.5, 3, 10]})"}}, {3.0f, 1.5f, 0.0f});  // its face at x = 2.75
    t.player({0.0f, 0.0f, 0.0f});
    t.scene.camera().yaw = 90.0f;  // looking along +x
    t.scene.camera().pitch = 0.0f;
    t.play();
    t.run(10);
    t.run(30, keys({Key::W}));  // 0.5 s at 4 m/s
    CHECK(t.eyes().x == Catch::Approx(2.0f).margin(0.05f));
    t.run(60, keys({Key::W, Key::LeftShift}));
    CHECK(t.eyes().x == Catch::Approx(2.75f - 0.3f).margin(2e-3));
    CHECK(std::abs(t.eyes().z) < 1e-3f);
}

TEST_CASE("Player: the mouse turns the view, and W walks the new way", "[Game][Player]") {
    TestScene t;
    t.floor();
    t.player({0.0f, 0.0f, 0.0f});
    t.scene.camera().yaw = 0.0f;  // looking along -z
    t.scene.camera().pitch = 0.0f;
    t.play();
    t.run(10);
    InputState turn;
    turn.mouseDelta = glm::vec2(900.0f, -100.0f);  // 0.1 degrees per pixel: 90 right, 10 up
    t.run(1, turn);
    CHECK(t.scene.camera().yaw == Catch::Approx(90.0f));
    CHECK(t.scene.camera().pitch == Catch::Approx(10.0f));
    t.run(60, keys({Key::W}));
    CHECK(t.eyes().x == Catch::Approx(4.0f).margin(0.1f));
    CHECK(std::abs(t.eyes().z) < 1e-3f);
}

TEST_CASE("Player: Space jumps, and it lands again", "[Game][Player]") {
    TestScene t;
    t.floor();
    t.player({0.0f, 0.0f, 0.0f});
    t.play();
    t.run(10);
    float highest = 0.0f;
    t.run(1, keys({}, {Key::Space}));
    for (int i = 0; i < 90; i++) {
        highest = std::max(highest, t.eyes().y);
        t.run(1);
    }
    CHECK(highest - 1.65f == Catch::Approx(25.0f / (2.0f * 9.81f)).epsilon(0.05));
    CHECK(t.eyes().y == Catch::Approx(1.65f).margin(2e-3));
}

TEST_CASE("Player: a closed door blocks it; opened with E, it walks through", "[Game][Player]") {
    TestScene t;
    t.floor();
    // The hinge at (2, 0, -0.5); the panel's box across the way at x = 2, from z = -0.5 to 0.5. Open: turned 90
    // degrees around the hinge, out of the way.
    EntityId door = t.entityWith("Door", {{"door", R"({"radius": 2})"}}, {2.0f, 0.0f, -0.5f});
    t.entityWith("Panel", {{"collider", R"({"size": [0.1, 2, 1]})"}}, {0.0f, 1.0f, 0.5f}, door);
    t.player({0.0f, 0.0f, 0.0f});
    t.scene.camera().yaw = 90.0f;
    t.scene.camera().pitch = 0.0f;
    t.play();
    t.run(90, keys({Key::W}));
    CHECK(t.eyes().x == Catch::Approx(1.95f - 0.3f).margin(2e-3));  // stopped at the panel
    t.run(120, keys({Key::W}, {Key::E}));
    CHECK(t.eyes().x > 3.5f);
}

TEST_CASE("Player: a hidden entity's collider doesn't block; shown again, it does", "[Game][Player]") {
    TestScene t;
    t.floor();
    EntityId wall = t.entityWith("Wall", {{"collider", R"({"size": [0.5, 3, 10]})"}}, {3.0f, 1.5f, 0.0f});
    t.scene.find(wall)->visible = false;
    t.player({0.0f, 0.0f, 0.0f});
    t.scene.camera().yaw = 90.0f;
    t.play();
    t.run(90, keys({Key::W}));
    CHECK(t.eyes().x > 4.0f);
    // Shown while playing: solid from then on
    TestScene u;
    u.floor();
    EntityId shown = u.entityWith("Wall", {{"collider", R"({"size": [0.5, 3, 10]})"}}, {3.0f, 1.5f, 0.0f});
    u.scene.find(shown)->visible = false;
    u.player({0.0f, 0.0f, 0.0f});
    u.scene.camera().yaw = 90.0f;
    u.play();
    u.run(5);
    u.scene.find(shown)->visible = true;
    u.run(90, keys({Key::W}));
    CHECK(u.eyes().x == Catch::Approx(2.75f - 0.3f).margin(2e-3));
}

TEST_CASE("Player: a collider without a size takes its model's box", "[Game][Player]") {
    TestScene t;
    t.floor();
    EntityId block = t.scene.addObject("Cube", false);
    t.scene.find(block)->local.position = {3.0f, 0.5f, 0.0f};
    t.scene.find(block)->local.scale = {1.0f, 3.0f, 10.0f};
    t.scene.setGame(block, {{"collider", ""}});
    glm::vec3 min, max;
    REQUIRE(t.scene.modelBox(block, min, max));
    float face = 3.0f + min.x;  // (scale 1 along x)
    t.player({0.0f, 0.0f, 0.0f});
    t.scene.camera().yaw = 90.0f;
    t.play();
    t.run(90, keys({Key::W}));
    CHECK(t.eyes().x == Catch::Approx(face - 0.3f).margin(2e-3));
}

TEST_CASE("Player: falling below respawnBelow puts it back at the start", "[Game][Player]") {
    TestScene t;
    t.player({0.0f, 1.0f, 0.0f}, R"({"respawnBelow": -1})");  // no floor
    t.play();
    float lowest = 1e9f;
    bool respawned = false;
    float last = t.eyes().y;
    for (int i = 0; i < 120; i++) {
        t.run(1);
        lowest = std::min(lowest, t.eyes().y);
        respawned = respawned || t.eyes().y > last + 0.5f;
        last = t.eyes().y;
    }
    CHECK(respawned);
    CHECK(lowest >= -1.0f + 1.65f - 0.2f);  // never more than a step below the line
}

TEST_CASE("Player: put somewhere else while paused, it goes on from there; a removed collider goes", "[Game][Player]") {
    TestScene t;
    t.floor();
    EntityId wall = t.entityWith("Wall", {{"collider", R"({"size": [0.5, 3, 10]})"}}, {3.0f, 1.5f, 0.0f});
    EntityId player = t.player({0.0f, 0.0f, 0.0f});
    t.play();
    t.run(30);
    CHECK(t.game.physicsWorld().bodyCount() == 2);
    t.pause();
    t.scene.find(player)->local.position = {0.0f, 0.0f, 8.0f};
    t.scene.remove(wall);
    t.resume();
    CHECK(t.game.physicsWorld().bodyCount() == 1);
    t.run(10);
    CHECK(t.eyes().z == Catch::Approx(8.0f).margin(2e-3));
    CHECK(t.eyes().y == Catch::Approx(1.65f).margin(2e-3));
}

TEST_CASE("Player: a moved collider (a bobbing platform) carries on solid", "[Game][Player]") {
    TestScene t;
    t.floor();
    t.entityWith("Platform", {{"collider", R"({"size": [4, 0.2, 4]})"}, {"bob", R"({"height": 0.5, "period": 4})"}},
                 {0.0f, 1.0f, 0.0f});
    t.player({0.0f, 2.0f, 0.0f});
    t.play();
    t.run(60);  // landed on it; it rose by up to 0.5 meanwhile
    float onTop = t.eyes().y - 1.65f;
    CHECK(onTop > 1.0f);  // on the platform, not on the floor
    t.run(120);  // half a period on: down 1 m from its top
    CHECK(t.eyes().y - 1.65f > 0.0f + 0.5f);
}

TEST_CASE("Player: only one thing drives the camera; the mouse is captured unless a camera path drives it",
          "[Game][Player]") {
    TestScene t;
    EntityId path = t.scene.addEmpty("Path", 0);
    for (float x : {0.0f, 10.0f}) {
        EntityId point = t.scene.addEmpty("Point", path);
        t.scene.find(point)->local.position = {x, 1.0f, 0.0f};
    }
    t.scene.setGame(path, {{"cameraPath", ""}});
    t.player({0.0f, 0.0f, 0.0f});
    std::vector<std::string> warnings = t.play();
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].find("player: the camera is already driven") != std::string::npos);
    CHECK_FALSE(t.game.capturesMouse());  // the camera path's

    TestScene u;
    CHECK(u.play().empty());
    CHECK(u.game.capturesMouse());  // nothing drives it: the game-style fly camera
}
