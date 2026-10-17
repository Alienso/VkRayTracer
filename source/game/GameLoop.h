//
// The game's clock (game/Game.h): the game advances in fixed steps of exactly 1/60 s, however long the frames are,
// so the same input gives the same state on any machine and tests are exact. Each frame's time goes into an
// accumulator, and as many whole steps run as it holds; at most maxStepsPerFrame, so a long frame (a model loading,
// a breakpoint) doesn't owe ever more steps: the time beyond that is dropped (the game runs slower than real time).
//
// Input that happens between steps isn't lost: presses, releases and mouse movement add up until a step sees them
// (frames faster than the steps run none), and only the first step of a frame gets them.
//

#ifndef VULKANDEMO_GAMELOOP_H
#define VULKANDEMO_GAMELOOP_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include "core/Input.h"

class GameLoop {
public:
    static constexpr int stepsPerSecond = 60;
    static constexpr double stepSeconds = 1.0 / stepsPerSecond;
    static constexpr int maxStepsPerFrame = 5;

    // At Play: no time owed, no steps yet, no input waiting
    void reset() {
        accumulator = 0.0;
        stepCount = 0;
        droppedTime = 0.0;
        pending = InputState();
    }

    // Input waiting for a step dropped (resuming after a pause: keys used in the editor meanwhile aren't the game's)
    void dropInput() { pending = InputState(); }

    // One frame: its time and its input. Calls step(const InputState&, uint64_t index) for each step it runs
    // (index: steps since reset, 0 first); returns how many ran.
    template <typename Step> int advance(double frameSeconds, const InputState& frameInput, Step&& step) {
        addInput(frameInput);
        accumulator += std::max(frameSeconds, 0.0);
        int steps = 0;
        while (accumulator >= stepSeconds && steps < maxStepsPerFrame) {
            step(static_cast<const InputState&>(pending), stepCount);
            accumulator -= stepSeconds;
            stepCount++;
            steps++;
            // Seen: the next steps of this frame get the keys held, not the presses again
            pending.pressed.reset();
            pending.released.reset();
            pending.mousePressed = {};
            pending.mouseDelta = glm::vec2(0.0f);
        }
        if (accumulator >= stepSeconds) {  // more than maxStepsPerFrame owed
            double kept = std::fmod(accumulator, stepSeconds);  // (the part of a step: the next frame goes on from it)
            droppedTime += accumulator - kept;
            accumulator = kept;
        }
        return steps;
    }

    [[nodiscard]] uint64_t steps() const { return stepCount; }  // since reset
    [[nodiscard]] double time() const { return static_cast<double>(stepCount) * stepSeconds; }  // the game's
    [[nodiscard]] double owed() const { return accumulator; }       // less than a step after a frame
    [[nodiscard]] double dropped() const { return droppedTime; }    // real time the game didn't follow

private:
    double accumulator = 0.0;
    uint64_t stepCount = 0;
    double droppedTime = 0.0;
    InputState pending;  // what the next step sees

    void addInput(const InputState& frame) {
        pending.down = frame.down;
        pending.pressed |= frame.pressed;
        pending.released |= frame.released;
        pending.mouseDown = frame.mouseDown;
        for (int b = 0; b < MouseButton::Count; b++) {
            pending.mousePressed[b] = pending.mousePressed[b] || frame.mousePressed[b];
        }
        pending.mouseDelta += frame.mouseDelta;
        pending.keyboard = frame.keyboard;
        pending.mouse = frame.mouse;
    }
};

#endif //VULKANDEMO_GAMELOOP_H
