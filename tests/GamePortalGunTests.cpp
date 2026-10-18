//
// The portal gun (game/behaviours/PortalGun.cpp): left and right click shoot the pair's two ends onto colliders; the
// pair works only once both are placed; a second shot moves an end.
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
        interpolation.enabled = false;
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
    EntityId entityWith(const std::string& name, std::vector<GameComponent> components, const glm::vec3& position) {
        EntityId id = scene.addEmpty(name, 0);
        scene.find(id)->local.position = position;
        scene.setGame(id, std::move(components));
        return id;
    }
    // The gun's pair's ends (A, B): the portal ends in the scene
    void ends(EntityId& a, EntityId& b) const {
        a = b = 0;
        for (const Entity& entity : scene.entities()) {
            if (const PortalComponent* portal = scene.portal(entity.id)) {
                (portal->endA ? a : b) = entity.id;
            }
        }
    }
    glm::vec3 world(EntityId id) const { return glm::vec3(scene.worldTransform(id)[3]); }
    glm::vec3 facing(EntityId id) const { return glm::normalize(glm::vec3(scene.worldTransform(id)[2])); }

    // One step with a mouse button clicked / the view turned (the player's mouse look: 0.1 degrees per pixel)
    void click(int button) {
        InputState input;
        input.mousePressed[button] = true;
        input.mouseDown[button] = true;
        run(1, input);
    }
    void turn(float degreesRight) {
        InputState input;
        input.mouseDelta.x = degreesRight / 0.1f;
        run(1, input);
    }
};

// A floor, a wall in front (its face at z = -5) and one to the left (its face at x = -6), the player at the origin
// looking along -z with the gun
void room(TestScene& t, const char* leftWall = R"({"size": [1, 4, 12]})") {
    t.entityWith("Floor", {{"collider", R"({"size": [60, 1, 60]})"}}, {0.0f, -0.5f, 0.0f});
    t.entityWith("Front wall", {{"collider", R"({"size": [12, 4, 1]})"}}, {0.0f, 2.0f, -5.5f});
    t.entityWith("Left wall", {{"collider", leftWall}}, {-6.5f, 2.0f, 0.0f});
    t.entityWith("Player", {{"player", ""}, {"portalGun", ""}}, {0.0f, 0.0f, 0.0f});
    t.scene.camera().yaw = 0.0f;
    t.scene.camera().pitch = 0.0f;
}

}  // namespace

TEST_CASE("Portal gun: one end placed is shown but the pair is off; with both placed it works", "[Game][Portals]") {
    TestScene t;
    room(t);
    t.play();
    t.run(5);
    EntityId a, b;
    t.ends(a, b);
    REQUIRE(a != 0);
    REQUIRE(b != 0);
    CHECK_FALSE(t.scene.find(a)->visible);  // made at Play, hidden until shot
    CHECK_FALSE(t.scene.find(b)->visible);
    CHECK(t.scene.portalOpenings().empty());

    t.click(MouseButton::Left);  // at the front wall, at eye height: down to the floor (too little room to hang)
    CHECK(t.scene.find(a)->visible);
    CHECK(glm::distance(t.world(a), glm::vec3(0.0f, 1.1f, -5.0f + 0.01f)) < 1e-3f);
    CHECK(glm::distance(t.facing(a), glm::vec3(0.0f, 0.0f, 1.0f)) < 1e-4f);  // out of the wall
    CHECK_FALSE(t.scene.find(b)->visible);
    CHECK(t.scene.portalOpenings().empty());  // one end: off

    t.turn(-90.0f);  // looking along -x, at the left wall
    t.click(MouseButton::Right);
    CHECK(t.scene.find(b)->visible);
    CHECK(glm::distance(t.world(b), glm::vec3(-6.0f + 0.01f, 1.1f, 0.0f)) < 1e-3f);
    CHECK(glm::distance(t.facing(b), glm::vec3(1.0f, 0.0f, 0.0f)) < 1e-4f);
    CHECK(t.scene.portalOpenings().size() == 2);  // both: on
}

