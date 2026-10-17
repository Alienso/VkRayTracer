//
// Physics (physics/): the capsule against box test, and the character walking, falling, sliding and stepping among
// boxes. No scene: the library knows none.
//

#include <catch_amalgamated.hpp>
#include <cmath>
#include <memory>
#include <glm/gtc/quaternion.hpp>
#include "physics/Collision.h"
#include "physics/PhysicsWorld.h"

using namespace physics;

namespace {

constexpr float stepSeconds = 1.0f / 60.0f;
const glm::vec3 gravity(0.0f, -9.81f, 0.0f);

Transform at(const glm::vec3& position, const glm::quat& rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f)) {
    return Transform{position, rotation};
}

glm::quat turned(float degrees, const glm::vec3& axis) {
    return glm::angleAxis(glm::radians(degrees), axis);
}

// A world with a floor whose top is at y = 0, and a character standing on it
struct TestWorld {
    std::unique_ptr<PhysicsWorld> world = makeSimplePhysicsWorld();
    BodyId floor;
    CharacterId player;

    explicit TestWorld(const glm::vec3& feet = glm::vec3(0.0f)) {
        floor = world->addBox({glm::vec3(20.0f, 0.5f, 20.0f)}, at({0.0f, -0.5f, 0.0f}));
        player = world->addCharacter(CharacterSettings(), feet);
    }
    CharacterState run(int steps, const glm::vec3& wanted = glm::vec3(0.0f), float jumpSpeed = 0.0f) {
        CharacterState state;
        for (int i = 0; i < steps; i++) {
            state = world->moveCharacter(player, wanted, i == 0 ? jumpSpeed : 0.0f, gravity, stepSeconds);
        }
        return state;
    }
};

}  // namespace

// ---------------------------------------------------------------------------------------------
// Capsule against box

TEST_CASE("Collision: a capsule just above, beside or off a box's corner touches it as expected", "[Physics]") {
    BoxShape box{glm::vec3(1.0f)};
    float r = 0.5f;
    // Its bottom 0.1 into the top face
    std::optional<Contact> top = capsuleBoxContact({0, 1.4f, 0}, {0, 3, 0}, r, at({0, 0, 0}), box.halfExtents);
    REQUIRE(top);
    CHECK(top->normal.y == Catch::Approx(1.0f));
    CHECK(top->depth == Catch::Approx(0.1f).margin(1e-4));
    CHECK(top->point.y == Catch::Approx(1.0f));
    // Beside the +X face
    std::optional<Contact> side = capsuleBoxContact({1.3f, -2, 0}, {1.3f, 2, 0}, r, at({0, 0, 0}), box.halfExtents);
    REQUIRE(side);
    CHECK(side->normal.x == Catch::Approx(1.0f));
    CHECK(side->depth == Catch::Approx(0.2f).margin(1e-4));
    // Off a vertical edge: pushed away from the edge, diagonally
    std::optional<Contact> edge = capsuleBoxContact({1.2f, -2, 1.2f}, {1.2f, 2, 1.2f}, r, at({0, 0, 0}),
                                                    box.halfExtents);
    REQUIRE(edge);
    CHECK(edge->normal.x == Catch::Approx(std::sqrt(0.5f)).margin(1e-3));
    CHECK(edge->normal.z == Catch::Approx(std::sqrt(0.5f)).margin(1e-3));
    CHECK(edge->depth == Catch::Approx(r - 0.2f * std::sqrt(2.0f)).margin(1e-3));
    // Apart
    CHECK_FALSE(capsuleBoxContact({0, 1.6f, 0}, {0, 3, 0}, r, at({0, 0, 0}), box.halfExtents));
    CHECK_FALSE(capsuleBoxContact({1.6f, -2, 1.6f}, {1.6f, 2, 1.6f}, r, at({0, 0, 0}), box.halfExtents));
}

TEST_CASE("Collision: a turned box pushes along its own face", "[Physics]") {
    glm::quat rotation = turned(45.0f, {0, 1, 0});
    glm::vec3 face = rotation * glm::vec3(1, 0, 0);
    glm::vec3 center = face * 1.3f;  // 0.3 from the face, radius 0.5
    std::optional<Contact> contact = capsuleBoxContact(center - glm::vec3(0, 2, 0), center + glm::vec3(0, 2, 0), 0.5f,
                                                       at({0, 0, 0}, rotation), glm::vec3(1.0f));
    REQUIRE(contact);
    CHECK(glm::dot(contact->normal, face) == Catch::Approx(1.0f).margin(1e-4));
    CHECK(contact->depth == Catch::Approx(0.2f).margin(1e-4));
}

TEST_CASE("Collision: a capsule deep in a box goes out the nearest face", "[Physics]") {
    // Its segment inside, near the top
    std::optional<Contact> contact = capsuleBoxContact({0, 0.5f, 0.2f}, {0, 0.8f, 0.2f}, 0.3f, at({0, 0, 0}),
                                                       glm::vec3(1.0f));
    REQUIRE(contact);
    CHECK(contact->normal.y == Catch::Approx(1.0f));
    CHECK(contact->depth == Catch::Approx(1.0f - 0.5f + 0.3f).margin(1e-4));  // its lowest point past the top
}

