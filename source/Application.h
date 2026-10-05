//
// Created by Alienson on 19.1.2024..
//

#ifndef VULKANDEMO_APPLICATION_H
#define VULKANDEMO_APPLICATION_H


#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <vector>
#include <functional>
#include <string>
#include <map>
#include "Camera.h"
#include "ModelLoader.h"

struct AccelerationStructure {
    VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceAddress address = 0;
};

// Must match the SceneUniforms block in shaders/raytrace.comp (std140: matrices and vec4s first, no vec3s)
struct SceneUniforms {
    glm::mat4 viewProjection;          // this frame, without jitter (motion vectors)
    glm::mat4 previousViewProjection;  // last frame's
    glm::vec4 cameraPosition;
    glm::vec4 cameraForward;
    glm::vec4 cameraRight;
    glm::vec4 cameraUp;
    glm::vec2 jitter;
    float tanHalfFov;
    float aspectRatio;
    glm::vec4 sunDirection;  // w = cos(sun angular radius)
    glm::vec4 sunColor;
    glm::vec4 skyZenithColor;
    glm::vec4 skyHorizonColor;
    glm::vec4 groundColor;
    uint32_t samplesPerPixel;
    uint32_t maxBounces;
    uint32_t frameIndex;
    uint32_t shadowsEnabled;
    uint32_t accumulatedFrames;
    uint32_t accumulate;
    uint32_t debugView;
    uint32_t emissiveTriangleCount;  // entries in the light list
    uint32_t emissiveSampling;       // EmissiveSampling
    uint32_t blurCaustics;           // 1 = path regularization, see tracePath in the shader
    float farDistance;               // view depth written where rays hit nothing
    uint32_t writeGuides;            // 1 = write the DLSS input images (depth, motion vectors, albedos, ...)
    uint32_t cacheQuery;             // 1 = image paths end in the radiance cache where they can
    uint32_t cacheCapacity;          // entries in the cache's hash table
    float cacheCellScale;            // cell size per unit of distance from the camera
    float cacheMinWeight;            // samples a cell needs before it is used
    uint32_t cacheUpdateTile;        // one update path per cacheUpdateTile x cacheUpdateTile pixels
};

// Must match the push constants in shaders/cache_resolve.comp
struct CacheResolvePushConstants {
    uint32_t capacity;
    float decay;      // weight the old samples keep each frame
    float maxWeight;  // cap of a cell's sample weight
    uint32_t maxAge;  // frames without new samples until a cell is freed
};

// Must match ExposureState in shaders/exposure.comp (scalar layout). Lives on the GPU; the display pass
// reads the exposure from it, and a copy comes back to the CPU (a few frames late) for the UI.
static constexpr uint32_t exposureBinCount = 256;
struct ExposureState {
    uint32_t histogram[exposureBinCount];      // being filled during a frame
    uint32_t lastHistogram[exposureBinCount];  // the finished histogram of the last metered frame
    float exposure;          // current multiplier
    float targetExposure;    // where the adaptation is heading
    float averageLuminance;  // of the metered part of the image
    float lowLuminance;      // metered range: darker and brighter pixels are ignored
    float highLuminance;
    uint32_t meteredPixels;
};

// Must match the push constants in shaders/exposure.comp
struct ExposurePushConstants {
    uint32_t pass;
    uint32_t reset;
    uint32_t centerWeighted;
    float minLog2;
    float log2Range;
    float lowFraction;
    float highFraction;
    float key;
    float minExposure;
    float maxExposure;
    float brightenSpeed;
    float darkenSpeed;
    float deltaTime;
};

// Must match the push constants in shaders/display.comp
struct DisplayPushConstants {
    float exposure;         // manual multiplier, or 2^compensation on top of the measured exposure
    uint32_t autoExposure;  // 1 = multiply by the measured exposure (ExposureState) as well
    uint32_t toneMapping;   // 0 = clamp, 1 = ACES
    uint32_t view;          // DisplayView
    float farDistance;      // sky depth, to show it apart in the depth views
};

