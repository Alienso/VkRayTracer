//
// Portals carry what the game moves (Game::carryThroughPortals, Scene::portalCrossing): an entity whose origin goes
// through an opening comes out of the other end with what is under it, its behaviours go on there; the player goes
// through with its view and speed turned, and the camera's jump reaches the render list.
//

#include <catch_amalgamated.hpp>
#include "TestHelpers.h"
#include "game/Colliders.h"
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

    void play() {
        loop.reset();
        interpolation.clear();
        REQUIRE(game.start(scene).empty());
    }
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
    // A pair whose ends are placed in the world as given (1 wide, 2 high, facing their +Z)
    void portalPair(const glm::vec3& a, const glm::quat& turnA, const glm::vec3& b, const glm::quat& turnB) {
        EntityId group = scene.addPortalPair();
        scene.find(group)->local = Transform();
        const std::vector<EntityId> ends = scene.find(group)->children;
        REQUIRE(ends.size() == 2);
        scene.find(ends[0])->local.position = a;
        scene.find(ends[0])->local.rotation = turnA;
        scene.find(ends[1])->local.position = b;
        scene.find(ends[1])->local.rotation = turnB;
    }
    EntityId entityWith(const std::string& name, std::vector<GameComponent> components, const glm::vec3& position,
                        EntityId parent = 0) {
        EntityId id = scene.addEmpty(name, parent);
        scene.find(id)->local.position = position;
        scene.setGame(id, std::move(components));
        return id;
    }
    glm::vec3 world(EntityId id) const { return glm::vec3(scene.worldTransform(id)[3]); }
};

const glm::quat facingUp = glm::angleAxis(glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f));  // +Z to +Y
const glm::quat facingDown = glm::angleAxis(glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f));  // +Z to -Y
const glm::quat none(1.0f, 0.0f, 0.0f, 0.0f);

}  // namespace

TEST_CASE("Portals: a line through an opening is carried to the other end; one beside it isn't", "[Game][Portals]") {
    TestScene t;
    t.portalPair({0.0f, 1.0f, -5.0f}, none, {10.0f, 1.0f, 0.0f}, none);  // both facing +Z
    std::optional<glm::mat4> crossing = t.scene.portalCrossing({0.2f, 1.5f, -4.9f}, {0.2f, 1.5f, -5.1f});
    REQUIRE(crossing);
    // Into A's front, out of B's front: half turned around the vertical
    glm::vec3 out = glm::vec3(*crossing * glm::vec4(0.2f, 1.5f, -5.1f, 1.0f));
    CHECK(out.x == Catch::Approx(10.0f - 0.2f).margin(1e-4));
    CHECK(out.y == Catch::Approx(1.5f).margin(1e-4));
    CHECK(out.z == Catch::Approx(0.1f).margin(1e-4));
    CHECK_FALSE(t.scene.portalCrossing({2.0f, 1.5f, -4.9f}, {2.0f, 1.5f, -5.1f}));  // beside the opening
    CHECK_FALSE(t.scene.portalCrossing({0.2f, 1.5f, -4.0f}, {0.2f, 1.5f, -4.5f}));  // not as far as it
}

