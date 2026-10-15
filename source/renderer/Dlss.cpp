//
// DLSS Ray Reconstruction through NVIDIA Streamline (Phase 7).
//
// Streamline is a set of DLLs (sl.interposer.dll + one plugin per feature). We load sl.interposer.dll at runtime
// with LoadLibrary and look its functions up by name, instead of linking NVIDIA's import library: those are
// plain C functions taking plain structs, so this works from MinGW even though NVIDIA builds with MSVC.
//
// "Manual hooking": we keep creating the Vulkan instance and device ourselves (with the extensions Streamline
// asks for) and hand them over with slSetVulkanInfo, instead of routing every Vulkan call through Streamline.
//
// Per frame in a DLSS mode (recordDlss): the trace shader renders one noisy frame at the render size plus the
// guide images; we pass camera constants, options and the images ("tags") to Streamline and record DLSS into our
// command buffer with slEvaluateFeature. Its output (window size, linear HDR) then goes through exposure and
// display like the accumulated image does in the reference mode.
//

#include <windows.h>  // LoadLibrary; Streamline's headers also need HRESULT
#include <vulkan/vulkan.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <string>
#include <glm/gtc/matrix_inverse.hpp>
#include <streamline/sl.h>
#include <streamline/sl_helpers_vk.h>
#include <streamline/sl_dlss_d.h>
#include <imgui/imgui.h>
#include "renderer/Renderer.h"
#include "core/Configuration.h"

