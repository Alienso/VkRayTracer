//
// What the renderer's parts share: the structs the shaders read (sizes checked), formats, the settings, the stats.
// Renderer.h has the renderer itself; GpuScene.h the scene on the GPU.
//

#ifndef VULKANDEMO_RENDERERTYPES_H
#define VULKANDEMO_RENDERERTYPES_H

#include <vulkan/vulkan.h>
#include <vma/vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <string>
#include <utility>
#include <vector>
#include "assets/ModelLoader.h"

// The structs the shaders read ("must match") are checked for size (static_assert after each): a change here fails
// the build, as a reminder to change the shader's copy too. The sizes are the shaders' (spirv-cross --reflect on the
// .spv). Only the size: two fields swapped, or a change on the shader side alone, still go unnoticed.

// Must match the SceneUniforms block in shaders/raytrace/bindings.glsl (std140: matrices and vec4s first, no vec3s)
struct SceneUniforms {
    glm::mat4 viewProjection;          // this frame, without jitter (motion vectors)
    glm::mat4 previousViewProjection;  // last frame's
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
    uint32_t environmentMap;         // 1 = the sky is the HDR map (binding 21), 0 = the gradient above
    uint32_t environmentSampling;    // 1 = shadow rays are aimed at the sky too (distribution in binding 22)
    uint32_t environmentWidth;       // cells of that distribution
    uint32_t environmentHeight;
    float environmentRotation;       // radians around the vertical axis
    float environmentIntensity;      // multiplies the HDR map
    uint32_t punctualLightCount;     // entries in the punctual light list (binding 23)
    uint32_t samplesPerPixel;
    uint32_t maxBounces;
    uint32_t frameIndex;
    uint32_t shadowsEnabled;
    uint32_t accumulatedFrames;
    uint32_t accumulate;
    uint32_t debugView;
    uint32_t emissiveTriangleCount;  // entries in the light list
    uint32_t emissiveSampling;       // EmissiveSampling
    uint32_t blurCaustics;           // 1 = path regularization, see tracePath in the shader
    float farDistance;               // view depth written where rays hit nothing
    uint32_t writeGuides;            // 1 = write the DLSS input images (depth, motion vectors, albedos, ...)
    uint32_t cacheQuery;             // 1 = image paths end in the radiance cache where they can
    uint32_t cacheCapacity;          // entries in the cache's hash table
    float cacheCellScale;            // cell size per unit of distance from the camera
    float cacheMinWeight;            // samples a cell needs before it is used
    uint32_t cacheUpdateTile;        // one update path per cacheUpdateTile x cacheUpdateTile pixels
    uint32_t lightTree;              // 1 = light sampling picks glowing triangles with the light tree (LightPicking)
    // Derived from the sun values above, once here instead of per pixel: kept in registers through the whole path,
    // they made the trace shader slower (see buildSceneUniforms)
    uint32_t sunOn;                  // 1 = the sun gives light
    uint32_t sunMis;                 // 1 = the sun has a size, so bounce rays find it too (MIS with its shadow rays)
    float sunPdf;                    // 1 / the disc's solid angle: the pdf of a shadow ray towards it
    float sunRadianceScale;          // sunColor times this = the light of one point of the disc (PI / solid angle)
    uint32_t textureMipmaps;         // 1 = textures are read at the mip level that fits the hit (ray cones)
    float pixelSpreadAngle;          // radians between neighboring camera rays (render resolution)
    uint32_t normalMapFiltering;     // 1 = bumps that a normal map's smaller levels average away widen the reflection
    uint32_t restirDi;               // 1 = ReSTIR DI: camera rays and light samples in a pass of their own (recordRayTracing)
    uint32_t restirTriangleCandidates;  // ReSTIR candidates per kind of light (0 when there are none of that kind)
    uint32_t restirPunctualCandidates;
    uint32_t restirSkyCandidates;
    uint32_t restirBrdfCandidates;
    float restirMinRoughness;        // rougher surfaces use ReSTIR
    uint32_t restirTemporal;         // 1 = reuse last frame's reservoirs
    float restirMaxHistory;          // last frame's reservoir counts for at most this many times this frame's candidates
    uint32_t restirHistoryValid;     // 0: last frame's reservoirs can't be used (see buildSceneUniforms)
    uint32_t restirCurrentHalf;      // which half of the reservoir and surface buffers is this frame's
    uint32_t restirPermutation;      // shuffle of last frame's pixels: bit 4 = on, bits 0-3 = this frame's offset
    uint32_t restirSpatialNeighbors; // neighbors' reservoirs each pixel resamples; 0 (and the next 0) = no spatial pass
    float restirSpatialRadius;       // within this many render pixels
    uint32_t restirDisocclusionNeighbors;  // as many for pixels that found no history
    uint32_t restirSpatialVisibility;      // 1 = unbiased spatial reuse (shadow rays from the neighbors' surfaces)
    float bounceLightClamp;          // 0 = off; else the most light one sample of bounce light adds (luminance)
    uint32_t restirGi;               // 1 = ReSTIR GI: the first bounce's light at ReSTIR surfaces is a sample (GI passes)
    float restirGiMinRoughness;      // second surfaces at least this rough give a GI sample
    uint32_t restirGiTemporal;       // 1 = reuse last frame's GI reservoirs
    float restirGiMaxHistory;        // last frame's GI reservoir counts for at most this many times this frame's samples
    uint32_t restirGiMaxAge;         // GI samples older than this many frames are dropped
    uint32_t restirGiHistoryValid;   // 0: last frame's GI reservoirs can't be used (see buildSceneUniforms)
    uint32_t restirGiPermutation;    // 1 = shuffle last frame's pixels (offset in restirPermutation's bits 0-3)
    uint32_t restirGiSpatialNeighbors;     // neighbors' GI reservoirs each pixel resamples; 0 (and the next 0) = none
    float restirGiSpatialRadius;           // within this many render pixels
    uint32_t restirGiDisocclusionNeighbors;  // as many for pixels that found no history
    uint32_t restirGiSpatialVisibility;    // 1 = rays from the neighbors' surfaces for the kept sample
    float restirGiBoilingFilter;     // 0 = off; else its strength (drops reservoirs far brighter than their neighbors')
    float restirGiRadianceClamp;     // 0 = off; else the most a GI sample's light may be (luminance)
    uint32_t restirGiTemporalVisibility;  // 1 = unbiased temporal GI reuse (a ray from last frame's surface)
    uint32_t pathSampling;           // the path's main decisions: 0 = white noise, 1 = Sobol, 2 = Sobol + blue noise
    uint32_t emissiveSolidAngle;     // 1 = points on glowing triangles picked by solid angle, 0 = by area
    uint32_t portalCount;            // entries in the portal list (binding 31): two per pair
    float fogTime;                   // seconds of fog drift (Material::fogDrift); 0 while no fog drifts
    uint32_t lightCandidates;        // glowing triangles picked by power to keep one of (sampleEmissiveLight); 1 = tree
    uint32_t cacheFog;               // 1 = scatter points in fog have radiance cache cells too
    uint32_t maxPortalCrossings;     // portals a ray may go through; at least 1
};
static_assert(sizeof(SceneUniforms) == 564, "SceneUniforms must match SceneUniforms in shaders/raytrace/bindings.glsl");