// Must match the push constants in shaders/noise.comp
struct NoisePushConstants {
    uint32_t pass;         // 0 = copy the image into the reference, 1 = compare with it
    float floorLuminance;  // added to the reference in the relative error's denominator
    glm::ivec2 regionMin;  // a rectangle whose brightness is also measured on its own
    glm::ivec2 regionMax;
};

// One frame of a noise measurement (Noise.cpp)
struct NoiseResult {
    uint32_t frame;      // 1 = the first frame after the start
    float relativeMse;   // mean of ((measured - reference) / (reference + floor))^2 over the image, luminance
    float brightness;    // measured / reference, summed over the image (1 = no bias)
    float regionBrightness;  // the same inside noiseRegion only (local bias, e.g. light leaking into a corner)
    float traceMs;       // GPU time of this frame's trace (+ radiance cache) and DLSS passes
    float dlssMs;
};
enum class NoiseState { Idle, BuildingReference, WaitingToMeasure, Measuring };

// What the display pass shows. Must match the constants in shaders/display.comp.
enum class DisplayView : uint32_t {
    Color = 0,        // the path traced image, with exposure and tone mapping
    PassThrough,      // a debug view of the trace shader, values as they are
    Depth, MotionVectors, DiffuseAlbedo, SpecularAlbedo, Normals, Roughness, SpecularHitDistance,
    ReprojectionError
};

// How the image is made. Accumulation: the path tracer averages frames at full size (the reference).
// DLSS RR: one noisy frame at a smaller render size, denoised and upscaled by DLSS Ray Reconstruction.
enum class RenderMode : int {
    Accumulation = 0, DlssDlaa, DlssQuality, DlssBalanced, DlssPerformance, DlssUltraPerformance
};

// Which network DLSS RR runs (its "render presets", see toDlssPreset in Dlss.cpp). The transformer models look
// better; the older CNN models cost about half on a 20/30 series GPU. Which ones exist depends on the DLSS RR version
// loaded at startup (dlssModelAvailable): the current one has transformer D-F, the one loaded with --dlss-cnn has
// transformer D-E and CNN A-C.
enum class DlssModel : int {
    TransformerF = 0, TransformerE, TransformerD, CnnA, CnnB, CnnC
};

// The Vulkan functions Streamline has to see with manual hooking (sl_hooks.h): swapchain, acquire, present,
// wait idle. Vulkan's own until Streamline is connected, then Streamline's proxies (which call Vulkan's).
struct PresentationFunctions {
    PFN_vkCreateSwapchainKHR createSwapchain = vkCreateSwapchainKHR;
    PFN_vkDestroySwapchainKHR destroySwapchain = vkDestroySwapchainKHR;
    PFN_vkGetSwapchainImagesKHR getSwapchainImages = vkGetSwapchainImagesKHR;
    PFN_vkAcquireNextImageKHR acquireNextImage = vkAcquireNextImageKHR;
    PFN_vkQueuePresentKHR queuePresent = vkQueuePresentKHR;
    PFN_vkDeviceWaitIdle deviceWaitIdle = vkDeviceWaitIdle;
};

// An image with its memory and view (the DLSS input images)
struct RenderImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

// Must match MeshInfo in shaders/raytrace.comp (scalar layout). One per mesh, indexed by the
// TLAS instance's custom index. Tells the shader where the mesh's data lives in the shared buffers.
struct MeshInfo {
    uint32_t materialIndex;  // into the material buffer
    uint32_t vertexOffset;  // first vertex in the vertex buffer
    uint32_t indexOffset;   // first index in the index buffer
    uint32_t vertexCount;
    uint32_t indexCount;
    uint32_t hasNormals;
    uint32_t hasTangents;
    float unitScale;  // file units per mesh unit (the glTF node's scale), for absorption distances
};

