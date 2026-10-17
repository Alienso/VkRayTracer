//
// Pausing the game (Game::pause / resume, the editor's F6): no steps while paused, and the editor's changes meanwhile
// taken in at resume: moved entities' behaviours go on from where they were put, new and changed components are read,
// removed entities' behaviours go.
//

#include <catch_amalgamated.hpp>
#include <glm/gtc/constants.hpp>
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
    // Steps at 60 fps: one per frame (as Application::updateGame)
    void run(int steps, const InputState& input = InputState()) {
        for (int i = 0; i < steps; i++) {
            interpolation.restore(scene);
            int ran = loop.advance(GameLoop::stepSeconds, input, [&](const InputState& stepInput, uint64_t index) {
                interpolation.beforeStep(scene);
                GameStep step{scene, settings, stepInput, GameLoop::stepSeconds,
                              static_cast<double>(index + 1) * GameLoop::stepSeconds, index, &interpolation};
                game.update(step);
            });
            interpolation.blend(scene, ran, static_cast<float>(loop.owed() / GameLoop::stepSeconds));
        }
    }
    // As Application::updateGame does when the editor pauses and resumes
    void pause() {
        interpolation.restore(scene);
        interpolation.clear();
        game.pause(scene);
    }
    std::vector<std::string> resume() {
        std::vector<std::string> warnings = game.resume(scene);
        loop.dropInput();
        interpolation.clear();
        return warnings;
    }
    EntityId entityWith(const std::string& name, std::vector<GameComponent> components,
                        const glm::vec3& position = glm::vec3(0.0f)) {
        EntityId id = scene.addEmpty(name, 0);
        scene.find(id)->local.position = position;
        scene.setGame(id, std::move(components));
        return id;
    }
    Entity& entity(EntityId id) { return *scene.find(id); }
};

bool nearlyEqual(const glm::quat& a, const glm::quat& b, float tolerance = 1e-4f) {
    return std::abs(std::abs(glm::dot(a, b)) - 1.0f) < tolerance;
}

glm::quat aroundY(float degrees) {
    return glm::angleAxis(glm::radians(degrees), glm::vec3(0.0f, 1.0f, 0.0f));
}

}  // namespace

TEST_CASE("Pause: no steps while paused; resumed, it goes on from where it stopped", "[Game][Pause]") {
    TestScene t;
    EntityId spinner = t.entityWith("Spinner", {{"spin", ""}});  // 90 degrees per second
    t.play();
    t.run(60);
    CHECK(nearlyEqual(t.entity(spinner).local.rotation, aroundY(90.0f)));
    t.pause();
    CHECK(t.game.paused());
    // A step reaching the game anyway does nothing
    GameStep step{t.scene, t.settings, InputState(), GameLoop::stepSeconds, 99.0, 999};
    t.game.update(step);
    CHECK(nearlyEqual(t.entity(spinner).local.rotation, aroundY(90.0f)));
    t.resume();
    CHECK_FALSE(t.game.paused());
    t.run(60);
    CHECK(nearlyEqual(t.entity(spinner).local.rotation, aroundY(180.0f)));
}

TEST_CASE("Pause: a bobbing entity moved while paused bobs around where it was put", "[Game][Pause]") {
    TestScene t;
    EntityId bobber = t.entityWith("Bobber", {{"bob", ""}});  // 0.25 up and down, every 2 s
    t.play();
    t.run(30);  // a quarter period: at the top
    CHECK(t.entity(bobber).local.position.y == Catch::Approx(0.25f).margin(1e-4));
    t.pause();
    t.entity(bobber).local.position = glm::vec3(5.0f, 2.25f, 0.0f);  // put 2 higher and 5 to the side
    t.resume();
    t.run(60);  // half a period on: at the bottom
    CHECK(t.entity(bobber).local.position.x == Catch::Approx(5.0f));
    CHECK(t.entity(bobber).local.position.y == Catch::Approx(2.0f - 0.25f).margin(1e-4));
}

TEST_CASE("Pause: a spinning entity turned while paused turns on from its new rotation", "[Game][Pause]") {
    TestScene t;
    EntityId spinner = t.entityWith("Spinner", {{"spin", ""}});
    t.play();
    t.run(60);
    t.pause();
    t.entity(spinner).local.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);  // turned back by hand
    t.resume();
    t.run(60);
    CHECK(nearlyEqual(t.entity(spinner).local.rotation, aroundY(90.0f)));
}

TEST_CASE("Pause: a door turned while paused closes to its new rotation", "[Game][Pause]") {
    TestScene t;
    EntityId door = t.entityWith("Door", {{"door", ""}}, glm::vec3(0.0f, 0.0f, -10.0f));  // out of reach
    t.play();
    t.run(10);
    t.pause();
    t.entity(door).local.rotation = aroundY(30.0f);
    t.resume();
    t.run(30);
    CHECK(nearlyEqual(t.entity(door).local.rotation, aroundY(30.0f)));
}

TEST_CASE("Pause: an entity's parent moved while paused leaves its own places alone", "[Game][Pause]") {
    TestScene t;
    EntityId parent = t.scene.addEmpty("Parent", 0);
    EntityId bobber = t.entityWith("Bobber", {{"bob", ""}});
    t.scene.setParent(bobber, parent, false);
    t.play();
    t.run(30);
    t.pause();
    t.entity(parent).local.position = glm::vec3(0.0f, 10.0f, 0.0f);
    t.resume();
    t.run(60);
    CHECK(t.entity(bobber).local.position.y == Catch::Approx(-0.25f).margin(1e-4));  // its own bob, under the parent
    CHECK(t.scene.worldTransform(bobber)[3].y == Catch::Approx(10.0f - 0.25f).margin(1e-4));
}

