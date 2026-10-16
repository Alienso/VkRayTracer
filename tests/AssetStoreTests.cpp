//
// AssetStore (assets/AssetStore.h): handles and their generations, references, the unused models it keeps and
// unloads (oldest first, indices of the others shifted down), failed loads.
//

#include <catch_amalgamated.hpp>
#include <set>
#include "TestHelpers.h"
#include "assets/BuiltInModels.h"

static const std::string cube = "Cube";
static const std::string cornellBox = "Cornell box";
static const std::string lampBox = "Cornell box (blocks, point light)";

TEST_CASE("Handles: a default handle is invalid; equal only with the same index and generation", "[Handles]") {
    ModelHandle none;
    CHECK_FALSE(none.valid());
    ModelHandle a{3, 1}, b{3, 1}, c{3, 2};
    CHECK(a.valid());
    CHECK(a == b);
    CHECK(a != c);
}

TEST_CASE("AssetStore: the floor is model 0 and is never unloaded", "[AssetStore]") {
    TestAssets test;
    AssetStore& assets = test.assets;
    ModelHandle floor = assets.addFloor();
    CHECK(floor == assets.floor());
    CHECK(floor.index == 0);
    REQUIRE(assets.isLoaded(floor));
    CHECK(assets.meshes().size() == 1);
    CHECK(assets.materials().size() == 1);
    assets.acquire(floor);
    assets.release(floor);
    assets.setUnusedBudget(0);
    CHECK(assets.isLoaded(floor));
    CHECK(assets.unusedModelCount() == 0);  // pinned: not counted as unused
}

TEST_CASE("AssetStore: built-in models are made at once and asked for again give the same handle", "[AssetStore]") {
    TestAssets test;
    AssetStore& assets = test.assets;
    assets.addFloor();
    uint64_t revision = assets.revision();
    ModelHandle a = assets.requestModel(cube);
    CHECK(assets.isLoaded(a));
    CHECK_FALSE(assets.isLoading(a));
    CHECK(assets.revision() > revision);
    CHECK(assets.requestModel(cube) == a);
    CHECK(assets.model(a).references == 0);  // requestModel takes no reference
    CHECK(assets.model(a).memorySize > 0);
    CHECK(assets.currentLoad().empty());
}

TEST_CASE("AssetStore: a released model stays loaded (unused) until the budget needs its room", "[AssetStore]") {
    TestAssets test;
    AssetStore& assets = test.assets;
    assets.addFloor();
    ModelHandle model = assets.requestModel(cube);
    assets.acquire(model);
    assets.acquire(model);
    assets.release(model);
    CHECK(assets.unusedModelCount() == 0);
    assets.release(model);
    CHECK(assets.isLoaded(model));
    CHECK(assets.unusedModelCount() == 1);
    CHECK(assets.unusedMemory() == assets.model(model).memorySize);

    uint64_t revision = assets.revision();
    assets.setUnusedBudget(0);
    CHECK_FALSE(assets.isLoaded(model));  // the handle is stale now
    CHECK(assets.unusedModelCount() == 0);
    CHECK(assets.revision() > revision);
    CHECK(assets.meshes().size() == 1);  // the floor's

    // A new load may reuse the slot; the old handle stays stale
    ModelHandle again = assets.requestModel(cube);
    CHECK(again != model);
    CHECK_FALSE(assets.isLoaded(model));
    CHECK(assets.isLoaded(again));
}

TEST_CASE("AssetStore: unused models go oldest released first", "[AssetStore]") {
    TestAssets test;
    AssetStore& assets = test.assets;
    assets.addFloor();
    ModelHandle first = assets.requestModel(cube);
    ModelHandle second = assets.requestModel(cornellBox);
    assets.acquire(first);
    assets.acquire(second);
    assets.release(second);  // released first: the oldest
    assets.release(first);
    assets.setUnusedBudget(assets.model(first).memorySize);  // room for one of them
    CHECK_FALSE(assets.isLoaded(second));
    CHECK(assets.isLoaded(first));
}

