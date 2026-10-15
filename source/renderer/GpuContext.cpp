//
// The Vulkan device and what everything on it is made with. See GpuContext.h.
//

#include <vulkan/vulkan.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <set>
#include <stdexcept>
#include "renderer/GpuContext.h"
#include "core/Configuration.h"
#include "renderer/VulkanExtensions.h"

struct QueueFamilyIndices {
    std::optional<uint32_t> graphicsFamily;
    std::optional<uint32_t> presentFamily;
    bool isComplete() {
        return graphicsFamily.has_value() && presentFamily.has_value();
    }
};

static bool checkValidationLayerSupport();
static std::vector<const char*> getRequiredExtensions();
static VkResult createDebugUtilsMessengerEXT(VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDebugUtilsMessengerEXT* pDebugMessenger);
static bool isDeviceSuitable(VkPhysicalDevice &device, VkSurfaceKHR &surface);
static QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device, VkSurfaceKHR &surface);
static bool checkDeviceExtensionSupport(VkPhysicalDevice device);
static bool checkDeviceFeatureSupport(VkPhysicalDevice device);
static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity, VkDebugUtilsMessageTypeFlagsEXT messageType, const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData, void* pUserData);
static void destroyDebugUtilsMessengerEXT(VkInstance instance, VkDebugUtilsMessengerEXT debugMessenger, const VkAllocationCallbacks* pAllocator) {
    auto func = (PFN_vkDestroyDebugUtilsMessengerEXT) vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
    if (func != nullptr) {
        func(instance, debugMessenger, pAllocator);
    }
}

void GpuContext::create(GLFWwindow* window, const Extras& extras) {
    createInstance(extras);
    setupDebugMessenger();
    createSurface(window);
    pickPhysicalDevice();
    createLogicalDevice(extras);
    createAllocator();
    createPipelineCache();  // before any pipeline
    createCommandPool();
}

void GpuContext::destroy() {
    vkDestroyCommandPool(device, commandPool, nullptr);
    vmaDestroyAllocator(allocator);  // everything allocated from it must be freed by now
    vkDestroyPipelineCache(device, pipelineCache, nullptr);
    vkDestroyDevice(device, nullptr);

    if (Configuration::enableValidationLayers) {
        destroyDebugUtilsMessengerEXT(instance, debugMessenger, nullptr);
    }

    vkDestroySurfaceKHR(instance, surface, nullptr);
    vkDestroyInstance(instance, nullptr);
}

void GpuContext::createInstance(const Extras& extras) {
    if (!checkValidationLayerSupport()) {
        throw std::runtime_error("validation layers requested, but not available!");
    }

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "Hello Triangle";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "No Engine";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 3, 0);
    appInfo.apiVersion = VK_API_VERSION_1_3;

    auto extensions = getRequiredExtensions();
    // What others asked for (DLSS: Streamline's, all core since Vulkan 1.1, but it checks the list)
    for (const std::string& name : extras.instanceExtensions) {
        if (std::none_of(extensions.begin(), extensions.end(), [&](const char* e) { return name == e; })) {
            extensions.push_back(name.c_str());
        }
    }
    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());;
    createInfo.ppEnabledExtensionNames = extensions.data();

    // Synchronization validation: switched on through the layer (VK_EXT_validation_features, which the layer provides)
    VkValidationFeatureEnableEXT syncValidation = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validationFeatures{};
    validationFeatures.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
    validationFeatures.enabledValidationFeatureCount = 1;
    validationFeatures.pEnabledValidationFeatures = &syncValidation;
    if (Configuration::enableValidationLayers) {
        createInfo.enabledLayerCount = static_cast<uint32_t>(Configuration::validationLayers.size());
        createInfo.ppEnabledLayerNames = Configuration::validationLayers.data();
        if (Configuration::enableSyncValidation) {
            extensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
            createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
            createInfo.ppEnabledExtensionNames = extensions.data();
            createInfo.pNext = &validationFeatures;
        }
    } else {
        createInfo.enabledLayerCount = 0;
    }

    if (vkCreateInstance(&createInfo, nullptr, &instance) != VK_SUCCESS) {
        throw std::runtime_error("failed to create instance!");
    }
}