namespace {

// The Streamline functions we use, looked up in sl.interposer.dll
struct StreamlineApi {
    PFun_slInit* init = nullptr;
    PFun_slShutdown* shutdown = nullptr;
    PFun_slIsFeatureSupported* isFeatureSupported = nullptr;
    PFun_slIsFeatureLoaded* isFeatureLoaded = nullptr;
    PFun_slGetFeatureRequirements* getFeatureRequirements = nullptr;
    PFun_slGetFeatureVersion* getFeatureVersion = nullptr;
    PFun_slSetVulkanInfo* setVulkanInfo = nullptr;
    PFun_slGetFeatureFunction* getFeatureFunction = nullptr;
    PFun_slGetNewFrameToken* getNewFrameToken = nullptr;
    PFun_slSetConstants* setConstants = nullptr;
    PFun_slSetTagForFrame* setTagForFrame = nullptr;
    PFun_slEvaluateFeature* evaluateFeature = nullptr;
    PFun_slFreeResources* freeResources = nullptr;
    // DLSS RR's own functions, asked from Streamline once the feature is loaded
    PFun_slDLSSDGetOptimalSettings* dlssdGetOptimalSettings = nullptr;
    PFun_slDLSSDGetState* dlssdGetState = nullptr;
    PFun_slDLSSDSetOptions* dlssdSetOptions = nullptr;
} sl_;

// One window, one view: always viewport 0
const sl::ViewportHandle dlssViewport(0u);

sl::DLSSMode toDlssMode(int renderMode) {
    switch (static_cast<RenderMode>(renderMode)) {
        case RenderMode::DlssDlaa: return sl::DLSSMode::eDLAA;
        case RenderMode::DlssQuality: return sl::DLSSMode::eMaxQuality;
        case RenderMode::DlssBalanced: return sl::DLSSMode::eBalanced;
        case RenderMode::DlssPerformance: return sl::DLSSMode::eMaxPerformance;
        case RenderMode::DlssUltraPerformance: return sl::DLSSMode::eUltraPerformance;
        default: return sl::DLSSMode::eOff;
    }
}

// DLSS RR's models are picked through render presets. The current nvngx_dlssd.dll (310.9.1, Streamline SDK 2.14.1)
// has the transformer models D, E and F. The CNN models (presets A-C) were dropped after 310.3.0 (SDK 2.8.0), which
// has A-E; our 2.14 headers no longer name A-C, hence the plain numbers. Only one of the two DLLs can be loaded
// in a process, so the choice is made at startup (--dlss-cnn, see initStreamline).
sl::DLSSDPreset toDlssPreset(int model) {
    switch (static_cast<DlssModel>(model)) {
        case DlssModel::TransformerF: return sl::DLSSDPreset::ePresetF;
        case DlssModel::TransformerE: return sl::DLSSDPreset::ePresetE;
        case DlssModel::CnnA: return static_cast<sl::DLSSDPreset>(1);
        case DlssModel::CnnB: return static_cast<sl::DLSSDPreset>(2);
        case DlssModel::CnnC: return static_cast<sl::DLSSDPreset>(3);
        default: return sl::DLSSDPreset::ePresetD;
    }
}

bool isCnnModel(int model) {
    return model >= static_cast<int>(DlssModel::CnnA);
}

// Streamline's matrices are row-major for row vectors (v * M, translation in the last row). That is exactly the
// memory layout of a glm matrix (column-major, for column vectors M * v), so the bytes can be copied as they are.
sl::float4x4 toSl(const glm::mat4& m) {
    sl::float4x4 result;
    static_assert(sizeof(result) == sizeof(m), "matrix sizes differ");
    std::memcpy(&result, &m, sizeof(m));
    return result;
}

sl::float3 toSl(const glm::vec3& v) {
    return {v.x, v.y, v.z};
}

// What Streamline needs to know about one of our images (Vulkan can't be asked for an image's description)
sl::Resource describeImage(VkImage image, VkDeviceMemory memory, VkImageView view, VkExtent2D extent,
                           VkFormat format, VkImageUsageFlags usage) {
    sl::Resource resource(sl::ResourceType::eTex2d, image, memory, view, VK_IMAGE_LAYOUT_GENERAL);
    resource.width = extent.width;
    resource.height = extent.height;
    resource.nativeFormat = format;
    resource.mipLevels = 1;
    resource.arrayLayers = 1;
    resource.flags = 0;
    resource.usage = usage;
    return resource;
}

const char* resultName(sl::Result result) {
    static const char* names[] = {
            "eOk", "eErrorIO", "eErrorDriverOutOfDate", "eErrorOSOutOfDate", "eErrorOSDisabledHWS",
            "eErrorDeviceNotCreated", "eErrorNoSupportedAdapterFound", "eErrorAdapterNotSupported", "eErrorNoPlugins",
            "eErrorVulkanAPI", "eErrorDXGIAPI", "eErrorD3DAPI", "eErrorNRDAPI", "eErrorNVAPI", "eErrorReflexAPI",
            "eErrorNGXFailed", "eErrorJSONParsing", "eErrorMissingProxy", "eErrorMissingResourceState",
            "eErrorInvalidIntegration", "eErrorMissingInputParameter", "eErrorNotInitialized", "eErrorComputeFailed",
            "eErrorInitNotCalled", "eErrorExceptionHandler", "eErrorInvalidParameter", "eErrorMissingConstants",
            "eErrorDuplicatedConstants", "eErrorMissingOrInvalidAPI", "eErrorCommonConstantsMissing",
            "eErrorUnsupportedInterface", "eErrorFeatureMissing", "eErrorFeatureNotSupported",
            "eErrorFeatureMissingHooks", "eErrorFeatureFailedToLoad", "eErrorFeatureWrongPriority",
            "eErrorFeatureMissingDependency", "eErrorFeatureManagerInvalidState", "eErrorInvalidState", "eWarnOutOfVRAM"};
    auto index = static_cast<size_t>(result);
    return index < std::size(names) ? names[index] : "unknown";
}

std::string versionString(const sl::Version& v) {
    return std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.build);
}

void logMessage(sl::LogType type, const char* message) {
    // Streamline's messages already end with a newline
    std::cout << (type == sl::LogType::eError ? "[streamline ERROR] " : type == sl::LogType::eWarn ? "[streamline warn] " : "[streamline] ")
              << message << std::flush;
}

}  // namespace

