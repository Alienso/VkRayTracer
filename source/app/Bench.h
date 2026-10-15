//
// Benches (--bench <file.json>): a scene, renderer settings and a camera from a file, a number of frames, and what to
// measure. The program runs it, writes results.json (and the captures) into the output folder, and exits: a timing
// or an image check is a command line instead of a change to the code. tools/bench.py runs benches against two
// builds and compares them. The files in benches/ are the standard ones.
//
// {
//   "scene": "../scenes/sponza.json",   relative to the bench file; absent: the default scene
//   "settings": {"renderMode": "DlssDlaa", "maxBounces": 4},   RenderSettings fields (RenderSettingsFile.h), the
//                                       others keep their defaults
//   "camera": {"position": [x, y, z], "target": [x, y, z]},   or "yaw" and "pitch" instead of "target"; "fov".
//                                       Absent: the scene file's camera.
//   "path": [{"frame": 1, "position": [..], "target": [..]}, {"frame": 120, ...}],   a camera moving from keyframe
//                                       to keyframe (linear; held before the first and after the last). Absolute
//                                       positions: the camera doesn't follow portals.
//   "deltaTime": 0.016667,              seconds per frame the renderer is told (auto exposure, fog drift), so runs
//                                       are the same however fast they go; 0: the real time
//   "warmupFrames": 400,                frames before measuring (radiance cache, exposure and DLSS settle)
//   "frames": 300,                      frames measured: GPU time per pass, wall time per frame, memory
//   "captures": [1024],                 frames to save as PNG (capture_<frame>.png)
//   "noise": {"mode": "DlssDlaa", "referenceFrames": 4096, "frames": 256, "region": [x0, y0, x1, y1]}
//                                       after the measured frames: the noise tool (Noise.cpp) against a reference
// }
//
// Frames count from the first one after everything the scene loads has arrived (frame 1), so captures and the
// measurement start at the same point in every run.
//

#ifndef VULKANDEMO_BENCH_H
#define VULKANDEMO_BENCH_H

#include <array>
#include <chrono>
#include <optional>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>
#include "assets/AssetStore.h"
#include "renderer/Renderer.h"
#include "scene/Scene.h"

class Bench {
public:
    // Reads the bench file; throws with what is wrong in it. Results go into outputDirectory (created when needed).
    Bench(const std::string& path, std::string outputDirectory);

    // The scene file to open ("" = the default scene)
    [[nodiscard]] const std::string& sceneFile() const { return scenePath; }
    // After the renderer is set up: its settings from the file
    void start(Renderer& renderer);
    // The time step for this frame: the file's, or the real one
    [[nodiscard]] float deltaTime(float realDeltaTime) const { return fixedDeltaTime > 0.0f ? fixedDeltaTime : realDeltaTime; }
    // Each frame before the render list is made: the camera, captures, the noise run
    void beforeRender(Scene& scene, Renderer& renderer, const AssetStore& assets);
    // Each frame after it was drawn: reads the measurements. True once the bench is over (results written).
    bool afterRender(Renderer& renderer);
    // The scene file didn't open: the bench fails
    void fail(const std::string& error);
    [[nodiscard]] bool failed() const { return !error.empty(); }

private:
    struct CameraKey {
        int frame = 1;
        glm::vec3 position{0.0f};
        std::optional<glm::vec3> target;  // else yaw and pitch
        float yaw = 0.0f;
        float pitch = 0.0f;
        std::optional<float> fov;
    };
    struct NoiseRun {
        int mode = 0;
        int referenceFrames = 4096;
        int frames = 256;
        glm::ivec4 region{0};
    };

    std::string benchPath;
    std::string name;  // the file's name without .json
    std::string outputDirectory;
    nlohmann::json fileContent;
    std::string scenePath;
    nlohmann::json settingsJson;
    std::vector<CameraKey> cameraKeys;  // one for "camera", several for "path"
    float fixedDeltaTime = 1.0f / 60.0f;
    int warmupFrames = 400;
    int measuredFrames = 300;
    std::vector<int> captureFrames;
    std::optional<NoiseRun> noise;

    // The run
    std::chrono::steady_clock::time_point startTime;  // when the bench was read (the program's start)
    double firstFrameSeconds = -1.0;
    double loadedSeconds = -1.0;  // everything the scene loads has arrived (frame 1 uploads it)
    double readySeconds = -1.0;   // frame 1 is done: the first frame with all of it
    int frame = 0;  // since everything loaded: 1 = the first; 0 while loading
    int requestedRenderMode = 0;
    bool noiseStarted = false;
    uint64_t gpuTimesRead = 0;
    std::vector<std::array<float, 6>> passTimes;  // per measured frame: cache, ReSTIR, trace, DLSS, exposure, display
    std::chrono::steady_clock::time_point measureStart;
    double frameMs = 0.0;
    nlohmann::ordered_json memory;
    std::vector<std::string> captures;  // file names written
    std::string error;

    void setCamera(Camera& camera) const;
    [[nodiscard]] int lastFrame() const;
    void writeResults(const Renderer& renderer);
};

#endif //VULKANDEMO_BENCH_H