void GpuContext::setupDebugMessenger() {
    if (Configuration::enableValidationLayers) {
        VkDebugUtilsMessengerCreateInfoEXT createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        createInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        createInfo.pfnUserCallback = debugCallback;
        createInfo.pUserData = nullptr;

        if (createDebugUtilsMessengerEXT(instance, &createInfo, nullptr, &debugMessenger) != VK_SUCCESS) {
            throw std::runtime_error("failed to set up debug messenger!");
        }
    }
}

void GpuContext::createSurface(GLFWwindow* window) {
    if (glfwCreateWindowSurface(instance, window, nullptr, &surface) != VK_SUCCESS) {
        throw std::runtime_error("failed to create window surface!");
    }
}

void GpuContext::pickPhysicalDevice() {
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
    if (deviceCount == 0) {
        throw std::runtime_error("failed to find GPUs with Vulkan support!");
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());
    for (auto& device : devices) {
        if (isDeviceSuitable(device, surface)) {
            physicalDevice = device;
            break;
        }
    }

    if (physicalDevice == VK_NULL_HANDLE) {
        throw std::runtime_error("failed to find a suitable GPU (ray query support is required)!");
    }

    vkGetPhysicalDeviceProperties(physicalDevice, &physicalDeviceProperties);
    std::cout << "Using GPU: " << physicalDeviceProperties.deviceName << '\n';
}

void GpuContext::createLogicalDevice(const Extras& extras) {
    QueueFamilyIndices indices = findQueueFamilies(physicalDevice, surface);

    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    std::set<uint32_t> uniqueQueueFamilies = {indices.graphicsFamily.value(), indices.presentFamily.value()};

    float queuePriority = 1.0f;
    for (uint32_t queueFamily : uniqueQueueFamilies) {
        VkDeviceQueueCreateInfo queueCreateInfo{};
        queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueCreateInfo.queueFamilyIndex = queueFamily;
        queueCreateInfo.queueCount = 1;
        queueCreateInfo.pQueuePriorities = &queuePriority;
        queueCreateInfos.push_back(queueCreateInfo);
    }

    // Features are enabled through a pNext chain:
    // features2 -> vulkan13Features -> vulkan12Features -> accelerationStructureFeatures -> rayQueryFeatures
    // -> atomicFloatFeatures (-> executableFeatures)
    VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures{};
    rayQueryFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
    rayQueryFeatures.rayQuery = VK_TRUE;

    VkPhysicalDeviceShaderAtomicFloatFeaturesEXT atomicFloatFeatures{};
    atomicFloatFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT;
    atomicFloatFeatures.shaderBufferFloat32AtomicAdd = VK_TRUE;  // radiance cache
    rayQueryFeatures.pNext = &atomicFloatFeatures;

    VkPhysicalDeviceAccelerationStructureFeaturesKHR accelerationStructureFeatures{};
    accelerationStructureFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    accelerationStructureFeatures.accelerationStructure = VK_TRUE;
    accelerationStructureFeatures.pNext = &rayQueryFeatures;

    VkPhysicalDeviceVulkan12Features vulkan12Features{};
    vulkan12Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    vulkan12Features.bufferDeviceAddress = VK_TRUE;
    vulkan12Features.descriptorIndexing = VK_TRUE;
    vulkan12Features.runtimeDescriptorArray = VK_TRUE;
    vulkan12Features.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    vulkan12Features.descriptorBindingPartiallyBound = VK_TRUE;
    vulkan12Features.scalarBlockLayout = VK_TRUE;
    vulkan12Features.pNext = &accelerationStructureFeatures;

    VkPhysicalDeviceVulkan13Features vulkan13Features{};
    vulkan13Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    vulkan13Features.pNext = &vulkan12Features;

    // Extensions and features others need (DLSS: Streamline's, VK_NVX_binary_import etc.), merged with ours
    std::vector<const char*> extensions = Configuration::deviceExtensions;
    if (extras.addDeviceRequirements) {
        extras.addDeviceRequirements(extensions, vulkan12Features, vulkan13Features);
    }

    // Optional: the driver's statistics about compiled shaders (registers per thread, ...), see the Debug panel
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR executableFeatures{};
    executableFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR;
    executableFeatures.pipelineExecutableInfo = VK_TRUE;
    uint32_t availableCount = 0;
    vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &availableCount, nullptr);
    std::vector<VkExtensionProperties> available(availableCount);
    vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &availableCount, available.data());
    for (const VkExtensionProperties& extension : available) {
        if (std::string(extension.extensionName) == VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME) {
            extensions.push_back(VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);
            executableFeatures.pNext = atomicFloatFeatures.pNext;
            atomicFloatFeatures.pNext = &executableFeatures;
            pipelineStatisticsSupported = true;
        }
        // Optional: how much memory the program uses and may use (gpuMemory)
        if (std::string(extension.extensionName) == VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) {
            extensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
            memoryBudgetSupported = true;
        }
    }

    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &vulkan13Features;
    // Storage images with 1 or 2 channels (rg16f motion vectors for DLSS) count as "extended formats"
    features2.features.shaderStorageImageExtendedFormats = VK_TRUE;
    // Reading a storage image without a format qualifier: display.comp and exposure.comp read the radiance image
    // (RGBA32F) or DLSS's output (RGBA16F) through the same descriptor
    features2.features.shaderStorageImageReadWithoutFormat = VK_TRUE;
    // Block-compressed textures (BC1-7, from .dds files): a quarter of the memory or less, every desktop GPU has them
    features2.features.textureCompressionBC = VK_TRUE;
    // Anisotropic filtering: sharp textures on surfaces seen at a slant (floors, streets), see getSampler
    features2.features.samplerAnisotropy = VK_TRUE;

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.pNext = &features2;
    createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos = queueCreateInfos.data();
    createInfo.pEnabledFeatures = nullptr;  // must be null when VkPhysicalDeviceFeatures2 is chained
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

    // Device layers are deprecated (ignored since Vulkan 1.0); validation is enabled on the instance only.
    createInfo.enabledLayerCount = 0;

    if (vkCreateDevice(physicalDevice, &createInfo, nullptr, &device) != VK_SUCCESS) {
        throw std::runtime_error("failed to create logical device!");
    }

    graphicsQueueFamily = indices.graphicsFamily.value();
    presentQueueFamily = indices.presentFamily.value();
    vkGetDeviceQueue(device, graphicsQueueFamily, 0, &graphicsQueue);
    if (extras.deviceCreated) {
        extras.deviceCreated();  // DLSS: tell Streamline about the device
    }
    vkGetDeviceQueue(device, presentQueueFamily, 0, &presentQueue);

    vkext::load(device);
}

