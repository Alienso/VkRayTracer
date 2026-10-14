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

    // Opens a scene file (the command line's): the scene becomes the file's, or the error shows in the Scene tab
    void openSceneFile(const std::string& path);
    // The window's close button: false while the scene has unsaved changes (it asks first, then closes the window)
    bool allowClose();

private:
    AssetStore& assets;
    Scene& scene;
    CameraController& cameraController;
    Renderer& renderer;
    Window& window;
    RendererStats stats;  // fetched at the start of build()

    // Scene tab (EditorScene.cpp)
    EntityId selected = 0;  // 0: none
    // The selected entity's rotation as the editor shows it (degrees): kept between frames, not recomputed from the
    // quaternion while it is being edited, so the angles don't jump (a rotation has several Euler forms). Recomputed
    // when another entity is selected or the rotation was changed elsewhere.
    EntityId eulerEntity = 0;
    glm::vec3 eulerDegrees = glm::vec3(0.0f);
    glm::quat eulerRotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);  // the rotation eulerDegrees stand for
    int modelToAdd = 0;  // index into assets.modelFiles()
    bool addAtOriginalSize = false;  // new objects keep the file's size (whole scenes like Sponza)
    std::string loadError;
    // Scene files (EditorFile.cpp)
    static constexpr const char* scenesDirectory = "scenes";  // next to the executable (CMakeLists.txt copies scenes/)
    std::string scenePath;   // the open scene's file; empty: not saved yet
    std::string savedText;   // Scene::contentText at the last new / open / save: different now = unsaved changes
    bool unsaved = false;    // checked once a second (contentText isn't free in huge scenes), and before asking
    double unsavedCheckTime = -1.0;
    std::vector<std::string> sceneFiles;  // .json files in scenesDirectory
    int sceneToOpen = 0;
    char saveAsName[128] = "";
    std::string fileMessage;                // the last error or "saved"
    std::vector<std::string> fileWarnings;  // what opening the scene skipped
    // New / Open / Exit wait for the unsaved-changes question
    enum class FileAction { None, New, Open, Exit };
    FileAction pendingAction = FileAction::None;
    std::string pendingPath;
    bool askAboutUnsaved = false;  // open the question this frame
    std::string shownTitle;        // the window title last set
    // Lighting tab
    std::string environmentError;  // why the picked environment map couldn't be loaded
    bool showImGuiDemo = false;

    // Editor.cpp
    void buildCameraUi();
    void buildLightingUi();
    void buildEnvironmentUi();
    void pickEnvironmentMap(const std::string& fileName);

    // EditorFile.cpp
    void buildFileUi();
    void buildUnsavedQuestion();
    void scanSceneFiles();
    bool hasUnsavedChanges();  // checks now
    // New / Open / Exit: right away, or after asking when there are unsaved changes
    void request(FileAction action, const std::string& path = "");
    void perform(FileAction action, const std::string& path);
    bool saveTo(const std::string& path);
    void updateTitle();

    // EditorScene.cpp
    // An entity dragged onto another in the tree: moved under it after the tree is drawn (parent 0: to the top)
    struct EntityMove {
        EntityId entity = 0;
        EntityId parent = 0;
    };
    void addObject(const std::string& fileName);
    void buildSceneUi();
    void buildEntityTree();
    void buildEntityNode(EntityId id, EntityMove& move);
    void buildEntityUi(EntityId id);
    void buildTransformUi(Entity& entity);
    void buildModelUi(EntityId id);
    void buildPortalUi(EntityId id);
    void buildMaterialUi(const SceneModel& model);
    void buildPunctualLightUi(SceneModel& model);
};

#endif //VULKANDEMO_EDITOR_H