// Must match Material in shaders/raytrace.comp (scalar layout). Material (ModelLoader.h) packed for the GPU:
// emission premultiplied by its strength, disabled textures set to -1.
struct GpuMaterial {
    glm::vec4 baseColor;
    glm::vec3 emissive;
    float metallic;
    float roughness;
    float transmission;
    float ior;
    float alphaCutoff;
    uint32_t alphaMode;
    int32_t baseColorTexture;
    int32_t metallicRoughnessTexture;
    int32_t transmissionTexture;
    int32_t emissiveTexture;
    int32_t normalTexture;
    float normalScale;
    uint32_t normalTwoChannel;  // 1 = the normal texture has X and Y only (BC5), Z is rebuilt
    uint32_t solid;          // 1 = closed volume that refracts, 0 = thin
    glm::vec3 absorption;    // per unit of distance inside a solid (Beer's law), 0 = clear
    glm::mat3x2 baseColorUvTransform;  // KHR_texture_transform per texture, see Material
    glm::mat3x2 metallicRoughnessUvTransform;
    glm::mat3x2 transmissionUvTransform;
    glm::mat3x2 emissiveUvTransform;
    glm::mat3x2 normalUvTransform;
    float diffuseTransmission;              // KHR_materials_diffuse_transmission, see Material
    glm::vec3 diffuseTransmissionColor;
    int32_t diffuseTransmissionTexture;
    int32_t diffuseTransmissionColorTexture;
    glm::mat3x2 diffuseTransmissionUvTransform;
    glm::mat3x2 diffuseTransmissionColorUvTransform;
};

// A texture on the GPU. Samplers are shared between textures with the same settings.
struct GpuTexture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
};

// Material in the scene, with UI switches to ignore its textures and look at the constants alone.
struct SceneMaterial {
    Material material;
    bool useBaseColorTexture = true;
    bool useMetallicRoughnessTexture = true;
    bool useTransmissionTexture = true;
    bool useEmissiveTexture = true;
    bool useNormalTexture = true;
    bool useDiffuseTransmissionTexture = true;
    bool useDiffuseTransmissionColorTexture = true;
};

// The first group is drawn by the trace shader (one property of the first hit). The "guide" group shows the
// DLSS input images as written during normal path tracing; the display pass draws them.
enum class DebugView : int {
    None = 0, Normals, TexCoords, MeshIndex, MaterialIndex,
    BaseColor, Metallic, Roughness, Transmission, Emissive, Tangents,
    RadianceCache,  // the cached light of the cell at the first hit (with exposure, like the image)
    GuideDepth, GuideMotionVectors, GuideDiffuseAlbedo, GuideSpecularAlbedo, GuideNormals, GuideRoughness,
    GuideSpecularHitDistance, GuideReprojectionError
};
inline bool isGuideView(int view) { return view >= static_cast<int>(DebugView::GuideDepth); }

// A model file, loaded once. Any number of objects can show it.
struct SceneModel {
    std::string name;                 // file name, or "Floor" for the built-in floor
    std::vector<MeshInstance> parts;  // meshIndex into Application::sceneMeshes, transform from the file's node tree
    glm::mat4 fit = glm::mat4(1.0f);  // scales the model to a standard size and stands it on y = 0 (unless originalSize)
    uint32_t firstMaterial = 0;       // the model's materials are sceneMaterials[firstMaterial .. + materialCount)
    uint32_t materialCount = 0;
};

// Something placed in the world: a model plus a transform edited in ImGui.
struct SceneObject {
    std::string name;
    uint32_t modelIndex = 0;
    bool visible = true;
    glm::vec3 position = glm::vec3(0.0f);
    glm::vec3 rotation = glm::vec3(0.0f);  // degrees around X, Y, Z; applied in the order Y (yaw), X (pitch), Z (roll)
    glm::vec3 scale = glm::vec3(1.0f);
    bool uniformScale = true;  // UI only: edit all three scale axes together
    bool originalSize = false;  // skip the model's fit: keep the file's own size and position (glTF: meters)
    std::vector<bool> hiddenParts;  // per part of the model; missing entries count as visible

    glm::mat4 transform() const;           // translate * rotate * scale
    glm::mat4 previousTransform = glm::mat4(1.0f);  // last frame's transform, for motion vectors
    bool hasPreviousTransform = false;              // false in the object's first frame (no motion then)
};

// Values edited through ImGui
// White: uniform radiance 1 from every direction ("white furnace"). A white material that neither creates nor
// loses energy then looks exactly as bright as the background; anything darker shows where energy is lost.
enum class Environment : int { Sky = 0, Black = 1, White = 2 };