// Per TLAS instance, binding 12. Must match InstanceData in shaders/raytrace/bindings.glsl (scalar layout). 56 bytes:
// Jungle Ruins has 2.9 million instances (the inverse matrix is the shader's to work out, the motion its object's).
struct GpuInstance {
    glm::mat4x3 objectToWorld;  // the shader reads this instead of the ray query's, so hits stored by the ReSTIR
                                // camera-ray pass can be rebuilt the same way
    uint32_t meshIndex;         // as the TLAS instance's custom index (fog volumes are known by their instance)
    uint32_t motion;            // index into the motions, binding 34 (RenderList::motions: one per object)
};
static_assert(sizeof(GpuInstance) == 56, "GpuInstance must match InstanceData in shaders/raytrace/bindings.glsl");

// One per render pixel, binding 25: the camera ray's hit. Must match PrimaryHit in shaders/raytrace/bindings.glsl.
struct GpuPrimaryHit {
    uint32_t meshIndex;
    uint32_t instanceIndex;
    int32_t primitive;
    glm::vec2 barycentrics;
    float t;
    uint32_t seed;
    float travelled;         // seen in mirrors: the ray's length before its last part (0: no mirror)
    glm::vec3 direction;     // its last part's direction
    uint32_t tintRG;         // the mirrors' color (half floats)
    uint32_t tintB;
};
static_assert(sizeof(GpuPrimaryHit) == 52, "GpuPrimaryHit must match PrimaryHit in shaders/raytrace/bindings.glsl");

// One per render pixel, binding 28: the image path's first bounce at a ReSTIR surface, traced by the ReSTIR pass as
// its BRDF candidate. Must match BounceHit in shaders/raytrace/bindings.glsl.
struct GpuBounceHit {
    uint32_t meshIndex;
    uint32_t instanceIndex;
    int32_t primitive;
    glm::vec2 barycentrics;
    float t;
    glm::vec3 direction;
    uint32_t weightRG;
    uint32_t weightBRoughness;
    float pdf;
};
static_assert(sizeof(GpuBounceHit) == 48, "GpuBounceHit must match BounceHit in shaders/raytrace/bindings.glsl");