TEST_CASE("Portal gun: a second shot moves that button's end", "[Game][Portals]") {
    TestScene t;
    room(t);
    t.play();
    t.run(5);
    EntityId a, b;
    t.ends(a, b);
    t.click(MouseButton::Left);
    glm::vec3 first = t.world(a);
    t.turn(30.0f);  // a little to the right: further along the front wall
    t.click(MouseButton::Left);
    CHECK(t.world(a).x > first.x + 1.0f);
    CHECK(t.world(a).z == Catch::Approx(-5.0f + 0.01f).margin(1e-3));
    CHECK_FALSE(t.scene.find(b)->visible);  // the other one untouched
}

TEST_CASE("Portal gun: a collider that isn't portalable stops the shot but takes no end", "[Game][Portals]") {
    TestScene t;
    room(t, R"({"size": [1, 4, 12], "portalable": false})");
    t.play();
    t.run(5);
    EntityId a, b;
    t.ends(a, b);
    t.turn(-90.0f);
    t.click(MouseButton::Right);
    CHECK_FALSE(t.scene.find(b)->visible);
    t.turn(90.0f);
    t.click(MouseButton::Right);  // the front wall takes it
    CHECK(t.scene.find(b)->visible);
}

TEST_CASE("Portal gun: the game asks for a crosshair while there is one", "[Game][Portals]") {
    TestScene t;
    room(t);
    CHECK_FALSE(t.game.wantsCrosshair());
    t.play();
    CHECK(t.game.wantsCrosshair());
    t.game.stop();
    CHECK_FALSE(t.game.wantsCrosshair());
}

TEST_CASE("Portal gun: shot at the floor, the end lies on it with its top away from the player", "[Game][Portals]") {
    TestScene t;
    room(t);
    t.scene.camera().pitch = -30.0f;  // the floor 1.65 / tan(30) ahead
    t.play();
    t.run(5);
    EntityId a, b;
    t.ends(a, b);
    t.click(MouseButton::Left);
    REQUIRE(t.scene.find(a)->visible);
    CHECK(glm::distance(t.world(a), glm::vec3(0.0f, 0.01f, -1.65f / std::tan(glm::radians(30.0f)))) < 1e-2f);
    CHECK(glm::distance(t.facing(a), glm::vec3(0.0f, 1.0f, 0.0f)) < 1e-4f);
    t.click(MouseButton::Right);  // the same spot: put beside A
    REQUIRE(t.scene.find(b)->visible);
    CHECK(glm::distance(t.world(a), t.world(b)) > 1.19f);
    CHECK(t.scene.portalOpenings().size() == 2);
}

TEST_CASE("Portal gun: an end goes under what it was put on and moves with it; the player walks into it",
          "[Game][Portals]") {
    TestScene t;
    room(t);
    t.play();
    t.run(5);
    EntityId a, b;
    t.ends(a, b);
    EntityId front = 0;
    for (const Entity& entity : t.scene.entities()) {
        if (entity.name == "Front wall") {
            front = entity.id;
        }
    }
    t.click(MouseButton::Left);
    CHECK(t.scene.find(a)->parent == front);
    glm::vec3 placed = t.world(a);
    t.scene.find(front)->local.position.x += 2.0f;  // the wall moves: the end with it
    CHECK(glm::distance(t.world(a), placed + glm::vec3(2.0f, 0.0f, 0.0f)) < 1e-4f);
    CHECK(glm::distance(t.facing(a), glm::vec3(0.0f, 0.0f, 1.0f)) < 1e-4f);  // (the wall's scale taken back out)
    t.scene.find(front)->local.position.x -= 2.0f;
    t.turn(-90.0f);
    t.click(MouseButton::Right);
    t.turn(90.0f);  // facing A again
    REQUIRE(t.scene.portalOpenings().size() == 2);
    InputState walk;
    walk.down[Key::W] = true;
    bool through = false;
    for (int i = 0; i < 120 && !through; i++) {
        t.run(1, walk);
        through = t.scene.camera().position.x < -4.0f;  // out of B, on the left wall
    }
    CHECK(through);
}

