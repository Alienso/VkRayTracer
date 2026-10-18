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
        REQUIRE(list.instances[i].clip == fresh.instances[i].clip);
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

    // Moving an end rewrites the rims; the end and its rim bars know where they were (the motion vectors)
    CHECK(t.list.portals[0].previous == t.list.portals[0].world);  // new: where it is
    uint64_t revision = t.list.instanceRevision();
    EntityId a = t.scene.find(pair)->children[0];
    glm::mat4 before = t.list.portals[0].world;
    glm::vec3 barBefore(t.list.instances[floorParts].world[3]);
    t.scene.find(a)->local.position.y += 1.0f;
    t.scene.buildRenderList(t.list);
    CHECK(t.list.instanceRevision() == revision);
    REQUIRE(t.list.changedInstances().size() == 1);
    CHECK(t.list.changedInstances()[0].first == floorParts);
    CHECK(t.list.motionsChanged());
    CHECK(t.list.portals[0].previous == before);
    CHECK(t.list.portals[1].previous == t.list.portals[1].world);
    const RenderInstance& bar = t.list.instances[floorParts];
    CHECK(nearlyEqual(glm::vec3(t.list.motions[bar.motion] * bar.world[3]), barBefore, 1e-5f));
    requireSameAsRemade(t.scene, t.list);
    // Still again: no motion
    t.scene.buildRenderList(t.list);
    CHECK(t.list.portals[0].previous == t.list.portals[0].world);
    CHECK(t.list.motions[t.list.instances[floorParts].motion] == glm::mat4(1.0f));

    // A hidden end: the pair is off, the other end's rim still shows where it is (in its own colour: B's)
    t.scene.find(a)->visible = false;
    t.scene.buildRenderList(t.list);
    CHECK(t.list.portals.empty());
    REQUIRE(t.list.instances.size() == floorParts + 4);
    const SceneModel& rim = t.assets.model(t.scene.portalRimModel());
    CHECK(t.list.instances[floorParts].mesh == rim.parts[1].meshIndex);
    requireSameAsRemade(t.scene, t.list);
    // Both hidden: nothing
    t.scene.find(t.scene.find(pair)->children[1])->visible = false;
    t.scene.buildRenderList(t.list);
    CHECK(t.list.instances.size() == floorParts);
}

namespace {

// A pair whose ends face +Z, A at (0, 1, 0) and B at (5, 1, -2), 1 wide and 2 high; and a cube 0.6 across
struct HalfThrough : TestScene {
    EntityId cubeId;
    glm::mat4 endA, endB;
    HalfThrough() {
        EntityId pair = scene.addPortalPair();
        scene.find(pair)->local = Transform();
        EntityId a = scene.find(pair)->children[0];
        EntityId b = scene.find(pair)->children[1];
        scene.find(a)->local = Transform();
        scene.find(a)->local.position = {0, 1, 0};
        scene.find(b)->local = Transform();
        scene.find(b)->local.position = {5, 1, -2};
        endA = scene.worldTransform(a);
        endB = scene.worldTransform(b);
        cubeId = scene.addObject(cube, false);
        glm::vec3 min, max;
        REQUIRE(scene.modelBox(cubeId, min, max));
        scene.find(cubeId)->local.scale = glm::vec3(0.6f / (max.x - min.x));
        place({0, 1, 3});  // away from the opening
    }
    void place(const glm::vec3& position) { scene.find(cubeId)->local.position = position; }
    // The cube's (real) instances: its entity's range
    InstanceRange cubeRange() {
        std::vector<InstanceRange> ranges = list.instanceRanges([&](uint64_t id) { return id == cubeId; });
        REQUIRE(!ranges.empty());
        return ranges[0];
    }
};

}  // namespace

