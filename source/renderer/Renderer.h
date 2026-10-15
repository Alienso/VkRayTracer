//
// Created by Alienson on 19.1.2024..
//

#ifndef VULKANDEMO_RENDERER_H
#define VULKANDEMO_RENDERER_H


#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <vector>
#include <functional>
#include <string>
#include <map>
#include "scene/Camera.h"
#include "assets/ModelLoader.h"
#include "assets/AssetStore.h"
#include "scene/Scene.h"
#include "scene/RenderList.h"
#include "renderer/GpuAssets.h"
#include "renderer/GpuContext.h"
#include "renderer/RendererTypes.h"
#include "renderer/GpuScene.h"
#include "core/Configuration.h"

// The path tracer: the Vulkan device, the swapchain of the window it is given, and everything it draws with. Every
// frame it is given a RenderList (the scene's instances, lights, camera, environment) and reads the AssetStore for
// what the list refers to. The program (Application.h) owns the window, the systems, the editor and the main loop.
//
// Its own settings panels (rendering, debug, light sampling) are drawn into the editor's window. It draws the UI too:
// the ImGui context is the program's, the Vulkan side of it is here.
class Renderer {
public:
    static constexpr uint32_t maxTextures = GpuScene::maxTextures;  // size of the texture descriptor array

    explicit Renderer(AssetStore& assets) : assets(assets) {}

    // Creates the device, the swapchain for the window, pipelines and images, and uploads what list shows. The
    // ImGui context must exist (its Vulkan side is set up here).
    void init(GLFWwindow* window, const RenderList& list);
    // Every frame: beginFrame, then (the program's ImGui frame and render list) render
    void beginFrame(float deltaTime);  // a capture recorded last frame becomes a PNG; the UI's Vulkan frame starts
    void render(const RenderList& list);  // draws the list, and the UI (ImGui::Render must have been called)
    // After the last frame: waits for the GPU, then frees everything (the window stays the caller's)
    void shutdown();

    RendererStats stats() const;
    GpuMemory gpuMemory() const;
    // The next frame's image, without the UI, as a PNG: into fileName, or capture_<date>_<time>.png when empty.
    // Written at the start of the frame after (beginFrame).
    void requestCapture(const std::string& fileName = "") {
        captureRequested = true;
        captureFileRequested = fileName;
    }
    [[nodiscard]] bool capturePending() const { return captureRequested || captureBuffer != VK_NULL_HANDLE; }

    // The settings the UI edits; a bench sets them from a file (RenderSettingsFile.h)
    [[nodiscard]] const RenderSettings& renderSettings() const { return settings; }
    void setRenderSettings(const RenderSettings& newSettings) { settings = newSettings; }

    // The noise tool, run from code: accumulates a reference of the current view for referenceFrames, then measures
    // measureFrames in renderMode (see Noise.cpp). Busy until the results are in noiseResultList.
    void startNoiseRun(int renderMode, int referenceFrames, int measureFrames, glm::ivec4 region);
    [[nodiscard]] bool noiseRunBusy() const {
        return noiseAutoRun || noiseAutoContinue || noiseState != NoiseState::Idle;
    }
    [[nodiscard]] const std::vector<NoiseResult>& noiseResultList() const { return noiseResults; }
    // The trace shader's pipelines, as the driver compiled them (empty without VK_KHR_pipeline_executable_properties)
    [[nodiscard]] const std::vector<PipelineStatistics>& tracePipelineStatistics() const { return rtPipelineStatisticsList; }

    // The renderer's settings panels, for the editor's window (ImGuiInit.cpp, Environment.cpp, Lights.cpp)
    void buildRenderingUi();
    void buildDebugUi();
    void buildShadowsUi();
    void buildSkySamplingUi();
    void buildPunctualLightSettingsUi();