// Before vkCreateInstance: Streamline must be initialized first, and it tells us which extensions it needs.
void Renderer::initStreamline() {
    if (dlssDisabledByFlag) {
        dlssStatus = "switched off with --no-dlss";
        std::cout << "DLSS: " << dlssStatus << std::endl;
        return;
    }
    // Next to the executable (copied there by the build)
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring directory(exePath);
    directory = directory.substr(0, directory.find_last_of(L"\\/"));

    HMODULE module = LoadLibraryW((directory + L"\\sl.interposer.dll").c_str());
    if (!module) {
        dlssStatus = "sl.interposer.dll not found";
        std::cout << "DLSS: " << dlssStatus << std::endl;
        return;
    }
    streamlineModule = module;

    auto load = [&](auto& function, const char* name) {
        function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(GetProcAddress(module, name));
        if (!function) {
            std::cout << "DLSS: missing function " << name << std::endl;
        }
        return function != nullptr;
    };
    bool ok = load(sl_.init, "slInit") & load(sl_.shutdown, "slShutdown") &
              load(sl_.isFeatureSupported, "slIsFeatureSupported") & load(sl_.isFeatureLoaded, "slIsFeatureLoaded") &
              load(sl_.getFeatureRequirements, "slGetFeatureRequirements") &
              load(sl_.getFeatureVersion, "slGetFeatureVersion") & load(sl_.setVulkanInfo, "slSetVulkanInfo") &
              load(sl_.getFeatureFunction, "slGetFeatureFunction") & load(sl_.getNewFrameToken, "slGetNewFrameToken") &
              load(sl_.setConstants, "slSetConstants") & load(sl_.setTagForFrame, "slSetTagForFrame") &
              load(sl_.evaluateFeature, "slEvaluateFeature") & load(sl_.freeResources, "slFreeResources");
    if (!ok) {
        dlssStatus = "sl.interposer.dll is missing functions";
        return;
    }

    // The plugins (sl.common, sl.dlss_d) come from a subfolder, and NGX loads nvngx_dlssd.dll from next to them:
    // dlss/transformer has the current DLSS RR, dlss/cnn the older one with the CNN models.
    std::wstring pluginDirectory = directory + (dlssCnnVersion ? L"\\dlss\\cnn" : L"\\dlss\\transformer");
    const wchar_t* pluginPaths[] = {pluginDirectory.c_str()};
    sl::Feature features[] = {sl::kFeatureDLSS_RR};
    sl::Preferences preferences;
    preferences.showConsole = false;
    preferences.logLevel = sl::LogLevel::eDefault;
    preferences.logMessageCallback = logMessage;
    preferences.pathsToPlugins = pluginPaths;
    preferences.numPathsToPlugins = 1;
    preferences.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking |
                        sl::PreferenceFlags::eUseFrameBasedResourceTagging;  // no over-the-air updates
    preferences.featuresToLoad = features;
    preferences.numFeaturesToLoad = 1;
    preferences.engine = sl::EngineType::eCustom;
    preferences.engineVersion = "1.0";
    preferences.projectId = "6c3d1e0a-7b52-4f0e-9a38-2d5f1c8e4b71";  // any fixed GUID identifying this project
    preferences.renderAPI = sl::RenderAPI::eVulkan;

    sl::Result result = sl_.init(preferences, sl::kSDKVersion);
    std::cout << "DLSS: slInit -> " << resultName(result) << std::endl;
    if (result != sl::Result::eOk) {
        dlssStatus = std::string("slInit failed: ") + resultName(result);
        return;
    }
    streamlineInitialized = true;

    sl::FeatureRequirements requirements;
    result = sl_.getFeatureRequirements(sl::kFeatureDLSS_RR, requirements);
    std::cout << "DLSS: slGetFeatureRequirements(DLSS RR) -> " << resultName(result) << std::endl;
    if (result == sl::Result::eOk) {
        bool vulkan = (static_cast<uint32_t>(requirements.flags) & static_cast<uint32_t>(sl::FeatureRequirementFlags::eVulkanSupported)) != 0;
        std::cout << "  Vulkan supported: " << (vulkan ? "yes" : "NO") << "\n"
                  << "  driver detected " << versionString(requirements.driverVersionDetected)
                  << ", required " << versionString(requirements.driverVersionRequired) << "\n"
                  << "  OS detected " << versionString(requirements.osVersionDetected)
                  << ", required " << versionString(requirements.osVersionRequired) << "\n"
                  << "  extra queues: compute " << requirements.vkNumComputeQueuesRequired
                  << ", graphics " << requirements.vkNumGraphicsQueuesRequired
                  << ", optical flow " << requirements.vkNumOpticalFlowQueuesRequired << "\n"
                  << "  required tags: " << requirements.numRequiredTags << "\n";
        for (uint32_t i = 0; i < requirements.vkNumInstanceExtensions; i++) {
            std::cout << "  instance extension: " << requirements.vkInstanceExtensions[i] << "\n";
            streamlineInstanceExtensions.emplace_back(requirements.vkInstanceExtensions[i]);
        }
        for (uint32_t i = 0; i < requirements.vkNumDeviceExtensions; i++) {
            std::cout << "  device extension: " << requirements.vkDeviceExtensions[i] << "\n";
            streamlineDeviceExtensions.emplace_back(requirements.vkDeviceExtensions[i]);
        }
        for (uint32_t i = 0; i < requirements.vkNumFeatures12; i++) {
            std::cout << "  Vulkan 1.2 feature: " << requirements.vkFeatures12[i] << "\n";
            streamlineFeatures12.emplace_back(requirements.vkFeatures12[i]);
        }
        for (uint32_t i = 0; i < requirements.vkNumFeatures13; i++) {
            std::cout << "  Vulkan 1.3 feature: " << requirements.vkFeatures13[i] << "\n";
            streamlineFeatures13.emplace_back(requirements.vkFeatures13[i]);
        }
        std::cout << std::flush;
    }
}