TEST_CASE("RenderList: a cube half through a portal is cut there, its ghost at the other end after the rims",
          "[RenderList][Portals]") {
    HalfThrough t;
    t.place({0, 1, 0.1f});  // 0.2 of it behind A
    t.scene.buildRenderList(t.list);
    size_t parts = t.visibleParts(t.cubeId);
    REQUIRE(t.list.clipRegions.size() == 2);  // the real one's, the ghost's
    InstanceRange real = t.cubeRange();
    CHECK(real.count == parts);
    CHECK(t.list.instances[real.first].clip == 1);
    CHECK(t.list.clipRegions[0].cutCount == 1);
    CHECK_FALSE(t.list.clipRegions[0].cuts[0].keepNear);
    // The ghost: the list's last instances, placed through the pair, cut to B's front
    size_t first = t.list.instances.size() - parts;
    for (size_t i = 0; i < parts; i++) {
        const RenderInstance& ghost = t.list.instances[first + i];
        CHECK(ghost.clip == 2);
        CHECK(ghost.mesh == t.list.instances[real.first + i].mesh);
        CHECK(nearlyEqual(ghost.world, portalToPartner(t.endA, t.endB) * t.list.instances[real.first + i].world,
                          1e-5f));
        CHECK(ghost.motion != t.list.instances[real.first + i].motion);  // a motion of its own
    }
    CHECK(t.list.clipRegions[1].cuts[0].keepNear);
    // It is the cube's: picked, and outlined with it
    CHECK(t.list.entityOfInstance(static_cast<uint32_t>(first)) == t.cubeId);
    std::vector<InstanceRange> ranges = t.list.instanceRanges([&](uint64_t id) { return id == t.cubeId; });
    REQUIRE(ranges.size() == 2);
    CHECK(ranges[1].first == first);
    std::vector<std::pair<uint64_t, glm::mat4>> ghosts = t.list.ghostsOf([&](uint64_t id) { return id == t.cubeId; });
    REQUIRE(ghosts.size() == 1);
    CHECK(nearlyEqual(ghosts[0].second, portalToPartner(t.endA, t.endB), 1e-5f));
}

TEST_CASE("RenderList: a ghost coming, moving and going changes only the list's end, not the revision",
          "[RenderList][Portals]") {
    HalfThrough t;
    t.scene.checkUpdates = true;
    t.scene.buildRenderList(t.list);
    size_t whole = t.list.instances.size();
    uint64_t revision = t.list.instanceRevision();
    size_t parts = t.visibleParts(t.cubeId);
    CHECK(t.list.clipRegions.empty());

    t.place({0, 1, 0.1f});  // into A
    t.scene.buildRenderList(t.list);
    CHECK(t.list.instanceRevision() == revision);
    CHECK(t.list.instances.size() == whole + parts);
    REQUIRE(t.list.remadeInstances().size() == 2);  // the cube (cut now), the ghosts
    CHECK(t.list.remadeInstances()[0].first == t.cubeRange().first);
    CHECK(t.list.remadeInstances()[1].first == whole);
    CHECK(t.list.remadeInstances()[1].count == parts);
    CHECK(t.list.motionsChanged());
    requireSameAsRemade(t.scene, t.list);

    t.place({0, 1, 0.05f});  // further in: the same pieces, moved
    t.scene.buildRenderList(t.list);
    CHECK(t.list.instanceRevision() == revision);
    CHECK(t.list.remadeInstances().empty());
    bool tailMoved = false;
    for (const InstanceRange& range : t.list.changedInstances()) {
        tailMoved = tailMoved || (range.first == whole && range.count == parts);
    }
    CHECK(tailMoved);
    CHECK(t.list.motions.back() != glm::mat4(1.0f));  // the ghost's motion: it moved too
    requireSameAsRemade(t.scene, t.list);

    t.scene.buildRenderList(t.list);  // still: nothing to do
    CHECK(t.list.remadeInstances().empty());
    CHECK(t.list.changedInstances().empty());

    t.place({0, 1, 3});  // out again
    t.scene.buildRenderList(t.list);
    CHECK(t.list.instanceRevision() == revision);
    CHECK(t.list.instances.size() == whole);  // (the end only shrank)
    REQUIRE(t.list.remadeInstances().size() == 1);
    CHECK(t.list.remadeInstances()[0].first == t.cubeRange().first);
    CHECK(t.list.clipRegions.empty());
    CHECK(t.list.instances[t.cubeRange().first].clip == 0);
    CHECK(t.scene.updateCheckFailures() == 0);
    requireSameAsRemade(t.scene, t.list);
}

TEST_CASE("RenderList: a room around a portal isn't cut, nor a cube half in the wall beside it", "[RenderList][Portals]") {
    HalfThrough t;
    EntityId room = t.scene.addObject(cornellBox, false);
    t.scene.find(room)->local.position = {0, 0, 0};  // the opening inside it
    glm::vec3 min, max;
    REQUIRE(t.scene.modelBox(room, min, max));
    INFO("room " << min.x << " " << min.y << " " << min.z << " .. " << max.x << " " << max.y << " " << max.z);
    REQUIRE(glm::all(glm::lessThanEqual(min, glm::vec3(-0.5f, 0.0f, -0.5f))));
    REQUIRE(glm::all(glm::greaterThanEqual(max, glm::vec3(0.5f, 2.0f, 0.5f))));
    t.place({0.6f, 1, 0.1f});                       // half across A's side edge
    t.scene.buildRenderList(t.list);
    CHECK(t.list.clipRegions.empty());
    for (const RenderInstance& instance : t.list.instances) {
        CHECK(instance.clip == 0);
    }
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
