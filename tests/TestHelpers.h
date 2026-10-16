//
// What several test files share: a scratch folder, waiting for the asset store's background loads, comparing
// matrices.
//

#ifndef VULKANDEMO_TESTHELPERS_H
#define VULKANDEMO_TESTHELPERS_H

#include <catch_amalgamated.hpp>
#include <chrono>
#include <filesystem>
#include <random>
#include <string>
#include <thread>
#include <glm/glm.hpp>
#include "assets/AssetStore.h"

// A folder of its own under the system's temp folder, removed with everything in it afterwards
struct TempFolder {
    std::filesystem::path path;

    TempFolder() {
        std::random_device random;
        path = std::filesystem::temp_directory_path() / ("vulkanDemo_tests_" + std::to_string(random()));
        std::filesystem::create_directories(path);
    }
    ~TempFolder() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    TempFolder(const TempFolder&) = delete;
    TempFolder& operator=(const TempFolder&) = delete;

    [[nodiscard]] std::string file(const std::string& name) const { return (path / name).generic_string(); }
};

// An asset store whose model and environment folders are an empty scratch folder: only built-in models, and files
// the test writes there
struct TestAssets {
    TempFolder folder;
    AssetStore assets{folder.path.generic_string(), folder.path.generic_string()};
};

// Runs AssetStore::update until no load is running or waiting (at most 20 s)
inline void waitForLoads(AssetStore& assets) {
    auto start = std::chrono::steady_clock::now();
    while (!assets.currentLoad().empty() || assets.waitingLoads() > 0) {
        assets.update();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        REQUIRE(std::chrono::steady_clock::now() - start < std::chrono::seconds(20));
    }
}

inline bool nearlyEqual(const glm::mat4& a, const glm::mat4& b, float tolerance = 1e-4f) {
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 4; r++) {
            if (std::abs(a[c][r] - b[c][r]) > tolerance) {
                return false;
            }
        }
    }
    return true;
}

inline bool nearlyEqual(const glm::vec3& a, const glm::vec3& b, float tolerance = 1e-4f) {
    return std::abs(a.x - b.x) <= tolerance && std::abs(a.y - b.y) <= tolerance && std::abs(a.z - b.z) <= tolerance;
}

inline bool allFinite(const glm::mat4& m) {
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 4; r++) {
            if (!std::isfinite(m[c][r])) {
                return false;
            }
        }
    }
    return true;
}

#endif //VULKANDEMO_TESTHELPERS_H