// VMA takes memory blocks per memory type and places buffers and images in them; big ones (half a block or more), and
// those the driver wants alone, get memory of their own. It calls Vulkan through the loader (vulkan-1), as the code
// before it did.
void GpuContext::createAllocator() {
    VmaAllocatorCreateInfo createInfo{};
    createInfo.vulkanApiVersion = VK_API_VERSION_1_3;
    createInfo.instance = instance;
    createInfo.physicalDevice = physicalDevice;
    createInfo.device = device;
    // 32 MB blocks instead of VMA's 256: what blocks hold but don't use (the last one per memory type is partly empty,
    // and VMA keeps one empty block) cost up to 250 MB with 256 MB blocks. 16 MB packs 2048x2048 textures badly (two
    // per block, 2.7 MB left: 56 MB in the two Bistros); 32 MB holds six. Measured: 2-17 MB unused in our scenes,
    // Bistro Exterior + Interior in 112 memory objects (4445 resources).
    createInfo.preferredLargeHeapBlockSize = 32ull << 20;
    // All memory with the DEVICE_ADDRESS flag: acceleration structure inputs, scratch and instances are found by their
    // GPU address
    createInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    if (memoryBudgetSupported) {
        createInfo.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
    }
    if (vmaCreateAllocator(&createInfo, &allocator) != VK_SUCCESS) {
        throw std::runtime_error("failed to create the memory allocator!");
    }
}

// The shaders the pipelines are made from (all .spv files the renderer loads), as one 64-bit hash (FNV-1a)
static uint64_t shaderBinariesHash() {
    uint64_t hash = 0xcbf29ce484222325ull;
    for (const char* name : {"raytrace.comp.spv", "display.comp.spv", "exposure.comp.spv", "noise.comp.spv",
                             "cache_resolve.comp.spv"}) {
        std::ifstream file(Configuration::shaderPath(name), std::ios::binary);
        for (std::istreambuf_iterator<char> it(file), end; it != end; ++it) {
            hash = (hash ^ static_cast<uint8_t>(*it)) * 0x100000001b3ull;
        }
    }
    return hash;
}

