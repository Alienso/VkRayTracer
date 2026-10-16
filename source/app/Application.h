//
// The program: the window, the systems and the main loop. It owns what is loaded (AssetStore), what stands where
// (Scene), the camera controls, the renderer and the editor (the UI); every frame the editor may change the scene,
// the scene makes a RenderList, and the renderer draws it.
//

#ifndef VULKANDEMO_APPLICATION_H
#define VULKANDEMO_APPLICATION_H

#include <memory>
#include "assets/AssetStore.h"
#include "app/Bench.h"
#include "app/CameraController.h"
#include "app/CpuFrameTimes.h"
#include "app/UserSettings.h"
#include "editor/Editor.h"
#include "scene/RenderList.h"
#include "renderer/Renderer.h"
#include "scene/Scene.h"
#include "app/Window.h"
#include "core/Configuration.h"
#include "core/Input.h"
#include "game/Game.h"
#include "game/GameLoop.h"
#include "game/StepInterpolation.h"

class Application {
public:
    // Returns the exit code: 1 when a bench failed
    int run();

    void setDlssDisabled(bool disabled) {  // --no-dlss
        renderer.setDlssDisabled(disabled);
    }
    void setDlssCnn(bool cnn) {  // --dlss-cnn
        renderer.setDlssCnn(cnn);
    }
    void setStartScene(const std::string& path) {  // a scene file on the command line: opened instead of the last one
        startScene = path;
    }
    void setPlayAtStart(bool play) {  // --play: Play once the scene is open (not with a bench)
        playAtStart = play;
    }
    // --bench: runs it (its scene instead of startScene), then closes
    void setBench(std::unique_ptr<Bench> newBench) {
        bench = std::move(newBench);
    }

private:
    Window window;
    std::string startScene;
    std::unique_ptr<Bench> bench;
    bool benchDone = false;  // its results are written (else the window was closed before)
    bool fullscreenKeyDown = false;  // F11 toggles once per press
    bool playAtStart = false;
    // Kept between runs (not with a bench): read at start, written a second after a change and on exit
    static constexpr const char* userSettingsFile = "user_settings.json";
    UserSettings userSettings;
    std::string savedUserSettings;  // the file's text as last read or written
    // The render mode and DLSS model as read: kept when this run can't have them (no DLSS, --no-dlss, a model this
    // DLSS version lacks), so a run that switched to Accumulation doesn't overwrite what the user picked
    int savedRenderMode = 0;
    int savedDlssModel = 0;
    double userSettingsCheckTime = 0.0;

    // Each refers to the ones before it, so they come in this order
    AssetStore assets{Configuration::resourceDirectory + "/models", Configuration::resourceDirectory + "/environments"};
    // The renderer renders with them and the editor edits them; the scene's fields of them are the scene's (saved
    // with it), the program's are kept between runs (user settings); a bench sets them
    RenderSettings renderSettings;
    Scene scene{assets, renderSettings};
    RenderList renderList;  // what this frame draws (Scene::buildRenderList), given to the renderer
    CameraController cameraController;  // moves scene.camera()
    CpuFrameTimes cpuTimes;  // measured by mainLoop, shown by the editor, recorded by a bench
    Renderer renderer{assets, renderSettings};
    Editor editor{assets, scene, cameraController, renderer, renderSettings, cpuTimes, userSettings, window};
    // The game: runs while the editor plays (started and stopped by updateGame), in fixed steps (gameLoop), with
    // the window's input (readInput, which keeps the last frame's keys to find presses)
    Game game;
    GameLoop gameLoop;
    StepInterpolation interpolation;  // what is shown between the steps (the editor's View menu turns it off)
    std::bitset<Key::Count> lastKeys;
    std::array<bool, MouseButton::Count> lastMouseButtons{};
    double lastCursorX = 0.0, lastCursorY = 0.0;
    bool cursorRead = false;

    void createDefaultScene();
    void mainLoop();
    // The window's state into userSettings, and the file written when its text changed
    void saveUserSettingsIfChanged();
    // This frame's keyboard and mouse for the game; a device the editor uses (ImGui) reads as idle
    InputState readInput(bool keyboard, bool mouse);
    // The editor's selection to the renderer (its outline), its clicks to the renderer and their answers back
    void updateSelection();
    // Starts or stops the game as the editor plays or stops, and runs this frame's steps
    void updateGame(float deltaTime, const InputState& input);
};

#endif //VULKANDEMO_APPLICATION_H
