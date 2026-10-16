//
// Scene files (scene/SceneFile.cpp): what is saved reads back the same; edits to models' materials and lights;
// files that are broken or from elsewhere.
//

#include <catch_amalgamated.hpp>
#include <fstream>
#include <sstream>
#include "TestHelpers.h"
#include "scene/Scene.h"

static const std::string cube = "Cube";
static const std::string cornellBox = "Cornell box";
static const std::string lampBox = "Cornell box (blocks, point light)";

namespace {  // (each test file has its own TestScene)

struct TestScene : TestAssets {
    Scene scene{assets, settings};
    TestScene() { scene.createDefault(); }

    void write(const std::string& name, const std::string& text) const {
        std::ofstream file(folder.file(name), std::ios::binary);
        file << text;
    }
    [[nodiscard]] std::string read(const std::string& name) const {
        std::ifstream file(folder.file(name), std::ios::binary);
        std::stringstream text;
        text << file.rdbuf();
        return text.str();
    }
};

}  // namespace

static const char* header = R"({"format": "vulkanDemo scene", "version": 1, )";

TEST_CASE("Scene file: a saved scene reads back the same", "[SceneFile]") {
    TestScene t;
    Scene& scene = t.scene;
    EntityId group = scene.addEmpty("Group", 0);
    EntityId box = scene.addObject(cornellBox, false);
    scene.setParent(box, group);
    scene.find(box)->local.position = {0.3f, -1.25f, 7.0f};
    scene.find(box)->local.rotation = eulerToQuat({10.0f, 20.0f, 30.0f});
    scene.find(box)->local.scale = {0.1f, 2.0f, 3.0f};
    scene.find(box)->uniformScale = false;
    scene.setPartHidden(box, 1, true);
    scene.setPartHidden(box, 0, !t.assets.model(scene.model(box)->model).parts[0].hiddenByDefault);
    EntityId hidden = scene.addObject(cube, false);
    scene.find(hidden)->visible = false;
    EntityId pair = scene.addPortalPair();
    EntityId end = scene.find(pair)->children[0];
    scene.portal(end)->shape = PortalShape::Ellipse;
    scene.portal(end)->size = {1.5f, 2.5f};
    scene.syncPortal(end);
    scene.camera().position = {1, 2, 3};
    scene.camera().yaw = 33.0f;
    scene.camera().pitch = -12.5f;
    scene.camera().fov = 75.0f;
    scene.environment().sunAzimuth = 123.0f;
    scene.environment().background = 2;
    scene.environment().skyZenithColor = {0.1f, 0.2f, 0.3f};

    std::string path = t.folder.file("scene.json");
    scene.save(path);
    Scene loaded(t.assets, t.settings);
    std::vector<std::string> warnings = loaded.load(path);
    CHECK(warnings.empty());
    CHECK(loaded.contentText() == scene.contentText());
    CHECK(loaded.camera().position == scene.camera().position);
    CHECK(loaded.camera().yaw == scene.camera().yaw);
    CHECK(loaded.camera().fov == scene.camera().fov);
    REQUIRE(loaded.find(box) != nullptr);
    CHECK(loaded.find(box)->local.rotation == scene.find(box)->local.rotation);  // exactly: shortest round-trip numbers
    CHECK(loaded.model(box)->hiddenParts == scene.model(box)->hiddenParts);
    CHECK(loaded.portal(end)->shape == PortalShape::Ellipse);
    CHECK(loaded.portal(scene.portal(end)->other)->size == glm::vec2(1.5f, 2.5f));

    // Saved again: the same text (camera included)
    loaded.save(t.folder.file("again.json"));
    CHECK(t.read("again.json") == t.read("scene.json"));

    // New entities go on after the file's ids
    EntityId next = loaded.addEmpty("new", 0);
    for (const Entity& entity : scene.entities()) {
        CHECK(next > entity.id);
    }
}

TEST_CASE("Scene file: numbers are written short (0.3, not 0.30000001)", "[SceneFile]") {
    TestScene t;
    EntityId e = t.scene.addEmpty("e", 0);
    t.scene.find(e)->local.position = {0.3f, 0.1f, 1e-7f};
    std::string text = t.scene.contentText();
    CHECK(text.find("[0.3,0.1,1e-07]") != std::string::npos);
}

TEST_CASE("Scene file: saving over a file replaces it", "[SceneFile]") {
    TestScene t;
    std::string path = t.folder.file("scenes/sub/scene.json");  // folders are made
    t.scene.save(path);
    t.scene.addEmpty("more", 0);
    t.scene.save(path);
    Scene loaded(t.assets, t.settings);
    loaded.load(path);
    CHECK(loaded.contentText() == t.scene.contentText());
    CHECK_FALSE(std::filesystem::exists(path + ".saving"));
}

