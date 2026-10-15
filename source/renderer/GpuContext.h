//
// The Vulkan device and what everything on it is made with: instance, window surface, physical and logical device,
// queues, the memory allocator (VMA), a command pool, the pipeline cache, and helpers for buffers, images, shader
// modules and one-off command buffers. The renderer (Renderer.h) owns one; the swapchain and the passes are its.
//
// DLSS (Streamline, Dlss.cpp) has to take part in creating the device: it asks for instance and device extensions and
// features, and is told about the device once it exists (GpuContext::Extras).
//

#ifndef VULKANDEMO_GPUCONTEXT_H
#define VULKANDEMO_GPUCONTEXT_H

#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>
#include <vma/vk_mem_alloc.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// An image with its memory and view (the render images, the DLSS input images)
struct RenderImage {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

// What a surface offers a swapchain (formats, present modes, sizes)
struct SwapChainSupportDetails {
    VkSurfaceCapabilitiesKHR capabilities;
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;
};
SwapChainSupportDetails querySwapChainSupport(VkPhysicalDevice device, VkSurfaceKHR surface);

class GpuContext {
public:
    // Others' part in making the device (DLSS)
    struct Extras {
        std::vector<std::string> instanceExtensions;
        // Adds device extensions and Vulkan 1.2 / 1.3 features to the ones about to be enabled
        std::function<void(std::vector<const char*>& extensions, VkPhysicalDeviceVulkan12Features& features12,
                           VkPhysicalDeviceVulkan13Features& features13)> addDeviceRequirements;
        // After vkCreateDevice: the device and the graphics queue exist
        std::function<void()> deviceCreated;
    };

    // Instance (with the validation layer if Configuration says so), the window's surface, the first GPU that can
    // ray trace, the device, the allocator, the command pool and the pipeline cache (from its file)
    void create(GLFWwindow* window, const Extras& extras);
    // All of it, in reverse; everything made with it must be gone (the pipeline cache isn't saved: savePipelineCache)
    void destroy();

    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties physicalDeviceProperties{};
    VkDevice device = VK_NULL_HANDLE;
    // Where every buffer's and image's memory comes from (Vulkan Memory Allocator): a few large blocks per memory type,
    // shared by many resources, instead of one vkAllocateMemory each (Vulkan only promises 4096 of those)
    VmaAllocator allocator = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    uint32_t graphicsQueueFamily = 0;
    VkQueue presentQueue = VK_NULL_HANDLE;
    uint32_t presentQueueFamily = 0;
    VkCommandPool commandPool = VK_NULL_HANDLE;  // graphics queue; command buffers can be reset one by one
    // What the driver reports about compiled pipelines (VK_KHR_pipeline_executable_properties: registers per thread
    // etc., for the Debug panel), and VK_EXT_memory_budget (Renderer::gpuMemory); both optional
    bool pipelineStatisticsSupported = false;
    bool memoryBudgetSupported = false;

    // The compiled pipelines, kept on disk between runs (pipelineCacheFile, next to the shaders: the working directory
    // unless --shader-dir): compiling the trace shader's 21 pipelines takes seconds; from the cache, a fraction of
    // that. Passed to every pipeline creation.
    static constexpr const char* pipelineCacheFile = "pipeline_cache.bin";
    VkPipelineCache pipelineCache = VK_NULL_HANDLE;
    void savePipelineCache();

    // Records commands with function, submits them and waits until the GPU is done
    void immediateSubmit(std::function<void(VkCommandBuffer)>&& function);
    VkShaderModule createShaderModule(const std::vector<char>& code);

    // ownMemory: a memory object of its own instead of a place in VMA's shared blocks, for what is remade whenever
    // the render size, the mode or a feature changes (render images, per-pixel buffers) or replaced by a bigger one
    // when it fills up (streamed buffers, the shared geometry buffers, the TLAS). Freed, the whole object goes back
    // to the driver; in the shared blocks it would leave holes (up to ~110 MB at 4K) or a kept empty block. Only a
    // few dozen resources, so the count of memory objects stays small.
    void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer& buffer,
                      VmaAllocation& bufferMemory, VmaPool pool = VK_NULL_HANDLE, bool ownMemory = false);
    void copyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size);
    void createDeviceLocalBuffer(const void* data, VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buffer,
                                 VmaAllocation& bufferMemory);
    VkDeviceAddress getBufferDeviceAddress(VkBuffer buffer);
    VkDeviceMemory deviceMemoryOf(VmaAllocation allocation) const;
    void createImage(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage, VkImage& image,
                     VmaAllocation& imageMemory, VkImageView& imageView, uint32_t mipLevels = 1, bool ownMemory = false);
    void createImage(VkExtent2D extent, VkFormat format, VkImageUsageFlags usage, RenderImage& image);  // own memory
    void destroyImage(RenderImage& image);

private:
    uint64_t pipelineCacheShaderHash = 0;  // of the shaders it was made from: written before the cache in the file

    void createInstance(const Extras& extras);
    void setupDebugMessenger();
    void createSurface(GLFWwindow* window);
    void pickPhysicalDevice();
    void createLogicalDevice(const Extras& extras);
    void createAllocator();
    void createCommandPool();
    void createPipelineCache();
};

#endif //VULKANDEMO_GPUCONTEXT_H