TEST_CASE("Portal gun: what an end was put on removed, a new pair is made", "[Game][Portals]") {
    TestScene t;
    room(t);
    t.play();
    t.run(5);
    EntityId a, b;
    t.ends(a, b);
    t.click(MouseButton::Left);
    t.scene.remove(t.scene.find(a)->parent);  // (its other end goes too)
    t.run(1);
    EntityId newA, newB;
    t.ends(newA, newB);
    REQUIRE(newA != 0);
    CHECK(newA != a);
    CHECK_FALSE(t.scene.find(newA)->visible);
    t.turn(-90.0f);
    t.click(MouseButton::Left);
    CHECK(t.scene.find(newA)->visible);
}

TEST_CASE("Portal gun: on a floor scaled unevenly, an end turned at an angle keeps its top along the floor's axis "
          "(no shear)", "[Game][Portals]") {
    TestScene t;
    EntityId floor = t.entityWith("Floor", {{"collider", R"({"size": [15, 1, 30]})"}}, {0.0f, -0.5f, 0.0f});
    t.scene.find(floor)->local.scale = glm::vec3(4.0f, 1.0f, 2.0f);  // 60 by 60, scaled unevenly
    t.entityWith("Player", {{"player", ""}, {"portalGun", ""}}, {0.0f, 0.0f, 0.0f});
    t.scene.camera().yaw = 30.0f;  // the shot goes off the floor's axes
    t.scene.camera().pitch = -30.0f;
    t.play();
    t.run(5);
    EntityId a, b;
    t.ends(a, b);
    t.click(MouseButton::Left);
    REQUIRE(t.scene.find(a)->visible);
    CHECK(t.scene.find(a)->parent == floor);
    glm::mat4 world = t.scene.worldTransform(a);
    glm::vec3 x = glm::normalize(glm::vec3(world[0]));
    glm::vec3 y = glm::normalize(glm::vec3(world[1]));
    glm::vec3 z = glm::normalize(glm::vec3(world[2]));
    CHECK(std::abs(glm::dot(x, y)) < 1e-4f);  // right angles: an opening, not a parallelogram
    CHECK(std::abs(glm::dot(y, z)) < 1e-4f);
    CHECK(glm::distance(z, glm::vec3(0.0f, 1.0f, 0.0f)) < 1e-4f);
    CHECK(glm::distance(y, glm::vec3(0.0f, 0.0f, -1.0f)) < 1e-4f);  // the axis nearest the shot's way
    CHECK(glm::length(glm::vec3(world[0])) == Catch::Approx(1.0f).margin(1e-4));  // its own size, not the floor's
}

TEST_CASE("Portal gun: an end isn't put over another pair's end", "[Game][Portals]") {
    TestScene t;
    room(t);
    // A pair of the scene's own with an end on the front wall, right where the gun aims
    EntityId pair = t.scene.addPortalPair();
    t.scene.find(pair)->local = Transform();
    EntityId fixedA = t.scene.find(pair)->children[0];
    EntityId fixedB = t.scene.find(pair)->children[1];
    t.scene.find(fixedA)->local.position = {0.0f, 1.0f, -5.0f + 0.01f};
    t.scene.find(fixedB)->local.position = {20.0f, 1.0f, 10.0f};
    t.play();
    t.run(5);
    EntityId a = 0, b = 0;
    for (const Entity& entity : t.scene.entities()) {
        const PortalComponent* portal = t.scene.portal(entity.id);
        if (portal != nullptr && entity.id != fixedA && entity.id != fixedB) {
            (portal->endA ? a : b) = entity.id;
        }
    }
    REQUIRE(a != 0);
    t.click(MouseButton::Left);
    CHECK_FALSE(t.scene.find(a)->visible);  // it would cover the scene's end
    t.turn(30.0f);  // further right along the wall: clear of it
    t.click(MouseButton::Left);
    CHECK(t.scene.find(a)->visible);
}

