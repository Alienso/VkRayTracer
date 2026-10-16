//
// The scene's tree (scene/Scene.h): adding, re-parenting, duplicating and removing entities, world transforms,
// visibility, and the model references the entities hold.
//

#include <catch_amalgamated.hpp>
#include <algorithm>
#include <glm/gtc/matrix_transform.hpp>
#include "TestHelpers.h"
#include "scene/Scene.h"

static const std::string cube = "Cube";

// A scene with the default floor
namespace {  // (each test file has its own TestScene)

struct TestScene : TestAssets {
    Scene scene{assets};
    TestScene() { scene.createDefault(); }

    Transform transform(glm::vec3 position, glm::vec3 degrees, glm::vec3 scale) {
        Transform t;
        t.position = position;
        t.rotation = eulerToQuat(degrees);
        t.scale = scale;
        return t;
    }
    uint32_t references(EntityId id) { return assets.model(scene.model(id)->model).references; }
};

}  // namespace

TEST_CASE("Scene: the default scene is the floor", "[Scene]") {
    TestScene t;
    REQUIRE(t.scene.roots().size() == 1);
    const Entity* floor = t.scene.find(t.scene.roots()[0]);
    REQUIRE(floor != nullptr);
    CHECK(floor->name == "Floor");
    REQUIRE(t.scene.model(floor->id) != nullptr);
    CHECK(t.scene.model(floor->id)->model == t.assets.floor());
    CHECK(t.references(floor->id) == 1);
}

TEST_CASE("Scene: world transforms are the parents' times the entity's own", "[Scene]") {
    TestScene t;
    EntityId parent = t.scene.addEmpty("parent", 0);
    EntityId child = t.scene.addEmpty("child", parent);
    t.scene.find(parent)->local = t.transform({1, 2, 3}, {0, 90, 0}, {2, 2, 2});
    t.scene.find(child)->local = t.transform({1, 0, 0}, {0, 0, 0}, {1, 1, 1});
    // Parent: scale 2, turn 90 degrees about Y (+X -> -Z), move to (1, 2, 3): the child's (1, 0, 0) lands at
    // (1, 2, 3) + 2 * (0, 0, -1)
    glm::vec3 origin = glm::vec3(t.scene.worldTransform(child) * glm::vec4(0, 0, 0, 1));
    CHECK(nearlyEqual(origin, {1, 2, 1}));
    CHECK(t.scene.isUnder(child, parent));
    CHECK(t.scene.isUnder(child, child));
    CHECK_FALSE(t.scene.isUnder(parent, child));
    CHECK(t.scene.find(parent)->children == std::vector<EntityId>{child});
}

TEST_CASE("Scene: ids are unique and never reused", "[Scene]") {
    TestScene t;
    EntityId a = t.scene.addEmpty("a", 0);
    t.scene.remove(a);
    EntityId b = t.scene.addEmpty("b", 0);
    CHECK(b > a);
    CHECK(t.scene.find(a) == nullptr);
}

TEST_CASE("Scene: setParent keeps the world placement, or the local one", "[Scene]") {
    TestScene t;
    EntityId parent = t.scene.addEmpty("parent", 0);
    EntityId entity = t.scene.addEmpty("entity", 0);
    t.scene.find(parent)->local = t.transform({5, 1, -2}, {10, 40, -20}, {2, 2, 2});
    t.scene.find(entity)->local = t.transform({1, 2, 3}, {30, -60, 15}, {0.5f, 0.5f, 0.5f});
    glm::mat4 world = t.scene.worldTransform(entity);

    REQUIRE(t.scene.setParent(entity, parent, true));
    CHECK(t.scene.find(entity)->parent == parent);
    CHECK(nearlyEqual(t.scene.worldTransform(entity), world));
    CHECK(std::find(t.scene.roots().begin(), t.scene.roots().end(), entity) == t.scene.roots().end());

    REQUIRE(t.scene.setParent(entity, 0, true));  // back to the top
    CHECK(nearlyEqual(t.scene.worldTransform(entity), world));
    CHECK(t.scene.roots().back() == entity);

    Transform local = t.scene.find(entity)->local;
    REQUIRE(t.scene.setParent(entity, parent, false));
    CHECK(t.scene.find(entity)->local.position == local.position);
}

