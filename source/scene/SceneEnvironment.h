//
// The scene's environment: the light coming from outside it, the sun and the background (a gradient sky or an HDR
// map). Part of the Scene, copied into the RenderList every frame. How the renderer samples it (sky sampling, ...)
// is the renderer's setting, not part of this.
//

#ifndef VULKANDEMO_SCENEENVIRONMENT_H
#define VULKANDEMO_SCENEENVIRONMENT_H

#include <glm/glm.hpp>
#include "core/Handles.h"

// What rays see when they leave the scene. Map: an HDR photo of the surroundings (equirectangular .hdr from the
// environments folder). White: uniform radiance 1 from every direction ("white furnace"). A white material that
// neither creates nor loses energy then looks exactly as bright as the background; anything darker shows where
// energy is lost.
enum class Background : int { Sky = 0, Map = 1, Black = 2, White = 3 };

struct SceneEnvironment {
    bool sunEnabled = true;
    float sunAzimuth = 35.0f;       // degrees around the Y axis
    float sunElevation = 50.0f;     // degrees above the horizon
    float sunAngularRadius = 2.0f;  // degrees; the real sun is ~0.27, bigger = softer shadows
    glm::vec3 sunColor = {1.0f, 0.95f, 0.85f};
    float sunIntensity = 1.0f;

    int background = static_cast<int>(Background::Sky);  // Background
    glm::vec3 skyZenithColor = {0.08f, 0.36f, 0.72f};
    glm::vec3 skyHorizonColor = {1.0f, 1.0f, 1.0f};
    glm::vec3 groundColor = {0.35f, 0.3f, 0.35f};
    float skyIntensity = 0.6f;
    EnvironmentHandle map;      // the HDR map (Scene::setEnvironmentMap); none or not loaded: black
    float mapRotation = 0.0f;   // degrees around the vertical axis
    float mapIntensity = 1.0f;  // multiplies the map
};

#endif //VULKANDEMO_SCENEENVIRONMENT_H