// The pipeline cache, with what the last run saved if it was made from the same shaders by this GPU and driver
// (anything else, or no file, starts empty). Pipelines whose shader and settings are in it are created without
// compiling. (The NVIDIA driver keeps a cache of its own too, but per executable: a rebuilt or renamed exe misses it.)
// The file starts with the hash of the shaders: after a shader change the cache starts empty, so it holds only the
// current shaders' pipelines (kept, every change would add ~8 MB of pipelines nothing uses any more).
void GpuContext::createPipelineCache() {
    pipelineCacheShaderHash = shaderBinariesHash();
    std::vector<char> data;
    std::ifstream file(Configuration::shaderPath(pipelineCacheFile), std::ios::binary | std::ios::ate);
    if (file) {
        size_t size = static_cast<size_t>(file.tellg());
        uint64_t shaderHash = 0;
        file.seekg(0);
        file.read(reinterpret_cast<char*>(&shaderHash), sizeof(shaderHash));
        if (file && size > sizeof(shaderHash) && shaderHash == pipelineCacheShaderHash) {
            data.resize(size - sizeof(shaderHash));
            file.read(data.data(), static_cast<std::streamsize>(data.size()));
        }
        VkPipelineCacheHeaderVersionOne header{};
        bool matches = file && data.size() >= sizeof(header);
        if (matches) {
            std::memcpy(&header, data.data(), sizeof(header));
            matches = header.headerVersion == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
                      header.vendorID == physicalDeviceProperties.vendorID &&
                      header.deviceID == physicalDeviceProperties.deviceID &&
                      std::memcmp(header.pipelineCacheUUID, physicalDeviceProperties.pipelineCacheUUID,
                                  VK_UUID_SIZE) == 0;
        }
        if (!matches) {
            data.clear();  // another GPU or driver version: compile anew
        }
    }
    VkPipelineCacheCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    createInfo.initialDataSize = data.size();
    createInfo.pInitialData = data.empty() ? nullptr : data.data();
    if (vkCreatePipelineCache(device, &createInfo, nullptr, &pipelineCache) != VK_SUCCESS) {
        throw std::runtime_error("failed to create the pipeline cache!");
    }
}

// Writes the cache to disk for the next run: at exit, and after new pipelines were compiled (the basic shader). Into
// a temporary file first, so a run that stops halfway leaves the old file intact.
void GpuContext::savePipelineCache() {
    size_t size = 0;
    if (vkGetPipelineCacheData(device, pipelineCache, &size, nullptr) != VK_SUCCESS || size == 0) {
        return;
    }
    std::vector<char> data(size);
    if (vkGetPipelineCacheData(device, pipelineCache, &size, data.data()) != VK_SUCCESS) {
        return;
    }
    std::string path = Configuration::shaderPath(pipelineCacheFile);
    std::string temporary = path + ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(&pipelineCacheShaderHash), sizeof(pipelineCacheShaderHash));
        file.write(data.data(), static_cast<std::streamsize>(size));
        if (!file) {
            return;
        }
    }
    std::remove(path.c_str());
    std::rename(temporary.c_str(), path.c_str());
}

VkShaderModule GpuContext::createShaderModule(const std::vector<char>& code) {
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule shaderModule;
    if (vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
        throw std::runtime_error("failed to create shader module!");
    }

    return shaderModule;
}

void GpuContext::createCommandPool() {
    QueueFamilyIndices queueFamilyIndices = findQueueFamilies(physicalDevice, surface);

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamilyIndices.graphicsFamily.value();

    if (vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool) != VK_SUCCESS) {
        throw std::runtime_error("failed to create command pool!");
    }
}

// Uploads `data` through a staging buffer into a new device local buffer.
void GpuContext::createDeviceLocalBuffer(const void* data, VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buffer, VmaAllocation& bufferMemory) {
    VkBuffer stagingBuffer;
    VmaAllocation stagingBufferMemory;
    createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stagingBuffer, stagingBufferMemory);

    void* mapped;
    vmaMapMemory(allocator, stagingBufferMemory, &mapped);
    memcpy(mapped, data, (size_t) size);
    vmaUnmapMemory(allocator, stagingBufferMemory);

    createBuffer(size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, buffer, bufferMemory);
    copyBuffer(stagingBuffer, buffer, size);

    vkDestroyBuffer(device, stagingBuffer, nullptr);
    vmaFreeMemory(allocator, stagingBufferMemory);
}

