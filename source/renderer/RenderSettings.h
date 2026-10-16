//
// The renderer's settings and the choices in them (render mode, debug view, light sampling, ...). No Vulkan: the
// program keeps them (Application::renderSettings), the scene saves its part of them (Scene, SceneFile.cpp), the
// program's part is kept between runs (app/UserSettings.h). As JSON: RenderSettingsFile.h, which also says which
// fields are the scene's and which the program's.
//

#ifndef VULKANDEMO_RENDERSETTINGS_H
#define VULKANDEMO_RENDERSETTINGS_H

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

// How light sampling picks one of the glowing triangles at a surface point
enum class LightPicking : int {
    Power = 0,  // by its share of the scene's light (area * brightness), wherever it is
    Tree = 1    // walking down the light tree: by how much each part of the scene could send to this point
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

#endif //VULKANDEMO_RENDERSETTINGS_H