// Three per render pixel (this frame's, last frame's, and the ReSTIR pass's result for the spatial pass), binding 26:
// ReSTIR DI's light sample. Must match Reservoir in shaders/raytrace/restir_di.glsl.
struct GpuReservoir {
    uint32_t light;
    uint32_t point;
    float W;
    float M;
};
static_assert(sizeof(GpuReservoir) == 16, "GpuReservoir must match Reservoir in shaders/raytrace/restir_di.glsl");

// Three per render pixel (this frame's, last frame's, the image pass's fresh samples), binding 29: ReSTIR GI's sample.
// Must match GiReservoir in shaders/raytrace/restir_gi.glsl.
struct GpuGiReservoir {
    glm::vec3 position;
    uint32_t normal;
    glm::vec3 radiance;
    float W;
    float M;
    uint32_t age;  // frames since the sample was found (bits 0-15), GI_SEEN flag; written by the shader only
};
static_assert(sizeof(GpuGiReservoir) == 40, "GpuGiReservoir must match GiReservoir in shaders/raytrace/restir_gi.glsl");

// Two per render pixel, binding 27: the first surface, packed, for ReSTIR's reuse. Must match RestirSurface in
// shaders/raytrace/restir_di.glsl.
struct GpuRestirSurface {
    glm::vec3 position;
    uint32_t normal;
    uint32_t geometricNormal;
    uint32_t albedoRoughness;
    uint32_t material;
    uint32_t diffuseTransmissionColor;
    glm::vec3 seenAt;        // where the camera sees it (in mirrors: its image)
    uint32_t view;           // towards the viewer (octahedral)
};
static_assert(sizeof(GpuRestirSurface) == 48,
              "GpuRestirSurface must match RestirSurface in shaders/raytrace/restir_di.glsl");

// Must match the push constants in shaders/cache_resolve.comp
struct CacheResolvePushConstants {
    uint32_t capacity;
    float decay;      // weight the old samples keep each frame
    float maxWeight;  // cap of a cell's sample weight
    uint32_t maxAge;  // frames without new samples until a cell is freed
};
static_assert(sizeof(CacheResolvePushConstants) == 16, "CacheResolvePushConstants must match the push constants in shaders/cache_resolve.comp");

// Must match ExposureState in shaders/exposure.comp (scalar layout). Lives on the GPU; the display pass
// reads the exposure from it, and a copy comes back to the CPU (a few frames late) for the UI.
static constexpr uint32_t exposureBinCount = 256;
struct ExposureState {
    uint32_t histogram[exposureBinCount];      // being filled during a frame
    uint32_t lastHistogram[exposureBinCount];  // the finished histogram of the last metered frame
    float exposure;          // current multiplier
    float targetExposure;    // where the adaptation is heading
    float averageLuminance;  // of the metered part of the image
    float lowLuminance;      // metered range: darker and brighter pixels are ignored
    float highLuminance;
    uint32_t meteredPixels;
};
static_assert(sizeof(ExposureState) == 2072, "ExposureState must match ExposureState in shaders/exposure.comp");

// Must match the push constants in shaders/exposure.comp
struct ExposurePushConstants {
    uint32_t pass;
    uint32_t reset;
    uint32_t centerWeighted;
    float minLog2;
    float log2Range;
    float lowFraction;
    float highFraction;
    float key;
    float minExposure;
    float maxExposure;
    float brightenSpeed;
    float darkenSpeed;
    float deltaTime;
};
static_assert(sizeof(ExposurePushConstants) == 52, "ExposurePushConstants must match the push constants in shaders/exposure.comp");

// Must match the push constants in shaders/display.comp
struct DisplayPushConstants {
    float exposure;         // manual multiplier, or 2^compensation on top of the measured exposure
    uint32_t autoExposure;  // 1 = multiply by the measured exposure (ExposureState) as well
    uint32_t toneMapping;   // 0 = clamp, 1 = ACES
    uint32_t view;          // DisplayView
    float farDistance;      // sky depth, to show it apart in the depth views
    float grainStrength;    // fog grain: 0 = off
    float grainSize;        // output pixels
    uint32_t frame;
};
static_assert(sizeof(DisplayPushConstants) == 32, "DisplayPushConstants must match the push constants in shaders/display.comp");

// Must match the push constants in shaders/noise.comp
struct NoisePushConstants {
    uint32_t pass;         // 0 = copy the image into the reference, 1 = compare with it
    float floorLuminance;  // added to the reference in the relative error's denominator
    glm::ivec2 regionMin;  // a rectangle whose brightness is also measured on its own
    glm::ivec2 regionMax;
};
static_assert(sizeof(NoisePushConstants) == 24, "NoisePushConstants must match the push constants in shaders/noise.comp");

