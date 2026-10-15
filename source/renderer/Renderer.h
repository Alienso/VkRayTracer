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
#include "renderer/Capture.h"
#include "renderer/Exposure.h"
#include "renderer/Noise.h"
#include "renderer/Display.h"
#include "renderer/RadianceCache.h"
#include "renderer/RayTracer.h"
#include "renderer/Dlss.h"
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
    void requestCapture(const std::string& fileName = "") { capture.request(fileName); }
    [[nodiscard]] bool capturePending() const { return capture.pending(); }

    // The settings the UI edits; a bench sets them from a file (RenderSettingsFile.h)
    [[nodiscard]] const RenderSettings& renderSettings() const { return settings; }
    void setRenderSettings(const RenderSettings& newSettings) { settings = newSettings; }

    // The noise tool, run from code: accumulates a reference of the current view for referenceFrames, then measures
    // measureFrames in renderMode (see Noise.cpp). Busy until the results are in noiseResultList.
    void startNoiseRun(int renderMode, int referenceFrames, int measureFrames, glm::ivec4 region) {
        noise.startNoiseRun(renderMode, referenceFrames, measureFrames, region);
    }
    [[nodiscard]] bool noiseRunBusy() const { return noise.noiseRunBusy(); }
    [[nodiscard]] const std::vector<NoiseResult>& noiseResultList() const { return noise.noiseResults; }
    // The trace shader's pipelines, as the driver compiled them (empty without VK_KHR_pipeline_executable_properties)
    [[nodiscard]] const std::vector<PipelineStatistics>& tracePipelineStatistics() const { return trace.rtPipelineStatisticsList; }

    // The renderer's settings panels, for the editor's window (ImGuiInit.cpp, Environment.cpp, Lights.cpp)
    void buildRenderingUi();
    void buildDebugUi();
    void buildShadowsUi();
    void buildSkySamplingUi();
    void buildPunctualLightSettingsUi();

    // The window's framebuffer changed size: the swapchain is remade after this frame
    void resize() { framebufferResized = true; }
    void setDlssDisabled(bool disabled) {  // --no-dlss
        dlss.dlssDisabledByFlag = disabled;
    }
    void setDlssCnn(bool cnn) {  // --dlss-cnn
        dlss.dlssCnnVersion = cnn;
    }

