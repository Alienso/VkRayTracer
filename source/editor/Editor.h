//
// The editor: dockable panels around the image (ImGui), with the scene's objects, materials, lights, camera, sun
// and sky, and the renderer's settings. It changes the Scene, the AssetStore's materials and lights and the program's
// RenderSettings; of the renderer it reads its numbers (Renderer::stats) and asks for captures and the like.
//

#ifndef VULKANDEMO_EDITOR_H
#define VULKANDEMO_EDITOR_H

#include <array>
#include <string>
#include "assets/AssetStore.h"
#include "app/CameraController.h"
#include "renderer/Renderer.h"
#include "scene/Scene.h"
#include "app/Window.h"

class Editor {
public:
    Editor(AssetStore& assets, Scene& scene, CameraController& cameraController, Renderer& renderer,
           RenderSettings& settings, Window& window);

    // After the scene was made: the last object selected, a load error to show
    void init(const std::string& error);

    // After ImGui::CreateContext: docking on, and which panels are open saved in imgui.ini with the layout
    void initUi();

    // This frame's menu bar and panels. Between ImGui::NewFrame and ImGui::Render; may change the scene.
    void build();

    // Opens a scene file (the command line's): the scene becomes the file's, or the error shows in the Log panel
    // (false)
    bool openSceneFile(const std::string& path);
    // The window's close button: false while the scene has unsaved changes (it asks first, then closes the window)
    bool allowClose();

private:
    AssetStore& assets;
    Scene& scene;
    CameraController& cameraController;
    Renderer& renderer;
    RenderSettings& settings;  // the program's: the renderer renders with them
    Window& window;
    RendererStats stats;  // fetched at the start of build()

    // Layout (EditorLayout.cpp): the panels, docked around the image or floating
    struct Panel {
        const char* name;  // also the ImGui window's: the saved layout finds it by this
        bool open;
        void (Editor::*build)();
    };
    std::array<Panel, 8> panels;
    bool showOverlay = true;  // FPS and GPU times over the image
    bool uiHidden = false;    // F1: only the image
    bool resetLayout = false;  // put the panels back where they start (also the first run, without imgui.ini)
    // Log panel: what loading, saving and capturing said
    enum class LogLevel { Info, Warning, Error };
    struct LogEntry {
        LogLevel level;
        std::string text;
    };
    std::vector<LogEntry> logEntries;
    bool showLogNow = false;  // an error came in: bring the Log panel to the front
    std::string loggedCaptureMessage;

    // Outliner and Details (EditorScene.cpp)
    EntityId selected = 0;  // 0: none
    // The selected entity's rotation as the editor shows it (degrees): kept between frames, not recomputed from the
    // quaternion while it is being edited, so the angles don't jump (a rotation has several Euler forms). Recomputed
    // when another entity is selected or the rotation was changed elsewhere.
    EntityId eulerEntity = 0;
    glm::vec3 eulerDegrees = glm::vec3(0.0f);
    glm::quat eulerRotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);  // the rotation eulerDegrees stand for
    bool addAtOriginalSize = false;  // new objects keep the file's size (whole scenes like Sponza)
    // Scene files (EditorFile.cpp)
    static constexpr const char* scenesDirectory = "scenes";  // next to the executable (CMakeLists.txt copies scenes/)
    std::string scenePath;   // the open scene's file; empty: not saved yet
    std::string savedText;   // Scene::contentText at the last new / open / save: different now = unsaved changes
    bool unsaved = false;    // checked once a second (contentText isn't free in huge scenes), and before asking
    double unsavedCheckTime = -1.0;
    std::vector<std::string> sceneFiles;  // .json files in scenesDirectory (scanned when File > Open opens)
    char saveAsName[128] = "";
    bool askSaveAs = false;     // open the Save As question this frame
    std::string saveError;      // why the last save failed (also in the log), shown in the save questions
    // New / Open / Exit wait for the unsaved-changes question
    enum class FileAction { None, New, Open, Exit };
    FileAction pendingAction = FileAction::None;
    std::string pendingPath;
    bool askAboutUnsaved = false;  // open the question this frame
    std::string shownTitle;        // the window title last set
    bool showImGuiDemo = false;

    // EditorLayout.cpp
    void log(LogLevel level, const std::string& text);
    void buildDockSpace();
    void buildDefaultLayout(unsigned int dockSpace);
    void buildMenuBar();
    void buildOverlay();
    void buildStatisticsUi();
    void buildLogUi();
    void buildDebugPanel();

    // Editor.cpp
    void buildCameraUi();
    void buildLightingUi();
    void buildEnvironmentUi();

    // EditorRenderer.cpp: the renderer's settings
    int noiseReferenceFrames = 4096;         // noise tool: frames accumulated for the reference
    int noiseMeasureFrames = 256;            // frames measured
    glm::ivec4 noiseRegion = glm::ivec4(0);  // x0, y0, x1, y1 in output pixels; all 0 = the whole image
    void buildRenderingPanel();
    void buildRenderModeUi();
    void buildRadianceCacheUi();
    void buildRestirUi();
    void buildExposureUi();
    void buildRendererDebugUi();  // the Debug panel's top: debug views, the noise tool, shader statistics
    void buildNoiseUi();
    void buildSkySamplingUi();            // in the Lighting panel's Sky part
    void buildPunctualLightSettingsUi();  // in the Lighting panel

    // EditorFile.cpp
    void buildFileMenu();
    void buildSaveAsQuestion();
    void buildUnsavedQuestion();
    void scanSceneFiles();
    bool hasUnsavedChanges();  // checks now
    // New / Open / Exit: right away, or after asking when there are unsaved changes
    void request(FileAction action, const std::string& path = "");
    void perform(FileAction action, const std::string& path);
    bool saveTo(const std::string& path);
    void updateTitle();

    // EditorScene.cpp
    // A change made in the tree (dragged onto another entity, or its right-click menu), done after the tree is drawn
    struct TreeEdit {
        enum Kind { None, Move, AddChild, Duplicate, Delete } kind = None;
        EntityId entity = 0;
        EntityId parent = 0;  // Move: the new parent, 0 for the top
    };
    void addObject(const std::string& fileName);
    void buildAddMenu();  // the "Add" menu's items: models, group, portal pair
    void buildOutliner();
    void buildDetails();
    void buildEntityTree();
    void buildEntityNode(EntityId id, TreeEdit& edit);
    void buildEntityUi(EntityId id);
    void buildTransformUi(Entity& entity);
    void buildModelUi(EntityId id);
    void buildPortalUi(EntityId id);
    void buildMaterialUi(const SceneModel& model);
    void buildPunctualLightUi(SceneModel& model);
};

#endif //VULKANDEMO_EDITOR_H