    // The window's framebuffer changed size: the swapchain is remade after this frame
    void resize() { framebufferResized = true; }
    void setDlssDisabled(bool disabled) {  // --no-dlss
        dlssDisabledByFlag = disabled;
    }
    void setDlssCnn(bool cnn) {  // --dlss-cnn
        dlssCnnVersion = cnn;
    }

private:
    GLFWwindow* window = nullptr;  // the program's
    // The device, its queues, memory allocator, command pool and pipeline cache, and helpers for buffers and images
    GpuContext gpu;
    VkSwapchainKHR swapChain;
    std::vector<VkImage> swapChainImages;
    VkFormat swapChainImageFormat;
    VkExtent2D swapChainExtent;
    std::vector<VkImageView> swapChainImageViews;
    VkRenderPass renderPass;
    std::vector<VkFramebuffer> swapChainFramebuffers;
    std::vector<VkCommandBuffer> commandBuffers;  // one per frame in flight, from gpu.commandPool
    std::vector<VkSemaphore> imageAvailableSemaphores;
    std::vector<VkSemaphore> renderFinishedSemaphores;
    std::vector<VkFence> inFlightFences;
    uint32_t currentFrame = 0;
    bool framebufferResized = false;

    // What is loaded (the program's, Application.h) and what this frame draws (given to render())
    AssetStore& assets;
    const RenderList* renderList = nullptr;

    // The scene on the GPU: geometry, BLASes and the TLAS, instances, materials and textures, lights, portals, the
    // environment, fog volumes, and the buffers that stream their changes (GpuScene.h)
    GpuScene scene{gpu, assets, settings};
    uint64_t frameNumber = 0;  // submitted frames (currentFrame is only the slot)

    // How much light the GGX reflection reflects in total, per view angle and roughness (see Materials.cpp)
    static constexpr uint32_t specularAlbedoSize = 32;
    VkBuffer specularAlbedoBuffer;
    VmaAllocation specularAlbedoMemory;
    // 64 x 64 blue-noise texels, 4 channels of 16 bits (BlueNoise.cpp), binding 30
    VkBuffer blueNoiseBuffer;
    VmaAllocation blueNoiseMemory;



    // Two sizes (chooseExtents): the path tracer renders at renderExtent, the display pass writes outputExtent
    // (the window, or bigger: RenderSettings::outputSize). Equal in accumulation mode; DLSS renders smaller and upscales.
    VkExtent2D renderExtent{};
    VkExtent2D outputExtent{};
    int activeOutputSize = 0;  // the RenderSettings::outputSize the images are built for

    // Ray tracing output at render size, recreated with the swapchain
    // Linear HDR: running average while accumulating, else this frame (or a debug view). RGB the light, alpha how much
    // fog the camera sees (averaged the same way; for the fog grain, display.comp).
    VkImage radianceImage;
    VmaAllocation radianceImageMemory;
    VkImageView radianceImageView;
    // DLSS inputs ("guides"), all describing the first surface the camera ray hits. Written only when
    // SceneUniforms::writeGuides is set (DLSS mode, guide debug views). Formats: radianceFormat and the others.
    RenderImage linearDepthImage;         // R32F: view-space depth (distance along the camera's forward axis)
    RenderImage motionVectorImage;        // RG16F: pixels to where this point was in the last frame
    RenderImage diffuseAlbedoImage;       // RGBA16F
    RenderImage specularAlbedoImage;      // RGBA16F
    RenderImage normalRoughnessImage;     // RGBA16F: world-space shading normal, roughness in alpha
    RenderImage specularHitDistanceImage; // R16F: first hit -> next hit along a reflection/refraction, 0 = unknown
    RenderImage guideHistoryImage;        // RGBA16F: last frame's diffuse albedo, for the reprojection error view
    VkBuffer primaryHitBuffer = VK_NULL_HANDLE;  // GpuPrimaryHit per render pixel (ReSTIR: camera rays traced first)
    VmaAllocation primaryHitMemory = VK_NULL_HANDLE;
    VkBuffer bounceHitBuffer = VK_NULL_HANDLE;   // GpuBounceHit per render pixel (ReSTIR: first bounce traced early)
    VmaAllocation bounceHitMemory = VK_NULL_HANDLE;
    VkBuffer reservoirBuffer = VK_NULL_HANDLE;   // GpuReservoir per render pixel, three parts (see GpuReservoir)
    VmaAllocation reservoirMemory = VK_NULL_HANDLE;
    VkBuffer restirSurfaceBuffer = VK_NULL_HANDLE;  // GpuRestirSurface per render pixel, this frame's and last frame's
    VmaAllocation restirSurfaceMemory = VK_NULL_HANDLE;
    VkBuffer giReservoirBuffer = VK_NULL_HANDLE;  // GpuGiReservoir per render pixel, three parts
    VmaAllocation giReservoirMemory = VK_NULL_HANDLE;
    // Which of the above have a pixel's worth for every render pixel. The others are 1 x 1 placeholders (the
    // descriptors need something), never touched by the shader: their features are off (buildSceneUniforms).
    // Remade when a feature is switched on or off (applyPerPixelBuffers).
    struct PerPixelBuffers {
        bool restir = false;    // primary hits, first bounces, DI reservoirs and surfaces: ReSTIR DI or GI on
        bool restirGi = false;  // GI reservoirs: ReSTIR GI on
        bool guides = false;    // the DLSS input images: writeGuides (DLSS modes, guide views)
        bool history = false;   // the guide history: the reprojection error view
        bool operator==(const PerPixelBuffers& other) const {
            return restir == other.restir && restirGi == other.restirGi && guides == other.guides &&
                   history == other.history;
        }
    };
    PerPixelBuffers perPixelBuffers;
    // Last frame's reservoirs can be reused: ReSTIR ran, at this size, with the same lights
    bool restirHistoryAvailable = false;
    bool restirLightsChanged = false;  // this frame uploads new lights (their indices may have changed)
    uint32_t restirCurrentHalf = 0;
    bool restirGiHistoryAvailable = false;  // ReSTIR GI ran last frame, at this size