TEST_CASE("Portals: a bobbing entity going down through an opening comes out of the other end and bobs on there",
          "[Game][Portals]") {
    TestScene t;
    // Openings lying flat at y = 1: down into A (facing up), down out of B (facing down), so the bob goes on its way
    t.portalPair({0.0f, 1.0f, 0.0f}, facingUp, {10.0f, 1.0f, 0.0f}, facingDown);
    EntityId bobber = t.entityWith("Bobber", {{"bob", R"({"height": 0.5, "period": 2})"}}, {0.0f, 1.2f, 0.0f});
    EntityId rider = t.scene.addEmpty("Rider", bobber);
    t.scene.find(rider)->local.position = {0.3f, 0.0f, 0.0f};
    EntityId beside = t.entityWith("Beside", {{"bob", R"({"height": 0.5, "period": 2})"}}, {3.0f, 1.2f, 0.0f});
    t.play();
    int crossedAt = -1;
    for (int i = 0; i < 120 && crossedAt < 0; i++) {
        t.run(1);
        if (t.world(bobber).x > 5.0f) {
            crossedAt = i;
        }
    }
    REQUIRE(crossedAt >= 0);
    // Out of B, just under its plane, with the rider along (turned with it: 0.3 away still)
    CHECK(t.world(bobber).x == Catch::Approx(10.0f).margin(1e-3));
    CHECK(t.world(bobber).y < 1.0f);
    CHECK(t.world(bobber).y > 0.9f);
    CHECK(glm::distance(t.world(rider), t.world(bobber)) == Catch::Approx(0.3f).margin(1e-4));
    CHECK(t.scene.find(rider)->local.position == glm::vec3(0.3f, 0.0f, 0.0f));  // (moved with its parent, not itself)
    // Its bob goes on from there, down and away from B
    float y = t.world(bobber).y;
    t.run(3);
    CHECK(t.world(bobber).x == Catch::Approx(10.0f).margin(1e-3));
    CHECK(t.world(bobber).y < y);
    CHECK(t.world(beside).x == Catch::Approx(3.0f));  // beside the openings: never carried
}

TEST_CASE("Portals: the player walking through comes out of the other end, its view and speed turned",
          "[Game][Portals]") {
    TestScene t;
    t.entityWith("Floor", {{"collider", R"({"size": [60, 1, 60]})"}}, {0.0f, -0.5f, 0.0f});
    t.portalPair({0.0f, 1.0f, -3.0f}, none, {10.0f, 1.0f, 0.0f}, none);  // both facing +Z
    t.entityWith("Player", {{"player", ""}}, {0.0f, 0.0f, 0.0f});
    t.scene.camera().yaw = 0.0f;  // looking along -z, at A's front
    t.scene.camera().pitch = 0.0f;
    t.play();
    t.run(10);
    RenderList list;
    t.scene.buildRenderList(list);  // (takes the camera's jumps so far: none)
    InputState walk;
    walk.down[Key::W] = true;
    bool crossed = false;
    for (int i = 0; i < 90 && !crossed; i++) {
        t.run(1, walk);
        crossed = t.scene.camera().position.x > 5.0f;
    }
    REQUIRE(crossed);
    CHECK(t.scene.camera().position.x == Catch::Approx(10.0f).margin(1e-3));
    CHECK(t.scene.camera().position.z > 0.0f);  // out of B's front
    CHECK(std::abs(std::abs(t.scene.camera().yaw) - 180.0f) < 1e-2f);  // turned to face +z
    t.scene.buildRenderList(list);
    CHECK(list.cameraTeleport != glm::mat4(1.0f));  // the renderer's history follows the jump
    float z = t.scene.camera().position.z;
    t.run(30, walk);
    CHECK(t.scene.camera().position.z > z + 1.5f);  // walking on, away from B
    CHECK(t.scene.camera().position.x == Catch::Approx(10.0f).margin(1e-3));
}

TEST_CASE("Portals: the player walks through a portal set in a wall collider (no gap in it); beside it, the wall stops "
          "it", "[Game][Portals]") {
    for (float x : {0.0f, 2.0f}) {
        TestScene t;
        t.entityWith("Floor", {{"collider", R"({"size": [60, 1, 60]})"}}, {0.0f, -0.5f, 0.0f});
        t.entityWith("Wall", {{"collider", R"({"size": [12, 4, 1]})"}}, {0.0f, 2.0f, -3.5f});  // its face at z = -3
        t.portalPair({0.0f, 1.0f, -3.0f}, none, {10.0f, 1.0f, 0.0f}, none);  // both facing +Z
        t.entityWith("Player", {{"player", ""}}, {x, 0.0f, 0.0f});
        t.scene.camera().yaw = 0.0f;  // looking along -z
        t.scene.camera().pitch = 0.0f;
        t.play();
        InputState walk;
        walk.down[Key::W] = true;
        t.run(60, walk);
        if (x == 0.0f) {
            CHECK(t.scene.camera().position.x == Catch::Approx(10.0f).margin(1e-3));
            CHECK(t.scene.camera().position.z > 0.5f);  // out of B's front, walking on
        } else {
            CHECK(t.scene.camera().position.z == Catch::Approx(-2.7f).margin(1e-3));
            CHECK(t.scene.camera().position.x == Catch::Approx(2.0f).margin(1e-3));
        }
    }
}