// One frame of a noise measurement (Noise.cpp)
struct NoiseResult {
    uint32_t frame;      // 1 = the first frame after the start
    float relativeMse;   // mean of ((measured - reference) / (reference + floor))^2 over the image, luminance
    float brightness;    // measured / reference, summed over the image (1 = no bias)
    float regionBrightness;  // the same inside noiseRegion only (local bias, e.g. light leaking into a corner)
    float traceMs;       // GPU time of this frame's trace (+ radiance cache and ReSTIR) and DLSS passes
    float restirMs;      // of which ReSTIR
    float dlssMs;
};
enum class NoiseState { Idle, BuildingReference, WaitingToMeasure, Measuring };

// What the display pass shows. Must match the constants in shaders/display.comp.
enum class DisplayView : uint32_t {
    Color = 0,        // the path traced image, with exposure and tone mapping
    PassThrough,      // a debug view of the trace shader, values as they are
    Depth, MotionVectors, DiffuseAlbedo, SpecularAlbedo, Normals, Roughness, SpecularHitDistance,
    ReprojectionError
};

// How the image is made. Accumulation: the path tracer averages frames at full size (the reference).
// DLSS RR: one noisy frame at a smaller render size, denoised and upscaled by DLSS Ray Reconstruction.
enum class RenderMode : int {
    Accumulation = 0, DlssDlaa, DlssQuality, DlssBalanced, DlssPerformance, DlssUltraPerformance
};

// Which network DLSS RR runs (its "render presets", see toDlssPreset in Dlss.cpp). The transformer models look
// better; the older CNN models cost about half on a 20/30 series GPU. Which ones exist depends on the DLSS RR version
// loaded at startup (dlssModelAvailable): the current one has transformer D-F, the one loaded with --dlss-cnn has
// transformer D-E and CNN A-C.
enum class DlssModel : int {
    TransformerF = 0, TransformerE, TransformerD, CnnA, CnnB, CnnC
};

// The Vulkan functions Streamline has to see with manual hooking (sl_hooks.h): swapchain, acquire, present,
// wait idle. Vulkan's own until Streamline is connected, then Streamline's proxies (which call Vulkan's).
struct PresentationFunctions {
    PFN_vkCreateSwapchainKHR createSwapchain = vkCreateSwapchainKHR;
    PFN_vkDestroySwapchainKHR destroySwapchain = vkDestroySwapchainKHR;
    PFN_vkGetSwapchainImagesKHR getSwapchainImages = vkGetSwapchainImagesKHR;
    PFN_vkAcquireNextImageKHR acquireNextImage = vkAcquireNextImageKHR;
    PFN_vkQueuePresentKHR queuePresent = vkQueuePresentKHR;
    PFN_vkDeviceWaitIdle deviceWaitIdle = vkDeviceWaitIdle;
};

// Formats of the images the trace shader writes (and DLSS reads); the shaders' format qualifiers must match.
// Float radiance, so tiny 1/n contributions to the running average don't round away. The DLSS inputs: exact depth;
// 16-bit motion (DLSS's usual format; steps of 1/32 pixel or finer below 64 pixels per frame), colors, normals and
// hit distances (the sky's is the largest half float, as DLSS RR asks).
inline constexpr VkFormat radianceFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
inline constexpr VkFormat linearDepthFormat = VK_FORMAT_R32_SFLOAT;
inline constexpr VkFormat motionVectorFormat = VK_FORMAT_R16G16_SFLOAT;
inline constexpr VkFormat guideColorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;  // albedos, normal + roughness
inline constexpr VkFormat hitDistanceFormat = VK_FORMAT_R16_SFLOAT;
// DLSS's output at output size: one frame, already denoised, so half floats do (display.comp reads it without a
// format qualifier, as it reads the radiance image through the same descriptor in Accumulation)
inline constexpr VkFormat dlssOutputFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