TEST_CASE("AssetStore: unloading moves the later models' meshes, materials and parts down", "[AssetStore]") {
    TestAssets test;
    AssetStore& assets = test.assets;
    assets.addFloor();
    ModelHandle gone = assets.requestModel(cube);
    ModelHandle kept = assets.requestModel(cornellBox);
    assets.acquire(gone);
    assets.acquire(kept);

    // What the kept model's parts show before: mesh names, their materials' names, the mesh ids
    auto describe = [&]() {
        std::vector<std::string> parts;
        const SceneModel& model = assets.model(kept);
        for (const MeshInstance& part : model.parts) {
            const Mesh& mesh = assets.meshes().at(part.meshIndex);
            parts.push_back(mesh.name + "/" + assets.materials().at(mesh.materialIndex).material.name + "/" +
                            std::to_string(assets.meshIds().at(part.meshIndex)));
        }
        return parts;
    };
    std::vector<std::string> before = describe();
    uint32_t firstMesh = assets.model(kept).firstMesh;
    uint32_t goneMeshes = assets.model(gone).meshCount;
    uint32_t goneMaterials = assets.model(gone).materialCount;
    uint32_t firstMaterial = assets.model(kept).firstMaterial;

    assets.release(gone);
    assets.setUnusedBudget(0);
    REQUIRE_FALSE(assets.isLoaded(gone));
    REQUIRE(assets.isLoaded(kept));
    CHECK(assets.model(kept).firstMesh == firstMesh - goneMeshes);
    CHECK(assets.model(kept).firstMaterial == firstMaterial - goneMaterials);
    CHECK(describe() == before);
    CHECK(assets.meshes().size() == assets.meshIds().size());
    for (const Mesh& mesh : assets.meshes()) {
        CHECK(mesh.materialIndex < assets.materials().size());
    }
}

TEST_CASE("AssetStore: mesh ids are never given to another mesh", "[AssetStore]") {
    TestAssets test;
    AssetStore& assets = test.assets;
    assets.addFloor();
    std::set<uint64_t> seen(assets.meshIds().begin(), assets.meshIds().end());
    for (int round = 0; round < 3; round++) {
        ModelHandle model = assets.requestModel(cube);
        assets.acquire(model);
        for (uint32_t i = 0; i < assets.model(model).meshCount; i++) {
            CHECK(seen.insert(assets.meshIds()[assets.model(model).firstMesh + i]).second);
        }
        assets.release(model);
        assets.setUnusedBudget(0);
        assets.setUnusedBudget(AssetStore::defaultUnusedBudget);
    }
}

TEST_CASE("AssetStore: a file that can't be loaded fails; its slot goes with the last reference", "[AssetStore]") {
    TestAssets test;
    AssetStore& assets = test.assets;
    assets.addFloor();
    ModelHandle model = assets.requestModel("missing.glb");
    CHECK(assets.isLoading(model));
    CHECK(assets.requestModel("missing.glb") == model);  // the same load
    assets.acquire(model);
    waitForLoads(assets);
    CHECK_FALSE(assets.isLoading(model));
    CHECK_FALSE(assets.isLoaded(model));
    CHECK_FALSE(assets.loadError(model).empty());
    CHECK(assets.modelName(model) == "missing.glb");
    assets.release(model);
    CHECK(assets.loadError(model).empty());  // freed: the handle is stale
}

TEST_CASE("AssetStore: a load nobody wants any more when it ends goes away", "[AssetStore]") {
    TestAssets test;
    AssetStore& assets = test.assets;
    assets.addFloor();
    ModelHandle model = assets.requestModel("missing.glb");
    assets.acquire(model);
    assets.release(model);  // while loading
    waitForLoads(assets);
    CHECK_FALSE(assets.isLoaded(model));
    CHECK(assets.loadError(model).empty());
}

TEST_CASE("AssetStore: an environment map that can't be loaded fails, and goes with its last reference", "[AssetStore]") {
    TestAssets test;
    AssetStore& assets = test.assets;
    EnvironmentHandle map = assets.requestEnvironment("missing.hdr");
    CHECK(assets.isLoading(map));
    assets.acquire(map);
    waitForLoads(assets);
    CHECK_FALSE(assets.isLoaded(map));
    CHECK_FALSE(assets.loadError(map).empty());
    assets.release(map);
    CHECK(assets.loadError(map).empty());
}

TEST_CASE("AssetStore: resetEdits puts the file's materials and lights back", "[AssetStore]") {
    TestAssets test;
    AssetStore& assets = test.assets;
    assets.addFloor();
    ModelHandle model = assets.requestModel(lampBox);
    SceneModel& lamp = assets.model(model);
    REQUIRE_FALSE(lamp.lights.empty());
    float roughness = assets.materials()[lamp.firstMaterial].material.roughness;
    float intensity = lamp.lights[0].intensity;
    assets.materials()[lamp.firstMaterial].material.roughness = roughness * 0.5f + 0.1f;
    assets.materials()[lamp.firstMaterial].useNormalTexture = false;
    lamp.lights[0].intensity = intensity * 3.0f;
    uint64_t materialRevision = assets.materialRevision();
    assets.resetEdits();
    CHECK(assets.materials()[lamp.firstMaterial].material.roughness == roughness);
    CHECK(assets.materials()[lamp.firstMaterial].useNormalTexture);
    CHECK(assets.model(model).lights[0].intensity == intensity);
    CHECK(assets.materialRevision() > materialRevision);
}