// How light from glowing (emissive) surfaces is found. All three converge to the same image; they differ in noise.
enum class EmissiveSampling : int {
    BouncesOnly = 0,   // only when a bounce ray happens to hit a glowing surface (what we had before)
    LightSampling = 1, // only by aiming shadow rays at random points on glowing triangles
    Mis = 2            // both, weighted by multiple importance sampling
};

// Must match EmissiveTriangle in shaders/raytrace.comp (scalar layout). One per triangle of every visible
// instance with an emissive material, in world space. Shadow rays are aimed at random points on these.
struct EmissiveTriangle {
    glm::vec3 p0, p1, p2;
    glm::vec2 uv0, uv1, uv2;  // to look up the emissive texture at the sampled point
    uint32_t materialIndex;
    float probability;  // chance of picking this triangle: proportional to area * brightness
    float cdf;          // sum of the probabilities up to and including this one (for picking with a binary search)
    float area;
};

// A device-local buffer the CPU refills from time to time. The CPU writes into a staging buffer of the
// current frame in flight (the other frame may still be copying from its own), then the GPU copies it over.
struct StreamedBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    std::vector<VkBuffer> staging;
    std::vector<VkDeviceMemory> stagingMemory;
    std::vector<void*> stagingMapped;
    VkDeviceSize capacity = 0;
};

struct RenderSettings {
    bool sunEnabled = true;
    float sunAzimuth = 35.0f;    // degrees around the Y axis
    float sunElevation = 50.0f;  // degrees above the horizon
    float sunAngularRadius = 2.0f;  // degrees; the real sun is ~0.27, bigger = softer shadows
    glm::vec3 sunColor = {1.0f, 0.95f, 0.85f};
    float sunIntensity = 1.0f;
    bool shadowsEnabled = true;

    int environment = static_cast<int>(Environment::Sky);  // what rays see when they leave the scene
    int emissiveSampling = static_cast<int>(EmissiveSampling::Mis);
    glm::vec3 skyZenithColor = {0.08f, 0.36f, 0.72f};
    glm::vec3 skyHorizonColor = {1.0f, 1.0f, 1.0f};
    glm::vec3 groundColor = {0.35f, 0.3f, 0.35f};
    float skyIntensity = 0.6f;

    int samplesPerPixel = 1;
    int maxBounces = 8;  // glass needs a few: in and out of each pane; Russian roulette ends matte paths early
    bool blurCaustics = true;  // less noise from light reflected by shiny surfaces onto matte ones, slightly biased
    bool radianceCache = true;      // paths stop after a diffuse bounce and read the light from the cache
    float cacheCellPixels = 32.0f;  // cache cells are about this many pixels wide on screen (measured best in Sponza)
    int cacheUpdateTile = 4;        // one cache update path per N x N pixels
    float cacheHistoryFrames = 32.0f;  // old cache samples fade out over about this many frames
    float cacheMinSamples = 4.0f;   // a cell is used once it has this much sample weight
    bool accumulate = true;

    float exposure = 0.6f;          // manual: brightness multiplier
    bool autoExposure = true;       // measure the image and pick the exposure like a camera
    float exposureCompensation = 0.0f;  // auto: stops brighter (+) or darker (-) than measured
    float exposureKey = 0.18f;      // auto: the metered average becomes this (middle grey)
    float meterLowPercent = 40.0f;  // auto: darkest pixels ignored (deep shadows, black background)
    float meterHighPercent = 95.0f; // auto: pixels above this ignored (sun, sky, fireflies)
    bool centerWeighted = false;    // auto: the image center counts up to 4x
    float minExposureStops = -4.0f; // auto: limits of the multiplier, in stops (multiplier 2^stops)
    float maxExposureStops = 10.0f;  // Sponza's courtyard in the default sun wants about +8.5
    float brightenSpeed = 1.5f;     // auto: adaptation rates per second (exponential); like the eye,
    float darkenSpeed = 3.0f;       //       getting used to the dark is slower than to bright light
    bool toneMapping = true;
    int debugView = static_cast<int>(DebugView::None);
    int renderMode = static_cast<int>(RenderMode::Accumulation);  // RenderMode, picked in the UI
    int dlssModel = static_cast<int>(DlssModel::TransformerF);     // DlssModel
};

