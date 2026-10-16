//
// The math the scene relies on: transforms, Euler angles, the camera's directions and matrices, portals.
//

#include <catch_amalgamated.hpp>
#include <random>
#include <glm/gtc/matrix_transform.hpp>
#include "TestHelpers.h"
#include "scene/Camera.h"
#include "scene/Portal.h"
#include "scene/Scene.h"

TEST_CASE("Transform: scaled, then rotated, then moved", "[Transform]") {
    Transform t;
    t.position = {0, 0, 5};
    t.rotation = eulerToQuat({0, 90, 0});  // +X turns to -Z
    t.scale = {2, 1, 1};
    glm::vec3 p = glm::vec3(t.matrix() * glm::vec4(1, 0, 0, 1));
    CHECK(nearlyEqual(p, {0, 0, 3}));
}

TEST_CASE("Euler angles: yaw, then pitch, then roll; converting back gives the same rotation", "[Transform]") {
    CHECK(nearlyEqual(glm::vec3(glm::mat4_cast(eulerToQuat({90, 0, 0})) * glm::vec4(0, 0, -1, 0)), {0, 1, 0}));
    std::mt19937 random(5);
    std::uniform_real_distribution<float> angle(-180.0f, 180.0f);
    for (int i = 0; i < 1000; i++) {
        glm::vec3 degrees(angle(random), angle(random), angle(random));
        glm::quat q = eulerToQuat(degrees);
        glm::quat back = eulerToQuat(quatToEuler(q));
        INFO(degrees.x << " " << degrees.y << " " << degrees.z);
        CHECK(nearlyEqual(glm::mat4_cast(back), glm::mat4_cast(q), 1e-3f));
    }
    // Inside the ranges the editor shows (pitch within +-90), the angles come back too
    glm::vec3 degrees(30, -45, 60);
    CHECK(nearlyEqual(quatToEuler(eulerToQuat(degrees)), degrees, 1e-3f));
}

TEST_CASE("Camera: yaw 0 looks down -Z, positive yaw turns right, positive pitch looks up", "[Camera]") {
    Camera camera;
    CHECK(nearlyEqual(camera.forward(), {0, 0, -1}));
    CHECK(nearlyEqual(camera.right(), {1, 0, 0}));
    CHECK(nearlyEqual(camera.up(), {0, 1, 0}));
    camera.yaw = 90.0f;
    CHECK(nearlyEqual(camera.forward(), {1, 0, 0}));
    camera.yaw = 0.0f;
    camera.pitch = 30.0f;
    CHECK(camera.forward().y == Catch::Approx(0.5f));
}

TEST_CASE("Camera: lookAt faces the target", "[Camera]") {
    Camera camera;
    camera.position = {1, 2, 3};
    glm::vec3 target = {-4, 0.5f, 7};
    camera.lookAt(target);
    CHECK(nearlyEqual(camera.forward(), glm::normalize(target - camera.position)));
}

// Straight up or down the camera has no right direction: pitch is kept within +-89 degrees (Camera.h), as the
// camera controls keep it
TEST_CASE("Camera: lookAt straight up keeps the camera usable", "[Camera]") {
    Camera camera;
    camera.lookAt({0, 10, 0});
    CHECK(camera.pitch <= 89.0f);
    CHECK(std::isfinite(camera.right().x));
    CHECK(allFinite(camera.viewMatrix()));
}

TEST_CASE("Camera: the view matrix puts the camera at the origin, looking down -Z", "[Camera]") {
    Camera camera;
    camera.position = {3, 1, -2};
    camera.yaw = 40.0f;
    camera.pitch = -20.0f;
    glm::mat4 view = camera.viewMatrix();
    CHECK(nearlyEqual(glm::vec3(view * glm::vec4(camera.position, 1)), {0, 0, 0}));
    CHECK(nearlyEqual(glm::vec3(view * glm::vec4(camera.forward(), 0)), {0, 0, -1}));
    CHECK(nearlyEqual(glm::vec3(view * glm::vec4(camera.up(), 0)), {0, 1, 0}));
}

