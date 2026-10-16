//
// The render list (scene/RenderList.h, Scene::buildRenderList): remade when the layout changes, otherwise only the
// moved entities' ranges rewritten; motions take this frame's positions to last frame's.
//

#include <catch_amalgamated.hpp>
#include <random>
#include "TestHelpers.h"
#include "scene/Scene.h"

static const std::string cube = "Cube";
static const std::string cornellBox = "Cornell box";
static const std::string lampBox = "Cornell box (blocks, point light)";

namespace {  // (each test file has its own TestScene)

struct TestScene : TestAssets {
    Scene scene{assets, settings};
    RenderList list;
    TestScene() { scene.createDefault(); }

    size_t visibleParts(EntityId id) {
        const ModelComponent* component = scene.model(id);
        const SceneModel& model = assets.model(component->model);
        size_t count = 0;
        for (size_t p = 0; p < model.parts.size(); p++) {
            count += p < component->hiddenParts.size() && component->hiddenParts[p] ? 0 : 1;
        }
        return count;
    }
};

}  // namespace

// The same scene's list made from scratch (a fresh RenderList has nothing to update)
static void requireSameAsRemade(Scene& scene, const RenderList& list) {
    RenderList fresh;
    scene.buildRenderList(fresh);
    REQUIRE(list.instances.size() == fresh.instances.size());
    for (size_t i = 0; i < list.instances.size(); i++) {
        REQUIRE(list.instances[i].mesh == fresh.instances[i].mesh);
        REQUIRE(nearlyEqual(list.instances[i].world, fresh.instances[i].world, 1e-5f));
    }
}

TEST_CASE("RenderList: one instance per visible part, floor first", "[RenderList]") {
    TestScene t;
    EntityId box = t.scene.addObject(cornellBox, false);
    t.scene.buildRenderList(t.list);
    size_t floorParts = t.visibleParts(t.scene.roots()[0]);
    CHECK(t.list.instances.size() == floorParts + t.visibleParts(box));
    CHECK(t.list.instanceRevision() == 1);
    CHECK(t.list.build() == 1);
    CHECK(t.list.motionsChanged());
    CHECK(t.list.motions.size() == 2);  // one per visible model entity
    for (const RenderInstance& instance : t.list.instances) {
        CHECK(instance.motion < t.list.motions.size());
        CHECK(instance.mesh < t.assets.meshes().size());
    }
}

TEST_CASE("RenderList: a move rewrites only that entity's range; its motion leads back to last frame", "[RenderList]") {
    TestScene t;
    EntityId a = t.scene.addObject(cube, false);
    EntityId b = t.scene.addObject(cornellBox, false);
    t.scene.buildRenderList(t.list);
    t.scene.buildRenderList(t.list);
    uint64_t revision = t.list.instanceRevision();
    CHECK_FALSE(t.list.motionsChanged());
    CHECK(t.list.changedInstances().empty());

    glm::mat4 before = t.scene.worldTransform(b);
    t.scene.find(b)->local.position += glm::vec3(1, 0, 0);
    t.scene.buildRenderList(t.list);
    CHECK(t.list.instanceRevision() == revision);  // same layout
    REQUIRE(t.list.changedInstances().size() == 1);
    InstanceRange range = t.list.changedInstances()[0];
    CHECK(range.count == t.visibleParts(b));
    CHECK(range.first == t.visibleParts(t.scene.roots()[0]) + t.visibleParts(a));
    CHECK(t.list.motionsChanged());
    const glm::mat4& motion = t.list.motions[t.list.instances[range.first].motion];
    glm::vec3 now = glm::vec3(t.scene.worldTransform(b)[3]);
    CHECK(nearlyEqual(glm::vec3(motion * glm::vec4(now, 1)), glm::vec3(before[3])));
    requireSameAsRemade(t.scene, t.list);

    // Still: the motion goes back to identity (a change), then nothing changes
    t.scene.buildRenderList(t.list);
    CHECK(t.list.motionsChanged());
    CHECK(t.list.changedInstances().empty());
    CHECK(t.list.motions[t.list.instances[range.first].motion] == glm::mat4(1.0f));
    t.scene.buildRenderList(t.list);
    CHECK_FALSE(t.list.motionsChanged());
}

TEST_CASE("RenderList: moving a parent moves its children's instances", "[RenderList]") {
    TestScene t;
    EntityId group = t.scene.addEmpty("group", 0);
    EntityId child = t.scene.addObject(cube, false);
    t.scene.setParent(child, group, false);
    t.scene.buildRenderList(t.list);
    t.scene.find(group)->local.position.y += 2.0f;
    t.scene.buildRenderList(t.list);
    REQUIRE(t.list.changedInstances().size() == 1);
    CHECK(t.list.changedInstances()[0].count == t.visibleParts(child));
    requireSameAsRemade(t.scene, t.list);
}

