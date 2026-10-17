//
// Physics (physics/): the capsule against box and box against box tests, moves that can be blocked, and the
// character walking, falling, sliding and stepping among boxes. No scene: the library knows none.
//

#include <catch_amalgamated.hpp>
#include <cmath>
#include <memory>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include "physics/Collision.h"
#include "physics/PhysicsWorld.h"
#include "physics/SimplePhysics.h"
#include "portals/PortalPieces.h"

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

// A pair of openings 1 wide and 2 high on the floor: A at z = -3 (in a wall's face, see walledPortal), B at x = 10,
// z = 0; both facing +Z
std::vector<portals::PortalOpening> doorPair() {
    std::vector<portals::PortalOpening> openings;
    portals::addPortalPair(openings, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.0f, -3.0f)),
                           glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 1.0f, 0.0f)), glm::vec2(0.5f, 1.0f),
                           false);
    return openings;
}

// The world with a wall whose face is at z = -3 (no gap in it) and doorPair's A in it
void walledPortal(TestWorld& t) {
    t.world->addBox({glm::vec3(6.0f, 2.0f, 0.5f)}, at({0.0f, 2.0f, -3.5f}));
    t.world->setPortals(doorPair(), 8);
}

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
// Box against box

TEST_CASE("Collision: two boxes face to face touch over the faces' overlap", "[Physics]") {
    std::vector<glm::vec3> points;
    std::optional<Contact> contact = boxBoxContact(at({1.8f, 0, 0}), glm::vec3(1.0f), at({0, 0, 0}), glm::vec3(1.0f),
                                                   &points);
    REQUIRE(contact);
    CHECK(contact->normal.x == Catch::Approx(1.0f));
    CHECK(contact->depth == Catch::Approx(0.2f).margin(1e-5));
    CHECK(points.size() == 4);  // the face's corners
    for (const glm::vec3& point : points) {
        CHECK(point.x == Catch::Approx(1.0f));  // on the second box's face
    }
    // The other way round: pushed the other way
    std::optional<Contact> back = boxBoxContact(at({0, 0, 0}), glm::vec3(1.0f), at({1.8f, 0, 0}), glm::vec3(1.0f));
    REQUIRE(back);
    CHECK(back->normal.x == Catch::Approx(-1.0f));
    CHECK(back->depth == Catch::Approx(0.2f).margin(1e-5));
    // Apart
    CHECK_FALSE(boxBoxContact(at({2.1f, 0, 0}), glm::vec3(1.0f), at({0, 0, 0}), glm::vec3(1.0f)));
}

TEST_CASE("Collision: two crossed edges touch at one point", "[Physics]") {
    // A turned so an edge along Z points down, B so an edge along X points up: the edges cross
    float r2 = std::sqrt(2.0f);
    glm::quat aTurn = turned(45.0f, {0, 0, 1});
    glm::quat bTurn = turned(45.0f, {1, 0, 0});
    std::vector<glm::vec3> points;
    std::optional<Contact> contact = boxBoxContact(at({0, 2.0f * r2 - 0.1f, 0}, aTurn), glm::vec3(1.0f),
                                                   at({0, 0, 0}, bTurn), glm::vec3(1.0f), &points);
    REQUIRE(contact);
    CHECK(contact->normal.y == Catch::Approx(1.0f).margin(1e-4));
    CHECK(contact->depth == Catch::Approx(0.1f).margin(1e-4));
    REQUIRE(points.size() == 1);
    CHECK(glm::length(contact->point - glm::vec3(0, r2, 0)) < 1e-3f);  // B's top edge, under A's
    CHECK_FALSE(boxBoxContact(at({0, 2.0f * r2 + 0.05f, 0}, aTurn), glm::vec3(1.0f), at({0, 0, 0}, bTurn),
                              glm::vec3(1.0f)));
}

TEST_CASE("Collision: a box's corner into a face touches at the corner", "[Physics]") {
    // B turned so a corner points straight up, A flat on it
    glm::vec3 corner = glm::normalize(glm::vec3(1.0f));
    glm::quat cornerUp = glm::angleAxis(std::acos(corner.y), glm::normalize(glm::cross(corner, glm::vec3(0, 1, 0))));
    float r3 = std::sqrt(3.0f);
    std::vector<glm::vec3> points;
    std::optional<Contact> contact = boxBoxContact(at({0.2f, r3 + 1.0f - 0.05f, 0.1f}), glm::vec3(1.0f),
                                                   at({0, 0, 0}, cornerUp), glm::vec3(1.0f), &points);
    REQUIRE(contact);
    CHECK(contact->normal.y == Catch::Approx(1.0f).margin(1e-4));
    CHECK(contact->depth == Catch::Approx(0.05f).margin(1e-4));
    REQUIRE(points.size() == 1);
    CHECK(glm::length(contact->point - glm::vec3(0, r3, 0)) < 1e-3f);
}