// Sets every VkBool32 that is set in "extra" in "target" as well. Both are the same feature struct:
// sType and pNext first, then nothing but VkBool32 members.
template <typename T>
static void mergeFeatures(T& target, const T& extra) {
    constexpr size_t first = offsetof(T, pNext) + sizeof(void*);
    auto* to = reinterpret_cast<VkBool32*>(reinterpret_cast<char*>(&target) + first);
    auto* from = reinterpret_cast<const VkBool32*>(reinterpret_cast<const char*>(&extra) + first);
    for (size_t i = 0; i < (sizeof(T) - first) / sizeof(VkBool32); i++) {
        to[i] = to[i] || from[i];
    }
}

// While creating the device: add what Streamline listed in its requirements.
void Renderer::addStreamlineDeviceRequirements(std::vector<const char*>& extensions,
                                                  VkPhysicalDeviceVulkan12Features& features12,
                                                  VkPhysicalDeviceVulkan13Features& features13) {
    if (!streamlineInitialized) {
        return;
    }
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(gpu.physicalDevice, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    vkEnumerateDeviceExtensionProperties(gpu.physicalDevice, nullptr, &count, available.data());
    auto has = [&](const std::vector<const char*>& list, const std::string& name) {
        return std::any_of(list.begin(), list.end(), [&](const char* e) { return name == e; });
    };

    for (const std::string& name : streamlineDeviceExtensions) {
        if (has(extensions, name)) {
            continue;
        }
        // The EXT version is the old one and may not be enabled together with the KHR (core 1.2) version.
        if (name == VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME &&
            std::count(streamlineDeviceExtensions.begin(), streamlineDeviceExtensions.end(), VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME)) {
            continue;
        }
        bool supported = std::any_of(available.begin(), available.end(),
                                     [&](const VkExtensionProperties& p) { return name == p.extensionName; });
        if (!supported) {
            std::cout << "DLSS: device extension " << name << " not supported, skipped" << std::endl;
            continue;
        }
        extensions.push_back(name.c_str());  // the string lives in streamlineDeviceExtensions
    }

    std::vector<const char*> names12, names13;
    for (const std::string& name : streamlineFeatures12) names12.push_back(name.c_str());
    for (const std::string& name : streamlineFeatures13) names13.push_back(name.c_str());
    mergeFeatures(features12, sl::getVkPhysicalDeviceVulkan12Features(static_cast<uint32_t>(names12.size()), names12.data()));
    mergeFeatures(features13, sl::getVkPhysicalDeviceVulkan13Features(static_cast<uint32_t>(names13.size()), names13.data()));
    features13.privateData = VK_TRUE;  // used by Streamline although it isn't in its list
}

// After vkCreateDevice: hand the device to Streamline and ask whether DLSS RR runs on it.
void Renderer::connectStreamline() {
    if (!streamlineInitialized) {
        return;
    }
    sl::VulkanInfo info;
    info.device = gpu.device;
    info.instance = gpu.instance;
    info.physicalDevice = gpu.physicalDevice;
    info.graphicsQueueFamily = gpu.graphicsQueueFamily;
    info.graphicsQueueIndex = 1;  // Streamline's own queues (if any) would start after our queue 0
    info.computeQueueFamily = gpu.graphicsQueueFamily;
    info.computeQueueIndex = 1;
    sl::Result result = sl_.setVulkanInfo(info);
    std::cout << "DLSS: slSetVulkanInfo -> " << resultName(result) << std::endl;
    if (result != sl::Result::eOk) {
        dlssStatus = std::string("slSetVulkanInfo failed: ") + resultName(result);
        return;
    }

    // From now on swapchain, acquire, present and wait idle go through Streamline's proxies: it has to see every
    // present (its per-frame bookkeeping) and the swapchain the presents go to.
    auto getDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
            GetProcAddress(static_cast<HMODULE>(streamlineModule), "vkGetDeviceProcAddr"));
    PresentationFunctions proxies;
    bool allFound = getDeviceProcAddr != nullptr;
    auto proxy = [&](auto& function, const char* name) {
        auto address = allFound ? getDeviceProcAddr(gpu.device, name) : nullptr;
        function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(address);
        allFound = allFound && address != nullptr;
    };
    proxy(proxies.createSwapchain, "vkCreateSwapchainKHR");
    proxy(proxies.destroySwapchain, "vkDestroySwapchainKHR");
    proxy(proxies.getSwapchainImages, "vkGetSwapchainImagesKHR");
    proxy(proxies.acquireNextImage, "vkAcquireNextImageKHR");
    proxy(proxies.queuePresent, "vkQueuePresentKHR");
    proxy(proxies.deviceWaitIdle, "vkDeviceWaitIdle");
    if (!allFound) {
        dlssStatus = "Streamline's Vulkan proxies are missing";
        std::cout << "DLSS: " << dlssStatus << std::endl;
        return;
    }
    vkp = proxies;

    sl::AdapterInfo adapter;
    adapter.vkPhysicalDevice = gpu.physicalDevice;
    result = sl_.isFeatureSupported(sl::kFeatureDLSS_RR, adapter);
    std::cout << "DLSS: slIsFeatureSupported(DLSS RR) -> " << resultName(result) << std::endl;
    if (result != sl::Result::eOk) {
        dlssStatus = std::string("DLSS RR not supported: ") + resultName(result);
        return;
    }

    bool loaded = false;
    result = sl_.isFeatureLoaded(sl::kFeatureDLSS_RR, loaded);
    std::cout << "DLSS: slIsFeatureLoaded(DLSS RR) -> " << resultName(result) << ", loaded = " << loaded << std::endl;

    sl::FeatureVersion version;
    if (sl_.getFeatureVersion(sl::kFeatureDLSS_RR, version) == sl::Result::eOk) {
        std::cout << "DLSS: Streamline " << versionString(version.versionSL) << ", NGX " << versionString(version.versionNGX) << std::endl;
        dlssVersion = versionString(version.versionNGX);
    }
    if (!loaded) {
        dlssStatus = "DLSS RR plugin not loaded";
        return;
    }

    auto featureFunction = [](auto& function, const char* name) {
        void* address = nullptr;
        sl::Result r = sl_.getFeatureFunction(sl::kFeatureDLSS_RR, name, address);
        function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(address);
        if (r != sl::Result::eOk || !function) {
            std::cout << "DLSS: slGetFeatureFunction(" << name << ") -> " << resultName(r) << std::endl;
        }
        return function != nullptr;
    };
    if (!(featureFunction(sl_.dlssdGetOptimalSettings, "slDLSSDGetOptimalSettings") &
          featureFunction(sl_.dlssdGetState, "slDLSSDGetState") &
          featureFunction(sl_.dlssdSetOptions, "slDLSSDSetOptions"))) {
        dlssStatus = "DLSS RR functions missing";
        return;
    }
    dlssSupported = true;
    dlssStatus = "DLSS Ray Reconstruction available";
}

