//
// The game's interactive behaviours (game/Game.h): door, pickup, trigger, spawner and the
// camera path, with a made-up InputState and the camera standing in for the player.
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
        interpolation.enabled = false;  // (the stepped values, to check them exactly; StepInterpolationTests blends)
    }

    std::vector<std::string> play() {
        loop.reset();
        interpolation.clear();
        return game.start(scene);
    }
    // Steps at 60 fps: one per frame
    void run(int steps, const InputState& input = InputState()) {
        for (int i = 0; i < steps; i++) {
            interpolation.restore(scene);
            int ran = loop.advance(GameLoop::stepSeconds, i == 0 ? input : held(input),
                                   [&](const InputState& stepInput, uint64_t index) {
                interpolation.beforeStep(scene);
                GameStep step{scene, settings, stepInput, GameLoop::stepSeconds,
                              static_cast<double>(index + 1) * GameLoop::stepSeconds, index, &interpolation};
                game.update(step);
            });
            interpolation.blend(scene, ran, static_cast<float>(loop.owed() / GameLoop::stepSeconds));
        }
    }
    // The same keys held, not pressed again
    static InputState held(const InputState& input) {
        InputState next = input;
        next.pressed.reset();
        next.released.reset();
        return next;
    }
    static InputState press(int key) {
        InputState input;
        input.down[key] = true;
        input.pressed[key] = true;
        return input;
    }
    EntityId entityAt(const std::string& name, const glm::vec3& position, EntityId parent = 0) {
        EntityId id = scene.addEmpty(name, parent);
        scene.find(id)->local.position = position;
        return id;
    }
};

bool nearlyEqual(const glm::quat& a, const glm::quat& b, float tolerance = 1e-5f) {
    return std::abs(std::abs(glm::dot(a, b)) - 1.0f) < tolerance;
}

bool contains(const std::vector<std::string>& warnings, const std::string& text) {
    for (const std::string& warning : warnings) {
        if (warning.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::string id(EntityId entity) {
    return std::to_string(entity);
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Door

TEST_CASE("Door: E near it opens it over its seconds, E again closes it", "[Game][Interaction]") {
    TestScene t;
    EntityId door = t.entityAt("Door", {0, 0, 0});
    t.scene.setGame(door, {{"door", R"({"openAngle": 90, "seconds": 0.5, "radius": 2})"}});
    t.scene.camera().position = {0, 0, 1.5f};
    CHECK(t.play().empty());
    glm::quat open = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0));

    t.run(30);  // no E: stays shut
    CHECK(nearlyEqual(t.scene.find(door)->local.rotation, glm::quat(1, 0, 0, 0)));
    t.run(15, TestScene::press(Key::E));  // a quarter of a second: half way
    CHECK(nearlyEqual(t.scene.find(door)->local.rotation, glm::angleAxis(glm::radians(45.0f), glm::vec3(0, 1, 0)),
                      1e-4f));
    t.run(30);  // and the rest, then it stays open
    CHECK(nearlyEqual(t.scene.find(door)->local.rotation, open));
    t.run(30, TestScene::press(Key::E));
    CHECK(nearlyEqual(t.scene.find(door)->local.rotation, glm::quat(1, 0, 0, 0)));
}

TEST_CASE("Door: too far away, or locked, E does nothing", "[Game][Interaction]") {
    TestScene t;
    EntityId far = t.entityAt("Far", {10, 0, 0});
    EntityId locked = t.entityAt("Locked", {0, 0, 0});
    t.scene.setGame(far, {{"door", ""}});
    t.scene.setGame(locked, {{"door", R"({"locked": true})"}});
    t.scene.camera().position = {0, 0, 1};
    CHECK(t.play().empty());
    t.run(90, TestScene::press(Key::E));
    CHECK(nearlyEqual(t.scene.find(far)->local.rotation, glm::quat(1, 0, 0, 0)));
    CHECK(nearlyEqual(t.scene.find(locked)->local.rotation, glm::quat(1, 0, 0, 0)));
}

TEST_CASE("Door: the distance is to where it is in the world, under a parent", "[Game][Interaction]") {
    TestScene t;
    EntityId house = t.entityAt("House", {20, 0, 0});
    EntityId door = t.entityAt("Door", {0, 0, 0}, house);
    t.scene.setGame(door, {{"door", R"({"seconds": 0})"}});
    t.scene.camera().position = {0, 0, 0};  // near the door's local position, far from where it is
    t.play();
    t.run(2, TestScene::press(Key::E));
    CHECK(nearlyEqual(t.scene.find(door)->local.rotation, glm::quat(1, 0, 0, 0)));
    t.scene.camera().position = {20, 0, 1};
    t.run(2, TestScene::press(Key::E));
    CHECK(nearlyEqual(t.scene.find(door)->local.rotation, glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0))));
}