TEST_CASE("Collision: a turned box against a box: its edge is what reaches", "[Physics]") {
    glm::quat turn = turned(45.0f, {0, 1, 0});
    float r2 = std::sqrt(2.0f);
    std::optional<Contact> contact = boxBoxContact(at({1.0f + r2 - 0.1f, 0, 0}, turn), glm::vec3(1.0f),
                                                   at({0, 0, 0}), glm::vec3(1.0f));
    REQUIRE(contact);
    CHECK(contact->normal.x == Catch::Approx(1.0f).margin(1e-4));
    CHECK(contact->depth == Catch::Approx(0.1f).margin(1e-4));
    CHECK(contact->point.x == Catch::Approx(1.0f).margin(1e-4));
    CHECK_FALSE(boxBoxContact(at({1.0f + r2 + 0.05f, 0, 0}, turn), glm::vec3(1.0f), at({0, 0, 0}), glm::vec3(1.0f)));
}

// ---------------------------------------------------------------------------------------------
// Moves that can be blocked

TEST_CASE("Physics: a box moved into a wall stops at it; moved at an angle, it slides along it", "[Physics]") {
    std::unique_ptr<PhysicsWorld> world = makeSimplePhysicsWorld();
    world->addBox({glm::vec3(0.5f, 2.0f, 2.0f)}, at({3.0f, 0, 0}));  // its face at x = 2.5
    BodyId box = world->addBox({glm::vec3(0.5f)}, at({0, 0, 0}));
    GroupMove straight = world->moveBodies({box}, at({0, 0, 0}), at({5.0f, 0, 0}));
    CHECK(straight.blocked);
    CHECK(straight.reached.position.x == Catch::Approx(2.0f).margin(5e-3));
    CHECK(straight.reached.position.x <= 2.0f + SimplePhysicsWorld::contactSkin);
    // From there, at an angle: it keeps the part along the wall
    GroupMove slid = world->moveBodies({box}, straight.reached, at({5.0f, 0, 1.0f}));
    CHECK(slid.blocked);
    CHECK(slid.reached.position.x == Catch::Approx(2.0f).margin(5e-3));
    CHECK(slid.reached.position.z == Catch::Approx(1.0f).margin(1e-3));
    // Free: all the way
    GroupMove back = world->moveBodies({box}, slid.reached, at({-3.0f, 0, 0}));
    CHECK_FALSE(back.blocked);
    CHECK(back.reached.position == glm::vec3(-3.0f, 0, 0));
}

TEST_CASE("Physics: a fast box doesn't pass through a thin wall", "[Physics]") {
    std::unique_ptr<PhysicsWorld> world = makeSimplePhysicsWorld();
    world->addBox({glm::vec3(0.05f, 2.0f, 2.0f)}, at({3.0f, 0, 0}));
    BodyId box = world->addBox({glm::vec3(0.2f)}, at({0, 0, 0}));
    GroupMove move = world->moveBodies({box}, at({0, 0, 0}), at({10.0f, 0, 0}));  // past it in one move
    CHECK(move.blocked);
    CHECK(move.reached.position.x == Catch::Approx(2.75f).margin(5e-3));
}

TEST_CASE("Physics: a turning door stops at a crate", "[Physics]") {
    std::unique_ptr<PhysicsWorld> world = makeSimplePhysicsWorld();
    // The hinge at the origin, the panel along +X; turned 90 degrees about Y it would end along -Z
    BodyId panel = world->addBox({glm::vec3(0.75f, 1.0f, 0.05f)}, at({0.75f, 0, 0}));
    BoxShape crate{glm::vec3(0.3f)};
    Transform crateAt = at({1.0f, 0, -1.0f});
    world->addBox(crate, crateAt);
    GroupMove move = world->moveBodies({panel}, at({0, 0, 0}), at({0, 0, 0}, turned(90.0f, {0, 1, 0})));
    CHECK(move.blocked);
    CHECK(move.reached.position == glm::vec3(0.0f));  // (a turn isn't slid)
    float degrees = glm::degrees(2.0f * std::acos(std::min(1.0f, std::abs(move.reached.rotation.w))));
    CHECK(degrees > 10.0f);
    CHECK(degrees < 80.0f);
    Transform panelAt{move.reached.rotation * glm::vec3(0.75f, 0, 0), move.reached.rotation};
    std::optional<Contact> contact = boxBoxContact(panelAt, glm::vec3(0.75f, 1.0f, 0.05f), crateAt, crate.halfExtents);
    CHECK((!contact || contact->depth <= 2e-3f));  // stopped at it, not in it
}