VkDeviceAddress GpuContext::getBufferDeviceAddress(VkBuffer buffer) {
    VkBufferDeviceAddressInfo addressInfo{};
    addressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    addressInfo.buffer = buffer;
    return vkGetBufferDeviceAddress(device, &addressInfo);
}

// 2D, device local image plus a matching view of all its mip levels. Starts in VK_IMAGE_LAYOUT_UNDEFINED.
void GpuContext::createImage(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage, VkImage& image, VmaAllocation& imageMemory, VkImageView& imageView, uint32_t mipLevels, bool ownMemory) {
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = mipLevels;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = usage;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    allocationInfo.flags = ownMemory ? VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT : 0;
    if (vmaCreateImage(allocator, &imageInfo, &allocationInfo, &image, &imageMemory, nullptr) != VK_SUCCESS) {
        throw std::runtime_error("failed to create image!");
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, 1};

    if (vkCreateImageView(device, &viewInfo, nullptr, &imageView) != VK_SUCCESS) {
        throw std::runtime_error("failed to create image view!");
    }
}

void GpuContext::createImage(VkExtent2D extent, VkFormat format, VkImageUsageFlags usage, RenderImage& image) {
    createImage(extent.width, extent.height, format, usage, image.image, image.memory, image.view, 1, true);
}

void GpuContext::destroyImage(RenderImage& image) {
    vkDestroyImageView(device, image.view, nullptr);
    vkDestroyImage(device, image.image, nullptr);
    vmaFreeMemory(allocator, image.memory);
    image = RenderImage{};
}

void GpuContext::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer& buffer, VmaAllocation& bufferMemory,
                            VmaPool pool, bool ownMemory) {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    // The memory type: the first that has these properties, as before VMA (with requiredFlags and no usage, VMA picks
    // the first fitting type too). Buffers whose GPU address we query need memory with the DEVICE_ADDRESS flag: VMA
    // allocates all its memory that way (createAllocator). A pool: memory set aside for short-lived buffers.
    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.requiredFlags = properties;
    allocationInfo.pool = pool;
    allocationInfo.flags = ownMemory ? VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT : 0;
    if (vmaCreateBuffer(allocator, &bufferInfo, &allocationInfo, &buffer, &bufferMemory, nullptr) != VK_SUCCESS) {
        throw std::runtime_error("failed to create buffer!");
    }
}

void GpuContext::copyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size) {
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = commandPool;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer;
    vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(commandBuffer, &beginInfo);

    VkBufferCopy copyRegion{};
    copyRegion.srcOffset = 0; // Optional
    copyRegion.dstOffset = 0; // Optional
    copyRegion.size = size;
    vkCmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, 1, &copyRegion);

    vkEndCommandBuffer(commandBuffer);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    vkQueueSubmit(graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphicsQueue);

    vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);
}

// The memory object an allocation lives in (shared with other allocations unless it got one of its own)
VkDeviceMemory GpuContext::deviceMemoryOf(VmaAllocation allocation) const {
    VmaAllocationInfo info;
    vmaGetAllocationInfo(allocator, allocation, &info);
    return info.deviceMemory;
}

// Records commands with `function`, submits them and waits until the GPU is done.
// Only meant for one-off setup work (uploads), not for per-frame rendering.
void GpuContext::immediateSubmit(std::function<void(VkCommandBuffer cmd)>&& function)
{
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = commandPool;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device, &allocInfo, &cmd);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(cmd, &beginInfo);
    function(cmd);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;

    vkQueueSubmit(graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphicsQueue);

    vkFreeCommandBuffers(device, commandPool, 1, &cmd);
}

static bool checkValidationLayerSupport() {
    uint32_t layerCount;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);

    std::vector<VkLayerProperties> availableLayers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

    for (const char* layerName : Configuration::validationLayers) {
        bool layerFound = false;

        for (const auto& layerProperties : availableLayers) {
            if (strcmp(layerName, layerProperties.layerName) == 0) {
                layerFound = true;
                break;
            }
        }

        if (!layerFound) {
            return false;
        }
    }

    return true;
}