// The render size DLSS wants for a mode and window size (e.g. Quality: 2/3 per axis, DLAA: the window size)
VkExtent2D Renderer::dlssRenderExtent(int renderMode, VkExtent2D output) {
    sl::DLSSDOptions options;
    options.mode = toDlssMode(renderMode);
    options.outputWidth = output.width;
    options.outputHeight = output.height;
    options.colorBuffersHDR = sl::Boolean::eTrue;
    sl::DLSSDOptimalSettings optimal;
    sl::Result result = sl_.dlssdGetOptimalSettings(options, optimal);
    if (result != sl::Result::eOk || optimal.optimalRenderWidth == 0 || optimal.optimalRenderHeight == 0) {
        std::cout << "DLSS: slDLSSDGetOptimalSettings -> " << resultName(result) << ", rendering at full size" << std::endl;
        return output;
    }
    return {optimal.optimalRenderWidth, optimal.optimalRenderHeight};
}

// DLSS keeps its own resources (history, network state) per viewport, sized for the current render and output
// sizes. It can't resize them on the fly, so they are freed when either changes and recreated by the next
// evaluate. The GPU must be idle.
void Renderer::freeDlssResources() {
    if (dlssResourcesInUse) {
        sl::Result result = sl_.freeResources(sl::kFeatureDLSS_RR, dlssViewport);
        if (result != sl::Result::eOk) {
            std::cout << "DLSS: slFreeResources -> " << resultName(result) << std::endl;
        }
        dlssResourcesInUse = false;
    }
}

bool Renderer::dlssModelAvailable(int model) const {
    if (dlssCnnVersion) {
        return model != static_cast<int>(DlssModel::TransformerF);
    }
    return !isCnnModel(model);
}

