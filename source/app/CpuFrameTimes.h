//
// CPU time per frame by phase, measured by the main loop (Application::mainLoop). Time the renderer spends blocked
// (waiting for a frame slot's fence, a swapchain image, or present: the GPU or the display is the slower one) is
// counted apart: it isn't CPU work, and with vsync it fills the frame up to the refresh.
//

#ifndef VULKANDEMO_CPUFRAMETIMES_H
#define VULKANDEMO_CPUFRAMETIMES_H

#include <cstdint>

struct CpuFrameTimes {
    // Update: events, loaded assets arriving, the scene, the camera, the render list (and writing captures).
    // Game: the game's steps while playing. Ui: the editor's panels and ImGui::Render. Render: the renderer's CPU
    // side (scene uploads, recording, submit). Wait: inside Render, blocked.
    enum Phase { Update, Game, Ui, Render, Wait, PhaseCount };
    static constexpr const char* phaseNames[PhaseCount] = {"update", "game", "ui", "render", "wait"};

    float ms[PhaseCount] = {};      // smoothed over ~20 frames
    float lastMs[PhaseCount] = {};  // the last frame's
    uint64_t frames = 0;

    [[nodiscard]] float workMs() const { return ms[Update] + ms[Game] + ms[Ui] + ms[Render]; }
    [[nodiscard]] float lastWorkMs() const { return lastMs[Update] + lastMs[Game] + lastMs[Ui] + lastMs[Render]; }

    void add(const float (&frame)[PhaseCount]) {
        for (int i = 0; i < PhaseCount; i++) {
            lastMs[i] = frame[i];
            ms[i] = frames == 0 ? frame[i] : ms[i] * 0.95f + frame[i] * 0.05f;  // as the GPU times (readTimestamps)
        }
        frames++;
    }
};

#endif //VULKANDEMO_CPUFRAMETIMES_H