    // Motion: last frame's unjittered view-projection, and per object a matrix that takes a point on it from this
    // frame's position to last frame's (object motion). Binding 34, uploaded when the instances change.
    glm::mat4 previousViewProjection = glm::mat4(1.0f);
    bool hasPreviousViewProjection = false;
    uint32_t jitterIndex = 0;
    glm::vec2 frameJitter = glm::vec2(0.0f);  // this frame's jitter in pixels (DLSS constants)
    glm::mat4 frameView = glm::mat4(1.0f);    // this frame's matrices, without jitter (DLSS constants)
    glm::mat4 frameProjection = glm::mat4(1.0f);
    glm::mat4 frameViewProjection = glm::mat4(1.0f);
    glm::mat4 framePreviousViewProjection = glm::mat4(1.0f);

    VkDescriptorSetLayout rtDescriptorSetLayout;
    VkDescriptorPool rtDescriptorPool;
    VkDescriptorSet rtDescriptorSet = VK_NULL_HANDLE;  // null until init creates it; updateScene runs before that
    VkPipelineLayout rtPipelineLayout;
    VkPipeline rtPipeline;
    VkPipeline rtCacheUpdatePipeline;  // the same shader with CACHE_UPDATE_PASS = 1
    VkPipeline rtRestirPipeline;       // the same shader with RESTIR_PASS = 1: camera rays and ReSTIR DI candidates
    VkPipeline rtRestirSpatialPipeline;  // RESTIR_PASS = 2: ReSTIR DI's spatial reuse
    VkPipeline rtRestirGiShadePipeline;  // RESTIR_PASS = 3: ReSTIR GI's shading, after the image pass
    VkPipeline rtRestirGiTemporalPipeline;  // RESTIR_PASS = 4: ReSTIR GI's temporal reuse, before its shading
    VkPipeline rtRestirGiSpatialPipeline;   // RESTIR_PASS = 5: ReSTIR GI's spatial reuse, between those two
    // The same seven with FOG = 1, used while some material has fog inside or an invisible surface or is solid glass
    // (the paths then keep which volumes they are in), and with FOG = 2 while some fog varies through its volume
    // (fogLevel). Scenes without either use the plain ones, which leave that code out (smaller; the register counts
    // don't follow the size, see FOG in the shader).
    VkPipeline rtFogPipelines[2][7] = {};
    uint32_t fogLevel = 0;  // this frame's FOG (buildSceneUniforms)
    VkPipeline tracePipeline(VkPipeline plain) const;  // plain, or its fog version in a fog scene
    // Whether the pipelines above were built as the basic shader (settings.basicShader). They are rebuilt when it
    // changes (updateTracePipelines, before a frame).
    bool basicTraceShader = false;
    // What the driver reports about the compiled trace shaders (VK_KHR_pipeline_executable_properties, if the driver
    // has it): registers per thread etc., for the Debug panel. Empty if not available.
    std::string rtPipelineStatistics;  // as text, for the UI
    std::vector<PipelineStatistics> rtPipelineStatisticsList;

