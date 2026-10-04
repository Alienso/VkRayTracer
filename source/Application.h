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
    glm::vec4 baseColor;
    uint32_t vertexOffset;  // first vertex in the vertex buffer
    uint32_t indexOffset;   // first index in the index buffer
    uint32_t vertexCount;
    uint32_t indexCount;
    uint32_t hasNormals;
};

enum class DebugView : int { None = 0, Normals = 1, TexCoords = 2, MeshIndex = 3 };

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

    // Scene geometry: every mesh's vertices/indices packed into one shared buffer each,
    // one BLAS per mesh, one TLAS over all instances. Rebuilt when another model is loaded.
    bool sceneLoaded = false;
    VkBuffer vertexBuffer;
    VkDeviceMemory vertexBufferMemory;
    VkBuffer indexBuffer;
    VkDeviceMemory indexBufferMemory;
    VkBuffer meshInfoBuffer;
    VkDeviceMemory meshInfoBufferMemory;
    VkBuffer instanceBuffer;
    VkDeviceMemory instanceBufferMemory;
    std::vector<MeshInfo> meshInfos;
    std::vector<MeshInstance> sceneInstances;
    std::vector<AccelerationStructure> blases;  // one per mesh
    AccelerationStructure tlas;
    VkDeviceSize blasSizeBeforeCompaction = 0;
    VkDeviceSize blasSizeAfterCompaction = 0;

    // Models found in the models/ folder next to the executable
    std::vector<std::string> availableModels;
    int loadedModel = -1;
    int requestedModel = -1;  // set by the UI, loaded at the start of the next frame
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
    void findAvailableModels();
    void loadScene(int modelIndex);
    void uploadSceneGeometry(const std::vector<Mesh>& meshes);
    void destroySceneGeometry();
    AccelerationStructure createAccelerationStructure(VkAccelerationStructureTypeKHR type, VkDeviceSize size);
    VkDeviceAddress createScratchBuffer(VkDeviceSize size, VkBuffer& buffer, VkDeviceMemory& memory);
    void destroyAccelerationStructure(AccelerationStructure& as);
    void createBottomLevelAccelerationStructures();
    void createTopLevelAccelerationStructure();
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