// ---------------------------------------------------------------------------------------------
// Pickup

TEST_CASE("Pickup: taken once when the camera comes near, hidden, counted", "[Game][Interaction]") {
    TestScene t;
    EntityId a = t.entityAt("A", {0, 0, 0});
    EntityId b = t.entityAt("B", {5, 0, 0});
    t.scene.setGame(a, {{"pickup", ""}});
    t.scene.setGame(b, {{"pickup", R"({"radius": 0.5})"}});
    t.scene.camera().position = {0, 0, 3};
    CHECK(t.play().empty());
    CHECK(t.game.status() == "Pickups: 0 / 2");
    t.run(5);
    CHECK(t.scene.find(a)->visible);
    t.scene.camera().position = {0, 0, 0.9f};
    t.run(1);
    CHECK_FALSE(t.scene.find(a)->visible);
    CHECK(t.game.status() == "Pickups: 1 / 2");
    t.scene.find(a)->visible = true;  // shown again (by hand): not taken twice
    t.run(5);
    CHECK(t.game.status() == "Pickups: 1 / 2");
    t.scene.camera().position = {5, 0, 0.4f};
    t.run(1);
    CHECK(t.game.status() == "Pickups: 2 / 2");
}

TEST_CASE("Pickup: \"remove\" removes it (with what is under it) after the step", "[Game][Interaction]") {
    TestScene t;
    EntityId pickup = t.entityAt("Coin", {0, 0, 0});
    EntityId child = t.entityAt("Shine", {0, 1, 0}, pickup);
    t.scene.setGame(pickup, {{"pickup", R"({"remove": true})"}, {"spin", ""}});
    t.scene.setGame(child, {{"bob", ""}});
    t.scene.camera().position = {0, 0, 0};
    t.play();
    t.run(1);
    CHECK(t.scene.find(pickup) == nullptr);
    CHECK(t.scene.find(child) == nullptr);
    CHECK(t.game.status() == "Pickups: 1 / 1");
    CHECK(t.game.behaviours() == 1);  // the spin and the bob are gone with them; the pickup stays counted
    t.run(10);                        // (and nothing looks for them)
}

// ---------------------------------------------------------------------------------------------
// Trigger