// ---------------------------------------------------------------------------------------------
// The character

TEST_CASE("Physics: the character falls onto the floor and stands still on it", "[Physics]") {
    TestWorld t({0.0f, 2.0f, 0.0f});
    CharacterState state = t.run(120);
    CHECK(state.onGround);
    CHECK(state.feet.y == Catch::Approx(0.0f).margin(1e-3));
    float y = state.feet.y;
    for (int i = 0; i < 120; i++) {
        state = t.run(1);
        CHECK(std::abs(state.feet.y - y) < 1e-4f);  // no jitter
        CHECK(state.onGround);
    }
    CHECK(std::abs(state.feet.x) < 1e-5f);
    CHECK(std::abs(state.feet.z) < 1e-5f);
}

TEST_CASE("Physics: a wall stops the character, and it slides along a wall it walks into at an angle", "[Physics]") {
    TestWorld t;
    t.world->addBox({glm::vec3(0.25f, 2.0f, 10.0f)}, at({2.25f, 2.0f, 0.0f}));  // its face at x = 2
    CharacterState state = t.run(120, {4.0f, 0.0f, 0.0f});
    CHECK(state.feet.x == Catch::Approx(2.0f - 0.3f).margin(2e-3));
    CHECK(state.onGround);
    // Along it, at 45 degrees into it: the z part goes on
    float z = state.feet.z;
    state = t.run(60, {2.0f, 0.0f, 2.0f});
    CHECK(state.feet.x == Catch::Approx(2.0f - 0.3f).margin(2e-3));
    CHECK(state.feet.z - z == Catch::Approx(2.0f).margin(0.05f));
}

TEST_CASE("Physics: a gentle ramp is walked up, a steep one isn't", "[Physics]") {
    // A long box turned around Z: its top face rises along +x from about x = 1
    auto ramp = [](TestWorld& t, float degrees) {
        glm::quat rotation = turned(degrees, {0, 0, 1});
        glm::vec3 halfExtents(5.0f, 0.5f, 3.0f);
        glm::vec3 lowEnd = rotation * glm::vec3(-halfExtents.x, halfExtents.y, 0.0f);  // the top face's low edge
        t.world->addBox({halfExtents}, at(glm::vec3(1.0f, 0.0f, 0.0f) - lowEnd, rotation));
    };
    SECTION("30 degrees") {
        TestWorld t;
        ramp(t, 30.0f);
        CharacterState state = t.run(90, {3.0f, 0.0f, 0.0f});
        CHECK(state.feet.y > 1.5f);
        CHECK(state.onGround);
    }
    SECTION("60 degrees") {
        TestWorld t;
        ramp(t, 60.0f);
        CharacterState state = t.run(90, {3.0f, 0.0f, 0.0f});
        CHECK(state.feet.y < 0.35f);  // at most a step up its foot
        CHECK(state.feet.x < 1.5f);
    }
}

TEST_CASE("Physics: a low step is climbed, a high one blocks", "[Physics]") {
    SECTION("0.25 m") {
        TestWorld t;
        t.world->addBox({glm::vec3(1.0f, 0.125f, 3.0f)}, at({2.0f, 0.125f, 0.0f}));  // from x = 1 to 3
        CharacterState state = t.run(60, {3.0f, 0.0f, 0.0f});
        CHECK(state.feet.x > 2.0f);
        CHECK(state.feet.y == Catch::Approx(0.25f).margin(2e-3));
        CHECK(state.onGround);
    }
    SECTION("0.5 m") {
        TestWorld t;
        t.world->addBox({glm::vec3(1.0f, 0.25f, 3.0f)}, at({2.0f, 0.25f, 0.0f}));
        CharacterState state = t.run(60, {3.0f, 0.0f, 0.0f});
        CHECK(state.feet.x == Catch::Approx(1.0f - 0.3f).margin(2e-3));
        CHECK(state.feet.y == Catch::Approx(0.0f).margin(2e-3));
    }
}

TEST_CASE("Physics: walking off a low ledge keeps the character on the ground", "[Physics]") {
    TestWorld t({0.0f, 0.2f, 0.0f});
    t.world->addBox({glm::vec3(1.0f, 0.1f, 3.0f)}, at({0.0f, 0.1f, 0.0f}));  // 0.2 high, to x = 1
    t.run(30);
    CharacterState state = t.run(60, {2.0f, 0.0f, 0.0f});  // off its edge onto the floor
    CHECK(state.feet.x > 1.2f);
    CHECK(state.feet.y == Catch::Approx(0.0f).margin(2e-3));
    CHECK(state.onGround);
}

TEST_CASE("Physics: a thin wall isn't passed even at high speed", "[Physics]") {
    TestWorld t;
    t.world->addBox({glm::vec3(0.0f, 2.0f, 5.0f)}, at({1.0f, 2.0f, 0.0f}));  // no thickness: grown
    CharacterState state = t.run(60, {40.0f, 0.0f, 0.0f});
    CHECK(state.feet.x < 1.0f);
}