TEST_CASE("Scene file: material and light edits are saved and put back on load", "[SceneFile]") {
    TestScene t;
    EntityId lamp = t.scene.addObject(lampBox, false);
    SceneModel& model = t.assets.model(t.scene.model(lamp)->model);
    SceneMaterial& material = t.assets.materials()[model.firstMaterial + 1];
    float roughness = material.material.roughness;
    material.material.roughness = 0.125f;
    material.useBaseColorTexture = false;
    float intensity = model.lights[0].intensity;
    model.lights[0].intensity = intensity * 4.0f;
    std::string path = t.folder.file("edits.json");
    t.scene.save(path);
    CHECK(t.read("edits.json").find("\"roughness\":0.125") != std::string::npos);

    t.assets.resetEdits();
    REQUIRE(material.material.roughness == roughness);
    Scene loaded(t.assets, t.settings);
    CHECK(loaded.load(path).empty());
    CHECK(t.assets.materials()[model.firstMaterial + 1].material.roughness == 0.125f);
    CHECK_FALSE(t.assets.materials()[model.firstMaterial + 1].useBaseColorTexture);
    CHECK(model.lights[0].intensity == intensity * 4.0f);
    // Only what differs from the model is written
    CHECK(t.read("edits.json").find("\"metallic\"") == std::string::npos);
}

TEST_CASE("Scene file: a file that can't be read leaves the scene as it was", "[SceneFile]") {
    TestScene t;
    t.scene.addEmpty("kept", 0);
    std::string before = t.scene.contentText();
    SECTION("not JSON") { t.write("bad.json", "{ not json"); }
    SECTION("not a scene file") { t.write("bad.json", R"({"format": "something else"})"); }
    SECTION("a newer version") { t.write("bad.json", R"({"format": "vulkanDemo scene", "version": 99})"); }
    SECTION("a repeated id") {
        t.write("bad.json", std::string(header) + R"("entities": [{"id": 1}, {"id": 1}]})");
    }
    SECTION("a wrong type") {
        t.write("bad.json", std::string(header) + R"("entities": [{"id": 1, "position": [1, "x", 3]}]})");
    }
    CHECK_THROWS(t.scene.load(t.folder.file("bad.json")));
    CHECK(t.scene.contentText() == before);
    CHECK(t.assets.model(t.assets.floor()).references == 1);  // nothing acquired by the failed load stays acquired
}

TEST_CASE("Scene file: a parent listed after its child puts the child at the top", "[SceneFile]") {
    TestScene t;
    t.write("order.json", std::string(header) + R"("entities": [{"id": 2, "parent": 1}, {"id": 1}]})");
    std::vector<std::string> warnings = t.scene.load(t.folder.file("order.json"));
    CHECK(warnings.size() == 1);
    CHECK(t.scene.find(2)->parent == 0);
}

TEST_CASE("Scene file: portal ends that don't name each other become plain entities", "[SceneFile]") {
    TestScene t;
    t.write("portal.json", std::string(header) +
                           R"("entities": [{"id": 1, "portal": {"other": 2, "endA": true}},
                                           {"id": 2, "portal": {"other": 3, "endA": false}}, {"id": 3}]})");
    std::vector<std::string> warnings = t.scene.load(t.folder.file("portal.json"));
    CHECK(warnings.size() == 2);
    CHECK(t.scene.portal(1) == nullptr);
    CHECK(t.scene.portal(2) == nullptr);
}

TEST_CASE("Scene file: a model file that can't be loaded is kept as it was written", "[SceneFile]") {
    TestScene t;
    t.write("missing.json", std::string(header) +
                            R"("entities": [{"id": 1, "name": "Gone", "model": {"file": "missing.glb", "hide": [3]}}]})");
    CHECK(t.scene.load(t.folder.file("missing.json")).empty());  // still loading: nothing known yet
    waitForLoads(t.assets);
    t.scene.update();
    std::vector<SceneMessage> messages = t.scene.takeMessages();
    REQUIRE(messages.size() == 1);
    CHECK(messages[0].level == SceneMessage::Level::Error);
    REQUIRE(t.scene.model(1) != nullptr);
    CHECK(t.scene.model(1)->missingFile == "missing.glb");
    CHECK(t.scene.contentText().find(R"("model":{"file":"missing.glb","hide":[3]})") != std::string::npos);
}

// A "builtIn" name that isn't one is refused without asking the store for it: asking would start loading a model
// file of that name in the background
TEST_CASE("Scene file: an unknown built-in model is refused without starting a load", "[SceneFile]") {
    TestScene t;
    t.write("builtin.json", std::string(header) + R"("entities": [{"id": 1, "model": {"builtIn": "No such"}}]})");
    std::vector<std::string> warnings = t.scene.load(t.folder.file("builtin.json"));
    CHECK(warnings.size() == 1);
    CHECK(t.assets.currentLoad().empty());
    CHECK(t.assets.waitingLoads() == 0);
    REQUIRE(t.scene.model(1) != nullptr);
    CHECK(t.scene.model(1)->missingFile == "No such");
    CHECK(t.scene.contentText().find(R"("builtIn":"No such")") != std::string::npos);
}