TEST_CASE("Trigger: shows its target while the camera is in its box", "[Game][Interaction]") {
    TestScene t;
    EntityId box = t.entityAt("Zone", {10, 0, 0});
    t.scene.find(box)->local.scale = {4, 2, 2};  // x from 8 to 12
    t.scene.find(box)->local.rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0));  // now z 8..12
    EntityId lamp = t.entityAt("Lamp", {0, 3, 0});
    EntityId sign = t.entityAt("Sign", {0, 3, 0});
    t.scene.setGame(box, {{"trigger", R"({"target": )" + id(lamp) + "}"}});
    EntityId other = t.entityAt("Zone 2", {10, 0, 0});
    t.scene.find(other)->local.scale = glm::vec3(4, 2, 2);
    t.scene.find(other)->local.rotation = t.scene.find(box)->local.rotation;
    t.scene.setGame(other, {{"trigger", R"({"target": )" + id(sign) + R"(, "show": false})"}});
    t.scene.camera().position = {0, 0, 0};
    CHECK(t.play().empty());
    t.run(1);  // outside: the lamp hidden, the sign shown
    CHECK_FALSE(t.scene.find(lamp)->visible);
    CHECK(t.scene.find(sign)->visible);
    t.scene.camera().position = {11.5f, 0, 0};  // past the box's x extent: turned, it is 2 wide along x
    t.run(1);
    CHECK_FALSE(t.scene.find(lamp)->visible);
    t.scene.camera().position = {10.5f, 0.5f, 1.5f};
    t.run(1);
    CHECK(t.scene.find(lamp)->visible);
    CHECK_FALSE(t.scene.find(sign)->visible);
    t.scene.camera().position = {10, 0, 3};
    t.run(1);
    CHECK_FALSE(t.scene.find(lamp)->visible);
    CHECK(t.scene.find(sign)->visible);
}

TEST_CASE("Trigger: changes its target only when the camera goes in or out", "[Game][Interaction]") {
    TestScene t;
    EntityId box = t.entityAt("Zone", {0, 0, 0});
    EntityId lamp = t.entityAt("Lamp", {0, 3, 0});
    t.scene.setGame(box, {{"trigger", R"({"target": )" + id(lamp) + "}"}});
    t.scene.camera().position = {0, 0, 0};
    t.play();
    t.run(1);
    CHECK(t.scene.find(lamp)->visible);
    t.scene.find(lamp)->visible = false;  // hidden by something else meanwhile: left so
    t.run(5);
    CHECK_FALSE(t.scene.find(lamp)->visible);
}

TEST_CASE("Trigger and spawner: a target that isn't there, or isn't given, is a warning", "[Game][Interaction]") {
    TestScene t;
    EntityId a = t.entityAt("A", {0, 0, 0});
    t.scene.setGame(a, {{"trigger", R"({"target": 999})"}, {"spawner", ""}, {"cameraPath", R"({"lookAt": "x"})"}});
    std::vector<std::string> warnings = t.play();
    CHECK(contains(warnings, "\"target\": there is no entity 999"));
    CHECK(contains(warnings, "spawner: needs \"target\""));
    CHECK(contains(warnings, "\"lookAt\" isn't an entity's id"));
    CHECK(contains(warnings, "cameraPath: needs at least 2 children"));
    CHECK(t.game.behaviours() == 0);
    CHECK_FALSE(t.game.controlsCamera());
}

// ---------------------------------------------------------------------------------------------
// Spawner: adding and removing entities while playing

TEST_CASE("Spawner: copies its target at itself, shown and working, removed after their lifetime",
          "[Game][Interaction]") {
    TestScene t;
    EntityId prototype = t.entityAt("Crate", {0, -50, 0});
    t.scene.find(prototype)->visible = false;
    EntityId part = t.entityAt("Lid", {0, 1, 0}, prototype);
    t.scene.setGame(prototype, {{"spin", R"({"speed": 90})"}});
    t.scene.setGame(part, {{"bob", ""}});
    EntityId spawner = t.entityAt("Spawner", {3, 4, 5});
    t.scene.setGame(spawner, {{"spawner", R"({"target": )" + id(prototype) +
                                          R"(, "interval": 1, "lifetime": 2.5, "max": 10})"}});
    CHECK(t.play().empty());
    size_t before = t.scene.entities().size();
    t.run(59);
    CHECK(t.scene.entities().size() == before);
    t.run(1);  // 1 s: the first copy
    REQUIRE(t.scene.entities().size() == before + 2);  // the crate and its lid
    const Entity& copy = t.scene.entities()[before];
    EntityId copyId = copy.id;
    CHECK(copy.parent == 0);
    CHECK(copy.visible);
    CHECK(copy.local.position == glm::vec3(3, 4, 5));
    CHECK(t.game.behaviours() == 3 + 2);  // spin, bob, spawner, and the copy's spin and bob
    // The copy spins on from the turn the (spinning) crate had when it was copied: 90 degrees in a second
    glm::quat atSpawn = t.scene.find(copyId)->local.rotation;
    t.run(60);
    CHECK(nearlyEqual(t.scene.find(copyId)->local.rotation,
                      atSpawn * glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0))));
    CHECK(t.scene.entities().size() == before + 4);  // and a second copy at 2 s
    t.run(90);  // 3.5 s: the first is gone (2.5 s old), a third came at 3 s
    CHECK(t.scene.find(copyId) == nullptr);
    CHECK(t.scene.entities().size() == before + 4);
    CHECK(t.scene.find(prototype) != nullptr);
}

