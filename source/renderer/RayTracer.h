//
// The trace pass: the path tracer itself (shaders/raytrace.comp and its parts). One shader, compiled as 21 pipelines
// (specialization constants: the image paths, the radiance cache's update paths, ReSTIR DI's and GI's passes, each
// with and without the fog code, and the basic shader without the rarer features). It writes the radiance image and,
// for DLSS, the guide images at render size; ReSTIR keeps per-pixel buffers between frames. See RayTracer.cpp.
//
// The renderer builds the uniforms and records the passes in order (Frame.cpp); this pass records its dispatches
// (recordRestir, recordImagePaths) and owns what they read and write.
//

#ifndef VULKANDEMO_RAYTRACER_H
#define VULKANDEMO_RAYTRACER_H

#include <vulkan/vulkan.h>
#include <functional>
#include <string>
#include <utility>
#include <vector>
#include "renderer/GpuContext.h"
#include "renderer/GpuScene.h"
#include "renderer/RadianceCache.h"
#include "renderer/RendererTypes.h"

class RayTracer {
public:
    static constexpr uint32_t maxTextures = GpuScene::maxTextures;  // size of the texture descriptor array

    // The renderer's: settings, the scene and the radiance cache (their buffers are bound), render size, this frame's
    // fog level (which pipelines), its accumulation and motion flags (reset with new images or pipelines), which
    // per-pixel buffers this frame's settings want, and its wait-idle (through Streamline with DLSS)
    RayTracer(GpuContext& gpu, const RenderSettings& settings, GpuScene& scene, RadianceCache& cache,
              const VkExtent2D& renderExtent, const uint32_t& fogLevel, bool& resetAccumulation,
              bool& hasPreviousViewProjection, std::function<PerPixelBuffers()> wantedPerPixelBuffers,
              std::function<void()> waitIdle)
        : gpu(gpu), settings(settings), scene(scene), cache(cache), renderExtent(renderExtent), fogLevel(fogLevel),
          resetAccumulation(resetAccumulation), hasPreviousViewProjection(hasPreviousViewProjection),
          wantedPerPixelBuffers(std::move(wantedPerPixelBuffers)), waitIdle(std::move(waitIdle)) {}

    void createUniformBuffer();
    // RayTracer.cpp
    void createRayTracingImages();  // at render size; the per-pixel buffers the settings want (wantedPerPixelBuffers)
    void destroyRayTracingImages();
    void createRayTracingDescriptors();
    void updateRayTracingDescriptorSet();  // a buffer or image was replaced; no frame may use the set
    void createRayTracingPipeline();
    void createTracePipelines(bool basic);
    void destroyTracePipelines();
    void updateTracePipelines();  // before a frame: rebuilt when settings.basicShader changed
    VkPipeline tracePipeline(VkPipeline plain) const;  // plain, or its fog version in a fog scene
    std::vector<std::pair<std::string, std::string>> readPipelineStatistics(VkPipeline pipeline);
    void recordRestir(VkCommandBuffer commandBuffer, const SceneUniforms& uniforms);
    void recordImagePaths(VkCommandBuffer commandBuffer, const SceneUniforms& uniforms);
    // Materials.cpp
    void createSpecularAlbedoTable();
    void destroySpecularAlbedoTable();
    // BlueNoise.cpp
    void createBlueNoiseTexture();
    void destroyBlueNoiseTexture();

    // Updated every frame with vkCmdUpdateBuffer (Frame.cpp)
    VkBuffer uniformBuffer;
    VmaAllocation uniformBufferMemory;

    // How much light the GGX reflection reflects in total, per view angle and roughness (see Materials.cpp)
    static constexpr uint32_t specularAlbedoSize = 32;
    VkBuffer specularAlbedoBuffer;
    VmaAllocation specularAlbedoMemory;
    // 64 x 64 blue-noise texels, 4 channels of 16 bits (BlueNoise.cpp), binding 30
    VkBuffer blueNoiseBuffer;
    VmaAllocation blueNoiseMemory;

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
    // Which of them have a pixel's worth for every render pixel. The others are 1 x 1 placeholders (the descriptors
    // need something), never touched by the shader: their features are off (buildSceneUniforms).
    PerPixelBuffers perPixelBuffers;
    // Last frame's reservoirs can be reused: ReSTIR ran, at this size, with the same lights
    bool restirHistoryAvailable = false;
    bool restirLightsChanged = false;  // this frame uploads new lights (their indices may have changed)
    uint32_t restirCurrentHalf = 0;
    bool restirGiHistoryAvailable = false;  // ReSTIR GI ran last frame, at this size

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
    // Whether the pipelines above were built as the basic shader (settings.basicShader). They are rebuilt when it
    // changes (updateTracePipelines, before a frame).
    bool basicTraceShader = false;
    // What the driver reports about the compiled trace shaders (VK_KHR_pipeline_executable_properties, if the driver
    // has it): registers per thread etc., for the Debug panel. Empty if not available.
    std::string rtPipelineStatistics;  // as text, for the UI
    std::vector<PipelineStatistics> rtPipelineStatisticsList;

private:
    GpuContext& gpu;
    const RenderSettings& settings;
    GpuScene& scene;
    RadianceCache& cache;
    const VkExtent2D& renderExtent;
    const uint32_t& fogLevel;
    bool& resetAccumulation;
    bool& hasPreviousViewProjection;
    std::function<PerPixelBuffers()> wantedPerPixelBuffers;
    std::function<void()> waitIdle;
};

#endif //VULKANDEMO_RAYTRACER_H
