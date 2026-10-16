//
// The keyboard and mouse as the game sees them (GAME_PLAN.md): filled by the program from the window each frame
// (Application.cpp), so the game includes no GLFW. Key numbers are GLFW's (Key:: names the ones in use).
//

#ifndef VULKANDEMO_INPUT_H
#define VULKANDEMO_INPUT_H

#include <array>
#include <bitset>
#include <glm/glm.hpp>

namespace Key {  // GLFW_KEY_*: letters and digits are their ASCII codes
constexpr int Space = 32;
constexpr int A = 65, D = 68, E = 69, F = 70, Q = 81, R = 82, S = 83, W = 87;
constexpr int Escape = 256, Enter = 257, Tab = 258;
constexpr int Right = 262, Left = 263, Down = 264, Up = 265;
constexpr int LeftShift = 340, LeftControl = 341;
constexpr int Count = 349;  // GLFW_KEY_LAST + 1
}  // namespace Key

namespace MouseButton {
constexpr int Left = 0, Right = 1, Middle = 2;
constexpr int Count = 3;
}  // namespace MouseButton

struct InputState {
    std::bitset<Key::Count> down;      // held now
    std::bitset<Key::Count> pressed;   // went down since the last time the game saw the input
    std::bitset<Key::Count> released;  // went up since then
    std::array<bool, MouseButton::Count> mouseDown{};
    std::array<bool, MouseButton::Count> mousePressed{};
    glm::vec2 mouseDelta = glm::vec2(0.0f);  // pixels moved since then
    // False while the editor uses the device (typing in a field, dragging a slider): its keys / buttons then read
    // as up and the mouse as still
    bool keyboard = true;
    bool mouse = true;

    [[nodiscard]] bool isDown(int key) const { return key >= 0 && key < Key::Count && down[key]; }
    [[nodiscard]] bool wasPressed(int key) const { return key >= 0 && key < Key::Count && pressed[key]; }
    [[nodiscard]] bool wasReleased(int key) const { return key >= 0 && key < Key::Count && released[key]; }
};

#endif //VULKANDEMO_INPUT_H