TEST_CASE("Spawner: at most max at a time; lifetime 0 keeps them", "[Game][Interaction]") {
    TestScene t;
    EntityId prototype = t.entityAt("Ball", {0, 0, 0});
    EntityId spawner = t.entityAt("Spawner", {0, 0, 0});
    t.scene.setGame(spawner, {{"spawner", R"({"target": )" + id(prototype) +
                                          R"(, "interval": 0.25, "lifetime": 0, "max": 3})"}});
    t.play();
    size_t before = t.scene.entities().size();
    t.run(600);
    CHECK(t.scene.entities().size() == before + 3);
}

TEST_CASE("Spawner: a copy taken as a pickup frees a place; the hidden target isn't a pickup", "[Game][Interaction]") {
    TestScene t;
    EntityId prototype = t.entityAt("Coin", {0, 0, 0});
    t.scene.find(prototype)->visible = false;  // where the copies come from: hidden, at the same place
    t.scene.setGame(prototype, {{"pickup", R"({"remove": true, "radius": 0.5})"}});
    EntityId spawner = t.entityAt("Spawner", {0, 0, 0});
    t.scene.setGame(spawner, {{"spawner", R"({"target": )" + id(prototype) +
                                          R"(, "interval": 1, "lifetime": 0, "max": 1})"}});
    t.scene.camera().position = {0, 0, 10};
    t.play();
    size_t before = t.scene.entities().size();
    CHECK(t.game.status().empty());  // (no pickup shown yet)
    t.run(60);
    CHECK(t.scene.entities().size() == before + 1);
    t.run(1);
    CHECK(t.game.status() == "Pickups: 0 / 1");
    t.scene.camera().position = {0, 0, 0};  // takes the copy, not the hidden coin
    t.run(1);
    CHECK(t.game.status() == "Pickups: 1 / 1");
    CHECK(t.scene.entities().size() == before);
    REQUIRE(t.scene.find(prototype) != nullptr);
    t.scene.camera().position = {0, 0, 10};
    t.run(60);  // a new copy: the place is free
    CHECK(t.scene.entities().size() == before + 1);
    t.run(1);
    CHECK(t.game.status() == "Pickups: 1 / 2");
}

// ---------------------------------------------------------------------------------------------
// Camera path

TEST_CASE("Camera path: the game drives the camera along the children, looping", "[Game][Interaction]") {
    TestScene t;
    EntityId path = t.entityAt("Path", {0, 1, 0});
    t.entityAt("P1", {0, 0, 0}, path);
    t.entityAt("P2", {4, 0, 0}, path);
    t.entityAt("P3", {4, 0, 3}, path);  // the loop back is 5 long: 12 in all
    t.scene.setGame(path, {{"cameraPath", R"({"speed": 2})"}});
    CHECK(t.play().empty());
    CHECK(t.game.controlsCamera());
    t.run(60);  // 2 units along
    CHECK(nearlyEqual(t.scene.camera().position, glm::vec3(2, 1, 0)));
    CHECK(nearlyEqual(t.scene.camera().forward(), glm::vec3(1, 0, 0), 1e-3f));  // looking along the path
    t.run(60 * 2);  // 6 along: 2 into the second piece
    CHECK(nearlyEqual(t.scene.camera().position, glm::vec3(4, 1, 2)));
    t.run(60 * 3);  // 12: round to the start
    CHECK(nearlyEqual(t.scene.camera().position, glm::vec3(0, 1, 0), 1e-3f));
}

