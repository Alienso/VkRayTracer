//
// Play / Stop's snapshot (Scene::text / loadText, Editor::play / stop): what Play keeps, Stop puts back, whatever
// changed in between; the program's settings are left alone.
//

#include <catch_amalgamated.hpp>
#include "TestHelpers.h"
#include "scene/Scene.h"

static const std::string lampBox = "Cornell box (blocks, point light)";

namespace {  // (each test file has its own TestScene)

struct TestScene : TestAssets {
    Scene scene{assets, settings};
    TestScene() { scene.createDefault(); }
};

}  // namespace

TEST_CASE("Play snapshot: Stop undoes moves, adds, removes, components and the camera", "[Play]") {
    TestScene t;
    Scene& scene = t.scene;
    EntityId group = scene.addEmpty("Group", 0);
    EntityId door = scene.addEmpty("Door", group);
    EntityId lamp = scene.addObject(lampBox, false);
    scene.setGame(door, {{"door", R"({"speed":2})"}});
    scene.find(door)->local.position = {1.0f, 2.0f, 3.0f};
    scene.camera().position = {4.0f, 5.0f, 6.0f};
    scene.camera().yaw = 30.0f;
    EntityId pair = scene.addPortalPair();
    std::string content = scene.contentText();
    std::string snapshot = scene.text();

    // Played: moved, hidden, re-parented, added, removed, components changed, the camera flown around
    scene.find(door)->local.position = {9.0f, 9.0f, 9.0f};
    scene.find(door)->local.rotation = eulerToQuat({0.0f, 90.0f, 0.0f});
    scene.find(lamp)->visible = false;
    scene.setParent(door, 0);
    EntityId spawned = scene.addEmpty("Spawned", 0);
    scene.remove(pair);
    scene.setGame(door, {});
    scene.setGame(lamp, {{"pickup", ""}});
    scene.camera().position = {0.0f, 0.0f, 0.0f};
    scene.camera().yaw = -90.0f;

    CHECK(scene.loadText(snapshot, "snapshot").empty());
    CHECK(scene.contentText() == content);
    CHECK(scene.text() == snapshot);
    CHECK(scene.camera().position == glm::vec3(4.0f, 5.0f, 6.0f));
    CHECK(scene.camera().yaw == 30.0f);
    // The same ids (the editor's selection still points at them); the one added while playing is gone
    REQUIRE(scene.find(door) != nullptr);
    CHECK(scene.find(door)->parent == group);
    CHECK(scene.find(door)->local.position == glm::vec3(1.0f, 2.0f, 3.0f));
    CHECK(scene.find(lamp)->visible);
    CHECK(scene.find(pair) != nullptr);
    CHECK(scene.portal(scene.find(pair)->children.at(0)) != nullptr);
    CHECK(scene.find(spawned) == nullptr);
    CHECK(scene.game(door) != nullptr);
    CHECK(scene.game(lamp) == nullptr);

    // Ids go on after the snapshot's: one added after Stop doesn't reuse a snapshot entity's
    EntityId next = scene.addEmpty("After", 0);
    CHECK(next > lamp);
}

TEST_CASE("Play snapshot: the scene's settings come back, the program's are left as they are", "[Play][Settings]") {
    TestScene t;
    t.settings.exposure = 2.5f;       // the scene's
    t.settings.samplesPerPixel = 3;   // the program's
    std::string snapshot = t.scene.text();

    t.settings.exposure = 0.25f;
    t.settings.maxBounces = 1;
    t.settings.samplesPerPixel = 7;
    t.settings.debugView = 2;
    CHECK(t.scene.loadText(snapshot, "snapshot").empty());
    CHECK(t.settings.exposure == 2.5f);
    CHECK(t.settings.maxBounces == RenderSettings().maxBounces);
    CHECK(t.settings.samplesPerPixel == 7);
    CHECK(t.settings.debugView == 2);
}

TEST_CASE("Play snapshot: material and light edits made while playing are undone", "[Play]") {
    TestScene t;
    EntityId lamp = t.scene.addObject(lampBox, false);
    SceneModel& model = t.assets.model(t.scene.model(lamp)->model);
    SceneMaterial& material = t.assets.materials()[model.firstMaterial + 1];
    material.material.roughness = 0.125f;
    std::string snapshot = t.scene.text();

    float intensity = model.lights[0].intensity;
    material.material.roughness = 0.75f;
    model.lights[0].intensity *= 10.0f;
    CHECK(t.scene.loadText(snapshot, "snapshot").empty());
    CHECK(t.assets.materials()[model.firstMaterial + 1].material.roughness == 0.125f);
    CHECK(model.lights[0].intensity == intensity);
}

TEST_CASE("Play snapshot: models stay loaded through Stop (no reload)", "[Play]") {
    TestScene t;
    EntityId lamp = t.scene.addObject(lampBox, false);
    ModelHandle handle = t.scene.model(lamp)->model;
    std::string snapshot = t.scene.text();
    t.scene.remove(lamp);  // the last entity showing it: kept unused
    CHECK(t.scene.loadText(snapshot, "snapshot").empty());
    REQUIRE(t.scene.model(lamp) != nullptr);
    CHECK(t.scene.model(lamp)->model == handle);
    CHECK_FALSE(t.scene.model(lamp)->loading);
}

TEST_CASE("Play snapshot: text that isn't a scene is refused, the scene unchanged", "[Play]") {
    TestScene t;
    EntityId id = t.scene.addEmpty("Kept", 0);
    std::string before = t.scene.text();
    CHECK_THROWS(t.scene.loadText("{ not json", "snapshot"));
    CHECK_THROWS(t.scene.loadText(R"({"format": "something else"})", "snapshot"));
    CHECK(t.scene.find(id) != nullptr);
    CHECK(t.scene.text() == before);
}