    // Radiance cache (RadianceCache.cpp): hash table of surface cells, on the GPU only
    static constexpr uint32_t cacheCapacity = 1u << 20;  // entries (40 MB in all)
    static constexpr float cacheMaxWeight = 256.0f;
    static constexpr uint32_t cacheMaxAge = 128;
    VkBuffer cacheKeyBuffer, cacheAccumulatorBuffer, cacheValueBuffer, cacheAgeBuffer, cacheStatsBuffer;
    VmaAllocation cacheKeyMemory, cacheAccumulatorMemory, cacheValueMemory, cacheAgeMemory, cacheStatsMemory;
    std::vector<VkBuffer> cacheStatsReadbackBuffers;
    std::vector<VmaAllocation> cacheStatsReadbackMemories;
    std::vector<void*> cacheStatsReadbackMapped;
    uint32_t cacheUsedEntries = 0;      // for the UI, a few frames late
    bool cacheClearPending = true;      // empty the cache before the next update (new scene, lighting changed)
    bool cacheQueriesSuspended = false; // noise reference: plain path tracing (the cache keeps filling)
    VkDescriptorSetLayout cacheDescriptorSetLayout;
    VkDescriptorPool cacheDescriptorPool;
    VkDescriptorSet cacheDescriptorSet;
    VkPipelineLayout cachePipelineLayout;
    VkPipeline cachePipeline;

    // DLSS Ray Reconstruction via Streamline (Dlss.cpp)
    bool dlssDisabledByFlag = false;   // --no-dlss: Streamline is never loaded
    bool dlssCnnVersion = false;       // --dlss-cnn: Streamline's plugins from dlss/cnn instead of dlss/transformer
    std::string dlssVersion;           // of the loaded DLSS RR (NGX), for the UI
    PresentationFunctions vkp;         // swapchain / present / wait idle, see PresentationFunctions
    void* streamlineModule = nullptr;  // HMODULE of sl.interposer.dll
    bool streamlineInitialized = false;
    bool dlssSupported = false;
    std::string dlssStatus = "not checked";
    int activeRenderMode = static_cast<int>(RenderMode::Accumulation);  // what the images are currently built for
    int activeDlssModel = static_cast<int>(DlssModel::TransformerF);    // what DLSS's resources were made for
    RenderImage dlssOutputImage;       // RGBA16F at output size: the denoised, upscaled HDR image (DLSS modes only)
    bool dlssResourcesInUse = false;   // DLSS created its own resources (on the first evaluate); free them on changes
    bool dlssReset = true;             // the next frame has no usable history (mode/size/scene changed)
    uint32_t dlssFrameIndex = 0;
    std::vector<std::string> streamlineInstanceExtensions;  // what Streamline asks for
    std::vector<std::string> streamlineDeviceExtensions;
    std::vector<std::string> streamlineFeatures12;  // Vulkan 1.2 / 1.3 feature names it needs enabled
    std::vector<std::string> streamlineFeatures13;

    // Automatic exposure (Exposure.cpp): histogram + adapted exposure on the GPU, read back for the UI
    VkBuffer exposureBuffer;
    VmaAllocation exposureMemory;
    std::vector<VkBuffer> exposureReadbackBuffers;  // one per frame in flight
    std::vector<VmaAllocation> exposureReadbackMemories;
    std::vector<void*> exposureReadbackMapped;
    ExposureState exposureInfo{};  // latest copy for the UI
    bool exposureInfoValid = false;
    bool exposureReset = true;     // snap to the target instead of adapting (start, auto just switched on)
    bool exposureWasAuto = false;
    VkDescriptorSetLayout exposureDescriptorSetLayout;
    VkDescriptorPool exposureDescriptorPool;
    VkDescriptorSet exposureDescriptorSet;
    VkPipelineLayout exposurePipelineLayout;
    VkPipeline exposurePipeline;
    float frameDeltaTime = 0.0f;   // seconds, given to beginFrame
    float fogTime = 0.0f;          // seconds the fog has drifted (only counts while some fog drifts)