TEST_CASE("Pause: components added, changed or removed while paused are taken in", "[Game][Pause]") {
    TestScene t;
    EntityId kept = t.entityWith("Kept", {{"spin", ""}});
    EntityId removed = t.entityWith("Removed", {{"spin", ""}});
    EntityId changed = t.entityWith("Changed", {{"spin", ""}});
    t.play();
    t.run(60);
    CHECK(t.game.behaviours() == 3);
    t.pause();
    EntityId added = t.entityWith("Added", {{"spin", R"({"speed": 45})"}});
    t.scene.remove(removed);
    t.scene.setGame(changed, {{"spin", R"({"speed": 180})"}});
    std::vector<std::string> warnings = t.resume();
    CHECK(warnings.empty());
    CHECK(t.game.behaviours() == 3);
    t.run(60);
    CHECK(nearlyEqual(t.entity(kept).local.rotation, aroundY(180.0f)));
    CHECK(nearlyEqual(t.entity(added).local.rotation, aroundY(45.0f)));  // from its rotation then (none), at 45
    CHECK(nearlyEqual(t.entity(changed).local.rotation, aroundY(90.0f + 180.0f)));  // read anew from where it was
}

TEST_CASE("Pause: a component made wrong while paused is a warning at resume", "[Game][Pause]") {
    TestScene t;
    EntityId spinner = t.entityWith("Spinner", {{"spin", ""}});
    t.play();
    t.pause();
    t.scene.setGame(spinner, {{"spin", R"({"speed": "fast"})"}});
    std::vector<std::string> warnings = t.resume();
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].find("\"speed\" isn't a number") != std::string::npos);
    CHECK(t.game.behaviours() == 1);
}

TEST_CASE("Pause: a pickup deleted while paused isn't counted any more", "[Game][Pause]") {
    TestScene t;
    t.entityWith("Coin 1", {{"pickup", ""}}, glm::vec3(10.0f, 0.0f, 0.0f));
    EntityId coin2 = t.entityWith("Coin 2", {{"pickup", ""}}, glm::vec3(-10.0f, 0.0f, 0.0f));
    t.play();
    t.run(1);
    CHECK(t.game.status() == "Pickups: 0 / 2");
    t.pause();
    t.scene.remove(coin2);
    t.resume();
    t.run(1);
    CHECK(t.game.status() == "Pickups: 0 / 1");
}

TEST_CASE("Pause: a spawner added while paused waits its interval from then", "[Game][Pause]") {
    TestScene t;
    EntityId crate = t.scene.addEmpty("Crate", 0);
    t.scene.find(crate)->visible = false;
    t.play();
    t.run(600);  // 10 s
    t.pause();
    EntityId spawner = t.entityWith("Spawner", {{"spawner", R"({"target": )" + std::to_string(crate) + "}"}});
    t.resume();
    size_t before = t.scene.entities().size();
    t.run(110);  // 1.83 s: not yet (every 2 s)
    CHECK(t.scene.entities().size() == before);
    t.run(15);
    CHECK(t.scene.entities().size() == before + 1);
    (void)spawner;
}

TEST_CASE("Pause: a camera path's entity moved while paused takes the path along", "[Game][Pause]") {
    TestScene t;
    EntityId path = t.scene.addEmpty("Path", 0);
    for (float x : {0.0f, 10.0f}) {
        EntityId point = t.scene.addEmpty("Point", path);
        t.scene.find(point)->local.position = glm::vec3(x, 1.0f, 0.0f);
    }
    t.scene.setGame(path, {{"cameraPath", R"({"loop": false, "speed": 1})"}});
    t.play();
    t.run(60);
    CHECK(t.scene.camera().position.x == Catch::Approx(1.0f).margin(1e-3));
    t.pause();
    t.entity(path).local.position = glm::vec3(0.0f, 0.0f, 5.0f);
    t.scene.camera().position = glm::vec3(100.0f);  // the editor's fly camera somewhere else
    t.resume();
    t.run(60);
    CHECK(t.scene.camera().position.x == Catch::Approx(2.0f).margin(1e-3));
    CHECK(t.scene.camera().position.z == Catch::Approx(5.0f).margin(1e-3));
}

TEST_CASE("Pause: input waiting for a step is dropped at resume", "[Game][Pause][GameLoop]") {
    GameLoop loop;
    loop.reset();
    InputState press;
    press.down[Key::E] = true;
    press.pressed[Key::E] = true;
    int steps = loop.advance(GameLoop::stepSeconds * 0.25, press, [](const InputState&, uint64_t) {});
    CHECK(steps == 0);  // waiting for the next step
    loop.dropInput();
    bool sawPress = false;
    loop.advance(GameLoop::stepSeconds, InputState(), [&](const InputState& input, uint64_t) {
        sawPress = sawPress || input.wasPressed(Key::E);
    });
    CHECK_FALSE(sawPress);
}

TEST_CASE("Pause: Stop while paused drops the pause", "[Game][Pause]") {
    TestScene t;
    t.entityWith("Spinner", {{"spin", ""}});
    t.play();
    t.pause();
    t.game.stop();
    CHECK_FALSE(t.game.paused());
    t.play();
    CHECK_FALSE(t.game.paused());
    CHECK(t.game.behaviours() == 1);
}