// Called once per frame before drawing: switches between accumulation and the DLSS modes (new render size), and
// between DLSS models (DLSS starts over with the new network).
void Renderer::applyRenderMode() {
    if (!dlssSupported) {
        settings.renderMode = static_cast<int>(RenderMode::Accumulation);
    }
    if (!dlssModelAvailable(settings.dlssModel)) {
        // The newest model the loaded DLSS version has
        settings.dlssModel = static_cast<int>(dlssCnnVersion ? DlssModel::TransformerE : DlssModel::TransformerF);
    }
    if (settings.dlssModel != activeDlssModel) {
        vkp.deviceWaitIdle(gpu.device);
        freeDlssResources();
        activeDlssModel = settings.dlssModel;
        dlssReset = true;
    }
    if (settings.renderMode == activeRenderMode) {
        return;
    }
    vkp.deviceWaitIdle(gpu.device);
    freeDlssResources();
    activeRenderMode = settings.renderMode;
    recreateRenderImages();
    hasPreviousViewProjection = false;
    jitterIndex = 0;
}

// Records DLSS into the frame's command buffer, right after the trace shader. Does nothing in accumulation mode.
void Renderer::recordDlss(VkCommandBuffer commandBuffer) {
    if (!dlssActive()) {
        return;
    }
    // The trace shader's debug views are shown without DLSS (they are drawn from the render-size image). DLSS's
    // history doesn't match the scene afterwards, so it starts over then.
    if (settings.debugView != static_cast<int>(DebugView::None) && !isGuideView(settings.debugView)) {
        dlssReset = true;
        return;
    }

    // The trace shader's images must be written before DLSS reads them (DLSS records its own compute work)
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);

    sl::FrameToken* frameToken = nullptr;
    uint32_t frame = dlssFrameIndex++;
    if (sl_.getNewFrameToken(frameToken, &frame) != sl::Result::eOk || !frameToken) {
        std::cout << "DLSS: slGetNewFrameToken failed" << std::endl;
        return;
    }

    // Camera: matrices without jitter, the jitter separately in pixels
    sl::Constants constants;
    glm::mat4 clipToPrevClip = framePreviousViewProjection * glm::inverse(frameViewProjection);
    constants.cameraViewToClip = toSl(frameProjection);
    constants.clipToCameraView = toSl(glm::inverse(frameProjection));
    constants.clipToLensClip = toSl(glm::mat4(1.0f));
    constants.clipToPrevClip = toSl(clipToPrevClip);
    constants.prevClipToClip = toSl(glm::inverse(clipToPrevClip));
    // Our rays go through pixel center + frameJitter; DLSS wants the offset the other way round (checked against
    // the accumulated reference: with the same sign the image is visibly softer)
    constants.jitterOffset = {-frameJitter.x, -frameJitter.y};
    constants.mvecScale = {1.0f / static_cast<float>(renderExtent.width), 1.0f / static_cast<float>(renderExtent.height)};
    constants.cameraPinholeOffset = {0.0f, 0.0f};
    constants.cameraPos = toSl(renderList->camera.position);
    constants.cameraUp = toSl(renderList->camera.up());
    constants.cameraRight = toSl(renderList->camera.right());
    constants.cameraFwd = toSl(renderList->camera.forward());
    constants.cameraNear = Configuration::cameraNear;
    constants.cameraFar = Configuration::cameraFar;
    constants.cameraFOV = glm::radians(renderList->camera.fov);
    constants.cameraAspectRatio = static_cast<float>(renderExtent.width) / static_cast<float>(renderExtent.height);
    constants.depthInverted = sl::Boolean::eFalse;
    constants.cameraMotionIncluded = sl::Boolean::eTrue;  // our motion vectors include the camera's motion
    constants.motionVectors3D = sl::Boolean::eFalse;
    constants.reset = dlssReset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    constants.orthographicProjection = sl::Boolean::eFalse;
    constants.motionVectorsDilated = sl::Boolean::eFalse;
    constants.motionVectorsJittered = sl::Boolean::eFalse;
    sl::Result result = sl_.setConstants(constants, *frameToken, dlssViewport);
    if (result != sl::Result::eOk) {
        std::cout << "DLSS: slSetConstants -> " << resultName(result) << std::endl;
        return;
    }

    sl::DLSSDOptions options;
    options.mode = toDlssMode(activeRenderMode);
    options.outputWidth = outputExtent.width;
    options.outputHeight = outputExtent.height;
    options.colorBuffersHDR = sl::Boolean::eTrue;
    options.normalRoughnessMode = sl::DLSSDNormalRoughnessMode::ePacked;  // roughness in the normals' alpha
    sl::DLSSDPreset preset = toDlssPreset(activeDlssModel);  // the same model for every mode
    options.dlaaPreset = options.qualityPreset = options.balancedPreset = preset;
    options.performancePreset = options.ultraPerformancePreset = options.ultraQualityPreset = preset;
    options.worldToCameraView = toSl(frameView);
    options.cameraViewToWorld = toSl(glm::inverse(frameView));
    result = sl_.dlssdSetOptions(dlssViewport, options);
    if (result != sl::Result::eOk) {
        std::cout << "DLSS: slDLSSDSetOptions -> " << resultName(result) << std::endl;
        return;
    }

    // The images. They stay in GENERAL and aren't touched again before the next frame's trace. (DLSS switches
    // them to SHADER_READ_ONLY_OPTIMAL while reading and back, hence the sampled usage.)
    const VkImageUsageFlags storage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    auto describe = [&](const RenderImage& image, VkExtent2D extent, VkFormat format, VkImageUsageFlags usage) {
        return describeImage(image.image, gpu.deviceMemoryOf(image.memory), image.view, extent, format, usage);
    };
    sl::Resource color = describeImage(radianceImage, gpu.deviceMemoryOf(radianceImageMemory), radianceImageView,
                                       renderExtent, radianceFormat, storage);
    sl::Resource output = describe(dlssOutputImage, outputExtent, dlssOutputFormat, storage);
    sl::Resource depth = describe(linearDepthImage, renderExtent, linearDepthFormat, storage);
    sl::Resource motion = describe(motionVectorImage, renderExtent, motionVectorFormat, storage);
    sl::Resource diffuse = describe(diffuseAlbedoImage, renderExtent, guideColorFormat,
                                    storage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    sl::Resource specular = describe(specularAlbedoImage, renderExtent, guideColorFormat, storage);
    sl::Resource normals = describe(normalRoughnessImage, renderExtent, guideColorFormat, storage);
    sl::Resource hitDistance = describe(specularHitDistanceImage, renderExtent, hitDistanceFormat, storage);
    sl::Extent renderArea{0, 0, renderExtent.width, renderExtent.height};
    sl::Extent outputArea{0, 0, outputExtent.width, outputExtent.height};
    const sl::ResourceLifecycle lifecycle = sl::ResourceLifecycle::eValidUntilPresent;
    sl::ResourceTag tags[] = {
            sl::ResourceTag(&color, sl::kBufferTypeScalingInputColor, lifecycle, &renderArea),
            sl::ResourceTag(&output, sl::kBufferTypeScalingOutputColor, lifecycle, &outputArea),
            sl::ResourceTag(&depth, sl::kBufferTypeLinearDepth, lifecycle, &renderArea),
            sl::ResourceTag(&motion, sl::kBufferTypeMotionVectors, lifecycle, &renderArea),
            sl::ResourceTag(&diffuse, sl::kBufferTypeAlbedo, lifecycle, &renderArea),
            sl::ResourceTag(&specular, sl::kBufferTypeSpecularAlbedo, lifecycle, &renderArea),
            sl::ResourceTag(&normals, sl::kBufferTypeNormalRoughness, lifecycle, &renderArea),
            sl::ResourceTag(&hitDistance, sl::kBufferTypeSpecularHitDistance, lifecycle, &renderArea),
    };
    result = sl_.setTagForFrame(*frameToken, dlssViewport, tags, static_cast<uint32_t>(std::size(tags)), commandBuffer);
    if (result != sl::Result::eOk) {
        std::cout << "DLSS: slSetTagForFrame -> " << resultName(result) << std::endl;
        return;
    }

    const sl::BaseStructure* inputs[] = {&dlssViewport};
    result = sl_.evaluateFeature(sl::kFeatureDLSS_RR, *frameToken, inputs, 1, commandBuffer);
    if (result != sl::Result::eOk) {
        std::cout << "DLSS: slEvaluateFeature -> " << resultName(result) << std::endl;
        return;
    }
    dlssResourcesInUse = true;
    dlssReset = false;
    // (Streamline doesn't restore our pipeline and descriptor bindings with manual hooking; the exposure and
    // display passes bind their own anyway.)

    // DLSS's output must be written before exposure and display read it
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);
}

