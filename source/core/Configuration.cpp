//
// Created by Alienso on 23.9.2022..
//

#include <vulkan/vulkan.h>
#include "core/Configuration.h"

const int Configuration:: wWidth = 1600;
const int Configuration:: wHeight = 900;
const float Configuration::fov = 60.0f;
const float Configuration::fovMax = 100.0f;
const float Configuration::cameraNear = 0.05f;
const float Configuration::cameraFar = 10000.0f;
const int Configuration::maxFramesInFlight = 2;

const std::vector<const char*> Configuration::validationLayers = {
        "VK_LAYER_KHRONOS_validation"
};
const std::vector<const char*> Configuration::deviceExtensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_RAY_QUERY_EXTENSION_NAME,
        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,  // required by VK_KHR_acceleration_structure
        VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME        // radiance cache: adding up samples with atomicAdd
};

#ifdef VULKANDEMO_RESOURCE_DIR
std::string Configuration::resourceDirectory = VULKANDEMO_RESOURCE_DIR;
#else
std::string Configuration::resourceDirectory = "resource";
#endif

#ifdef NDEBUG
bool Configuration::enableValidationLayers = false;
#else
bool Configuration::enableValidationLayers = true;
#endif