    // Noise measurement (Noise.cpp): a converged reference of the current view, then the error of the displayed
    // image against it, frame by frame. Resources are created on first use.
    bool noiseResourcesCreated = false;
    RenderImage noiseReferenceImage;          // R32F (luminance) at output size
    VkExtent2D noiseReferenceExtent{};
    bool noiseReferenceValid = false;
    float noiseReferenceLuminance = 0.0f;     // average over the reference image
    uint32_t noiseGroupCapacity = 0;          // workgroup sums the buffers below have room for
    std::vector<VkBuffer> noiseSumBuffers;    // one per frame in flight, host visible: the GPU writes, the CPU reads
    std::vector<VmaAllocation> noiseSumMemories;
    std::vector<void*> noiseSumMapped;
    struct NoisePending { uint32_t pass = 0; uint32_t frame = 0; uint32_t groups = 0; bool active = false; };
    std::vector<NoisePending> noisePending;   // what each frame slot's buffer holds once its fence has passed
    VkDescriptorSetLayout noiseDescriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool noiseDescriptorPool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> noiseDescriptorSets;  // one per frame in flight (each with its own sum buffer)
    VkPipelineLayout noisePipelineLayout = VK_NULL_HANDLE;
    VkPipeline noisePipeline = VK_NULL_HANDLE;
    NoiseState noiseState = NoiseState::Idle;
    int noiseReferenceFrames = 4096;          // UI: frames accumulated for the reference
    int noiseMeasureFrames = 256;             // UI: frames measured
    int noiseMeasureMode = 0;                 // RenderMode the measurement runs in
    glm::ivec4 noiseRegion = glm::ivec4(0);   // x0, y0, x1, y1 in output pixels; empty = the whole image
    uint32_t noiseFrame = 0;                  // frames measured so far
    uint32_t noiseRestartsAtStart = 0;
    std::vector<NoiseResult> noiseResults;
    std::string noiseStatus;
    // Run from code (startNoiseRun): reference, then measure in noiseAutoMode
    bool noiseAutoRun = false;
    int noiseAutoMode = 0;
    bool noiseAutoContinue = false;  // the run's reference is being built

    // Display pass (Display.cpp): exposure + tone mapping into an 8-bit image at output size, blitted to the swapchain
    VkImage displayImage;
    VmaAllocation displayImageMemory;
    VkImageView displayImageView;
    VkDescriptorSetLayout displayDescriptorSetLayout;
    VkDescriptorPool displayDescriptorPool;
    VkDescriptorSet displayDescriptorSet;
    VkPipelineLayout displayPipelineLayout;
    VkPipeline displayPipeline;

    // GPU time of each pass (cache, ReSTIR, trace, DLSS, exposure, display): timestamps before and after them, one set of queries
    // per frame in flight, read when that frame slot is recorded again. Smoothed over ~20 frames, in ms.
    static constexpr uint32_t timestampsPerFrame = 7;  // before the cache, after cache / ReSTIR / trace / DLSS / exposure / display
    VkQueryPool timestampQueryPool = VK_NULL_HANDLE;
    std::vector<bool> timestampsWritten;
    float gpuPassTimes[timestampsPerFrame - 1] = {};
    float lastPassTimes[timestampsPerFrame - 1] = {};  // the same, unsmoothed, of the frame last read back
    uint64_t timestampFramesRead = 0;

    // Updated every frame with vkCmdUpdateBuffer
    VkBuffer uniformBuffer;
    VmaAllocation uniformBufferMemory;

    // Frame capture (Capture.cpp): the button sets captureRequested, the frame copies the display image into
    // captureBuffer, the next loop iteration writes it as a PNG
    bool captureRequested = false;
    std::string captureFileRequested;  // "": a name from the date and time
    std::string captureFile;           // of the copy recorded
    VkBuffer captureBuffer = VK_NULL_HANDLE;
    VmaAllocation captureMemory = VK_NULL_HANDLE;
    VkExtent2D captureExtent{};
    uint32_t captureFrame = 0;   // frame in flight that recorded the copy
    std::string captureMessage;  // shown next to the button

    RenderSettings settings;
    uint32_t frameIndex = 0;

