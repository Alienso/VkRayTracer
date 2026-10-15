//
// Created by Alienso on 23.9.2022..
//

#ifndef GRASSTERRAIN_CONFIGURATION_H
#define GRASSTERRAIN_CONFIGURATION_H

#include <vector>

class Configuration{

public:
    static const int wWidth;
    static const int wHeight;
    static const float fov;
    static const float fovMax;
    static const float cameraNear;  // projection matrix planes (motion vectors, DLSS); rays aren't clipped by them
    static const float cameraFar;   // also the depth written where rays hit nothing (sky)
    static const std::vector<const char*> validationLayers;
    static const std::vector<const char*> deviceExtensions;
    // The Vulkan validation layer: on in Debug builds, off in Release; --validation / --no-validation (main.cpp)
    // override it. It slows the CPU side (every Vulkan call is checked): off for timing runs.
    static bool enableValidationLayers;
    static const int maxFramesInFlight;

    Configuration()= default;
};

#endif //GRASSTERRAIN_CONFIGURATION_H
