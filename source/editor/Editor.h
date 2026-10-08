//
// The editor: the settings window over the image (ImGui), with the scene's objects, materials, lights, camera, sun
// and sky, and the renderer's own settings panels. It changes the Scene and the AssetStore's materials and lights;
// it asks the renderer only for its panels, its numbers (Renderer::stats) and a capture.
//

#ifndef VULKANDEMO_EDITOR_H
#define VULKANDEMO_EDITOR_H

#include <string>
#include "assets/AssetStore.h"
#include "app/CameraController.h"
#include "renderer/Renderer.h"
#include "scene/Scene.h"
#include "app/Window.h"

class Editor {
public:
    Editor(AssetStore& assets, Scene& scene, CameraController& cameraController, Renderer& renderer, Window& window);

    // After the scene was made: the model the "add" combo starts at, the last object selected, a load error to show
    void init(const std::string& defaultModel, const std::string& error);

    // This frame's window. Between ImGui::NewFrame and ImGui::Render; may change the scene.
    void build();

private:
    AssetStore& assets;
    Scene& scene;
    CameraController& cameraController;
    Renderer& renderer;
    Window& window;
    RendererStats stats;  // fetched at the start of build()

    // Scene tab (EditorScene.cpp)
    int selectedObject = -1;
    int modelToAdd = 0;  // index into assets.modelFiles()
    bool addAtOriginalSize = false;  // new objects keep the file's size (whole scenes like Sponza)
    std::string loadError;
    // Lighting tab
    std::string environmentError;  // why the picked environment map couldn't be loaded
    bool showImGuiDemo = false;

    // Editor.cpp
    void buildCameraUi();
    void buildLightingUi();
    void buildEnvironmentUi();
    void pickEnvironmentMap(const std::string& fileName);

    // EditorScene.cpp
    void addObject(const std::string& fileName);
    void buildSceneUi();
    void buildMaterialUi(const SceneModel& model);
    void buildPunctualLightUi(SceneModel& model);
};

#endif //VULKANDEMO_EDITOR_H