TEST_CASE("RenderList: layout changes remake the list", "[RenderList]") {
    TestScene t;
    EntityId box = t.scene.addObject(cornellBox, false);
    t.scene.buildRenderList(t.list);
    uint64_t revision = t.list.instanceRevision();
    size_t count = t.list.instances.size();

    SECTION("hiding an entity") {
        t.scene.find(box)->visible = false;
        t.scene.buildRenderList(t.list);
        CHECK(t.list.instanceRevision() > revision);
        CHECK(t.list.instances.size() == count - t.visibleParts(box));
    }
    SECTION("hiding a part") {
        t.scene.setPartHidden(box, 0, true);
        t.scene.buildRenderList(t.list);
        CHECK(t.list.instanceRevision() > revision);
        CHECK(t.list.instances.size() == count - 1);
    }
    SECTION("adding an entity") {
        t.scene.addObject(cube, false);
        t.scene.buildRenderList(t.list);
        CHECK(t.list.instanceRevision() > revision);
    }
    SECTION("removing an entity") {
        t.scene.remove(box);
        t.scene.buildRenderList(t.list);
        CHECK(t.list.instanceRevision() > revision);
        CHECK(t.list.instances.size() == t.visibleParts(t.scene.roots()[0]));
    }
    SECTION("re-parenting") {
        EntityId group = t.scene.addEmpty("group", 0);
        t.scene.buildRenderList(t.list);
        revision = t.list.instanceRevision();
        t.scene.setParent(box, group);
        t.scene.buildRenderList(t.list);
        CHECK(t.list.instanceRevision() > revision);
    }
    requireSameAsRemade(t.scene, t.list);
}

TEST_CASE("RenderList: a portal pair gives two portal ends and rim bars after the entities", "[RenderList]") {
    TestScene t;
    EntityId pair = t.scene.addPortalPair();
    t.scene.buildRenderList(t.list);
    REQUIRE(t.list.portals.size() == 2);
    CHECK(t.list.portals[0].halfSize == glm::vec2(0.5f, 1.0f));
    size_t floorParts = t.visibleParts(t.scene.roots()[0]);
    CHECK(t.list.instances.size() == floorParts + 8);  // a rectangle's rim: 4 bars per end

    // Moving an end rewrites the rims
    uint64_t revision = t.list.instanceRevision();
    EntityId a = t.scene.find(pair)->children[0];
    t.scene.find(a)->local.position.y += 1.0f;
    t.scene.buildRenderList(t.list);
    CHECK(t.list.instanceRevision() == revision);
    REQUIRE(t.list.changedInstances().size() == 1);
    CHECK(t.list.changedInstances()[0].first == floorParts);
    requireSameAsRemade(t.scene, t.list);

    // A hidden end: the pair is off
    t.scene.find(a)->visible = false;
    t.scene.buildRenderList(t.list);
    CHECK(t.list.portals.empty());
    CHECK(t.list.instances.size() == floorParts);
}

TEST_CASE("RenderList: punctual lights of visible models, with the object's scale", "[RenderList]") {
    TestScene t;
    EntityId lamp = t.scene.addObject(lampBox, false);
    t.scene.buildRenderList(t.list);
    size_t lights = t.assets.model(t.scene.model(lamp)->model).lights.size();
    REQUIRE(lights > 0);
    CHECK(t.list.lights.size() == lights);
    float scale = t.list.lights[0].scale;
    t.scene.find(lamp)->local.scale = glm::vec3(2.0f);
    t.scene.buildRenderList(t.list);
    CHECK(t.list.lights[0].scale == Catch::Approx(2.0f * scale));
    t.scene.find(lamp)->visible = false;
    t.scene.buildRenderList(t.list);
    CHECK(t.list.lights.empty());
}

// Random edits, each build checked against a remake (Scene::checkUpdates, the Debug panel's check) and against a
// list built from scratch
TEST_CASE("RenderList: random edits give the same list as a remake", "[RenderList]") {
    TestScene t;
    t.scene.checkUpdates = true;
    std::mt19937 random(777);
    std::vector<EntityId> ids;
    auto pick = [&]() { return ids[std::uniform_int_distribution<size_t>(0, ids.size() - 1)(random)]; };
    for (int i = 0; i < 4; i++) {
        ids.push_back(t.scene.addObject(i % 2 ? cube : cornellBox, false));
    }
    ids.push_back(t.scene.addEmpty("group", 0));
    ids.push_back(t.scene.addPortalPair());
    for (int step = 0; step < 300; step++) {
        int action = std::uniform_int_distribution<int>(0, 9)(random);
        EntityId id = pick();
        Entity* entity = t.scene.find(id);
        if (entity == nullptr) {
            continue;
        }
        if (action <= 4) {
            entity->local.position += glm::vec3(0.1f, 0.0f, -0.05f);
            entity->local.rotation = glm::normalize(entity->local.rotation * eulerToQuat({0, 7, 0}));
        } else if (action == 5) {
            entity->visible = !entity->visible;
        } else if (action == 6 && t.scene.model(id) != nullptr) {
            t.scene.setPartHidden(id, std::uniform_int_distribution<size_t>(0, 3)(random), step % 2 == 0);
        } else if (action == 7) {
            t.scene.setParent(id, pick());  // refused when it would loop
        } else if (action == 8) {
            ids.push_back(t.scene.duplicate(id));
        } else if (t.scene.model(id) != nullptr) {
            t.scene.model(id)->originalSize = !t.scene.model(id)->originalSize;
        }
        t.scene.buildRenderList(t.list);
        REQUIRE(t.scene.updateCheckFailures() == 0);
        if (step % 25 == 0) {
            requireSameAsRemade(t.scene, t.list);
        }
    }
    requireSameAsRemade(t.scene, t.list);
}
