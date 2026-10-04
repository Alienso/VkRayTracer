//
// Created by Alienso on 23.9.2022..
//

#include <vulkan/vulkan.h>
#include "Configuration.h"

const int Configuration:: wWidth = 1600;
const int Configuration:: wHeight = 900;
const float Configuration::fov = 60.0f;
const float Configuration::fovMax = 100.0f;
const int Configuration::maxFramesInFlight = 2;

const std::vector<const char*> Configuration::validationLayers = {
        "VK_LAYER_KHRONOS_validation"
};
const std::vector<const char*> Configuration::deviceExtensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME
};

const bool Configuration::enableValidationLayers = true;