TEST_CASE("Scene: setParent refuses to make a loop", "[Scene]") {
    TestScene t;
    EntityId a = t.scene.addEmpty("a", 0);
    EntityId b = t.scene.addEmpty("b", a);
    EntityId c = t.scene.addEmpty("c", b);
    CHECK_FALSE(t.scene.setParent(a, a));
    CHECK_FALSE(t.scene.setParent(a, c));  // under its own grandchild
    CHECK_FALSE(t.scene.setParent(a, 12345));  // no such parent
    CHECK(t.scene.find(a)->parent == 0);
    CHECK(t.scene.find(c)->parent == b);
}

TEST_CASE("Scene: setParent onto the same parent puts it last", "[Scene]") {
    TestScene t;
    EntityId group = t.scene.addEmpty("group", 0);
    EntityId a = t.scene.addEmpty("a", group);
    EntityId b = t.scene.addEmpty("b", group);
    REQUIRE(t.scene.setParent(a, group));
    CHECK(t.scene.find(group)->children == std::vector<EntityId>{b, a});
}

// Under a parent scaled to 0 there is no local transform that keeps the world one: the entity must not end up with
// NaNs (they would also make the scene file unreadable: JSON has no NaN)
TEST_CASE("Scene: setParent under a parent of scale 0 leaves a usable transform", "[Scene]") {
    TestScene t;
    EntityId flat = t.scene.addEmpty("flat", 0);
    EntityId entity = t.scene.addEmpty("entity", 0);
    t.scene.find(flat)->local.scale = glm::vec3(1, 0, 1);
    t.scene.find(entity)->local.position = glm::vec3(1, 2, 3);
    REQUIRE(t.scene.setParent(entity, flat, true));
    CHECK(allFinite(t.scene.find(entity)->local.matrix()));
    CHECK(allFinite(t.scene.worldTransform(entity)));
    CHECK(t.scene.find(entity)->local.position == glm::vec3(1, 2, 3));  // kept as it was

    // Scaled to 0 itself: the same
    EntityId parent = t.scene.addEmpty("parent", 0);
    t.scene.find(parent)->local.position = glm::vec3(5, 0, 0);
    EntityId zero = t.scene.addEmpty("zero", 0);
    t.scene.find(zero)->local.scale = glm::vec3(0.0f);
    REQUIRE(t.scene.setParent(zero, parent, true));
    CHECK(allFinite(t.scene.find(zero)->local.matrix()));
}

TEST_CASE("Scene: a hidden parent hides its children", "[Scene]") {
    TestScene t;
    EntityId parent = t.scene.addEmpty("parent", 0);
    EntityId child = t.scene.addEmpty("child", parent);
    CHECK(t.scene.visibleInWorld(child));
    t.scene.find(parent)->visible = false;
    CHECK_FALSE(t.scene.visibleInWorld(child));
    CHECK(t.scene.find(child)->visible);
}

TEST_CASE("Scene: built-in models go to the middle, each object referencing its model", "[Scene]") {
    TestScene t;
    // (The line-up along X is for model files: ModelLoaderTests, which writes one)
    EntityId a = t.scene.addObject(cube, false);
    EntityId b = t.scene.addObject(cube, false);
    CHECK(t.scene.find(a)->local.position == glm::vec3(0.0f));
    CHECK(t.scene.find(b)->local.position == glm::vec3(0.0f));
    CHECK(t.references(a) == 2);
    CHECK(t.scene.find(a)->name == "Cube");
}

TEST_CASE("Scene: duplicate copies the subtree next to the original, with model references", "[Scene]") {
    TestScene t;
    EntityId group = t.scene.addEmpty("group", 0);
    EntityId model = t.scene.addObject(cube, false);
    REQUIRE(t.scene.setParent(model, group, false));
    t.scene.find(model)->local.position = glm::vec3(0, 1, 0);
    EntityId after = t.scene.addEmpty("after", 0);
    t.scene.setPartHidden(model, 0, true);

    EntityId copy = t.scene.duplicate(group);
    CHECK(copy != group);
    const std::vector<EntityId>& roots = t.scene.roots();
    auto at = std::find(roots.begin(), roots.end(), group);
    REQUIRE(at + 1 != roots.end());
    CHECK(*(at + 1) == copy);  // right after the original
    CHECK(*(at + 2) == after);
    CHECK(t.scene.find(copy)->local.position.x == Scene::objectSpacing);
    REQUIRE(t.scene.find(copy)->children.size() == 1);
    EntityId modelCopy = t.scene.find(copy)->children[0];
    CHECK(modelCopy != model);
    CHECK(t.scene.find(modelCopy)->local.position == glm::vec3(0, 1, 0));
    REQUIRE(t.scene.model(modelCopy) != nullptr);
    CHECK(t.scene.model(modelCopy)->hiddenParts == t.scene.model(model)->hiddenParts);
    CHECK(t.references(model) == 2);
}

