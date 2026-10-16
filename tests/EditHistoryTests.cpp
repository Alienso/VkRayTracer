//
// The editor's undo (editor/EditHistory.h): every change as the scene's text before and after it. Deleting,
// duplicating, re-parenting, transforms, components, materials and the scene's settings are undone and redone; the
// camera and the program's settings stay; ids come back the same.
//

#include <catch_amalgamated.hpp>
#include "TestHelpers.h"
#include "editor/EditHistory.h"
#include "scene/Scene.h"

static const std::string lampBox = "Cornell box (blocks, point light)";
static const std::string cube = "Cube";

namespace {  // (each test file has its own TestScene)

struct TestScene : TestAssets {
    Scene scene{assets, settings};
    EditHistory history;
    TestScene() {
        scene.createDefault();
        history.reset(scene);
    }
};

}  // namespace

TEST_CASE("Edit history: a deleted subtree comes back with its ids, components and models; redo deletes again",
          "[Undo]") {
    TestScene t;
    Scene& scene = t.scene;
    EntityId group = scene.addEmpty("Group", 0);
    EntityId lamp = scene.addObject(lampBox, false);
    scene.setParent(lamp, group);
    scene.setGame(lamp, {{"spin", R"({"speed":30})"}});
    EntityId pair = scene.addPortalPair();
    scene.setParent(pair, group);
    ModelHandle model = scene.model(lamp)->model;
    t.history.commit(scene);
    std::string before = scene.contentText();

    scene.remove(group);
    REQUIRE(t.history.commit(scene));
    REQUIRE(scene.find(lamp) == nullptr);

    REQUIRE(t.history.undo(scene));
    CHECK(scene.contentText() == before);
    REQUIRE(scene.find(lamp) != nullptr);
    CHECK(scene.find(lamp)->parent == group);
    CHECK(scene.game(lamp) != nullptr);
    CHECK(scene.model(lamp)->model == model);  // the same model, still loaded: nothing to wait for
    CHECK_FALSE(scene.model(lamp)->loading);
    CHECK(scene.portal(scene.find(pair)->children.at(0)) != nullptr);

    REQUIRE(t.history.redo(scene));
    CHECK(scene.find(group) == nullptr);
    CHECK(scene.find(lamp) == nullptr);
    CHECK_FALSE(t.history.canRedo());
    CHECK(t.history.canUndo());
}

TEST_CASE("Edit history: changes undo in order, back to the start; no change makes no entry", "[Undo]") {
    TestScene t;
    Scene& scene = t.scene;
    EntityId a = scene.addEmpty("A", 0);  // (not committed yet: part of the first change)
    REQUIRE(t.history.commit(scene));
    CHECK_FALSE(t.history.commit(scene));  // nothing changed since
    scene.find(a)->local.position.x = 1.0f;
    t.history.commit(scene);
    scene.find(a)->local.position.x = 2.0f;
    scene.find(a)->name = "Renamed";
    t.history.commit(scene);
    CHECK(t.history.undoCount() == 3);

    REQUIRE(t.history.undo(scene));
    CHECK(scene.find(a)->local.position.x == 1.0f);
    CHECK(scene.find(a)->name == "A");
    REQUIRE(t.history.undo(scene));
    CHECK(scene.find(a)->local.position.x == 0.0f);
    REQUIRE(t.history.undo(scene));
    CHECK(scene.find(a) == nullptr);
    CHECK_FALSE(t.history.undo(scene));
    REQUIRE(t.history.redo(scene));
    REQUIRE(t.history.redo(scene));
    CHECK(scene.find(a)->local.position.x == 1.0f);
}

TEST_CASE("Edit history: a new change drops the redo; a change not committed is committed before undoing",
          "[Undo]") {
    TestScene t;
    Scene& scene = t.scene;
    EntityId a = scene.addEmpty("A", 0);
    t.history.commit(scene);
    scene.find(a)->local.position.x = 1.0f;
    t.history.commit(scene);
    t.history.undo(scene);
    CHECK(t.history.canRedo());
    scene.find(a)->local.position.y = 5.0f;
    t.history.commit(scene);
    CHECK_FALSE(t.history.canRedo());

    scene.find(a)->local.position.z = 7.0f;  // not committed (a key pressed in the same frame)
    REQUIRE(t.history.undo(scene));          // undoes just that
    CHECK(scene.find(a)->local.position.z == 0.0f);
    CHECK(scene.find(a)->local.position.y == 5.0f);
}

TEST_CASE("Edit history: the camera stays where it is; the program's settings aren't touched", "[Undo][Settings]") {
    TestScene t;
    Scene& scene = t.scene;
    scene.addEmpty("A", 0);
    t.history.commit(scene);
    t.settings.exposure = 3.0f;  // the scene's
    t.history.commit(scene);
    scene.camera().position = {9.0f, 8.0f, 7.0f};
    scene.camera().yaw = 42.0f;
    t.settings.samplesPerPixel = 6;  // the program's
    REQUIRE(t.history.undo(scene));
    CHECK(t.settings.exposure == RenderSettings().exposure);
    CHECK(t.settings.samplesPerPixel == 6);
    CHECK(scene.camera().position == glm::vec3(9.0f, 8.0f, 7.0f));
    CHECK(scene.camera().yaw == 42.0f);
}

TEST_CASE("Edit history: duplicating, re-parenting and material edits undo", "[Undo]") {
    TestScene t;
    Scene& scene = t.scene;
    EntityId group = scene.addEmpty("Group", 0);
    EntityId lamp = scene.addObject(lampBox, false);
    t.history.commit(scene);
    std::string start = scene.contentText();

    EntityId copy = scene.duplicate(lamp);
    t.history.commit(scene);
    scene.setParent(lamp, group);
    t.history.commit(scene);
    SceneModel& model = t.assets.model(scene.model(lamp)->model);
    float roughness = t.assets.materials()[model.firstMaterial].material.roughness;
    t.assets.materials()[model.firstMaterial].material.roughness = 0.125f;
    t.history.commit(scene);

    REQUIRE(t.history.undo(scene));
    CHECK(t.assets.materials()[model.firstMaterial].material.roughness == roughness);
    REQUIRE(t.history.undo(scene));
    CHECK(scene.find(lamp)->parent == 0);
    REQUIRE(t.history.undo(scene));
    CHECK(scene.find(copy) == nullptr);
    CHECK(scene.contentText() == start);
}

TEST_CASE("Edit history: sync moves the start without an entry; reset forgets everything", "[Undo]") {
    TestScene t;
    Scene& scene = t.scene;
    EntityId a = scene.addEmpty("A", 0);
    t.history.commit(scene);
    scene.find(a)->local.position.x = 4.0f;  // (as Stop puts back a scene: no change for undo)
    t.history.sync(scene);
    CHECK_FALSE(t.history.commit(scene));
    CHECK(t.history.undoCount() == 1);
    t.history.reset(scene);
    CHECK_FALSE(t.history.canUndo());
    CHECK_FALSE(t.history.canRedo());
}

TEST_CASE("Edit history: it keeps the last 100 changes", "[Undo]") {
    TestScene t;
    Scene& scene = t.scene;
    EntityId a = scene.addEmpty("A", 0);
    t.history.commit(scene);
    for (int i = 1; i <= 150; i++) {
        scene.find(a)->local.position.x = static_cast<float>(i);
        t.history.commit(scene);
    }
    CHECK(t.history.undoCount() == EditHistory::maxChanges);
    while (t.history.undo(scene)) {
    }
    CHECK(scene.find(a)->local.position.x == 50.0f);  // as far back as it keeps
}
