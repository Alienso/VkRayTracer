//
// The program: the window, the systems and the main loop. It owns what is loaded (AssetStore), what stands where
// (Scene), the camera controls, the renderer and the editor (the UI); every frame the editor may change the scene,
// the scene makes a RenderList, and the renderer draws it.
//

#ifndef VULKANDEMO_APPLICATION_H
#define VULKANDEMO_APPLICATION_H

#include "assets/AssetStore.h"
#include "app/CameraController.h"
#include "editor/Editor.h"
#include "scene/RenderList.h"
#include "renderer/Renderer.h"
#include "scene/Scene.h"
#include "app/Window.h"

class Application {
public:
    void run();

    void setDlssDisabled(bool disabled) {  // --no-dlss
        renderer.setDlssDisabled(disabled);
    }
    void setDlssCnn(bool cnn) {  // --dlss-cnn
        renderer.setDlssCnn(cnn);
    }
    void setStartScene(const std::string& path) {  // a scene file on the command line: opened instead of the default
        startScene = path;
    }

private:
    Window window;
    std::string startScene;
    bool fullscreenKeyDown = false;  // F11 toggles once per press

    // Each refers to the ones before it, so they come in this order
    AssetStore assets{"models", "environments"};  // folders copied next to the executable from resource/
    Scene scene{assets};
    RenderList renderList;  // what this frame draws (Scene::buildRenderList), given to the renderer
    CameraController cameraController;  // moves scene.camera()
    Renderer renderer{assets};
    Editor editor{assets, scene, cameraController, renderer, window};

    void createDefaultScene();
    void mainLoop();
};

#endif //VULKANDEMO_APPLICATION_H
