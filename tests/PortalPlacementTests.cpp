//
// Portal placement (portals/PortalPlacement.h): where the portal gun puts an end on the face a shot hit.
//

#include <catch_amalgamated.hpp>
#include <cmath>
#include <vector>
#include "portals/PortalPlacement.h"

using namespace portals;

namespace {

const glm::vec2 doorHalf(0.6f, 1.1f);  // 1.2 wide, 2.2 high

// A wall's front at z = 0 facing +Z, from x = -5 to 5 and y = 0 to 3
PortalSurface wall() {
    PortalSurface surface;
    surface.center = {0.0f, 1.5f, 0.0f};
    surface.normal = {0.0f, 0.0f, 1.0f};
    surface.axes[0] = {1.0f, 0.0f, 0.0f};
    surface.axes[1] = {0.0f, 1.0f, 0.0f};
    surface.half = {5.0f, 1.5f};
    return surface;
}

bool near(const glm::vec3& a, const glm::vec3& b) {
    return glm::distance(a, b) < 1e-3f;
}

}  // namespace

TEST_CASE("Portal placement: on a wall, upright, facing out of it, a little in front, hanging down from the hit",
          "[Portals]") {
    std::optional<glm::mat4> end = placePortalEnd(wall(), {1.0f, 2.5f, 0.0f}, {0.0f, 0.0f, -1.0f}, doorHalf, nullptr);
    REQUIRE(end);
    CHECK(near(glm::vec3((*end)[0]), {1.0f, 0.0f, 0.0f}));
    CHECK(near(glm::vec3((*end)[1]), {0.0f, 1.0f, 0.0f}));
    CHECK(near(glm::vec3((*end)[2]), {0.0f, 0.0f, 1.0f}));
    CHECK(near(glm::vec3((*end)[3]), {1.0f, 2.5f - 1.1f, surfaceOffset}));  // its top edge at the hit
    // A shot at an angle: still upright
    end = placePortalEnd(wall(), {1.0f, 1.5f, 0.0f}, glm::normalize(glm::vec3(1.0f, -0.5f, -1.0f)), doorHalf, nullptr);
    REQUIRE(end);
    CHECK(near(glm::vec3((*end)[1]), {0.0f, 1.0f, 0.0f}));
}

TEST_CASE("Portal placement: near an edge it is slid in to fit; a face too small takes none", "[Portals]") {
    std::optional<glm::mat4> end = placePortalEnd(wall(), {4.9f, 0.3f, 0.0f}, {0.0f, 0.0f, -1.0f}, doorHalf, nullptr);
    REQUIRE(end);
    CHECK(near(glm::vec3((*end)[3]), {5.0f - 0.6f, 1.1f, surfaceOffset}));  // its corner at the wall's
    PortalSurface narrow = wall();
    narrow.half.x = 0.5f;  // 1 wide: the 1.2 m opening doesn't fit
    CHECK_FALSE(placePortalEnd(narrow, {0.0f, 1.5f, 0.0f}, {0.0f, 0.0f, -1.0f}, doorHalf, nullptr));
}

TEST_CASE("Portal placement: on a floor its top points the way the shot went", "[Portals]") {
    PortalSurface floor;
    floor.center = {0.0f, 0.0f, 0.0f};
    floor.normal = {0.0f, 1.0f, 0.0f};
    floor.axes[0] = {1.0f, 0.0f, 0.0f};
    floor.axes[1] = {0.0f, 0.0f, 1.0f};
    floor.half = {10.0f, 10.0f};
    std::optional<glm::mat4> end =
            placePortalEnd(floor, {2.0f, 0.0f, -3.0f}, glm::normalize(glm::vec3(0.0f, -1.0f, -1.0f)), doorHalf, nullptr);
    REQUIRE(end);
    CHECK(near(glm::vec3((*end)[2]), {0.0f, 1.0f, 0.0f}));
    CHECK(near(glm::vec3((*end)[1]), {0.0f, 0.0f, -1.0f}));  // away from the shooter
    CHECK(near(glm::vec3((*end)[0]), {1.0f, 0.0f, 0.0f}));
    CHECK(near(glm::vec3((*end)[3]), {2.0f, surfaceOffset, -3.0f}));
}

TEST_CASE("Portal placement: kept off the other end on the same surface; another surface's doesn't matter",
          "[Portals]") {
    glm::mat4 other(1.0f);
    other[3] = glm::vec4(1.0f, 1.4f, surfaceOffset, 1.0f);  // upright on the same wall
    std::optional<glm::mat4> end = placePortalEnd(wall(), {1.5f, 2.5f, 0.0f}, {0.0f, 0.0f, -1.0f}, doorHalf, &other);
    REQUIRE(end);
    CHECK(near(glm::vec3((*end)[3]), {1.0f + 1.2f, 1.4f, surfaceOffset}));  // pushed right until they just touch
    // No room beside it on a narrow wall: none
    PortalSurface narrow = wall();
    narrow.half.x = 1.0f;
    other[3] = glm::vec4(0.0f, 1.5f, surfaceOffset, 1.0f);
    CHECK_FALSE(placePortalEnd(narrow, {0.1f, 1.5f, 0.0f}, {0.0f, 0.0f, -1.0f}, doorHalf, &other));
    // The other end on a wall behind this one (facing the other way): not in the way
    glm::mat4 behind = glm::mat4(1.0f);
    behind[0] = glm::vec4(-1.0f, 0.0f, 0.0f, 0.0f);
    behind[2] = glm::vec4(0.0f, 0.0f, -1.0f, 0.0f);
    behind[3] = glm::vec4(1.0f, 1.5f, -0.21f, 1.0f);
    end = placePortalEnd(wall(), {1.0f, 2.5f, 0.0f}, {0.0f, 0.0f, -1.0f}, doorHalf, &behind);
    REQUIRE(end);
    CHECK(near(glm::vec3((*end)[3]), {1.0f, 1.4f, surfaceOffset}));
}