class Application {
public:
    static constexpr uint32_t maxTextures = 1024;  // size of the texture descriptor array

    void run();
    void setFramebufferResized(bool b){
        this->framebufferResized = b;
    }
    void setDlssDisabled(bool disabled) {  // --no-dlss
        dlssDisabledByFlag = disabled;
    }
    void setDlssCnn(bool cnn) {  // --dlss-cnn
        dlssCnnVersion = cnn;
    }

private:
    GLFWwindow* window;
    VkInstance instance;
    VkDebugUtilsMessengerEXT debugMessenger;
    VkSurfaceKHR surface;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties physicalDeviceProperties;
    VkDevice device;
    VkQueue graphicsQueue;
    VkQueue presentQueue;
    VkSwapchainKHR swapChain;
    std::vector<VkImage> swapChainImages;
    VkFormat swapChainImageFormat;
    VkExtent2D swapChainExtent;
    std::vector<VkImageView> swapChainImageViews;
    VkRenderPass renderPass;
    std::vector<VkFramebuffer> swapChainFramebuffers;
    VkCommandPool commandPool;
    std::vector<VkCommandBuffer> commandBuffers;
    std::vector<VkSemaphore> imageAvailableSemaphores;
    std::vector<VkSemaphore> renderFinishedSemaphores;
    std::vector<VkFence> inFlightFences;
    uint32_t currentFrame = 0;
    bool framebufferResized = false;

    // Scene content (CPU side). Models are loaded once and stay loaded; objects place them in the world.
    std::vector<Mesh> sceneMeshes;    // every mesh of every loaded model
    std::vector<SceneMaterial> sceneMaterials;  // every material of every loaded model; [0] is the floor's
    std::vector<TextureData> sceneTextures;     // pixels are freed once uploaded to the GPU
    std::vector<SceneModel> sceneModels;
    std::vector<SceneObject> sceneObjects;
    int selectedObject = -1;
    bool geometryDirty = false;       // a model was loaded: re-upload meshes and rebuild BLASes before the next frame

    // Geometry (GPU side): every mesh's vertices/indices packed into one shared buffer each, one BLAS per mesh.
    // Only rebuilt when a new model is loaded.
    bool geometryUploaded = false;
    VkBuffer vertexBuffer;
    VkDeviceMemory vertexBufferMemory;
    VkBuffer indexBuffer;
    VkDeviceMemory indexBufferMemory;
    VkBuffer meshInfoBuffer;
    VkDeviceMemory meshInfoBufferMemory;
    std::vector<MeshInfo> meshInfos;
    std::vector<AccelerationStructure> blases;  // one per mesh
    VkDeviceSize blasSizeBeforeCompaction = 0;
    VkDeviceSize blasSizeAfterCompaction = 0;

    // Textures (one bindless array in the shader) and materials. Textures are only ever added;
    // materials are re-uploaded inside the frame when edited in the UI.
    std::vector<GpuTexture> gpuTextures;
    std::map<uint32_t, VkSampler> samplers;  // key: filter and wrap modes, see getSampler
    VkDeviceSize textureMemorySize = 0;
    VkBuffer materialBuffer;
    VkDeviceMemory materialBufferMemory;
    bool materialBufferCreated = false;
    bool materialsDirty = false;
    // How much light the GGX reflection reflects in total, per view angle and roughness (see Materials.cpp)
    static constexpr uint32_t specularAlbedoSize = 32;
    VkBuffer specularAlbedoBuffer;
    VkDeviceMemory specularAlbedoMemory;

