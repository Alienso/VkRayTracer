//
// The editor's selection, the parts without the GPU: which entity an instance belongs to and the instance ranges of
// a selection (RenderList), the boxes the outline is looked for in (Scene::selectionBoxes). Undo: EditHistoryTests.cpp.
//

#include <catch_amalgamated.hpp>
#include "TestHelpers.h"
#include "scene/Scene.h"

static const std::string cornellBox = "Cornell box";
static const std::string cube = "Cube";

namespace {  // (each test file has its own TestScene)

struct TestScene : TestAssets {
    Scene scene{assets, settings};
    RenderList list;
    TestScene() { scene.createDefault(); }
};

}  // namespace

// ---------------------------------------------------------------------------------------------
// RenderList: instances and entities

TEST_CASE("Selection: each instance names the model entity it is a part of", "[Selection]") {
    TestScene t;
    EntityId box = t.scene.addObject(cornellBox, false);
    EntityId group = t.scene.addEmpty("Group", 0);
    EntityId inGroup = t.scene.addObject(cube, false);
    t.scene.setParent(inGroup, group);
    t.scene.buildRenderList(t.list);
    REQUIRE(!t.list.instances.empty());
    std::map<uint64_t, int> counts;
    for (uint32_t i = 0; i < t.list.instances.size(); i++) {
        counts[t.list.entityOfInstance(i)]++;
    }
    CHECK(counts[box] == static_cast<int>(t.assets.model(t.scene.model(box)->model).parts.size()) - 1);  // (front wall hidden)
    CHECK(counts[inGroup] == 1);
    CHECK(counts.count(group) == 0);  // (a group has no parts)
    CHECK(t.list.entityOfInstance(static_cast<uint32_t>(t.list.instances.size())) == 0);  // past the end: none
}

TEST_CASE("Selection: a group's instance ranges cover its models, merged where they meet", "[Selection]") {
    TestScene t;
    EntityId group = t.scene.addEmpty("Group", 0);
    EntityId a = t.scene.addObject(cube, false);
    EntityId b = t.scene.addObject(cornellBox, false);
    t.scene.setParent(a, group);
    t.scene.setParent(b, group);
    EntityId outside = t.scene.addObject(cube, false);
    t.scene.buildRenderList(t.list);
    std::vector<InstanceRange> ranges = t.list.instanceRanges([&](uint64_t id) { return t.scene.isUnder(id, group); });
    REQUIRE(ranges.size() == 1);  // the two models are next to each other in tree order
    uint32_t count = 0;
    for (uint32_t i = ranges[0].first; i < ranges[0].first + ranges[0].count; i++) {
        uint64_t owner = t.list.entityOfInstance(i);
        CHECK((owner == a || owner == b));
        count++;
    }
    uint32_t expected = 0;
    for (uint32_t i = 0; i < t.list.instances.size(); i++) {
        uint64_t owner = t.list.entityOfInstance(i);
        expected += owner == a || owner == b ? 1 : 0;
    }
    CHECK(count == expected);
    std::vector<InstanceRange> one = t.list.instanceRanges([&](uint64_t id) { return id == outside; });
    REQUIRE(one.size() == 1);
    CHECK(one[0].count == 1);
    // A hidden model has no instances: nothing to outline
    t.scene.find(outside)->visible = false;
    t.scene.buildRenderList(t.list);
    CHECK(t.list.instanceRanges([&](uint64_t id) { return id == outside; }).empty());
}

// ---------------------------------------------------------------------------------------------
// Scene::selectionBoxes

TEST_CASE("Selection: boxes around the visible models under an entity, placed in the world", "[Selection]") {
    TestScene t;
    EntityId group = t.scene.addEmpty("Group", 0);
    t.scene.find(group)->local.position = {10.0f, 0.0f, 0.0f};
    EntityId cubeEntity = t.scene.addObject(cube, false);
    t.scene.setParent(cubeEntity, group, false);
    t.scene.find(cubeEntity)->local = Transform();
    t.scene.find(cubeEntity)->local.scale = glm::vec3(2.0f);
    EntityId hidden = t.scene.addObject(cube, false);
    t.scene.setParent(hidden, group, false);
    t.scene.find(hidden)->visible = false;

    std::vector<WorldBox> boxes = t.scene.selectionBoxes(group);
    REQUIRE(boxes.size() == 1);  // the hidden one has none
    CHECK(boxes[0].min == glm::vec3(-0.5f));
    CHECK(boxes[0].max == glm::vec3(0.5f));
    glm::vec3 corner = glm::vec3(boxes[0].transform * glm::vec4(boxes[0].max, 1.0f));
    CHECK(nearlyEqual(corner, glm::vec3(11.0f, 1.0f, 1.0f)));  // (a built-in model: no fit)
    CHECK(t.scene.selectionBoxes(t.scene.addEmpty("Empty", 0)).empty());
    CHECK(t.scene.selectionBoxes(9999).empty());
}

TEST_CASE("Selection: the floor has a box too", "[Selection]") {
    TestScene t;
    EntityId floor = t.scene.roots().at(0);
    std::vector<WorldBox> boxes = t.scene.selectionBoxes(floor);
    REQUIRE(boxes.size() == 1);
    CHECK(boxes[0].max.x > boxes[0].min.x);
    CHECK(boxes[0].max.z > boxes[0].min.z);
}