TEST_CASE("Camera: the projection is Vulkan's: y down, depth 0 (near) to 1 (far)", "[Camera]") {
    Camera camera;
    glm::mat4 projection = camera.projectionMatrix(16.0f / 9.0f, 0.1f, 100.0f);
    auto project = [&](glm::vec3 viewPoint) {
        glm::vec4 clip = projection * glm::vec4(viewPoint, 1);
        return glm::vec3(clip) / clip.w;
    };
    CHECK(project({0, 0, -0.1f}).z == Catch::Approx(0.0f).margin(1e-5));
    CHECK(project({0, 0, -100.0f}).z == Catch::Approx(1.0f).margin(1e-5));
    CHECK(project({0, 1, -5}).y < 0.0f);  // above the middle: up in the image, negative y
    // The top edge of the vertical field of view lands on y = -1
    float half = glm::radians(camera.fov * 0.5f);
    CHECK(project({0, std::tan(half) * 5.0f, -5}).y == Catch::Approx(-1.0f));
}

TEST_CASE("Portals: going through one end comes out of the other, turned half around", "[Portal]") {
    glm::mat4 a = glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, 0));
    glm::mat4 b = glm::rotate(glm::translate(glm::mat4(1.0f), glm::vec3(10, 0, 0)), glm::radians(90.0f), glm::vec3(0, 1, 0));
    glm::mat4 through = portalToPartner(a, b);
    // The opening's center maps to the other's center; into A's front (-Z) is out of B's front (B's +Z)
    CHECK(nearlyEqual(glm::vec3(through * glm::vec4(0, 0, 0, 1)), {10, 0, 0}));
    glm::vec3 out = glm::vec3(through * glm::vec4(0, 0, -1, 0));
    CHECK(nearlyEqual(out, glm::vec3(b * glm::vec4(0, 0, 1, 0))));
    // Rigid, and the way back is the inverse
    CHECK(glm::determinant(glm::mat3(through)) == Catch::Approx(1.0f));
    CHECK(nearlyEqual(portalToPartner(b, a) * through, glm::mat4(1.0f)));
}

TEST_CASE("Portals: the camera walking through an opening comes out of the other end", "[Portal]") {
    TestAssets test;
    Scene scene(test.assets, test.settings);
    scene.createDefault();
    scene.camera().position = {0, 1, 0};
    scene.camera().yaw = 0.0f;
    scene.camera().pitch = 0.0f;
    EntityId pair = scene.addPortalPair();  // 3 ahead (-Z), ends at x = -1.5 and +1.5, facing the camera
    EntityId a = scene.find(pair)->children[0];
    EntityId b = scene.find(pair)->children[1];
    glm::vec3 endA = glm::vec3(scene.worldTransform(a)[3]);
    glm::vec3 endB = glm::vec3(scene.worldTransform(b)[3]);

    SECTION("through the opening") {
        glm::vec3 from = endA + glm::vec3(0, 0, 0.5f);  // in front of A, its center height
        scene.camera().position = endA + glm::vec3(0, 0, -0.5f);  // behind it now
        scene.moveCameraThroughPortals(from);
        // Out of B's front, 0.5 in front of it (B faces +Z too), looking away from it
        CHECK(nearlyEqual(scene.camera().position, endB + glm::vec3(0, 0, 0.5f), 1e-3f));
        CHECK(nearlyEqual(scene.camera().forward(), {0, 0, 1}, 1e-3f));
        RenderList list;
        scene.buildRenderList(list);
        CHECK_FALSE(list.cameraTeleport == glm::mat4(1.0f));
    }
    SECTION("past the side of the opening") {
        glm::vec3 from = endA + glm::vec3(1.2f, 0, 0.5f);  // half the width is 0.5: beside it
        glm::vec3 to = endA + glm::vec3(1.2f, 0, -0.5f);
        scene.camera().position = to;
        scene.moveCameraThroughPortals(from);
        CHECK(scene.camera().position == to);
    }
}