TEST_CASE("Physics: a group (a platform and the box on it) stops together; the platform alone is stopped by the box",
          "[Physics]") {
    std::unique_ptr<PhysicsWorld> world = makeSimplePhysicsWorld();
    world->addBox({glm::vec3(3.0f, 0.1f, 3.0f)}, at({0, 2.1f, 0}));  // a ceiling at y = 2
    BodyId platform = world->addBox({glm::vec3(1.0f, 0.1f, 1.0f)}, at({0, 0, 0}));
    BodyId box = world->addBox({glm::vec3(0.3f)}, at({0, 0.4f, 0}));  // resting on it, its top at 0.7
    GroupMove alone = world->moveBodies({platform}, at({0, 0, 0}), at({0, 3.0f, 0}));
    CHECK(alone.blocked);
    CHECK(alone.reached.position.y < 0.01f);  // the box on it is in the way
    GroupMove together = world->moveBodies({platform, box}, alone.reached, at({0, 3.0f, 0}));
    CHECK(together.blocked);
    CHECK(together.reached.position.y == Catch::Approx(1.3f).margin(5e-3));  // the box's top at the ceiling
}

TEST_CASE("Physics: a moved box pushes the character; squeezed against a wall, the box is blocked", "[Physics]") {
    TestWorld t;
    t.run(10);
    BodyId box = t.world->addBox({glm::vec3(0.5f)}, at({2.0f, 0.5f, 0}));
    GroupMove push = t.world->moveBodies({box}, at({2.0f, 0.5f, 0}), at({0.6f, 0.5f, 0}));  // its face at 0.1
    CHECK_FALSE(push.blocked);
    CharacterState state = t.run(1);
    CHECK(state.feet.x == Catch::Approx(0.1f - 0.3f).margin(2e-3));
    t.world->addBox({glm::vec3(0.5f, 1.0f, 1.0f)}, at({-1.5f, 1.0f, 0}));  // a wall, its face at x = -1
    GroupMove squeeze = t.world->moveBodies({box}, push.reached, at({-0.5f, 0.5f, 0}));
    CHECK(squeeze.blocked);
    CHECK(squeeze.reached.position.x == Catch::Approx(-1.0f + 0.6f + 0.5f).margin(0.02));  // room for the capsule
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

// ---------------------------------------------------------------------------------------------
// Through portals

TEST_CASE("Physics: the character walks into a portal set in a wall and comes out of the other end, on the ground "
          "all the way", "[Physics][Portals]") {
    TestWorld t;
    walledPortal(t);
    glm::vec3 wanted(0.0f, 0.0f, -4.0f);
    int crossed = 0;
    bool grounded = true;
    float lowest = 0.0f;
    float highest = 0.0f;
    for (int i = 0; i < 60; i++) {
        CharacterState state = t.run(1, wanted);
        if (state.crossedPortal) {
            crossed++;
            wanted = glm::mat3(state.portalCrossing) * wanted;  // (as the player's view turns)
        }
        grounded = grounded && state.onGround;
        lowest = std::min(lowest, state.feet.y);
        highest = std::max(highest, state.feet.y);
    }
    CharacterState state = t.world->character(t.player);
    CHECK(crossed == 1);
    CHECK(grounded);
    CHECK(lowest > -1e-3f);
    CHECK(highest < 1e-3f);
    CHECK(state.feet.x == Catch::Approx(10.0f).margin(1e-3));
    CHECK(state.feet.z > 0.8f);  // walking on, out of B's front
}

TEST_CASE("Physics: the wall beside a portal still blocks", "[Physics][Portals]") {
    TestWorld t({2.0f, 0.0f, 0.0f});
    walledPortal(t);
    CharacterState state = t.run(60, {0.0f, 0.0f, -4.0f});
    CHECK(state.feet.z == Catch::Approx(-2.7f).margin(1e-3));
    CHECK(state.feet.x == Catch::Approx(2.0f).margin(1e-3));
    // Half beside the opening (more of it than fits): blocked too
    t.world->teleport(t.player, {0.4f, 0.0f, 0.0f}, glm::vec3(0.0f));
    state = t.run(60, {0.0f, 0.0f, -4.0f});
    CHECK(state.feet.z == Catch::Approx(-2.7f).margin(1e-3));
}

TEST_CASE("Physics: half through a portal, what is beyond the exit stops the character", "[Physics][Portals]") {
    TestWorld t;
    walledPortal(t);
    t.world->addBox({glm::vec3(1.0f, 1.0f, 0.25f)}, at({10.0f, 1.0f, 0.4f}));  // its face 0.15 in front of B
    bool crossed = false;
    CharacterState state;
    for (int i = 0; i < 60; i++) {
        state = t.run(1, {0.0f, 0.0f, -4.0f});
        crossed = crossed || state.crossedPortal;
    }
    CHECK_FALSE(crossed);
    CHECK(state.feet.z == Catch::Approx(-3.0f + 0.3f - 0.15f).margin(2e-3));  // its front 0.15 into A
    CHECK(state.feet.x == Catch::Approx(0.0f).margin(1e-3));
}

TEST_CASE("Physics: a box half through a portal stops the character at the other end too", "[Physics][Portals]") {
    TestWorld t({10.0f, 0.0f, 3.0f});
    t.world->setPortals(doorPair(), 8);
    t.world->addBox({glm::vec3(0.25f)}, at({0.0f, 1.0f, -2.9f}));  // 0.15 of it beyond A: out of B's front
    CharacterState state = t.run(90, {0.0f, 0.0f, -4.0f});
    CHECK(state.feet.z == Catch::Approx(0.15f + 0.3f).margin(2e-3));
    CHECK_FALSE(state.crossedPortal);
}

TEST_CASE("Physics: portals far away change nothing", "[Physics][Portals]") {
    TestWorld plain;
    TestWorld far;
    std::vector<portals::PortalOpening> openings;
    portals::addPortalPair(openings, glm::translate(glm::mat4(1.0f), glm::vec3(100.0f, 1.0f, 0.0f)),
                           glm::translate(glm::mat4(1.0f), glm::vec3(-100.0f, 1.0f, 0.0f)), glm::vec2(0.5f, 1.0f),
                           false);
    far.world->setPortals(openings, 8);
    for (TestWorld* t : {&plain, &far}) {
        t->world->addBox({glm::vec3(1.0f, 0.15f, 1.0f)}, at({0.0f, 0.15f, -2.0f}));  // a step
        t->world->addBox({glm::vec3(3.0f, 2.0f, 0.2f)}, at({0.0f, 2.0f, -5.0f}));   // a wall
    }
    for (int i = 0; i < 120; i++) {
        glm::vec3 wanted(std::sin(0.1f * static_cast<float>(i)), 0.0f, -4.0f);
        float jump = i % 40 == 0 ? 5.0f : 0.0f;
        CharacterState a = plain.world->moveCharacter(plain.player, wanted, jump, gravity, stepSeconds);
        CharacterState b = far.world->moveCharacter(far.player, wanted, jump, gravity, stepSeconds);
        REQUIRE(a.feet == b.feet);
        REQUIRE(a.velocity == b.velocity);
        REQUIRE(a.onGround == b.onGround);
    }
}

TEST_CASE("Physics: a solid box moved into a portal set in a wall goes in; what is beyond the exit stops it",
          "[Physics][Portals]") {
    std::unique_ptr<PhysicsWorld> world = makeSimplePhysicsWorld();
    world->addBox({glm::vec3(6.0f, 2.0f, 0.5f)}, at({0.0f, 2.0f, -3.5f}));  // the wall, its face at z = -3
    BodyId box = world->addBox({glm::vec3(0.25f)}, at({0.0f, 1.0f, -2.0f}));
    GroupMove move = world->moveBodies({box}, at({0.0f, 1.0f, -2.0f}), at({0.0f, 1.0f, -3.2f}));
    CHECK(move.blocked);  // no portal: the wall
    CHECK(move.reached.position.z == Catch::Approx(-2.75f).margin(2e-3));

    world->setPortals(doorPair(), 8);
    world->setTransform(box, at({0.0f, 1.0f, -2.0f}));
    move = world->moveBodies({box}, at({0.0f, 1.0f, -2.0f}), at({0.0f, 1.0f, -3.2f}));
    CHECK_FALSE(move.blocked);  // into the opening (its center past the plane: the game carries it through next)

    world->addBox({glm::vec3(1.0f, 1.0f, 0.25f)}, at({10.0f, 1.0f, 0.4f}));  // its face 0.15 in front of B
    world->setTransform(box, at({0.0f, 1.0f, -2.0f}));
    move = world->moveBodies({box}, at({0.0f, 1.0f, -2.0f}), at({0.0f, 1.0f, -3.2f}));
    CHECK(move.blocked);
    CHECK(move.reached.position.z == Catch::Approx(-3.0f + 0.25f - 0.15f).margin(2e-3));  // 0.15 of it out of B
}

TEST_CASE("Physics: a solid box half through a portal is blocked at both ends", "[Physics][Portals]") {
    for (bool atExit : {false, true}) {
        std::unique_ptr<PhysicsWorld> world = makeSimplePhysicsWorld();
        world->setPortals(doorPair(), 8);
        BodyId box = world->addBox({glm::vec3(0.25f)}, at({0.0f, 1.0f, -2.9f}));  // 0.15 of it beyond A: at B
        if (atExit) {
            // A post in front of B where its part there goes when it moves +x (out of B it moves -x)
            world->addBox({glm::vec3(0.1f, 1.0f, 0.25f)}, at({9.5f, 1.0f, 0.25f}));
        } else {
            world->addBox({glm::vec3(0.1f, 1.0f, 0.25f)}, at({0.5f, 1.0f, -2.75f}));
        }
        // (Within the opening all the way: past it, nothing of it is at B)
        GroupMove move = world->moveBodies({box}, at({0.0f, 1.0f, -2.9f}), at({0.2f, 1.0f, -2.9f}));
        CHECK(move.blocked);
        CHECK(move.reached.position.x == Catch::Approx(0.15f).margin(2e-3));
        CHECK(move.reached.position.z == Catch::Approx(-2.9f).margin(1e-4));  // (nothing to slide along)
    }
}

TEST_CASE("Physics: a solid stick going into a portal is stopped by its own part coming out of the other end",
          "[Physics][Portals]") {
    // A at z = 0 facing +Z; B above the stick's way facing down: what goes into A comes down out of B onto the stick
    std::vector<portals::PortalOpening> openings;
    portals::addPortalPair(openings, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.0f, 0.0f)),
                           glm::rotate(glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.6f, 1.0f)),
                                       glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f)),
                           glm::vec2(0.5f, 1.0f), false);
    for (bool portals : {false, true}) {
        std::unique_ptr<PhysicsWorld> world = makeSimplePhysicsWorld();
        if (portals) {
            world->setPortals(openings, 8);
        }
        BodyId stick = world->addBox({glm::vec3(0.1f, 0.1f, 1.0f)}, at({0.0f, 1.0f, 1.5f}));
        GroupMove move = world->moveBodies({stick}, at({0.0f, 1.0f, 1.5f}), at({0.0f, 1.0f, 0.0f}));
        if (portals) {
            CHECK(move.blocked);  // 0.5 in: its part out of B reaches its top
            CHECK(move.reached.position.z == Catch::Approx(0.5f).margin(5e-3));
        } else {
            CHECK_FALSE(move.blocked);
        }
    }
}