// Must match Material in shaders/raytrace/bindings.glsl (scalar layout). Material (ModelLoader.h) packed for the GPU:
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
    uint32_t normalTwoChannel;  // 1 = the normal texture has X and Y only (BC5), Z is rebuilt
    uint32_t solid;          // 1 = closed volume that refracts, 0 = thin
    glm::vec3 absorption;    // per unit of distance inside a solid (Beer's law), 0 = clear
    glm::mat3x2 baseColorUvTransform;  // KHR_texture_transform per texture, see Material
    glm::mat3x2 metallicRoughnessUvTransform;
    glm::mat3x2 transmissionUvTransform;
    glm::mat3x2 emissiveUvTransform;
    glm::mat3x2 normalUvTransform;
    float diffuseTransmission;              // KHR_materials_diffuse_transmission, see Material
    glm::vec3 diffuseTransmissionColor;
    int32_t diffuseTransmissionTexture;
    int32_t diffuseTransmissionColorTexture;
    glm::mat3x2 diffuseTransmissionUvTransform;
    glm::mat3x2 diffuseTransmissionColorUvTransform;
    int32_t normalVarianceTexture;  // R: the normal map's averaged-away bumps as GGX alpha (Toksvig), see Material
    float fogDensity;               // fog inside, see Material
    glm::vec3 fogColor;
    float fogForward;
    uint32_t invisible;             // 1 = rays pass the surface unchanged
    uint32_t fogNoise;              // varying fog, see Material
    float fogNoiseScale;            // 1 / the noise texture's repeat, in mesh units
    float fogCoverage;
    float fogEdge;
    glm::vec3 fogNoiseOffset;       // from the seed, in texture repeats
    glm::vec3 fogDrift;             // in texture repeats per second
};
static_assert(sizeof(GpuMaterial) == 356, "GpuMaterial must match Material in shaders/raytrace/bindings.glsl");

// The fog noise texture holds this many blobs along each side (Volumes.cpp)
static constexpr float fogNoiseBlobs = 4.0f;

// Fog whose density varies through the volume: noise or an edge fade (see "Varying density" there)
inline bool fogVaries(const Material& material) {
    return material.fogDensity > 0.0f && (material.fogNoise || material.fogEdge > 0.0f);
}

// The first group is drawn by the trace shader (one property of the first hit). The "guide" group shows the
// DLSS input images as written during normal path tracing; the display pass draws them.
enum class DebugView : int {
    None = 0, Normals, TexCoords, MeshIndex, MaterialIndex,
    BaseColor, Metallic, Roughness, Transmission, Emissive, Tangents,
    RadianceCache,  // the cached light of the cell at the first hit (with exposure, like the image)
    RestirM,        // how many candidates each pixel's ReSTIR reservoir stands for (history and neighbors)
    RestirLight,    // which kind of light each pixel's ReSTIR reservoir picked
    GuideDepth, GuideMotionVectors, GuideDiffuseAlbedo, GuideSpecularAlbedo, GuideNormals, GuideRoughness,
    GuideSpecularHitDistance, GuideReprojectionError
};
inline bool isGuideView(int view) { return view >= static_cast<int>(DebugView::GuideDepth); }

// Values edited through ImGui

// Whether surfaces aim a shadow ray at the sky (see Environment.cpp). It costs a ray per bounce. It pays off where the
// sky has bright spots (an HDR map's sun), and for the gradient too wherever the sky is seen through openings
// (interiors): there bounce rays rarely find it, and its light comes as rare, very bright samples (see LIMITATIONS.md).
enum class SkySampling : int {
    Auto = 0,  // HDR maps only
    On = 1,
    Off = 2
};

// How light from glowing (emissive) surfaces is found. All three converge to the same image; they differ in noise.
enum class EmissiveSampling : int {
    BouncesOnly = 0,   // only when a bounce ray happens to hit a glowing surface (what we had before)
    LightSampling = 1, // only by aiming shadow rays at random points on glowing triangles
    Mis = 2            // both, weighted by multiple importance sampling
};

// Must match EmissiveTriangle in shaders/raytrace/bindings.glsl (scalar layout). One per triangle of every visible
// instance with an emissive material, in world space. Shadow rays are aimed at random points on these.
struct EmissiveTriangle {
    glm::vec3 p0, p1, p2;
    glm::vec2 uv0, uv1, uv2;  // to look up the emissive texture at the sampled point
    uint32_t materialIndex;
    float probability;  // chance of picking this triangle: proportional to area * brightness
    float cdf;          // sum of the probabilities up to and including this one (for picking with a binary search)
    float area;
    uint32_t treeTrail; // its way down the light tree: bit d = 1 means "right child" at depth d
};
static_assert(sizeof(EmissiveTriangle) == 80,
              "EmissiveTriangle must match EmissiveTriangle in shaders/raytrace/bindings.glsl");

// How light sampling picks one of the glowing triangles at a surface point
enum class LightPicking : int {
    Power = 0,  // by its share of the scene's light (area * brightness), wherever it is
    Tree = 1    // walking down the light tree: by how much each part of the scene could send to this point
};