    // TLAS: rebuilt on the GPU, inside the frame's command buffer, whenever an object moves.
    // It has room for tlasCapacity instances and is only recreated when more are needed.
    AccelerationStructure tlas;
    uint32_t tlasCapacity = 0;
    VkBuffer tlasScratchBuffer;
    VkDeviceMemory tlasScratchMemory;
    VkDeviceAddress tlasScratchAddress = 0;
    // The CPU writes instances while the previous frame may still be building from its buffer, hence one per frame in flight.
    std::vector<VkBuffer> instanceBuffers;
    std::vector<VkDeviceMemory> instanceBufferMemories;
    std::vector<void*> instanceBuffersMapped;
    std::vector<VkAccelerationStructureInstanceKHR> frameInstances;  // this frame's instances, from the objects
    std::vector<VkAccelerationStructureInstanceKHR> builtInstances;  // what the TLAS currently contains
    std::vector<glm::mat4> frameInstanceTransforms;  // world transform of each of frameInstances
    std::vector<glm::mat4> frameInstanceMotion;      // per instance: this frame's world position -> last frame's
    VkBuffer instanceMotionBuffer = VK_NULL_HANDLE;  // frameInstanceMotion on the GPU, room for tlasCapacity
    VkDeviceMemory instanceMotionMemory = VK_NULL_HANDLE;
    std::vector<uint32_t> frameInstanceMeshes;       // mesh of each of frameInstances
    bool tlasDirty = true;

    // Light list for sampling emissive surfaces. Rebuilt on the CPU when instances or materials change.
    std::vector<EmissiveTriangle> emissiveTriangles;
    size_t sampledEmissiveTriangles = 0;      // of those, how many light sampling can pick (probability > 0)
    std::vector<int32_t> instanceFirstLight;  // per TLAS instance: index of its first triangle in the list, or -1
    StreamedBuffer emissiveTriangleBuffer;
    StreamedBuffer instanceLightBuffer;
    bool lightsUploadPending = false;

    // Models found in the models/ folder next to the executable
    std::vector<std::string> availableModels;
    int modelToAdd = 0;  // UI: index into availableModels
    bool addAtOriginalSize = false;  // UI: new objects keep the file's size (whole scenes like Sponza)
    std::string loadError;

    // Two sizes (chooseExtents): the path tracer renders at renderExtent, the display pass writes outputExtent
    // (the window). Equal for now; DLSS will render smaller and upscale.
    VkExtent2D renderExtent{};
    VkExtent2D outputExtent{};

    // Ray tracing output at render size, recreated with the swapchain
    VkImage radianceImage;  // linear HDR: running average while accumulating, else this frame (or a debug view)
    VkDeviceMemory radianceImageMemory;
    VkImageView radianceImageView;
    // DLSS inputs ("guides"), all describing the first surface the camera ray hits. Written only when
    // SceneUniforms::writeGuides is set (DLSS mode, guide debug views).
    RenderImage linearDepthImage;         // R32F: view-space depth (distance along the camera's forward axis)
    RenderImage motionVectorImage;        // RG32F: pixels to where this point was in the last frame
    RenderImage diffuseAlbedoImage;       // RGBA16F
    RenderImage specularAlbedoImage;      // RGBA16F
    RenderImage normalRoughnessImage;     // RGBA16F: world-space shading normal, roughness in alpha
    RenderImage specularHitDistanceImage; // R32F: first hit -> next hit along a reflection/refraction, 0 = unknown
    RenderImage guideHistoryImage;        // RGBA16F: last frame's diffuse albedo, for the reprojection error view

    // Motion: last frame's unjittered view-projection, and per TLAS instance a matrix that takes a point on it
    // from this frame's position to last frame's (object motion). Binding 12, uploaded every frame.
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
    VkDescriptorSet rtDescriptorSet = VK_NULL_HANDLE;  // null until created, see loadScene
    VkPipelineLayout rtPipelineLayout;
    VkPipeline rtPipeline;
    VkPipeline rtCacheUpdatePipeline;  // the same shader with CACHE_UPDATE_PASS = 1