TEST_CASE("Camera path: not looping it stops at the end; lookAt keeps it on an entity", "[Game][Interaction]") {
    TestScene t;
    EntityId path = t.entityAt("Path", {0, 0, 0});
    t.entityAt("P1", {0, 0, 0}, path);
    t.entityAt("P2", {0, 0, -4}, path);
    EntityId statue = t.entityAt("Statue", {10, 0, -2});
    t.scene.setGame(path, {{"cameraPath", R"({"speed": 1, "loop": false, "lookAt": )" + id(statue) + "}"}});
    CHECK(t.play().empty());
    t.run(60 * 10);
    CHECK(nearlyEqual(t.scene.camera().position, glm::vec3(0, 0, -4)));
    glm::vec3 toStatue = glm::normalize(glm::vec3(10, 0, 2));
    CHECK(nearlyEqual(t.scene.camera().forward(), toStatue, 1e-3f));
}

TEST_CASE("Camera path: blended between steps like entities", "[Game][Interaction][Interpolation]") {
    TestScene t;
    t.interpolation.enabled = true;
    EntityId path = t.entityAt("Path", {0, 0, 0});
    t.entityAt("P1", {0, 0, 0}, path);
    t.entityAt("P2", {100, 0, 0}, path);
    t.scene.setGame(path, {{"cameraPath", R"({"speed": 6, "loop": false})"}});
    t.play();
    std::vector<float> moves;
    float last = 0.0f;
    for (int f = 0; f < 144; f++) {
        t.interpolation.restore(t.scene);
        int ran = t.loop.advance(1.0 / 144.0, InputState(), [&](const InputState& input, uint64_t index) {
            t.interpolation.beforeStep(t.scene);
            GameStep step{t.scene, t.settings, input, GameLoop::stepSeconds,
                          static_cast<double>(index + 1) * GameLoop::stepSeconds, index, &t.interpolation};
            t.game.update(step);
        });
        t.interpolation.blend(t.scene, ran, static_cast<float>(t.loop.owed() / GameLoop::stepSeconds));
        float x = t.scene.camera().position.x;
        if (f >= 4) {
            moves.push_back(x - last);
        }
        last = x;
    }
    for (float move : moves) {
        CHECK(move == Catch::Approx(6.0 / 144.0).margin(1e-3));
    }
}

// ---------------------------------------------------------------------------------------------
// The checked-in game scenes

TEST_CASE("Game scenes: scenes/game_demo.json, game_flythrough.json and game_walk.json play without a warning",
          "[Game][Scenes]") {
    for (const char* name : {"game_demo.json", "game_flythrough.json", "game_walk.json"}) {
        INFO(name);
        TestScene t;  // (only built-in models here: the helmet is missing, its components stay)
        std::vector<std::string> sceneWarnings = t.scene.load(std::string(VULKANDEMO_SCENES_DIR) + "/" + name);
        for (const std::string& warning : sceneWarnings) {
            CHECK(warning.find("DamagedHelmet.glb") != std::string::npos);
        }
        std::vector<std::string> warnings = t.play();
        for (const std::string& warning : warnings) {
            UNSCOPED_INFO(warning);
        }
        CHECK(warnings.empty());
        CHECK(t.game.behaviours() > 0);
        t.run(600);  // 10 s: doors, pickups, triggers, spawners (or the camera path) running
    }
}