static std::vector<const char*> getRequiredExtensions() {
    uint32_t glfwExtensionCount = 0;
    const char** glfwExtensions;
    glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

    std::vector<const char*> extensions(glfwExtensions, glfwExtensions + glfwExtensionCount);

    if (Configuration::enableValidationLayers) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    return extensions;
}

static bool isDeviceSuitable(VkPhysicalDevice &device, VkSurfaceKHR &surface) {
    QueueFamilyIndices indices = findQueueFamilies(device, surface);
    bool extensionsSupported = checkDeviceExtensionSupport(device);

    bool swapChainAdequate = false;
    if (extensionsSupported) {
        SwapChainSupportDetails swapChainSupport = querySwapChainSupport(device, surface);
        swapChainAdequate = !swapChainSupport.formats.empty() && !swapChainSupport.presentModes.empty();
    }

    return indices.isComplete() && extensionsSupported && swapChainAdequate && checkDeviceFeatureSupport(device);
}

static QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device, VkSurfaceKHR &surface) {
    QueueFamilyIndices indices;

    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);

    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

    int i = 0;
    for (const auto& queueFamily : queueFamilies) {
        if (queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            indices.graphicsFamily = i;
        }

        VkBool32 presentSupport = false;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &presentSupport);

        if (presentSupport) {
            indices.presentFamily = i;
        }
        if (indices.isComplete()) {
            break;
        }

        i++;
    }
    return indices;
}

static bool checkDeviceExtensionSupport(VkPhysicalDevice device) {
    uint32_t extensionCount;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);

    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, availableExtensions.data());

    std::set<std::string> requiredExtensions(Configuration::deviceExtensions.begin(), Configuration::deviceExtensions.end());

    for (const auto& extension : availableExtensions) {
        requiredExtensions.erase(extension.extensionName);
    }

    return requiredExtensions.empty();
}

// Part of createLogicalDevice's chain (not the 1.3 and atomic float features), queried to see what the GPU supports.
static bool checkDeviceFeatureSupport(VkPhysicalDevice device) {
    VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures{};
    rayQueryFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;

    VkPhysicalDeviceAccelerationStructureFeaturesKHR accelerationStructureFeatures{};
    accelerationStructureFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    accelerationStructureFeatures.pNext = &rayQueryFeatures;

    VkPhysicalDeviceVulkan12Features vulkan12Features{};
    vulkan12Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    vulkan12Features.pNext = &accelerationStructureFeatures;

    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &vulkan12Features;

    vkGetPhysicalDeviceFeatures2(device, &features2);

    return features2.features.shaderStorageImageExtendedFormats &&
           features2.features.shaderStorageImageReadWithoutFormat &&
           features2.features.textureCompressionBC &&
           features2.features.samplerAnisotropy &&
           vulkan12Features.bufferDeviceAddress &&
           vulkan12Features.descriptorIndexing &&
           vulkan12Features.runtimeDescriptorArray &&
           vulkan12Features.shaderSampledImageArrayNonUniformIndexing &&
           vulkan12Features.descriptorBindingPartiallyBound &&
           vulkan12Features.scalarBlockLayout &&
           accelerationStructureFeatures.accelerationStructure &&
           rayQueryFeatures.rayQuery;
}

SwapChainSupportDetails querySwapChainSupport(VkPhysicalDevice device, VkSurfaceKHR surface) {

    SwapChainSupportDetails details;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &details.capabilities);

    uint32_t formatCount;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, nullptr);
    if (formatCount != 0) {
        details.formats.resize(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, details.formats.data());
    }

    uint32_t presentModeCount;
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, nullptr);
    if (presentModeCount != 0) {
        details.presentModes.resize(presentModeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, details.presentModes.data());
    }

    return details;
}

static VkResult createDebugUtilsMessengerEXT(VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDebugUtilsMessengerEXT* pDebugMessenger) {
    auto func = (PFN_vkCreateDebugUtilsMessengerEXT) vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
    if (func != nullptr) {
        return func(instance, pCreateInfo, pAllocator, pDebugMessenger);
    } else {
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
}

static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
        VkDebugUtilsMessageTypeFlagsEXT messageType,
        const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
        void* pUserData) {

    std::cerr << "validation layer: " << (pCallbackData->pMessageIdName ? pCallbackData->pMessageIdName : "") << ": "
              << pCallbackData->pMessage << std::endl;

    return VK_FALSE;
}