private:
    GLFWwindow* window = nullptr;  // the program's
    // The device, its queues, memory allocator, command pool and pipeline cache, and helpers for buffers and images
    GpuContext gpu;

    // The swapchain (VulkanInitHelper.cpp), the render pass the UI is drawn in, a command buffer per frame in flight
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
    // Swapchain, acquire, present and wait idle: Vulkan's, or Streamline's proxies once DLSS is connected
    PresentationFunctions vkp;

    // What is loaded (the program's, Application.h) and what this frame draws (given to render())
    AssetStore& assets;
    const RenderList* renderList = nullptr;
    RenderSettings settings;  // the UI edits them (the settings panels); benches set them

    // Two sizes (chooseExtents): the path tracer renders at renderExtent, the display pass writes outputExtent
    // (the window, or bigger: RenderSettings::outputSize). Equal in accumulation mode; DLSS renders smaller and upscales.
    VkExtent2D renderExtent{};
    VkExtent2D outputExtent{};
    int activeOutputSize = 0;  // the RenderSettings::outputSize the images are built for
    int activeRenderMode = static_cast<int>(RenderMode::Accumulation);  // what the images are currently built for
    RenderImage dlssOutputImage;  // RGBA16F at output size: the denoised, upscaled HDR image (DLSS modes only)

    // This frame: its number, the uniforms and their inputs (Frame.cpp)
    uint64_t frameNumber = 0;  // submitted frames (currentFrame is only the slot)
    uint32_t frameIndex = 0;
    uint32_t fogLevel = 0;     // this frame's FOG (buildSceneUniforms): which of the trace pipelines are used
    float frameDeltaTime = 0.0f;  // seconds, given to beginFrame
    float fogTime = 0.0f;         // seconds the fog has drifted (only counts while some fog drifts)
    // Accumulation restarts whenever the uniforms (camera, light, settings) differ from the previous frame.
    uint32_t accumulatedFrames = 0;
    uint32_t accumulationRestarts = 0;  // counts the restarts (also in DLSS modes: the uniforms changed)
    bool resetAccumulation = true;
    SceneUniforms previousUniforms{};
    bool dlssReset = true;  // the next frame has no usable history for DLSS (mode/size/scene changed)
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
    // GPU time of each pass (cache, ReSTIR, trace, DLSS, exposure, display): timestamps before and after them, one set
    // of queries per frame in flight, read when that frame slot is recorded again. Smoothed over ~20 frames, in ms.
    static constexpr uint32_t timestampsPerFrame = 7;  // before the cache, after cache / ReSTIR / trace / DLSS / exposure / display
    VkQueryPool timestampQueryPool = VK_NULL_HANDLE;
    std::vector<bool> timestampsWritten;
    float gpuPassTimes[timestampsPerFrame - 1] = {};
    float lastPassTimes[timestampsPerFrame - 1] = {};  // the same, unsmoothed, of the frame last read back
    uint64_t timestampFramesRead = 0;

    // The parts, in the order a frame uses them. Each gets the renderer state it reads or sets as references (its
    // constructor lists them); one that binds to another part's members is declared after it.
    //
    // The scene on the GPU: geometry, BLASes and the TLAS, instances, materials and textures, lights, portals, the
    // environment, fog volumes, and the buffers that stream their changes (GpuScene.h)
    GpuScene scene{gpu, assets, settings};
    // The trace pass (RayTracer.h): the images and per-pixel buffers the trace shader writes, its descriptors and
    // pipelines
    RayTracer trace{gpu, settings, scene, cache, renderExtent, fogLevel, resetAccumulation, hasPreviousViewProjection,
                    [this]() { return wantedPerPixelBuffers(); }, [this]() { vkp.deviceWaitIdle(gpu.device); }};
    // Radiance cache (RadianceCache.h): hash table of surface cells, on the GPU only
    RadianceCache cache{gpu, settings, currentFrame, renderExtent, trace.rtDescriptorSet, trace.rtPipelineLayout,
                        [this]() { return trace.tracePipeline(trace.rtCacheUpdatePipeline); }};
    // DLSS Ray Reconstruction via Streamline (Dlss.h)
    Dlss dlss{gpu, vkp, settings, trace, outputExtent, renderExtent, renderList, frameJitter, frameView, frameProjection,
              frameViewProjection, framePreviousViewProjection, dlssOutputImage, dlssReset, activeRenderMode};
    // Automatic exposure (Exposure.h): histogram + adapted exposure on the GPU, read back for the UI
    ExposurePass exposure{gpu, settings, currentFrame, outputExtent, frameDeltaTime, [this]() { return displayedHdrView(); }};
    // Noise measurement (Noise.h): the error of the displayed image against an accumulated reference
    NoiseTool noise{gpu, settings, currentFrame, outputExtent, accumulatedFrames, accumulationRestarts, activeRenderMode,
                    resetAccumulation, cache.cacheQueriesSuspended, dlssReset, lastPassTimes,
                    [this]() { return displayedHdrView(); }, [this]() { return dlssActive(); },
                    [this]() { vkp.deviceWaitIdle(gpu.device); }};
    // Display pass (Display.h): exposure + tone mapping into an 8-bit image at output size, blitted to the swapchain
    DisplayPass display{gpu, settings, swapChainImageFormat, outputExtent, renderExtent, fogLevel, frameNumber,
                        trace.perPixelBuffers, exposure.exposureBuffer, trace.radianceImageView,
                        trace.linearDepthImage, trace.motionVectorImage, trace.diffuseAlbedoImage,
                        trace.specularAlbedoImage, trace.normalRoughnessImage, trace.specularHitDistanceImage,
                        trace.guideHistoryImage, [this]() { return displayedHdrView(); }};
    // Frame capture (Capture.h): the display image of a frame as a PNG
    FrameCapture capture{gpu, inFlightFences, currentFrame, display.displayImage, outputExtent};

    void cleanup();
    void drawFrame();

    // VulkanInitHelper.cpp
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

    // Frame.cpp
    bool dlssActive() const { return activeRenderMode != static_cast<int>(RenderMode::Accumulation); }
    // What a change in the scene asks of the rest (GpuScene::changes): applied after the scene's update and uploads
    void applySceneChanges();
    SceneUniforms buildSceneUniforms();
    void createTimestampQueries();
    void readTimestamps();
    void writeTimestamp(VkCommandBuffer commandBuffer, uint32_t index);
    void recordFrame(VkCommandBuffer commandBuffer, uint32_t imageIndex);
    void cleanupRayTracing();

    // RenderImages.cpp
    void chooseExtents();
    void applyOutputSize();
    // RenderSettings::outputSize -> rows of the output image (0: the window's); names in buildRenderModeUi
    static constexpr uint32_t outputHeights[] = {0, 1080, 1440, 2160};
    void recreateRenderImages();
    PerPixelBuffers wantedPerPixelBuffers();
    void applyPerPixelBuffers();
    void applyRenderMode();  // accumulation <-> the DLSS modes (new render size), DLSS models
    VkImageView displayedHdrView() const;  // what exposure and display read: DLSS output or the radiance image
    void createDisplayImages();  // the display image (DisplayPass) and, in the DLSS modes, DLSS's output image
    void destroyDisplayImages();

    // ImGuiInit.cpp: the UI's Vulkan side, and the settings panels
    void initImGui();
    void cleanupImGui();
    void buildRestirUi();
    void buildRenderModeUi();
};


#endif //VULKANDEMO_RENDERER_H