// Must match LightTreeNode in shaders/raytrace/bindings.glsl (scalar layout). The light tree: a binary tree over the
// glowing triangles that light sampling can pick (probability > 0), built in Lights.cpp. Node 0 is the root.
struct LightTreeNode {
    glm::vec3 boundsMin;  // box around all its triangles
    float power;          // sum of their probabilities (= their share of the scene's light)
    glm::vec3 boundsMax;
    int32_t child;        // >= 0: the first of its two children (the second follows); < 0: a leaf, triangle -child-1
};
static_assert(sizeof(LightTreeNode) == 32, "LightTreeNode must match LightTreeNode in shaders/raytrace/bindings.glsl");

// Must match PunctualLight in shaders/raytrace/bindings.glsl (scalar layout). One per enabled light of every visible
// object, in world space, rebuilt every frame (see updateScene).
struct GpuPunctualLight {
    glm::vec3 position;
    uint32_t type;           // LightType
    glm::vec3 direction;     // normalized, the way the light shines (spot axis, directional light's rays)
    float range;             // 0 = no limit
    glm::vec3 intensity;     // color * intensity * the UI's scale: candela (point, spot) or lux (directional)
    float cosOuterCone;      // spot: cos of the outer and inner cone angles
    float cosInnerCone;
};
static_assert(sizeof(GpuPunctualLight) == 52,
              "GpuPunctualLight must match PunctualLight in shaders/raytrace/bindings.glsl");

// Must match Portal in shaders/raytrace/portals.glsl (scalar layout). One per end of every enabled portal pair, in
// world space, rebuilt every frame (see updateScene); a pair's ends are entries 2k and 2k + 1.
struct GpuPortal {
    glm::vec3 center;
    float halfWidth;
    glm::vec3 normal;        // the side it faces
    float halfHeight;
    glm::vec3 right;         // along its width
    uint32_t shape;          // PortalShape
    glm::mat4x3 toPartner;   // takes points (and directions) from this end to the other one
};
static_assert(sizeof(GpuPortal) == 96, "GpuPortal must match Portal in shaders/raytrace/portals.glsl");

// A device-local buffer the CPU refills from time to time. The CPU writes what changed into the current frame's part
// of the upload ring (Renderer::uploadRings), then the GPU copies it over.
struct StreamedBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation memory = VK_NULL_HANDLE;
    VkDeviceSize capacity = 0;
    VkBufferUsageFlags usage = 0;      // what the buffer is used as (besides a copy's destination)
    VkPipelineStageFlags readers = 0;  // the stages that read it
};

// One piece of an upload into a StreamedBuffer: `size` bytes from `data` to `offset` in the buffer
struct StreamedPiece {
    const void* data;
    VkDeviceSize offset;
    VkDeviceSize size;
};

// A frame in flight's host-visible staging buffer, shared by all streamed buffers
struct UploadRing {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
};

// The renderer's settings. What is lit and how (sun, sky, map) is the scene's: SceneEnvironment.
struct RenderSettings {
    bool shadowsEnabled = true;

    int emissiveSampling = static_cast<int>(EmissiveSampling::Mis);
    int lightPicking = static_cast<int>(LightPicking::Tree);
    int lightCandidates = 8;  // picking by power: this many glowing triangles, one kept (by distance and angle)
    int skySampling = static_cast<int>(SkySampling::On);  // shadow rays towards the sky (see Environment.cpp)
    bool punctualLights = true;        // the lights that came with the models (KHR_lights_punctual)
    float punctualLightScale = 1.0f;   // multiplies them all