// The floor is made by Scene::createDefault; a scene file read into a store that hasn't made it yet must still get it
TEST_CASE("Scene file: the floor loads into a store that hasn't made it yet", "[SceneFile]") {
    TestAssets test;
    Scene scene(test.assets, test.settings);
    std::ofstream(test.folder.file("floor.json"))
            << header << R"("entities": [{"id": 1, "name": "Floor", "model": {"builtIn": "Floor"}}]})";
    CHECK(scene.load(test.folder.file("floor.json")).empty());
    REQUIRE(scene.model(1) != nullptr);
    CHECK(test.assets.isLoaded(scene.model(1)->model));
    CHECK(scene.model(1)->model == test.assets.floor());
    CHECK_NOTHROW(scene.contentText());
}

TEST_CASE("Scene file: a rotation typed in by hand is normalized", "[SceneFile]") {
    TestScene t;
    t.write("rotation.json", std::string(header) + R"("entities": [{"id": 1, "rotation": [0, 2, 0, 0]}]})");
    t.scene.load(t.folder.file("rotation.json"));
    glm::quat q = t.scene.find(1)->local.rotation;
    CHECK(glm::length(q) == Catch::Approx(1.0f));
    CHECK(q.y == Catch::Approx(1.0f));
}

TEST_CASE("Scene file: the scene's render settings are saved, only those that differ from the defaults", "[SceneFile][Settings]") {
    TestScene t;
    t.settings.exposure = 2.5f;
    t.settings.maxBounces = 3;
    t.settings.renderMode = static_cast<int>(RenderMode::DlssDlaa);  // the program's: not saved with the scene
    std::string text = t.scene.contentText();
    CHECK(text.find(R"("settings": {"maxBounces":3,"exposure":2.5})") != std::string::npos);
    CHECK(text.find("renderMode") == std::string::npos);
}

TEST_CASE("Scene file: opening a scene sets its settings and the others to the defaults, not the program's", "[SceneFile][Settings]") {
    TestScene t;
    t.settings.exposure = 2.5f;
    std::string path = t.folder.file("exposure.json");
    t.scene.save(path);

    t.settings.exposure = 1.0f;
    t.settings.maxBounces = 3;  // not in the file: back to the default
    t.settings.renderMode = static_cast<int>(RenderMode::DlssQuality);
    t.settings.samplesPerPixel = 4;
    CHECK(t.scene.load(path).empty());
    CHECK(t.settings.exposure == 2.5f);
    CHECK(t.settings.maxBounces == RenderSettings{}.maxBounces);
    CHECK(t.settings.renderMode == static_cast<int>(RenderMode::DlssQuality));
    CHECK(t.settings.samplesPerPixel == 4);
}

TEST_CASE("Scene file: a scene that can't be opened leaves the settings as they were", "[SceneFile][Settings]") {
    TestScene t;
    t.settings.exposure = 3.0f;
    t.write("bad.json", std::string(header) + R"("settings": {"exposure": 9}, "entities": [{"id": 1}, {"id": 1}]})");
    CHECK_THROWS(t.scene.load(t.folder.file("bad.json")));
    CHECK(t.settings.exposure == 3.0f);
}

TEST_CASE("Scene file: settings that can't be read are skipped with a warning", "[SceneFile][Settings]") {
    TestScene t;
    t.write("odd.json", std::string(header) +
                        R"("settings": {"exposure": "bright", "maxBounces": 2, "renderMode": "DlssDlaa", "nope": 1}})");
    std::vector<std::string> warnings = t.scene.load(t.folder.file("odd.json"));
    CHECK(warnings.size() == 3);  // the wrong type, the program's setting, the unknown one
    CHECK(t.settings.maxBounces == 2);
    CHECK(t.settings.exposure == RenderSettings{}.exposure);
    CHECK(t.settings.renderMode == RenderSettings{}.renderMode);
}

TEST_CASE("Scene: a new scene starts from the default scene settings, the program's stay", "[Scene][Settings]") {
    TestScene t;
    t.settings.exposure = 2.0f;
    t.settings.outputSize = 2;
    t.scene.clear();
    CHECK(t.settings.exposure == RenderSettings{}.exposure);
    CHECK(t.settings.outputSize == 2);
}

TEST_CASE("Scene file: a changed setting is a change to save (the unsaved mark)", "[SceneFile][Settings]") {
    TestScene t;
    std::string before = t.scene.contentText();
    t.settings.debugView = 3;  // the program's
    CHECK(t.scene.contentText() == before);
    t.settings.toneMapping = false;
    CHECK(t.scene.contentText() != before);
}