    // Accumulation restarts whenever the uniforms (camera, light, settings) differ from the previous frame.
    uint32_t accumulatedFrames = 0;
    uint32_t accumulationRestarts = 0;  // counts the restarts (also in DLSS modes: the uniforms changed)
    bool resetAccumulation = true;
    SceneUniforms previousUniforms{};




    void cleanup();

    void drawFrame();

    void createSwapChain();
    void createImageViews();
    void createRenderPass();
    void createFramebuffers();
    void createCommandBuffers();
    void recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex);
    void createSyncObjects();
    void createRenderFinishedSemaphores();
    void destroyRenderFinishedSemaphores();
    void recreateSwapChain();
    void cleanupSwapChain();

    // RayTracer.cpp
    void createUniformBuffer();
    // What a change in the scene asks of the rest (GpuScene::changes): applied after the scene's update and uploads
    void applySceneChanges();


    // Materials.cpp
    void createSpecularAlbedoTable();
    void destroySpecularAlbedoTable();
    // BlueNoise.cpp
    void createBlueNoiseTexture();
    void destroyBlueNoiseTexture();
    // Dlss.cpp
    void initStreamline();
    void addStreamlineDeviceRequirements(std::vector<const char*>& extensions, VkPhysicalDeviceVulkan12Features& features12,
                                         VkPhysicalDeviceVulkan13Features& features13);
    void connectStreamline();
    void shutdownStreamline();
    bool dlssActive() const { return activeRenderMode != static_cast<int>(RenderMode::Accumulation); }
    bool dlssModelAvailable(int model) const;
    VkExtent2D dlssRenderExtent(int renderMode, VkExtent2D output);
    void applyRenderMode();
    void recordDlss(VkCommandBuffer commandBuffer);
    void freeDlssResources();
    void buildRenderModeUi();

    // Exposure.cpp
    void createExposureResources();
    void updateExposureDescriptorSet();
    void recordExposure(VkCommandBuffer commandBuffer);
    void destroyExposureResources();
    void buildExposureUi();

    // RadianceCache.cpp
    void createRadianceCache();
    void recordRadianceCache(VkCommandBuffer commandBuffer);
    void destroyRadianceCache();
    void buildRadianceCacheUi();
    void buildRestirUi();

    // Noise.cpp
    void ensureNoiseResources();
    void updateNoiseDescriptorSets();
    void startNoiseReference();
    void startNoiseMeasurement(int renderMode);
    void readNoiseResults();
    void finishNoiseMeasurement();
    void recordNoise(VkCommandBuffer commandBuffer);
    void destroyNoiseResources();
    void buildNoiseUi();

    // Display.cpp
    void chooseExtents();
    void applyOutputSize();
    // RenderSettings::outputSize -> rows of the output image (0: the window's); names in buildRenderModeUi
    static constexpr uint32_t outputHeights[] = {0, 1080, 1440, 2160};
    void recreateRenderImages();
    PerPixelBuffers wantedPerPixelBuffers();
    void applyPerPixelBuffers();
    VkImageView displayedHdrView() const;  // what exposure and display read: DLSS output or the radiance image
    void createDisplayImages();
    void destroyDisplayImages();
    void createDisplayResources();
    void updateDisplayDescriptorSet();
    void recordDisplay(VkCommandBuffer commandBuffer);
    void destroyDisplayResources();

    void createRayTracingImages();
    void destroyRayTracingImages();
    void createRayTracingDescriptors();
    void updateRayTracingDescriptorSet();
    void createRayTracingPipeline();
    void createTracePipelines(bool basic);
    void destroyTracePipelines();
    void updateTracePipelines();
    std::vector<std::pair<std::string, std::string>> readPipelineStatistics(VkPipeline pipeline);
    SceneUniforms buildSceneUniforms();
    void recordCapture(VkCommandBuffer commandBuffer);
    void finishCapture();
    void createTimestampQueries();
    void readTimestamps();
    void writeTimestamp(VkCommandBuffer commandBuffer, uint32_t index);
    void recordRayTracing(VkCommandBuffer commandBuffer, uint32_t imageIndex);
    void cleanupRayTracing();

    void initImGui();
    void cleanupImGui();
};


#endif //VULKANDEMO_RENDERER_H