    // Radiance cache (RadianceCache.cpp): hash table of surface cells, on the GPU only
    static constexpr uint32_t cacheCapacity = 1u << 20;  // entries (40 MB in all)
    static constexpr float cacheMaxWeight = 256.0f;
    static constexpr uint32_t cacheMaxAge = 128;
    VkBuffer cacheKeyBuffer, cacheAccumulatorBuffer, cacheValueBuffer, cacheAgeBuffer, cacheStatsBuffer;
    VkDeviceMemory cacheKeyMemory, cacheAccumulatorMemory, cacheValueMemory, cacheAgeMemory, cacheStatsMemory;
    std::vector<VkBuffer> cacheStatsReadbackBuffers;
    std::vector<VkDeviceMemory> cacheStatsReadbackMemories;
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
    RenderImage dlssOutputImage;       // RGBA32F at output size: the denoised, upscaled HDR image (DLSS modes only)
    bool dlssResourcesInUse = false;   // DLSS created its own resources (on the first evaluate); free them on changes
    bool dlssReset = true;             // the next frame has no usable history (mode/size/scene changed)
    uint32_t dlssFrameIndex = 0;
    std::vector<std::string> streamlineInstanceExtensions;  // what Streamline asks for
    std::vector<std::string> streamlineDeviceExtensions;
    std::vector<std::string> streamlineFeatures12;  // Vulkan 1.2 / 1.3 feature names it needs enabled
    std::vector<std::string> streamlineFeatures13;

    // Automatic exposure (Exposure.cpp): histogram + adapted exposure on the GPU, read back for the UI
    VkBuffer exposureBuffer;
    VkDeviceMemory exposureMemory;
    std::vector<VkBuffer> exposureReadbackBuffers;  // one per frame in flight
    std::vector<VkDeviceMemory> exposureReadbackMemories;
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
    float frameDeltaTime = 0.0f;   // seconds, set in mainLoop

    // Noise measurement (Noise.cpp): a converged reference of the current view, then the error of the displayed
    // image against it, frame by frame. Resources are created on first use.
    bool noiseResourcesCreated = false;
    RenderImage noiseReferenceImage;          // RGBA32F at output size
    VkExtent2D noiseReferenceExtent{};
    bool noiseReferenceValid = false;
    float noiseReferenceLuminance = 0.0f;     // average over the reference image
    uint32_t noiseGroupCapacity = 0;          // workgroup sums the buffers below have room for
    std::vector<VkBuffer> noiseSumBuffers;    // one per frame in flight, host visible: the GPU writes, the CPU reads
    std::vector<VkDeviceMemory> noiseSumMemories;
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
    // Scripted run (set before the first frame, e.g. in createDefaultScene): reference, measure in
    // noiseAutoMode, print the results, then close the window if noiseAutoExit.
    bool noiseAutoRun = false;
    int noiseAutoMode = 0;
    bool noiseAutoExit = false;
    bool noiseAutoContinue = false;  // the scripted run's reference is being built

    // Display pass (Display.cpp): exposure + tone mapping into an 8-bit image at output size, blitted to the swapchain
    VkImage displayImage;
    VkDeviceMemory displayImageMemory;
    VkImageView displayImageView;
    VkDescriptorSetLayout displayDescriptorSetLayout;
    VkDescriptorPool displayDescriptorPool;
    VkDescriptorSet displayDescriptorSet;
    VkPipelineLayout displayPipelineLayout;
    VkPipeline displayPipeline;

    // GPU time of each pass (trace, exposure, display): timestamps before and after them, one set of queries
    // per frame in flight, read when that frame slot is recorded again. Smoothed over ~20 frames, in ms.
    static constexpr uint32_t timestampsPerFrame = 6;  // before the cache, after cache / trace / DLSS / exposure / display
    VkQueryPool timestampQueryPool = VK_NULL_HANDLE;
    std::vector<bool> timestampsWritten;
    float gpuPassTimes[timestampsPerFrame - 1] = {};
    float lastPassTimes[timestampsPerFrame - 1] = {};  // the same, unsmoothed, of the frame last read back

    // Updated every frame with vkCmdUpdateBuffer
    VkBuffer uniformBuffer;
    VkDeviceMemory uniformBufferMemory;
    uint32_t primitiveCount = 0;  // triangles over all instances

    Camera camera;
    RenderSettings settings;
    uint32_t frameIndex = 0;

    // Accumulation restarts whenever the uniforms (camera, light, settings) differ from the previous frame.
    uint32_t accumulatedFrames = 0;
    uint32_t accumulationRestarts = 0;  // counts the restarts (also in DLSS modes: the uniforms changed)
    bool resetAccumulation = true;
    SceneUniforms previousUniforms{};


    VkDescriptorPool imGuiPool;
    bool showImGuiDemo = false;