// The "Renderer" combo at the top of the Rendering section
void Renderer::buildRenderModeUi() {
    const char* modes[] = {"Accumulation (reference)", "DLSS RR: DLAA (full size)", "DLSS RR: Quality",
                           "DLSS RR: Balanced", "DLSS RR: Performance", "DLSS RR: Ultra Performance"};
    const char* models[] = {"Transformer F (newest)", "Transformer E", "Transformer D", "CNN A", "CNN B", "CNN C"};
    ImGui::BeginDisabled(!dlssSupported);
    ImGui::Combo("Renderer", &settings.renderMode, modes, static_cast<int>(std::size(modes)));
    ImGui::EndDisabled();
    const char* sizes[] = {"Window", "1080p", "1440p", "2160p (4K)"};
    static_assert(std::size(sizes) == std::size(outputHeights));
    ImGui::Combo("Image size", &settings.outputSize, sizes, static_cast<int>(std::size(sizes)));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("The size of the image, and of what Capture saves. Bigger than the window: shown shrunk\n"
                          "to fit, and slower in proportion (4K has 5.8x the pixels of 1600 x 900). Keeps the\n"
                          "window's shape. For a sharp 4K capture: Accumulation, let it settle, then Capture.");
    }
    ImGui::TextDisabled("traces %u x %u, image %u x %u", renderExtent.width, renderExtent.height, outputExtent.width,
                        outputExtent.height);
    ImGui::BeginDisabled(!dlssSupported);
    // Models the loaded DLSS version doesn't have are shown but can't be picked
    if (ImGui::BeginCombo("DLSS model", models[settings.dlssModel])) {
        for (int i = 0; i < static_cast<int>(std::size(models)); i++) {
            ImGuiSelectableFlags flags = dlssModelAvailable(i) ? 0 : ImGuiSelectableFlags_Disabled;
            if (ImGui::Selectable(models[i], settings.dlssModel == i, flags)) {
                settings.dlssModel = i;
            }
            if (!dlssModelAvailable(i) && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip(dlssCnnVersion ? "only in the current DLSS version: start without --dlss-cnn"
                                                 : "only in the older DLSS version: start with --dlss-cnn");
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("The network DLSS Ray Reconstruction runs. Transformer: the newer models, sharper and\n"
                          "steadier. CNN: the older models, about half the cost on a 20/30 series GPU.\n"
                          "DLSS %s is loaded (%s).", dlssVersion.c_str(),
                          dlssCnnVersion ? "--dlss-cnn: CNN A-C, transformer D-E"
                                         : "transformer D-F; start with --dlss-cnn for the CNN models");
    }
    ImGui::Checkbox("Clamp bounce light", &settings.clampBounceLight);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
    ImGui::SliderFloat("##bounceClamp", &settings.bounceLightClampLevel, 0.25f, 64.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("DLSS modes: one sample of light that reached the first surface via another surface adds at\n"
                          "most this much on screen (after exposure, 1 = white before tone mapping). Rare very bright\n"
                          "samples (a bounce ray that happens to find a lamp) otherwise stand out, and DLSS smears\n"
                          "them over many frames: ghosting, and an image that settles slowly. Biased: a bit darker.\n"
                          "Measured in Bistro at night: no less noise or drift, so off by default.");
    }
    ImGui::EndDisabled();
    if (!dlssSupported) {
        ImGui::TextDisabled("DLSS unavailable: %s", dlssStatus.c_str());
    } else if (dlssActive()) {
        sl::DLSSDState state;
        double megabytes = sl_.dlssdGetState(dlssViewport, state) == sl::Result::eOk
                           ? static_cast<double>(state.estimatedVRAMUsageInBytes) / (1024.0 * 1024.0) : 0.0;
        ImGui::TextDisabled("render %ux%u -> %ux%u (%.0f%% per axis), DLSS memory %.0f MB",
                            renderExtent.width, renderExtent.height, outputExtent.width, outputExtent.height,
                            100.0 * renderExtent.width / outputExtent.width, megabytes);
    }
}

// Before vkDestroyDevice (the GPU is idle)
void Renderer::shutdownStreamline() {
    if (streamlineInitialized) {
        freeDlssResources();
        sl::Result result = sl_.shutdown();
        std::cout << "DLSS: slShutdown -> " << resultName(result) << std::endl;
        streamlineInitialized = false;
    }
    if (streamlineModule) {
        FreeLibrary(static_cast<HMODULE>(streamlineModule));
        streamlineModule = nullptr;
    }
}
