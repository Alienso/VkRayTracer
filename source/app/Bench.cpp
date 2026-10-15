//
// Benches: see Bench.h.
//

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include "app/Bench.h"
#include "core/Configuration.h"
#include "renderer/RenderSettingsFile.h"

static const char* passNames[6] = {"cache", "restir", "trace", "dlss", "exposure", "display"};

static glm::vec3 readVec3(const nlohmann::json& json, const char* what) {
    if (!json.is_array() || json.size() != 3 || !json[0].is_number() || !json[1].is_number() || !json[2].is_number()) {
        throw std::runtime_error(std::string(what) + ": expected [x, y, z]");
    }
    return {json[0].get<float>(), json[1].get<float>(), json[2].get<float>()};
}

static int readInt(const nlohmann::json& json, const char* what, int minimum) {
    if (!json.is_number_integer() || json.get<int>() < minimum) {
        throw std::runtime_error(std::string(what) + ": expected a whole number, at least " + std::to_string(minimum));
    }
    return json.get<int>();
}

// Throws for keys the object shouldn't have (a typo would otherwise be a silently different bench)
static void checkKeys(const nlohmann::json& json, const char* what, std::set<std::string> allowed) {
    if (!json.is_object()) {
        throw std::runtime_error(std::string(what) + ": expected an object");
    }
    for (auto it = json.begin(); it != json.end(); ++it) {
        if (allowed.count(it.key()) == 0) {
            throw std::runtime_error(std::string(what) + ": unknown key \"" + it.key() + "\"");
        }
    }
}

// A render mode by name ("DlssDlaa") or number, as in the settings
static int readRenderMode(const nlohmann::json& json) {
    RenderSettings settings;
    std::vector<std::string> problems = applyRenderSettingsJson(settings, {{"renderMode", json}});
    if (!problems.empty()) {
        throw std::runtime_error("noise: mode: " + problems[0]);
    }
    return settings.renderMode;
}

