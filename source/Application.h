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

// Must match the SceneUniforms block in shaders/raytrace.comp (std140: vec4s first, no vec3s)
struct SceneUniforms {
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
    float exposure;
    uint32_t toneMapping;
    uint32_t debugView;
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
};

enum class DebugView : int {
    None = 0, Normals, TexCoords, MeshIndex, MaterialIndex,
    BaseColor, Metallic, Roughness, Transmission, Emissive, Tangents
};

// A model file, loaded once. Any number of objects can show it.
struct SceneModel {
    std::string name;                 // file name, or "Floor" for the built-in floor
    std::vector<MeshInstance> parts;  // meshIndex into Application::sceneMeshes, transform from the file's node tree
    glm::mat4 fit = glm::mat4(1.0f);  // scales the model to a standard size and stands it on y = 0
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

    glm::mat4 transform() const;           // translate * rotate * scale
    glm::mat4 previousTransform = glm::mat4(1.0f);  // last frame's transform, for motion vectors (DLSS, Phase 7)
};

// Values edited through ImGui
struct RenderSettings {
    float sunAzimuth = 35.0f;    // degrees around the Y axis
    float sunElevation = 50.0f;  // degrees above the horizon
    float sunAngularRadius = 2.0f;  // degrees; the real sun is ~0.27, bigger = softer shadows
    glm::vec3 sunColor = {1.0f, 0.95f, 0.85f};
    float sunIntensity = 1.0f;
    bool shadowsEnabled = true;

    glm::vec3 skyZenithColor = {0.08f, 0.36f, 0.72f};
    glm::vec3 skyHorizonColor = {1.0f, 1.0f, 1.0f};
    glm::vec3 groundColor = {0.35f, 0.3f, 0.35f};
    float skyIntensity = 0.6f;

    int samplesPerPixel = 1;
    int maxBounces = 4;
    bool accumulate = true;

    float exposure = 0.6f;
    bool toneMapping = true;
    int debugView = static_cast<int>(DebugView::None);
};

class Application {
public:
    static constexpr uint32_t maxTextures = 1024;  // size of the texture descriptor array

    void run();
    void setFramebufferResized(bool b){
        this->framebufferResized = b;
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
    bool tlasDirty = true;

    // Models found in the models/ folder next to the executable
    std::vector<std::string> availableModels;
    int modelToAdd = 0;  // UI: index into availableModels
    std::string loadError;

    // Ray tracing output, recreated with the swapchain
    VkImage rtColorImage;
    VkDeviceMemory rtColorImageMemory;
    VkImageView rtColorImageView;
    VkImage rtDepthImage;
    VkDeviceMemory rtDepthImageMemory;
    VkImageView rtDepthImageView;
    VkImage accumulationImage;
    VkDeviceMemory accumulationImageMemory;
    VkImageView accumulationImageView;

    VkDescriptorSetLayout rtDescriptorSetLayout;
    VkDescriptorPool rtDescriptorPool;
    VkDescriptorSet rtDescriptorSet = VK_NULL_HANDLE;  // null until created, see loadScene
    VkPipelineLayout rtPipelineLayout;
    VkPipeline rtPipeline;

    // Updated every frame with vkCmdUpdateBuffer
    VkBuffer uniformBuffer;
    VkDeviceMemory uniformBufferMemory;
    uint32_t primitiveCount = 0;  // triangles over all instances

    Camera camera;
    RenderSettings settings;
    uint32_t frameIndex = 0;

    // Accumulation restarts whenever the uniforms (camera, light, settings) differ from the previous frame.
    uint32_t accumulatedFrames = 0;
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

    // Materials.cpp
    VkSampler getSampler(const TextureData& texture);
    void uploadNewTextures();
    void destroyTextures();
    std::vector<GpuMaterial> packMaterials() const;
    void createMaterialBuffer();
    void destroyMaterialBuffer();
    void recordMaterialUpdate(VkCommandBuffer commandBuffer);
    void createRayTracingImages();
    void destroyRayTracingImages();
    void createRayTracingDescriptors();
    void updateRayTracingDescriptorSet();
    void createRayTracingPipeline();
    SceneUniforms buildSceneUniforms();
    void recordRayTracing(VkCommandBuffer commandBuffer, uint32_t imageIndex);
    void cleanupRayTracing();

    void initImGui();
    void buildImGui();
    void cleanupImGui();
    void immediateSubmit(std::function<void(VkCommandBuffer)> &&function);
};


#endif //VULKANDEMO_APPLICATION_H