TEST_CASE("Scene: remove takes the subtree and releases its models", "[Scene]") {
    TestScene t;
    EntityId group = t.scene.addEmpty("group", 0);
    EntityId model = t.scene.addObject(cube, false);
    t.scene.setParent(model, group);
    ModelHandle handle = t.scene.model(model)->model;
    size_t count = t.scene.entities().size();
    t.scene.remove(group);
    CHECK(t.scene.entities().size() == count - 2);
    CHECK(t.scene.find(model) == nullptr);
    CHECK(t.scene.model(model) == nullptr);
    CHECK(t.assets.model(handle).references == 0);
    CHECK(t.assets.isLoaded(handle));  // kept unused
    // The index still finds the others
    for (const Entity& entity : t.scene.entities()) {
        CHECK(t.scene.find(entity.id) == &entity);
    }
}

TEST_CASE("Scene: portal pairs: removing one end removes the other; the rim is referenced per pair", "[Scene]") {
    TestScene t;
    EntityId pair = t.scene.addPortalPair();
    REQUIRE(t.scene.find(pair)->children.size() == 2);
    EntityId a = t.scene.find(pair)->children[0];
    EntityId b = t.scene.find(pair)->children[1];
    REQUIRE(t.scene.portal(a) != nullptr);
    CHECK(t.scene.portal(a)->other == b);
    CHECK(t.scene.portal(b)->other == a);
    CHECK(t.scene.portal(a)->endA != t.scene.portal(b)->endA);
    ModelHandle rim = t.scene.portalRimModel();
    CHECK(t.assets.model(rim).references == 1);

    EntityId second = t.scene.duplicate(pair);
    CHECK(t.assets.model(rim).references == 2);
    EntityId copyA = t.scene.find(second)->children[0];
    CHECK(t.scene.portal(copyA)->other == t.scene.find(second)->children[1]);

    t.scene.remove(b);
    CHECK(t.scene.find(a) == nullptr);
    CHECK(t.scene.find(pair) != nullptr);  // the group stays
    CHECK(t.assets.model(rim).references == 1);
}

TEST_CASE("Scene: a portal end duplicated alone is a plain entity", "[Scene]") {
    TestScene t;
    EntityId pair = t.scene.addPortalPair();
    EntityId a = t.scene.find(pair)->children[0];
    EntityId copy = t.scene.duplicate(a);
    CHECK(t.scene.portal(copy) == nullptr);
    CHECK(t.assets.model(t.scene.portalRimModel()).references == 1);
}

TEST_CASE("Scene: syncPortal copies shape, size and rim to the other end", "[Scene]") {
    TestScene t;
    EntityId pair = t.scene.addPortalPair();
    EntityId a = t.scene.find(pair)->children[0];
    EntityId b = t.scene.find(pair)->children[1];
    t.scene.portal(a)->shape = PortalShape::Ellipse;
    t.scene.portal(a)->size = {3, 4};
    t.scene.portal(a)->rimWidth = 0.2f;
    t.scene.syncPortal(a);
    CHECK(t.scene.portal(b)->shape == PortalShape::Ellipse);
    CHECK(t.scene.portal(b)->size == glm::vec2(3, 4));
    CHECK(t.scene.portal(b)->rimWidth == 0.2f);
    CHECK(t.scene.portal(b)->other == a);
}

TEST_CASE("Scene: clear starts over with the floor and releases everything", "[Scene]") {
    TestScene t;
    EntityId model = t.scene.addObject(cube, false);
    ModelHandle handle = t.scene.model(model)->model;
    t.scene.addPortalPair();
    ModelHandle rim = t.scene.portalRimModel();
    t.scene.clear();
    CHECK(t.scene.roots().size() == 1);
    CHECK(t.assets.model(handle).references == 0);
    CHECK(t.assets.model(rim).references == 0);
    CHECK(t.assets.model(t.assets.floor()).references == 1);
}
