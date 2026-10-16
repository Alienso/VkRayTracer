//
// Showing the game's steps between them (game/StepInterpolation.h): smooth at frame rates that
// aren't the step rate, never seen by the game, jumps shown at once, other changes left alone.
//

#include <catch_amalgamated.hpp>
#include "TestHelpers.h"
#include "game/Game.h"
#include "game/GameLoop.h"
#include "game/StepInterpolation.h"
#include "scene/Scene.h"

namespace {  // (each test file has its own TestScene)

// A behaviour the tests write themselves: called each step with the scene
using StepFunction = std::function<void(const GameStep&)>;

struct TestScene : TestAssets {
    Scene scene{assets, settings};
    GameLoop loop;
    StepInterpolation interpolation;
    TestScene() {
        scene.createDefault();
        loop.reset();
    }

    // One frame as Application::updateGame runs it; returns the steps run
    int frame(double seconds, const StepFunction& step) {
        interpolation.restore(scene);
        int steps = loop.advance(seconds, InputState(), [&](const InputState& input, uint64_t index) {
            interpolation.beforeStep(scene);
            GameStep s{scene, settings, input, GameLoop::stepSeconds,
                       static_cast<double>(index + 1) * GameLoop::stepSeconds, index, &interpolation};
            step(s);
        });
        interpolation.blend(scene, steps, static_cast<float>(loop.owed() / GameLoop::stepSeconds));
        return steps;
    }
};

// Moves the entity along X at 1 unit per second, from the position it has (as a behaviour that adds up would)
StepFunction slide(EntityId id) {
    return [id](const GameStep& step) { step.scene.find(id)->local.position.x += static_cast<float>(step.seconds); };
}

}  // namespace

TEST_CASE("Step interpolation: at 144 fps a slide moves the same distance every frame", "[Game][Interpolation]") {
    for (bool blended : {true, false}) {
        TestScene t;
        t.interpolation.enabled = blended;
        EntityId id = t.scene.addEmpty("Slider", 0);
        StepFunction step = slide(id);
        std::vector<float> moves;
        float last = 0.0f;
        for (int f = 0; f < 288; f++) {
            t.frame(1.0 / 144.0, step);
            float x = t.scene.find(id)->local.position.x;
            if (f >= 4) {  // (the first step: nothing to blend from yet)
                moves.push_back(x - last);
            }
            last = x;
        }
        float most = *std::max_element(moves.begin(), moves.end());
        float least = *std::min_element(moves.begin(), moves.end());
        INFO("blended " << blended);
        if (blended) {
            CHECK(least == Catch::Approx(1.0 / 144.0).margin(1e-4));  // even
            CHECK(most == Catch::Approx(1.0 / 144.0).margin(1e-4));
        } else {
            CHECK(least == 0.0f);                                     // held in some frames
            CHECK(most == Catch::Approx(1.0 / 60.0).margin(1e-5));    // a whole step in others
        }
        // Either way the game itself is where 2 s of steps put it: the blend is only what is shown
        t.interpolation.restore(t.scene);
        CHECK(t.scene.find(id)->local.position.x == Catch::Approx(static_cast<double>(t.loop.steps()) / 60.0));
    }
}

TEST_CASE("Step interpolation: the game sees its own transforms, not the blended ones", "[Game][Interpolation]") {
    TestScene t;
    EntityId id = t.scene.addEmpty("Slider", 0);
    std::vector<float> seen;
    StepFunction step = [&](const GameStep& s) {
        seen.push_back(s.scene.find(id)->local.position.x);
        s.scene.find(id)->local.position.x += static_cast<float>(s.seconds);
    };
    for (int f = 0; f < 500; f++) {
        t.frame(1.0 / 97.0, step);
    }
    for (size_t i = 0; i < seen.size(); i++) {
        CHECK(seen[i] == Catch::Approx(static_cast<double>(i) / 60.0).margin(1e-4));  // whole steps only
    }
}