TEST_CASE("Physics: a solid move far from portals is the same as without them", "[Physics][Portals]") {
    std::vector<portals::PortalOpening> far;
    portals::addPortalPair(far, glm::translate(glm::mat4(1.0f), glm::vec3(100.0f, 1.0f, 0.0f)),
                           glm::translate(glm::mat4(1.0f), glm::vec3(-100.0f, 1.0f, 0.0f)), glm::vec2(0.5f, 1.0f),
                           false);
    GroupMove moves[2];
    for (int i = 0; i < 2; i++) {
        std::unique_ptr<PhysicsWorld> world = makeSimplePhysicsWorld();
        if (i == 1) {
            world->setPortals(far, 8);
        }
        world->addBox({glm::vec3(0.2f, 2.0f, 3.0f)}, at({2.0f, 1.0f, 0.0f}));
        BodyId box = world->addBox({glm::vec3(0.5f)}, at({0.0f, 1.0f, 0.0f}, turned(20.0f, {0, 1, 0})));
        moves[i] = world->moveBodies({box}, at({0.0f, 1.0f, 0.0f}, turned(20.0f, {0, 1, 0})),
                                     at({3.0f, 1.0f, 1.0f}, turned(50.0f, {0, 1, 0})));
    }
    CHECK(moves[0].blocked);
    CHECK(moves[0].blocked == moves[1].blocked);
    CHECK(moves[0].reached.position == moves[1].reached.position);
    CHECK(moves[0].reached.rotation == moves[1].reached.rotation);
}