    void initWindow();
    void initVulkan();
    void mainLoop();
    void cleanup();

    void drawFrame();

    void createInstance();
    void setupDebugMessenger();
    void createSurface();
    void pickPhysicalDevice();
    void createLogicalDevice();
    void createSwapChain();
    void createImageViews();
    void createRenderPass();
    VkShaderModule createShaderModule(const std::vector<char>& code);
    void createFramebuffers();
    void createCommandPool();
    void createCommandBuffers();
    void recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex);
    void createSyncObjects();
    void createRenderFinishedSemaphores();
    void destroyRenderFinishedSemaphores();
    void recreateSwapChain();
    void cleanupSwapChain();

    void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& bufferMemory);
    void copyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size);
    void createDeviceLocalBuffer(const void* data, VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buffer, VkDeviceMemory& bufferMemory);
    VkDeviceAddress getBufferDeviceAddress(VkBuffer buffer);
    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);
    void createImage(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage, VkImage& image, VkDeviceMemory& imageMemory, VkImageView& imageView);
    void createImage(VkExtent2D extent, VkFormat format, VkImageUsageFlags usage, RenderImage& image);
    void destroyImage(RenderImage& image);

    // RayTracer.cpp
    void createUniformBuffer();
    AccelerationStructure createAccelerationStructure(VkAccelerationStructureTypeKHR type, VkDeviceSize size);
    VkDeviceAddress createScratchBuffer(VkDeviceSize size, VkBuffer& buffer, VkDeviceMemory& memory);
    void destroyAccelerationStructure(AccelerationStructure& as);
    void createBottomLevelAccelerationStructures();
    void createTopLevelAccelerationStructure(uint32_t capacity);
    void destroyTopLevelAccelerationStructure();
    void recordTopLevelBuild(VkCommandBuffer commandBuffer);

    // Scene.cpp
    void findAvailableModels();
    void createDefaultScene();
    int findOrLoadModel(const std::string& fileName);
    void addObject(const std::string& fileName);
    void updateScene();
    void uploadSceneGeometry();
    void destroySceneGeometry();
    void buildSceneUi();
    void buildMaterialUi(const SceneModel& model);

    // Lights.cpp
    void createStreamedBuffer(StreamedBuffer& streamed, VkDeviceSize capacity);
    void destroyStreamedBuffer(StreamedBuffer& streamed);
    bool ensureStreamedCapacity(StreamedBuffer& streamed, VkDeviceSize size);
    void recordStreamedUpload(VkCommandBuffer commandBuffer, StreamedBuffer& streamed, const void* data, VkDeviceSize size);
    void createLightBuffers();
    void destroyLightBuffers();
    void buildLightList();
    void recordLightUpload(VkCommandBuffer commandBuffer);

    // Materials.cpp
    VkSampler getSampler(const TextureData& texture);
    void uploadNewTextures();
    void destroyTextures();
    std::vector<GpuMaterial> packMaterials() const;
    void createMaterialBuffer();
    void destroyMaterialBuffer();
    void recordMaterialUpdate(VkCommandBuffer commandBuffer);
    void createSpecularAlbedoTable();
    void destroySpecularAlbedoTable();
    // Dlss.cpp
    void initStreamline();
    void addStreamlineDeviceRequirements(std::vector<const char*>& extensions, VkPhysicalDeviceVulkan12Features& features12,
                                         VkPhysicalDeviceVulkan13Features& features13);
    void connectStreamline(uint32_t graphicsQueueFamily);
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
    void recreateRenderImages();
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
    SceneUniforms buildSceneUniforms();
    void recordInstanceMotionUpload(VkCommandBuffer commandBuffer);
    void createTimestampQueries();
    void readTimestamps();
    void writeTimestamp(VkCommandBuffer commandBuffer, uint32_t index);
    void recordRayTracing(VkCommandBuffer commandBuffer, uint32_t imageIndex);
    void cleanupRayTracing();

    void initImGui();
    void buildImGui();
    void cleanupImGui();
    void immediateSubmit(std::function<void(VkCommandBuffer)> &&function);
};


#endif //VULKANDEMO_APPLICATION_H