    int samplesPerPixel = 1;
    // Random numbers for the main decisions of a path (camera offset, sun, sky, glowing triangles, bounces), see
    // pathSample in the shader: independent (white noise), Sobol points (a pixel's samples spread evenly), or Sobol
    // points shifted per pixel by a blue-noise texture (also neighboring pixels' samples differ evenly). Blue noise
    // measured no better under DLSS and 22% worse in Bistro at night when accumulating (see LIMITATIONS.md).
    int pathSampling = 1;
    // Points on glowing triangles: spread evenly over the triangle's solid angle as the surface sees it (Arvo 1995),
    // or evenly over its area (then a large panel close by gets most samples in its far parts)
    bool emissiveSolidAngle = true;
    int maxBounces = 8;  // passes through glass don't count (see glassPasses); Russian roulette ends matte paths early
    // Portals one ray may go through: two ends facing each other would pass a ray back and forth forever. Beyond this
    // it is lost (no light: the deep end of such a tunnel is black). Costs only where rays go that deep.
    int maxPortalCrossings = 8;
    bool blurCaustics = true;  // less noise from light reflected by shiny surfaces onto matte ones, slightly biased
    bool textureMipmaps = true;  // read textures at the level that fits each hit (off: always the full size)
    bool normalMapFiltering = true;  // Toksvig: distant bumpy surfaces keep their blurrier reflection
    // The trace shader without the code for portals, lamps, glass, cut-outs and diffuse transmission (FEATURES = 0):
    // ~14% faster, but wrong in scenes that have any of them (see basicTraceShader)
    bool basicShader = false;
    bool radianceCache = true;      // paths stop after a diffuse bounce and read the light from the cache
    float cacheCellPixels = 32.0f;  // cache cells are about this many pixels wide on screen (measured best in Sponza)
    int cacheUpdateTile = 4;        // one cache update path per N x N pixels
    float cacheHistoryFrames = 32.0f;  // old cache samples fade out over about this many frames
    bool cacheFog = true;           // paths also stop at their second scatter point in fog and read the light there
    // ReSTIR DI (LIMITATIONS.md): direct light at the first surface. Set separately for Accumulation (off: the
    // plain path tracer stays the reference) and the DLSS modes (on: less noise and flicker for DLSS to work with).
    bool restirDiAccumulation = false;
    bool restirDiDlss = true;
    bool& restirDi(int mode) {
        return mode == static_cast<int>(RenderMode::Accumulation) ? restirDiAccumulation : restirDiDlss;
    }
    int restirLightCandidates = 4;   // candidate samples of glowing triangles per pixel (of punctual lights: up to this)
    int restirSkyCandidates = 2;     // of the sky, when it is light-sampled
    int restirBrdfCandidates = 1;    // directions picked by the surface's reflection (a ray each)
    float restirMinRoughness = 0.25f;  // smoother surfaces keep the usual light sampling
    bool restirTemporal = true;        // reuse last frame's samples
    bool restirTemporalVisibility = true;   // with shadow rays from last frame's surface (else Sponza ends up 3.7% too dark)
    bool restirPermutation = true;     // take last frame's samples from shuffled pixels (keeps DLSS from painting noise)
    // Last frame's samples count for at most this many times this frame's candidates. 20 (RTXDI's default) made
    // patches flicker under DLSS: a lucky bright sample held for many frames, then lost (8x more flickering pixels
    // in Bistro at night than 5); the noise was the same.
    float restirMaxHistory = 5.0f;
    bool restirSpatial = true;         // reuse neighbors' samples (spatial pass): less ghosting and flicker under DLSS
    int restirSpatialNeighbors = 3;
    float restirSpatialRadius = 30.0f;   // render pixels
    int restirDisocclusionNeighbors = 8;  // for pixels without history (just uncovered)
    bool restirSpatialVisibility = true;  // unbiased: else Sponza ends up 9.7% too dark (neighbors within 30 pixels)
    // ReSTIR GI (LIMITATIONS.md): light reaching the first surface via a second one. Independent of ReSTIR DI,
    // set separately for Accumulation and the DLSS modes as it is. Off in both: under DLSS it barely lowered the noise
    // once unbiased (boiling filter off, temporal visibility ray), for 3-10 ms, and its kept samples flash.
    bool restirGiAccumulation = false;
    bool restirGiDlss = false;
    bool& restirGi(int mode) {
        return mode == static_cast<int>(RenderMode::Accumulation) ? restirGiAccumulation : restirGiDlss;
    }
    float restirGiMinRoughness = 0.0f;  // second surfaces at least this rough give samples (0: all but glass, as RTXDI)
    bool restirGiTemporal = true;       // reuse last frame's samples
    bool restirGiTemporalVisibility = true;  // with a ray from last frame's surface (else Sponza's aisle: 6% too dark)
    bool restirGiPermutation = true;    // from shuffled pixels (as DI; RTXDI's default is off, DLSS RR's guide asks for it)
    float restirGiMaxHistory = 8.0f;    // RTXDI's default; to be set by the flicker test as DI's
    int restirGiMaxAge = 30;            // frames (RTXDI's default)
    bool restirGiSpatial = true;        // reuse neighbors' samples
    int restirGiSpatialNeighbors = 2;   // RTXDI's defaults
    float restirGiSpatialRadius = 32.0f;
    int restirGiDisocclusionNeighbors = 4;  // for pixels without history
    bool restirGiSpatialVisibility = true;   // rays for the kept sample: half the darkening of RTXDI's "basic" (none)
    // Drop reservoirs far brighter than their neighbors' (RTXDI). Off: where light comes as rare bright samples it
    // takes much of it away and flickers (Sponza's side aisle: 9% darker); elsewhere it lowers noise 15-20%.
    bool restirGiBoilingFilter = false;
    float restirGiBoilingStrength = 0.2f;    // RTXDI's default: dropped above 41 x the average
    bool restirGiClampRadiance = false;      // limit a sample's light, set on screen as the bounce light clamp
    float restirGiRadianceClampLevel = 8.0f; // brightness on screen (after exposure, before tone mapping)
    // DLSS modes: light that reached the first surface via another one (bounce light) is limited per sample, so a
    // rare very bright path doesn't stand out for DLSS to smear over many frames. Biased (a little darker). Off: in
    // Bistro at night (DLAA) it changed neither the noise nor how long a still image keeps changing, only made it
    // darker (the street: -2% at 4, -5% at 1). Kept for judging by eye in motion.
    bool clampBounceLight = false;
    float bounceLightClampLevel = 4.0f;  // the limit on screen: brightness after exposure, before tone mapping
    float cacheMinSamples = 4.0f;   // a cell is used once it has this much sample weight
    bool accumulate = true;

