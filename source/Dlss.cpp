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
// So far this only checks whether DLSS Ray Reconstruction can run here and prints the answer.
//

#include <windows.h>  // LoadLibrary; Streamline's headers also need HRESULT
#include <vulkan/vulkan.h>
#include <algorithm>
#include <cstddef>
#include <iostream>
#include <string>
#include "../include/streamline/sl.h"
#include "../include/streamline/sl_helpers_vk.h"
#include "../include/streamline/sl_dlss_d.h"
#include "Application.h"

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
} sl_;

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
void Application::initStreamline() {
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
              load(sl_.getFeatureVersion, "slGetFeatureVersion") & load(sl_.setVulkanInfo, "slSetVulkanInfo");
    if (!ok) {
        dlssStatus = "sl.interposer.dll is missing functions";
        return;
    }

    const wchar_t* pluginPaths[] = {directory.c_str()};
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
void Application::addStreamlineDeviceRequirements(std::vector<const char*>& extensions,
                                                  VkPhysicalDeviceVulkan12Features& features12,
                                                  VkPhysicalDeviceVulkan13Features& features13) {
    if (!streamlineInitialized) {
        return;
    }
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, available.data());
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
void Application::connectStreamline(uint32_t graphicsQueueFamily) {
    if (!streamlineInitialized) {
        return;
    }
    sl::VulkanInfo info;
    info.device = device;
    info.instance = instance;
    info.physicalDevice = physicalDevice;
    info.graphicsQueueFamily = graphicsQueueFamily;
    info.graphicsQueueIndex = 1;  // Streamline's own queues (if any) would start after our queue 0
    info.computeQueueFamily = graphicsQueueFamily;
    info.computeQueueIndex = 1;
    sl::Result result = sl_.setVulkanInfo(info);
    std::cout << "DLSS: slSetVulkanInfo -> " << resultName(result) << std::endl;
    if (result != sl::Result::eOk) {
        dlssStatus = std::string("slSetVulkanInfo failed: ") + resultName(result);
        return;
    }

    sl::AdapterInfo adapter;
    adapter.vkPhysicalDevice = physicalDevice;
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
    }
    dlssSupported = loaded;
    dlssStatus = loaded ? "DLSS Ray Reconstruction available" : "DLSS RR plugin not loaded";
}

// Before vkDestroyDevice
void Application::shutdownStreamline() {
    if (streamlineInitialized) {
        sl::Result result = sl_.shutdown();
        std::cout << "DLSS: slShutdown -> " << resultName(result) << std::endl;
        streamlineInitialized = false;
    }
    if (streamlineModule) {
        FreeLibrary(static_cast<HMODULE>(streamlineModule));
        streamlineModule = nullptr;
    }
}