TEST_CASE("Step interpolation: a frame without a step blends further between the same two", "[Game][Interpolation]") {
    TestScene t;
    EntityId id = t.scene.addEmpty("Slider", 0);
    StepFunction step = slide(id);
    REQUIRE(t.frame(GameLoop::stepSeconds, step) == 1);  // 0 -> 1/60
    REQUIRE(t.frame(GameLoop::stepSeconds, step) == 1);  // 1/60 -> 2/60, nothing owed: shown before it (a step behind)
    CHECK(t.scene.find(id)->local.position.x == Catch::Approx(1.0 / 60.0));
    REQUIRE(t.frame(GameLoop::stepSeconds * 0.25, step) == 0);  // a quarter into the next step
    CHECK(t.scene.find(id)->local.position.x == Catch::Approx(1.25 / 60.0));
    REQUIRE(t.frame(GameLoop::stepSeconds * 0.5, step) == 0);
    CHECK(t.scene.find(id)->local.position.x == Catch::Approx(1.75 / 60.0));
}

TEST_CASE("Step interpolation: rotations turn the short way, at an even rate", "[Game][Interpolation]") {
    TestScene t;
    EntityId id = t.scene.addEmpty("Spinner", 0);
    t.scene.setGame(id, {{"spin", R"({"speed": 600})"}});  // 10 degrees a step
    Game game;
    game.start(t.scene);
    StepFunction step = [&](const GameStep& s) { game.update(s); };
    glm::quat last = t.scene.find(id)->local.rotation;
    std::vector<float> turns;
    for (int f = 0; f < 200; f++) {
        t.frame(1.0 / 144.0, step);
        glm::quat now = t.scene.find(id)->local.rotation;
        if (f >= 4) {
            turns.push_back(glm::degrees(glm::angle(glm::inverse(last) * now)));
        }
        last = now;
    }
    for (float turn : turns) {
        CHECK(turn == Catch::Approx(600.0 / 144.0).margin(0.05));
    }
}

TEST_CASE("Step interpolation: a snapped jump is shown at once", "[Game][Interpolation]") {
    TestScene t;
    EntityId id = t.scene.addEmpty("Jumper", 0);
    bool jump = false;
    StepFunction step = [&](const GameStep& s) {
        if (jump) {
            s.scene.find(id)->local.position = {100.0f, 0.0f, 0.0f};
            s.snap(id);
            jump = false;
        }
    };
    t.frame(GameLoop::stepSeconds * 1.5, step);
    jump = true;
    t.frame(GameLoop::stepSeconds, step);  // a step runs, half a step owed
    CHECK(t.scene.find(id)->local.position.x == 100.0f);

    // Not snapped: shown half way
    EntityId other = t.scene.addEmpty("Slid", 0);
    StepFunction move = [&](const GameStep& s) { s.scene.find(other)->local.position.x = 100.0f; };
    t.frame(GameLoop::stepSeconds, move);
    CHECK(t.scene.find(other)->local.position.x == Catch::Approx(50.0f));
}

TEST_CASE("Step interpolation: a change made between frames (the editor) is kept", "[Game][Interpolation]") {
    TestScene t;
    EntityId id = t.scene.addEmpty("Slider", 0);
    StepFunction step = slide(id);
    for (int f = 0; f < 10; f++) {
        t.frame(1.0 / 144.0, step);
    }
    REQUIRE(t.interpolation.blended() == 1);
    t.scene.find(id)->local.position = {5.0f, 6.0f, 7.0f};  // typed into the Details panel
    t.interpolation.restore(t.scene);
    CHECK(t.scene.find(id)->local.position == glm::vec3(5.0f, 6.0f, 7.0f));
    CHECK(t.interpolation.blended() == 0);
}

TEST_CASE("Step interpolation: entities added or removed in a step", "[Game][Interpolation]") {
    TestScene t;
    EntityId slider = t.scene.addEmpty("Slider", 0);
    EntityId added = 0;
    EntityId removed = t.scene.addEmpty("Removed", 0);
    StepFunction moveSlider = slide(slider);
    t.frame(GameLoop::stepSeconds * 1.5, moveSlider);
    StepFunction step = [&](const GameStep& s) {
        moveSlider(s);
        if (added == 0) {
            added = s.scene.addEmpty("Added", 0);
            s.scene.find(added)->local.position.x = 3.0f;
            s.scene.remove(removed);
        }
    };
    t.frame(GameLoop::stepSeconds, step);
    CHECK(t.scene.find(added)->local.position.x == 3.0f);  // where it was put: nothing to blend from
    CHECK(t.scene.find(slider)->local.position.x == Catch::Approx(1.5 / 60.0));  // still blended, by id
    t.frame(GameLoop::stepSeconds * 0.25, step);  // (the removed one is skipped)
    CHECK(t.interpolation.blended() == 1);
}