    float exposure = 0.6f;          // manual: brightness multiplier
    bool autoExposure = true;       // measure the image and pick the exposure like a camera
    float exposureCompensation = 0.0f;  // auto: stops brighter (+) or darker (-) than measured
    float exposureKey = 0.18f;      // auto: the metered average becomes this (middle grey)
    float meterLowPercent = 40.0f;  // auto: darkest pixels ignored (deep shadows, black background)
    float meterHighPercent = 95.0f; // auto: pixels above this ignored (sun, sky, fireflies)
    bool centerWeighted = false;    // auto: the image center counts up to 4x
    float minExposureStops = -4.0f; // auto: limits of the multiplier, in stops (multiplier 2^stops)
    float maxExposureStops = 10.0f;  // Sponza's courtyard in the default sun wants about +8.5
    float brightenSpeed = 1.5f;     // auto: adaptation rates per second (exponential); like the eye,
    float darkenSpeed = 3.0f;       //       getting used to the dark is slower than to bright light
    bool toneMapping = true;
    // Fog grain (display.comp): film-like grain where the camera sees fog, added after DLSS (which removes the path
    // tracer's own noise). A look only: the traced light is unchanged.
    bool fogGrain = true;
    float fogGrainStrength = 0.25f;  // how far it moves a pixel's brightness, at most (where it's all fog)
    float fogGrainSize = 1.0f;       // output pixels
    int debugView = static_cast<int>(DebugView::None);
    int renderMode = static_cast<int>(RenderMode::Accumulation);  // RenderMode, picked in the UI
    // The image's size (what Capture saves): 0 = the window's, else this many rows (outputHeights), as wide as the
    // window's shape allows. Shown shrunk to fit the window.
    int outputSize = 0;
    int dlssModel = static_cast<int>(DlssModel::TransformerF);     // DlssModel
};

// Device-local memory (Renderer::gpuMemory)
struct GpuMemory {
    VkDeviceSize used = 0;    // by this program
    VkDeviceSize budget = 0;  // what it can have without slowing down (the driver's estimate, less what other programs
                              // use); 0: unknown (no VK_EXT_memory_budget)
    // All heaps: the memory objects VMA got from the driver (its blocks and the resources with memory of their own;
    // Vulkan only promises 4096) and the buffers and images placed in them
    uint32_t memoryObjects = 0;
    uint32_t allocations = 0;
};

// What the driver says about one compiled pipeline (VK_KHR_pipeline_executable_properties): registers, binary size,
// local memory, ... as name and value
struct PipelineStatistics {
    std::string pipeline;
    std::vector<std::pair<std::string, std::string>> values;
};

// What the renderer reports for the UI (Renderer::stats)
struct RendererStats {
    std::string deviceName;
    GpuMemory gpuMemory;
    float gpuPassMs[6] = {};  // GPU time per pass: cache, ReSTIR, trace, DLSS, exposure, display (smoothed)
    float lastGpuPassMs[6] = {};  // the same, of the last frame read back (Configuration::maxFramesInFlight ago)
    uint64_t gpuTimesRead = 0;    // frames whose GPU times were read back so far
    uint64_t triangles = 0;   // over all instances
    size_t instances = 0;
    size_t meshes = 0;
    size_t textures = 0;
    VkDeviceSize textureMemory = 0;
    size_t emissiveTriangles = 0;
    size_t sampledEmissiveTriangles = 0;  // of those, bright enough to be picked
    VkDeviceSize blasBuildSize = 0;       // BLAS memory before / after compaction
    VkDeviceSize blasSize = 0;
    bool accumulating = false;  // Accumulation mode with Accumulate on
    uint32_t accumulatedFrames = 0;
    bool debugView = false;     // a debug view replaces the image
    std::string captureMessage;
    glm::uvec2 environmentMapSize = glm::uvec2(0);  // the scene's HDR map, once it is on the GPU (else 0)
};

#endif //VULKANDEMO_RENDERERTYPES_H
