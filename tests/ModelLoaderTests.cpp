//
// glTF loading (assets/ModelLoader.h) on a file written here: one triangle, its buffer inside the file (base64). And
// what the asset store and the scene do with a model file: background load, the fit to the standard size, the
// line-up of new objects.
//

#include <catch_amalgamated.hpp>
#include <cstring>
#include <fstream>
#include "TestHelpers.h"
#include "assets/ModelLoader.h"
#include "scene/Scene.h"

static std::string base64(const std::vector<uint8_t>& bytes) {
    static const char* digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string text;
    for (size_t i = 0; i < bytes.size(); i += 3) {
        uint32_t n = static_cast<uint32_t>(bytes[i]) << 16;
        if (i + 1 < bytes.size()) n |= static_cast<uint32_t>(bytes[i + 1]) << 8;
        if (i + 2 < bytes.size()) n |= bytes[i + 2];
        text += digits[(n >> 18) & 63];
        text += digits[(n >> 12) & 63];
        text += i + 1 < bytes.size() ? digits[(n >> 6) & 63] : '=';
        text += i + 2 < bytes.size() ? digits[n & 63] : '=';
    }
    return text;
}

// A triangle (0,0,0), (2,0,0), (0,4,0), placed by a node (nodeJson: its translation, scale, ...)
static std::string triangleGltf(const std::string& nodeJson) {
    std::vector<uint8_t> buffer(44, 0);
    float positions[9] = {0, 0, 0, 2, 0, 0, 0, 4, 0};
    uint16_t indices[3] = {0, 1, 2};
    std::memcpy(buffer.data(), positions, sizeof(positions));
    std::memcpy(buffer.data() + 36, indices, sizeof(indices));
    return R"({"asset": {"version": "2.0"},
  "buffers": [{"byteLength": 44, "uri": "data:application/octet-stream;base64,)" + base64(buffer) + R"("}],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}, {"buffer": 0, "byteOffset": 36, "byteLength": 6}],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [2, 4, 0]},
    {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}],
  "nodes": [{"mesh": 0)" + nodeJson + R"(}],
  "scenes": [{"nodes": [0]}], "scene": 0})";
}

static void write(const std::string& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary);
    file << text;
}

TEST_CASE("glTF: a triangle: one mesh, its indices, bounds and the default material", "[ModelLoader]") {
    TempFolder folder;
    write(folder.file("tri.gltf"), triangleGltf(""));
    Model model = loadGltfModel(folder.file("tri.gltf"));
    CHECK(model.name == "tri.gltf");
    REQUIRE(model.meshes.size() == 1);
    const Mesh& mesh = model.meshes[0];
    CHECK(mesh.vertices.size() == 3);
    CHECK(mesh.indices == std::vector<uint32_t>{0, 1, 2});
    CHECK(mesh.vertices[1].position == glm::vec3(2, 0, 0));
    CHECK(mesh.boundsMin == glm::vec3(0, 0, 0));
    CHECK(mesh.boundsMax == glm::vec3(2, 4, 0));
    REQUIRE(model.materials.size() == 1);  // the default one, added for the primitive without a material
    CHECK(mesh.materialIndex == 0);
    REQUIRE(model.instances.size() == 1);
    CHECK(model.boundsMax == glm::vec3(2, 4, 0));
}

TEST_CASE("glTF: the node's transform places the instance and the model's bounds", "[ModelLoader]") {
    TempFolder folder;
    write(folder.file("tri.gltf"), triangleGltf(R"(, "translation": [1, 0, 0], "scale": [2, 2, 2])"));
    Model model = loadGltfModel(folder.file("tri.gltf"));
    REQUIRE(model.instances.size() == 1);
    CHECK(nearlyEqual(glm::vec3(model.instances[0].transform * glm::vec4(2, 0, 0, 1)), {5, 0, 0}));
    CHECK(nearlyEqual(model.boundsMin, {1, 0, 0}));
    CHECK(nearlyEqual(model.boundsMax, {5, 8, 0}));
}

TEST_CASE("glTF: files that aren't models throw", "[ModelLoader]") {
    TempFolder folder;
    write(folder.file("broken.gltf"), "{ not json");
    CHECK_THROWS(loadGltfModel(folder.file("broken.gltf")));
    write(folder.file("empty.gltf"), R"({"asset": {"version": "2.0"}})");
    CHECK_THROWS(loadGltfModel(folder.file("empty.gltf")));
    CHECK_THROWS(loadGltfModel(folder.file("not there.gltf")));
}

TEST_CASE("AssetStore: a model file loads in the background, fitted to the standard size", "[AssetStore][ModelLoader]") {
    TestAssets test;
    write(test.folder.file("tri.gltf"), triangleGltf(R"(, "translation": [1, 0, 0], "scale": [2, 2, 2])"));
    test.assets.addFloor();
    ModelHandle model = test.assets.requestModel("tri.gltf");
    CHECK(test.assets.isLoading(model));
    CHECK(test.assets.currentLoad() == "tri.gltf");
    test.assets.acquire(model);
    waitForLoads(test.assets);
    REQUIRE(test.assets.isLoaded(model));
    const SceneModel& loaded = test.assets.model(model);
    CHECK(loaded.meshCount == 1);
    CHECK(loaded.parts[0].meshIndex == loaded.firstMesh);
    // Bounds (1,0,0)..(5,8,0): the largest side (8) becomes 2, the bottom center (3,0,0) the origin
    const glm::mat4 world = loaded.fit * loaded.parts[0].transform;
    CHECK(nearlyEqual(glm::vec3(world * glm::vec4(0, 4, 0, 1)), {-0.5f, 2.0f, 0.0f}));
    CHECK(nearlyEqual(glm::vec3(world * glm::vec4(0, 0, 0, 1)), {-0.5f, 0.0f, 0.0f}));
}

TEST_CASE("Scene: new objects from files line up 0, +1, -1, +2 spacings along X", "[Scene][ModelLoader]") {
    TestAssets test;
    write(test.folder.file("tri.gltf"), triangleGltf(""));
    Scene scene(test.assets);
    scene.createDefault();
    std::vector<float> x;
    for (int i = 0; i < 4; i++) {
        EntityId id = scene.addObject("tri.gltf", false);
        x.push_back(scene.find(id)->local.position.x);
        CHECK(scene.find(id)->name == "tri");
    }
    CHECK(x == std::vector<float>{0.0f, Scene::objectSpacing, -Scene::objectSpacing, 2 * Scene::objectSpacing});
    // They show up once the file has arrived
    CHECK(scene.loading());
    RenderList list;
    scene.buildRenderList(list);
    size_t floorOnly = list.instances.size();
    waitForLoads(test.assets);
    scene.update();
    CHECK_FALSE(scene.loading());
    scene.buildRenderList(list);
    CHECK(list.instances.size() == floorOnly + 4);
}