Bench::Bench(const std::string& path, std::string outputDirectory)
    : benchPath(path), outputDirectory(std::move(outputDirectory)), startTime(std::chrono::steady_clock::now()) {
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("bench: cannot open " + path);
    }
    try {
        fileContent = nlohmann::json::parse(file);
        name = std::filesystem::path(path).stem().string();
        checkKeys(fileContent, "bench", {"scene", "settings", "camera", "path", "deltaTime", "warmupFrames", "frames",
                                         "captures", "noise", "description"});
        const nlohmann::json& json = fileContent;
        if (json.contains("scene")) {
            // Relative to the bench file
            std::filesystem::path scene = std::filesystem::path(path).parent_path() / json["scene"].get<std::string>();
            scenePath = scene.lexically_normal().string();
        }
        if (json.contains("settings")) {
            settingsJson = json["settings"];
            RenderSettings check;
            std::vector<std::string> problems = applyRenderSettingsJson(check, settingsJson);
            if (!problems.empty()) {
                std::string list;
                for (const std::string& problem : problems) {
                    list += "\n  " + problem;
                }
                throw std::runtime_error("settings:" + list);
            }
            requestedRenderMode = check.renderMode;
        } else {
            requestedRenderMode = RenderSettings{}.renderMode;
        }
        auto readKey = [](const nlohmann::json& key, const char* what, bool withFrame) {
            checkKeys(key, what, {"frame", "position", "target", "yaw", "pitch", "fov"});
            CameraKey camera;
            if (withFrame) {
                camera.frame = readInt(key.at("frame"), "path: frame", 1);
            }
            camera.position = readVec3(key.at("position"), what);
            if (key.contains("target")) {
                camera.target = readVec3(key["target"], what);
            } else {
                camera.yaw = key.value("yaw", 0.0f);
                camera.pitch = key.value("pitch", 0.0f);
            }
            if (key.contains("fov")) {
                camera.fov = key["fov"].get<float>();
            }
            return camera;
        };
        if (json.contains("camera") && json.contains("path")) {
            throw std::runtime_error("camera and path: give one of them");
        }
        if (json.contains("camera")) {
            cameraKeys.push_back(readKey(json["camera"], "camera", false));
        }
        if (json.contains("path")) {
            for (const nlohmann::json& key : json["path"]) {
                cameraKeys.push_back(readKey(key, "path", true));
            }
            if (cameraKeys.empty()) {
                throw std::runtime_error("path: no keyframes");
            }
            std::sort(cameraKeys.begin(), cameraKeys.end(), [](const CameraKey& a, const CameraKey& b) { return a.frame < b.frame; });
        }
        fixedDeltaTime = json.value("deltaTime", fixedDeltaTime);
        if (json.contains("warmupFrames")) {
            warmupFrames = readInt(json["warmupFrames"], "warmupFrames", 0);
        }
        if (json.contains("frames")) {
            measuredFrames = readInt(json["frames"], "frames", 0);
        }
        if (json.contains("captures")) {
            for (const nlohmann::json& capture : json["captures"]) {
                captureFrames.push_back(readInt(capture, "captures", 1));
            }
        }
        if (json.contains("noise")) {
            const nlohmann::json& n = json["noise"];
            checkKeys(n, "noise", {"mode", "referenceFrames", "frames", "region"});
            NoiseRun run;
            run.mode = n.contains("mode") ? readRenderMode(n["mode"]) : requestedRenderMode;
            if (n.contains("referenceFrames")) {
                run.referenceFrames = readInt(n["referenceFrames"], "noise: referenceFrames", 1);
            }
            if (n.contains("frames")) {
                run.frames = readInt(n["frames"], "noise: frames", 1);
            }
            if (n.contains("region")) {
                const nlohmann::json& r = n["region"];
                if (!r.is_array() || r.size() != 4) {
                    throw std::runtime_error("noise: region: expected [x0, y0, x1, y1]");
                }
                run.region = glm::ivec4(r[0].get<int>(), r[1].get<int>(), r[2].get<int>(), r[3].get<int>());
            }
            noise = run;
        }
    } catch (const nlohmann::json::exception& e) {
        throw std::runtime_error("bench " + path + ": " + e.what());
    } catch (const std::runtime_error& e) {
        throw std::runtime_error("bench " + path + ": " + e.what());
    }
    std::filesystem::create_directories(this->outputDirectory);
    // Results of an earlier run of this bench would otherwise look like this run's if it fails early
    std::filesystem::remove(std::filesystem::path(this->outputDirectory) / "results.json");
}

void Bench::start(Renderer& renderer) {
    RenderSettings settings = renderer.renderSettings();
    if (!settingsJson.is_null()) {
        applyRenderSettingsJson(settings, settingsJson);  // checked when the file was read
    }
    renderer.setRenderSettings(settings);
}

// The camera at this frame: the keyframes around it, blended
void Bench::setCamera(Camera& camera) const {
    if (cameraKeys.empty()) {
        return;
    }
    size_t next = 0;
    while (next < cameraKeys.size() && cameraKeys[next].frame <= frame) {
        next++;
    }
    const CameraKey& a = cameraKeys[next == 0 ? 0 : next - 1];
    const CameraKey& b = cameraKeys[std::min(next, cameraKeys.size() - 1)];
    float t = b.frame > a.frame ? std::clamp(static_cast<float>(frame - a.frame) / static_cast<float>(b.frame - a.frame), 0.0f, 1.0f) : 0.0f;
    camera.position = glm::mix(a.position, b.position, t);
    if (a.target && b.target) {
        camera.lookAt(glm::mix(*a.target, *b.target, t));
    } else {
        camera.yaw = glm::mix(a.yaw, b.yaw, t);
        camera.pitch = glm::mix(a.pitch, b.pitch, t);
    }
    if (a.fov && b.fov) {
        camera.fov = glm::mix(*a.fov, *b.fov, t);
    }
}