TEST_CASE("Portals: a solid collider slides into a portal set in a wall collider and out of the other end; something "
          "there stops it half through", "[Game][Portals]") {
    for (bool blocker : {false, true}) {
        TestScene t;
        t.portalPair({0.0f, 1.0f, -5.0f}, none, {10.0f, 1.0f, 0.0f}, none);  // both facing +Z
        t.entityWith("Wall", {{"collider", R"({"size": [12, 4, 1]})"}}, {0.0f, 2.0f, -5.5f});  // its face at z = -5
        if (blocker) {
            t.entityWith("Blocker", {{"collider", R"({"size": [2, 2, 0.5]})"}}, {10.0f, 1.0f, 0.4f});  // 0.15 out
        }
        EntityId box = t.entityWith("Box", {{"collider", R"({"size": [0.4, 0.4, 0.4], "solid": true})"},
                                            {"slide", R"({"axis": [0, 0, -1], "distance": 4, "period": 8})"}},
                                    {0.0f, 1.0f, -3.0f});
        t.play();
        t.run(150);  // at A (z = -5) after 2 s
        if (blocker) {
            CHECK(t.world(box).z == Catch::Approx(-5.0f + 0.2f - 0.15f).margin(3e-3));  // held, 0.15 of it out of B
        } else {
            CHECK(t.world(box).x == Catch::Approx(10.0f).margin(1e-3));
            CHECK(t.world(box).z > 0.0f);
        }
    }
}

TEST_CASE("Portals: a solid collider carried through isn't stopped by what is between the two ends", "[Game][Portals]") {
    TestScene t;
    t.portalPair({0.0f, 1.0f, -5.0f}, none, {10.0f, 1.0f, 0.0f}, none);  // both facing +Z
    t.entityWith("Wall between", {{"collider", R"({"size": [0.2, 4, 20]})"}}, {5.0f, 1.0f, -2.5f});
    EntityId box = t.entityWith("Box", {{"collider", R"({"size": [0.4, 0.4, 0.4], "solid": true})"},
                                        {"slide", R"({"axis": [0, 0, -1], "distance": 4, "period": 8})"}},
                                {0.0f, 1.0f, -3.0f});
    t.play();
    t.run(150);  // at A (z = -5) after 2 s: through it, out of B
    CHECK(t.world(box).x == Catch::Approx(10.0f).margin(1e-3));
    CHECK(t.world(box).z > 0.0f);
}

TEST_CASE("Portals: a collider half through is outlined at both ends", "[Game][Portals]") {
    TestScene t;
    t.portalPair({0.0f, 1.0f, -5.0f}, none, {10.0f, 1.0f, 0.0f}, none);  // both facing +Z
    EntityId box = t.entityWith("Box", {{"collider", R"({"size": [0.4, 0.4, 0.4]})"}}, {0.0f, 1.0f, -4.9f});
    std::vector<WorldBox> boxes = colliderBoxes(t.scene, box);
    REQUIRE(boxes.size() == 2);
    CHECK_FALSE(boxes[0].ghost);
    CHECK(boxes[1].ghost);
    glm::vec3 out(boxes[1].transform[3]);  // its origin taken through: 0.1 behind B (the part beyond A is in front)
    CHECK(glm::distance(out, glm::vec3(10.0f, 1.0f, -0.1f)) < 1e-4f);
    t.scene.find(box)->local.position.x = 3.0f;  // beside the opening: once
    CHECK(colliderBoxes(t.scene, box).size() == 1);
}