TEST_CASE("Portal placement: on a wall it goes down when there is room below, else rests on what is there",
          "[Portals]") {
    PortalSurface tall = wall();  // from y = -1 to 3: the face itself doesn't stop it at the floor
    tall.center.y = 1.0f;
    tall.half.y = 2.0f;
    PlacementOptions options;
    std::vector<glm::vec3> asked;
    options.freeDistance = [&](const glm::vec3& from, const glm::vec3& direction, float maxDistance) {
        asked.push_back(from);
        CHECK(near(direction, {0.0f, -1.0f, 0.0f}));
        return std::min(maxDistance, from.y);  // a floor at y = 0
    };
    glm::vec3 shot(0.0f, 0.0f, -1.0f);
    // At eye height: too little room for its 2.2 m below, so it stands on the floor
    std::optional<glm::mat4> end = placePortalEnd(tall, {1.0f, 1.65f, 0.0f}, shot, doorHalf, nullptr, options);
    REQUIRE(end);
    CHECK(near(glm::vec3((*end)[3]), {1.0f, 1.1f, surfaceOffset}));
    REQUIRE(asked.size() == 3);  // down its middle and near both sides, in front of the wall
    CHECK(asked[0].z > 0.0f);
    CHECK(std::abs(asked[0].x - asked[2].x) > 1.0f);
    // High up: room enough, it hangs from the hit
    end = placePortalEnd(tall, {1.0f, 2.8f, 0.0f}, shot, doorHalf, nullptr, options);
    REQUIRE(end);
    CHECK(near(glm::vec3((*end)[3]), {1.0f, 2.8f - 1.1f, surfaceOffset}));
    // Nothing below: down from the hit, into the face's part below the floor's level
    options.freeDistance = [](const glm::vec3&, const glm::vec3&, float maxDistance) { return maxDistance; };
    end = placePortalEnd(tall, {1.0f, 1.65f, 0.0f}, shot, doorHalf, nullptr, options);
    REQUIRE(end);
    CHECK(near(glm::vec3((*end)[3]), {1.0f, 1.65f - 1.1f, surfaceOffset}));
}

TEST_CASE("Portal placement: on a floor its top can be kept along the face's nearest axis", "[Portals]") {
    PortalSurface floor;
    floor.normal = {0.0f, 1.0f, 0.0f};
    floor.axes[0] = {1.0f, 0.0f, 0.0f};
    floor.axes[1] = {0.0f, 0.0f, 1.0f};
    floor.half = {10.0f, 10.0f};
    glm::vec3 shot = glm::normalize(glm::vec3(0.3f, -1.0f, -1.0f));
    std::optional<glm::mat4> end = placePortalEnd(floor, {0.0f, 0.0f, 0.0f}, shot, doorHalf, nullptr);
    REQUIRE(end);
    CHECK(std::abs(glm::vec3((*end)[1]).x) > 0.2f);  // the way the shot went
    PlacementOptions options;
    options.topAlongFaceAxis = true;
    end = placePortalEnd(floor, {0.0f, 0.0f, 0.0f}, shot, doorHalf, nullptr, options);
    REQUIRE(end);
    CHECK(near(glm::vec3((*end)[1]), {0.0f, 0.0f, -1.0f}));
}

TEST_CASE("Portal placement: another pair's end on the surface isn't covered", "[Portals]") {
    glm::mat4 theirs(1.0f);
    theirs[3] = glm::vec4(0.0f, 1.0f, surfaceOffset, 1.0f);  // upright on the wall, 1 wide (half 0.5, 1)
    PlacementOptions options;
    options.keepClearOf.emplace_back(theirs, glm::vec2(0.5f, 1.0f));
    glm::vec3 shot(0.0f, 0.0f, -1.0f);
    CHECK_FALSE(placePortalEnd(wall(), {0.5f, 2.2f, 0.0f}, shot, doorHalf, nullptr, options));  // over it: none
    std::optional<glm::mat4> end = placePortalEnd(wall(), {2.0f, 2.2f, 0.0f}, shot, doorHalf, nullptr, options);
    REQUIRE(end);  // beside it
    CHECK(near(glm::vec3((*end)[3]), {2.0f, 1.1f, surfaceOffset}));
    theirs[2] = glm::vec4(0.0f, 0.0f, -1.0f, 0.0f);  // on the wall's back instead: not in the way
    theirs[3].z = -0.2f;
    options.keepClearOf[0].first = theirs;
    CHECK(placePortalEnd(wall(), {0.5f, 2.2f, 0.0f}, shot, doorHalf, nullptr, options));
}