// The frame after which nothing more is measured: the GPU times arrive maxFramesInFlight frames late, a capture is
// written at the start of the frame after it
int Bench::lastFrame() const {
    int last = warmupFrames + measuredFrames + Configuration::maxFramesInFlight;
    for (int capture : captureFrames) {
        last = std::max(last, capture + 1);
    }
    return last;
}

void Bench::fail(const std::string& message) {
    if (error.empty()) {
        error = message;
        std::cerr << "bench: " << message << std::endl;
    }
}

void Bench::beforeRender(Scene& scene, Renderer& renderer, const AssetStore& assets) {
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count();
    if (firstFrameSeconds < 0.0) {
        firstFrameSeconds = seconds;
    }
    if (frame == 0 && assets.currentLoad().empty() && assets.waitingLoads() == 0) {
        loadedSeconds = seconds;  // this frame uploads the last of it
        std::cout << "bench: loaded after " << seconds << " s" << std::endl;
    }
    if (frame > 0 || loadedSeconds >= 0.0) {
        frame++;
    }
    if (frame == 2) {
        readySeconds = seconds;
    }
    setCamera(scene.camera());
    if (frame == 0) {
        return;
    }
    if (std::find(captureFrames.begin(), captureFrames.end(), frame) != captureFrames.end()) {
        std::string file = (std::filesystem::path(outputDirectory) / ("capture_" + std::to_string(frame) + ".png")).string();
        renderer.requestCapture(file);
        captures.push_back(file);
    }
    if (frame == warmupFrames + 1) {
        if (renderer.renderSettings().renderMode != requestedRenderMode) {
            fail("the render mode asked for is not available (DLSS: --no-dlss, no NVIDIA RTX GPU, or no Streamline DLLs)");
        }
        measureStart = std::chrono::steady_clock::now();
    }
    if (frame == warmupFrames + measuredFrames + 1) {
        if (measuredFrames > 0) {
            frameMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - measureStart).count() / measuredFrames;
        }
        PROCESS_MEMORY_COUNTERS_EX counters{};
        counters.cb = sizeof(counters);
        if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
            memory["privateBytes"] = static_cast<uint64_t>(counters.PrivateUsage);
            memory["workingSetBytes"] = static_cast<uint64_t>(counters.WorkingSetSize);
        }
        GpuMemory gpu = renderer.gpuMemory();
        memory["gpuUsedBytes"] = static_cast<uint64_t>(gpu.used);
        memory["gpuBudgetBytes"] = static_cast<uint64_t>(gpu.budget);
        memory["gpuMemoryObjects"] = gpu.memoryObjects;
        if (noise) {
            renderer.startNoiseRun(noise->mode, noise->referenceFrames, noise->frames, noise->region);
            noiseStarted = true;
        }
    }
}

bool Bench::afterRender(Renderer& renderer) {
    if (!error.empty()) {
        writeResults(renderer);
        return true;
    }
    RendererStats stats = renderer.stats();
    if (stats.gpuTimesRead != gpuTimesRead) {
        gpuTimesRead = stats.gpuTimesRead;
        // Read back during this frame: the times of the frame maxFramesInFlight before it
        int timedFrame = frame - Configuration::maxFramesInFlight;
        if (timedFrame > warmupFrames && timedFrame <= warmupFrames + measuredFrames) {
            std::array<float, 6> times{};
            std::copy(std::begin(stats.lastGpuPassMs), std::end(stats.lastGpuPassMs), times.begin());
            passTimes.push_back(times);
        }
    }
    if (frame < lastFrame() || renderer.capturePending() || (noise && (!noiseStarted || renderer.noiseRunBusy()))) {
        return false;
    }
    writeResults(renderer);
    return true;
}

// Mean, median, smallest and largest
static nlohmann::ordered_json summary(std::vector<float> values) {
    nlohmann::ordered_json json = nlohmann::ordered_json::object();
    if (values.empty()) {
        return json;
    }
    std::sort(values.begin(), values.end());
    double sum = 0.0;
    for (float value : values) {
        sum += value;
    }
    size_t middle = values.size() / 2;
    json["mean"] = sum / static_cast<double>(values.size());
    json["median"] = values.size() % 2 == 1 ? values[middle] : 0.5f * (values[middle - 1] + values[middle]);
    json["min"] = values.front();
    json["max"] = values.back();
    return json;
}