TEST_CASE("Physics: a box moved into the character pushes it out", "[Physics]") {
    TestWorld t;
    t.run(10);
    BodyId crate = t.world->addBox({glm::vec3(0.5f)}, at({2.0f, 0.5f, 0.0f}));
    t.world->setTransform(crate, at({0.6f, 0.5f, 0.0f}));  // its face at x = 0.1: into the capsule
    CharacterState state = t.run(1);
    CHECK(state.feet.x == Catch::Approx(0.1f - 0.3f).margin(2e-3));
}

TEST_CASE("Physics: a disabled box is passed through; enabled again, it blocks", "[Physics]") {
    TestWorld t({0.0f, 0.5f, 0.0f});
    t.world->setEnabled(t.floor, false);
    CharacterState state = t.run(30);
    CHECK(state.feet.y < 0.0f);
    CHECK_FALSE(state.onGround);
    TestWorld u({0.0f, 0.5f, 0.0f});
    u.world->setEnabled(u.floor, false);
    u.world->setEnabled(u.floor, true);
    CHECK(u.run(60).feet.y == Catch::Approx(0.0f).margin(1e-3));
}

TEST_CASE("Physics: a jump rises as high as its speed and gravity say, and lands", "[Physics]") {
    TestWorld t;
    t.run(10);
    float highest = 0.0f;
    CharacterState state = t.run(1, glm::vec3(0.0f), 5.0f);
    CHECK_FALSE(state.onGround);
    for (int i = 0; i < 120; i++) {
        highest = std::max(highest, state.feet.y);
        state = t.run(1);
    }
    CHECK(highest == Catch::Approx(25.0f / (2.0f * 9.81f)).epsilon(0.05));
    CHECK(state.onGround);
    CHECK(state.feet.y == Catch::Approx(0.0f).margin(1e-3));
    // No jump in the air
    TestWorld u({0.0f, 3.0f, 0.0f});
    CharacterState falling = u.run(1, glm::vec3(0.0f), 5.0f);
    CHECK(falling.velocity.y < 0.0f);
}

TEST_CASE("Physics: a ceiling stops a jump", "[Physics]") {
    TestWorld t;
    t.world->addBox({glm::vec3(2.0f, 0.5f, 2.0f)}, at({0.0f, 2.5f, 0.0f}));  // its bottom at 2: 0.2 above the head
    t.run(10);
    float highest = 0.0f;
    CharacterState state = t.run(1, glm::vec3(0.0f), 5.0f);
    for (int i = 0; i < 120; i++) {
        highest = std::max(highest, state.feet.y);
        state = t.run(1);
    }
    CHECK(highest <= 0.2f + 1e-3f);
    CHECK(state.onGround);
}

TEST_CASE("Physics: in the air the character steers only slowly", "[Physics]") {
    TestWorld t({0.0f, 10.0f, 0.0f});
    CharacterState state = t.run(6, {4.0f, 0.0f, 0.0f});  // 0.1 s at 10 m/s^2: about 1 m/s
    CHECK(state.velocity.x == Catch::Approx(1.0f).margin(0.05f));
}

TEST_CASE("Physics: teleport puts the character there, going on at the given velocity", "[Physics]") {
    TestWorld t;
    t.run(10);
    t.world->teleport(t.player, {5.0f, 3.0f, 0.0f}, {0.0f, 0.0f, 3.0f});
    CharacterState state = t.world->character(t.player);
    CHECK(state.feet == glm::vec3(5.0f, 3.0f, 0.0f));
    CHECK_FALSE(state.onGround);
    state = t.run(1, {0.0f, 0.0f, 3.0f});
    CHECK(state.feet.z == Catch::Approx(3.0f * stepSeconds).margin(1e-4));
}

TEST_CASE("Physics: bodies and characters come and go; ids are used again", "[Physics]") {
    std::unique_ptr<PhysicsWorld> world = makeSimplePhysicsWorld();
    BodyId a = world->addBox({glm::vec3(1.0f)}, at({0, 0, 0}));
    BodyId b = world->addBox({glm::vec3(1.0f)}, at({5, 0, 0}));
    CHECK(world->bodyCount() == 2);
    CapsuleShape capsule{0.3f, 0.5f};
    CHECK(world->overlapsCapsule(capsule, at({0, 1.5f, 0})));
    CHECK_FALSE(world->overlapsCapsule(capsule, at({0, 2.0f, 0})));
    world->remove(a);
    CHECK(world->bodyCount() == 1);
    CHECK_FALSE(world->overlapsCapsule(capsule, at({0, 1.5f, 0})));
    world->remove(a);  // (twice: nothing)
    CHECK(world->bodyCount() == 1);
    BodyId c = world->addBox({glm::vec3(1.0f)}, at({0, 0, 0}));
    CHECK(c.value == a.value);
    CharacterId player = world->addCharacter(CharacterSettings(), glm::vec3(0.0f, 5.0f, 0.0f));
    world->clear();
    CHECK(world->bodyCount() == 0);
    CHECK(world->character(player).feet == glm::vec3(0.0f));  // gone: the default state
    (void)b;
}