void Bench::writeResults(const Renderer& renderer) {
    RendererStats stats = renderer.stats();
    nlohmann::ordered_json results;
    results["bench"] = name;
    results["benchFile"] = benchPath;
    if (!error.empty()) {
        results["error"] = error;
    }
    results["device"] = stats.deviceName;
#ifdef NDEBUG
    results["build"] = "Release";
#else
    results["build"] = "Debug";
#endif
    results["shaderDirectory"] = Configuration::shaderDirectory;
    results["startup"] = {{"firstFrameSeconds", firstFrameSeconds}, {"loadedSeconds", loadedSeconds},
                          {"readySeconds", readySeconds}};
    results["frames"] = {{"warmup", warmupFrames}, {"measured", measuredFrames}, {"gpuTimesRead", passTimes.size()}};
    results["settings"] = renderSettingsToJson(renderer.renderSettings());
    results["scene"] = {{"triangles", stats.triangles}, {"instances", stats.instances}, {"meshes", stats.meshes},
                        {"textures", stats.textures}, {"emissiveTriangles", stats.emissiveTriangles}};

    nlohmann::ordered_json gpu = nlohmann::ordered_json::object();
    std::vector<float> total(passTimes.size(), 0.0f);
    for (int pass = 0; pass < 6; pass++) {
        std::vector<float> values;
        for (size_t i = 0; i < passTimes.size(); i++) {
            values.push_back(passTimes[i][pass]);
            total[i] += passTimes[i][pass];
        }
        gpu[passNames[pass]] = summary(values);
    }
    gpu["total"] = summary(total);
    results["gpuMs"] = gpu;
    // Wall time per frame: the slowest of the CPU's work, the GPU's, and the display's refresh (with DLSS loaded,
    // Streamline makes the driver wait for vsync)
    results["frameMs"] = frameMs;
    results["memory"] = memory;

    nlohmann::ordered_json statistics = nlohmann::ordered_json::object();
    for (const PipelineStatistics& pipeline : renderer.tracePipelineStatistics()) {
        nlohmann::ordered_json values = nlohmann::ordered_json::object();
        for (const auto& [statistic, value] : pipeline.values) {
            char* end = nullptr;
            double number = std::strtod(value.c_str(), &end);
            if (!value.empty() && end != nullptr && *end == '\0') {
                values[statistic] = number;
            } else {
                values[statistic] = value;
            }
        }
        statistics[pipeline.pipeline] = values;
    }
    results["pipelineStatistics"] = statistics;

    nlohmann::ordered_json captureList = nlohmann::ordered_json::array();
    for (const std::string& capture : captures) {
        captureList.push_back(std::filesystem::path(capture).filename().string());
    }
    results["captures"] = captureList;

    if (noise) {
        nlohmann::ordered_json frames = nlohmann::ordered_json::array();
        for (const NoiseResult& result : renderer.noiseResultList()) {
            frames.push_back({{"frame", result.frame}, {"relativeMse", result.relativeMse}, {"brightness", result.brightness},
                              {"regionBrightness", result.regionBrightness}, {"traceMs", result.traceMs},
                              {"restirMs", result.restirMs}, {"dlssMs", result.dlssMs}});
        }
        results["noise"] = {{"mode", renderSettingsToJson([&] { RenderSettings s; s.renderMode = noise->mode; return s; }())["renderMode"]},
                            {"referenceFrames", noise->referenceFrames}, {"frames", frames}};
    }

    std::filesystem::path path = std::filesystem::path(outputDirectory) / "results.json";
    std::ofstream file(path);
    file << results.dump(2) << std::endl;
    std::cout << "bench: " << (error.empty() ? "results in " : "failed, see ") << path.string() << std::endl;
}
