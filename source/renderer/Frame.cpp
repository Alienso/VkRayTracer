//
// A frame: the uniforms the trace shader gets (camera, lights, settings, accumulation), and the passes recorded in
// order (scene uploads, radiance cache, ReSTIR, trace, DLSS, exposure, display, blit, capture), timed with GPU
// timestamps.
//

#include <vulkan/vulkan.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <glm/gtc/constants.hpp>
#include "renderer/Renderer.h"
#include "core/Configuration.h"

// Sub-pixel camera offsets for DLSS: the Halton sequence (bases 2 and 3) covers the pixel evenly over any run of
// frames. Returns 0..1.
static float halton(uint32_t index, uint32_t base) {
    float fraction = 1.0f;
    float result = 0.0f;
    while (index > 0) {
        fraction /= static_cast<float>(base);
        result += fraction * static_cast<float>(index % base);
        index /= base;
    }
    return result;
}

SceneUniforms Renderer::buildSceneUniforms() {
    const Camera& camera = renderList->camera;
    const SceneEnvironment& environment = renderList->environment;
    float azimuth = glm::radians(environment.sunAzimuth);
    float elevation = glm::radians(environment.sunElevation);
    glm::vec3 towardsSun = {std::cos(elevation) * std::sin(azimuth),
                            std::sin(elevation),
                            std::cos(elevation) * std::cos(azimuth)};

    SceneUniforms uniforms{};  // zero-initialized, so padding compares equal in memcmp below
    float aspectRatio = (float) renderExtent.width / (float) renderExtent.height;
    frameView = camera.viewMatrix();
    frameProjection = camera.projectionMatrix(aspectRatio, Configuration::cameraNear, Configuration::cameraFar);
    frameViewProjection = frameProjection * frameView;
    // The camera went through a portal: last frame it saw this frame's points where they were before going through
    // with it, so the motion vectors (and DLSS's history) carry on as if it had seen them directly
    bool teleported = renderList->cameraTeleport != glm::mat4(1.0f);
    if (teleported && hasPreviousViewProjection) {
        previousViewProjection = previousViewProjection * glm::inverse(renderList->cameraTeleport);
    }
    framePreviousViewProjection = hasPreviousViewProjection ? previousViewProjection : frameViewProjection;
    uniforms.viewProjection = frameViewProjection;
    uniforms.previousViewProjection = framePreviousViewProjection;
    previousViewProjection = frameViewProjection;
    hasPreviousViewProjection = true;
    uniforms.cameraPosition = glm::vec4(camera.position, 1.0f);
    uniforms.cameraForward = glm::vec4(camera.forward(), 0.0f);
    uniforms.cameraRight = glm::vec4(camera.right(), 0.0f);
    uniforms.cameraUp = glm::vec4(camera.up(), 0.0f);
    uniforms.jitter = glm::vec2(0.0f);
    uniforms.tanHalfFov = std::tan(glm::radians(camera.fov) * 0.5f);
    uniforms.aspectRatio = aspectRatio;
    uniforms.sunDirection = glm::vec4(towardsSun, std::cos(glm::radians(environment.sunAngularRadius)));
    // Sun off = sun color 0 (the shader then skips its shadow rays). Black environment = all sky colors 0.
    // An HDR map replaces the gradient (whose colors are then 0) once it is loaded (see updateEnvironment).
    float sun = environment.sunEnabled ? environment.sunIntensity : 0.0f;
    float sky = environment.background == static_cast<int>(Background::Sky) ? environment.skyIntensity : 0.0f;
    uniforms.sunColor = glm::vec4(environment.sunColor * sun, 0.0f);
    // The disc: its total light PI * sunColor (see tracePath) spread evenly over its solid angle. A disc of size 0
    // can only be found by shadow rays.
    float sunSolidAngle = 2.0f * glm::pi<float>() * (1.0f - uniforms.sunDirection.w);
    uniforms.sunOn = glm::any(glm::greaterThan(glm::vec3(uniforms.sunColor), glm::vec3(0.0f))) ? 1 : 0;
    uniforms.sunMis = uniforms.sunOn && sunSolidAngle > 1e-6f && settings.shadowsEnabled ? 1 : 0;
    uniforms.sunPdf = 1.0f / std::max(sunSolidAngle, 1e-6f);
    uniforms.sunRadianceScale = glm::pi<float>() / std::max(sunSolidAngle, 1e-4f);
    uniforms.skyZenithColor = glm::vec4(environment.skyZenithColor * sky, 0.0f);
    uniforms.skyHorizonColor = glm::vec4(environment.skyHorizonColor * sky, 0.0f);
    uniforms.groundColor = glm::vec4(environment.groundColor * sky, 0.0f);
    if (environment.background == static_cast<int>(Background::White)) {
        uniforms.skyZenithColor = uniforms.skyHorizonColor = uniforms.groundColor = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);
    }
    uniforms.environmentMap = scene.environmentMapReady() ? 1 : 0;
    uniforms.environmentSampling = scene.environmentCells.x > 0 ? 1 : 0;
    uniforms.environmentWidth = scene.environmentCells.x;
    uniforms.environmentHeight = scene.environmentCells.y;
    uniforms.environmentRotation = glm::radians(environment.mapRotation);
    uniforms.environmentIntensity = environment.mapIntensity;
    uniforms.punctualLightCount = static_cast<uint32_t>(scene.uploadedPunctualLights.size());
    uniforms.portalCount = static_cast<uint32_t>(scene.uploadedPortals.size());
    fogLevel = 0;
    bool fogDrifts = false;
    for (const SceneMaterial& material : assets.materials()) {
        if (material.material.transmission > 0.0f && material.material.solid) {
            fogLevel = std::max(fogLevel, 1u);  // glass volumes (see FOG in the shader)
        }
        if (fogVaries(material.material)) {
            fogLevel = 2;
            fogDrifts = fogDrifts || (material.material.fogNoise && material.material.fogDrift != glm::vec3(0.0f));
        } else if (material.material.fogDensity > 0.0f || material.material.invisibleSurface) {
            fogLevel = std::max(fogLevel, 1u);
        }
    }
    // Drifting fog changes the image every frame (accumulation restarts); still fog leaves the uniforms alone. The
    // clock stops while nothing drifts, so fog that starts drifting again goes on from where it was.
    if (fogDrifts) {
        fogTime += std::min(frameDeltaTime, 0.25f);  // a long stall (loading a model) shouldn't jump
    }
    uniforms.fogTime = fogDrifts ? fogTime : 0.0f;
    uniforms.samplesPerPixel = static_cast<uint32_t>(std::max(settings.samplesPerPixel, 1));
    uniforms.maxBounces = static_cast<uint32_t>(std::max(settings.maxBounces, 1));
    uniforms.maxPortalCrossings = static_cast<uint32_t>(std::max(settings.maxPortalCrossings, 1));
    uniforms.shadowsEnabled = settings.shadowsEnabled ? 1 : 0;
    uniforms.emissiveTriangleCount = static_cast<uint32_t>(scene.emissiveTriangles.size());
    uniforms.emissiveSampling = static_cast<uint32_t>(settings.emissiveSampling);
    uniforms.lightTree = settings.lightPicking == static_cast<int>(LightPicking::Tree) && !scene.lightTree.empty() ? 1 : 0;
    // Only for picking by power: the tree already weighs distance and angle, candidates only added time there
    uniforms.lightCandidates = uniforms.lightTree != 0
                               ? 1u : static_cast<uint32_t>(std::clamp(settings.lightCandidates, 1, 16));
    uniforms.blurCaustics = settings.blurCaustics ? 1 : 0;
    uniforms.textureMipmaps = settings.textureMipmaps ? 1 : 0;
    uniforms.normalMapFiltering = settings.normalMapFiltering ? 1 : 0;
    // Hit debug views overwrite the radiance image. Guide views are drawn by the display pass from the guide
    // images, while the trace shader path traces as usual.
    uniforms.debugView = isGuideView(settings.debugView) ? 0 : static_cast<uint32_t>(settings.debugView);
    // ReSTIR DI: candidates of each kind of light there is. Debug views trace their own camera ray (except the one
    // showing ReSTIR's reservoirs).
    bool restirView = uniforms.debugView == static_cast<uint32_t>(DebugView::RestirM) ||
                      uniforms.debugView == static_cast<uint32_t>(DebugView::RestirLight);
    // (ReSTIR and the guides only with their buffers at full size, see PerPixelBuffers: always, once
    // applyPerPixelBuffers has seen these settings)
    uniforms.restirDi = settings.restirDi(settings.renderMode) && trace.perPixelBuffers.restir &&
                        (uniforms.debugView == 0 || restirView) ? 1 : 0;
    bool sampleTriangles = !scene.emissiveTriangles.empty() && settings.emissiveSampling != static_cast<int>(EmissiveSampling::BouncesOnly);
    uint32_t lightCandidates = static_cast<uint32_t>(std::max(settings.restirLightCandidates, 1));
    uniforms.restirTriangleCandidates = sampleTriangles ? lightCandidates : 0;
    // A few punctual lights keep a shadow ray each (exact, MAX_PUNCTUAL_LIGHTS_PER_HIT in the shader); ReSTIR is
    // for many
    uniforms.restirPunctualCandidates = uniforms.punctualLightCount > 8 ? std::min(lightCandidates, uniforms.punctualLightCount) : 0;
    // At least one whenever the sky is light-sampled: the bounce ray of a ReSTIR surface leaves the sky to it
    uniforms.restirSkyCandidates = uniforms.environmentSampling ? static_cast<uint32_t>(std::max(settings.restirSkyCandidates, 1)) : 0;
    uniforms.restirBrdfCandidates = static_cast<uint32_t>(std::max(settings.restirBrdfCandidates, 0));
    uniforms.restirMinRoughness = settings.restirMinRoughness;
    uniforms.restirTemporal = settings.restirTemporal ? (settings.restirTemporalVisibility ? 2 : 1) : 0;
    uniforms.restirMaxHistory = settings.restirMaxHistory;
    bool spatial = settings.restirSpatial && uniforms.restirDi != 0;
    uniforms.restirSpatialNeighbors = spatial ? static_cast<uint32_t>(std::max(settings.restirSpatialNeighbors, 0)) : 0;
    uniforms.restirSpatialRadius = settings.restirSpatialRadius;
    uniforms.restirSpatialVisibility = settings.restirSpatialVisibility ? 1 : 0;
    uniforms.restirDisocclusionNeighbors = spatial ? static_cast<uint32_t>(std::max(settings.restirDisocclusionNeighbors, 0)) : 0;
    // ReSTIR GI: independent of DI; the ReSTIR pass runs for either (its camera rays and surfaces)
    uniforms.restirGi = settings.restirGi(settings.renderMode) && trace.perPixelBuffers.restirGi && uniforms.debugView == 0 ? 1 : 0;
    uniforms.restirGiMinRoughness = settings.restirGiMinRoughness;
    uniforms.restirGiTemporal = settings.restirGiTemporal ? 1 : 0;
    uniforms.restirGiMaxHistory = settings.restirGiMaxHistory;
    uniforms.restirGiMaxAge = static_cast<uint32_t>(std::max(settings.restirGiMaxAge, 1));
    uniforms.restirGiPermutation = settings.restirGiPermutation ? 1 : 0;
    uniforms.restirGiTemporalVisibility = settings.restirGiTemporalVisibility ? 1 : 0;
    uniforms.pathSampling = static_cast<uint32_t>(std::clamp(settings.pathSampling, 0, 2));
    uniforms.emissiveSolidAngle = settings.emissiveSolidAngle ? 1 : 0;
    bool giSpatial = settings.restirGiSpatial && uniforms.restirGi != 0;
    uniforms.restirGiSpatialNeighbors = giSpatial ? static_cast<uint32_t>(std::max(settings.restirGiSpatialNeighbors, 0)) : 0;
    uniforms.restirGiSpatialRadius = settings.restirGiSpatialRadius;
    uniforms.restirGiDisocclusionNeighbors = giSpatial ? static_cast<uint32_t>(std::max(settings.restirGiDisocclusionNeighbors, 0)) : 0;
    uniforms.restirGiSpatialVisibility = settings.restirGiSpatialVisibility ? 1 : 0;
    uniforms.restirGiBoilingFilter = settings.restirGiBoilingFilter ? std::max(settings.restirGiBoilingStrength, 1e-3f) : 0.0f;
    // How much of the scene one ray stands for (the start of its ray cone, see uvFootprint in the shader): half an
    // output pixel, vertically (pixels are square). The image ends up sharper than one render pixel: accumulation
    // averages many offsets within each pixel, and DLSS adds detail up to the output resolution (NVIDIA's guide
    // asks for a mip bias of log2(render / output size) - 1, which is this). A whole render pixel looks soft.
    uniforms.pixelSpreadAngle = 0.5f * std::atan(2.0f * uniforms.tanHalfFov / static_cast<float>(outputExtent.height));
    // DLSS mode: every frame on its own (DLSS does the averaging over time), plus the guides it needs
    uniforms.accumulate = settings.accumulate && !dlssActive() ? 1 : 0;
    uniforms.farDistance = Configuration::cameraFar;
    uniforms.writeGuides = (dlssActive() || isGuideView(settings.debugView)) && trace.perPixelBuffers.guides ? 1 : 0;
    uniforms.cacheQuery = settings.radianceCache && !cache.cacheQueriesSuspended ? 1 : 0;
    uniforms.cacheCapacity = cache.cacheCapacity;
    // A cell about cacheCellPixels render pixels wide at distance 1, growing with the distance. Pixels as rendered
    // for the window's size: a bigger output (a 4K capture) keeps the same cells, else it would need many more.
    float windowRenderRows = static_cast<float>(renderExtent.height) * static_cast<float>(swapChainExtent.height)
                             / static_cast<float>(std::max(outputExtent.height, 1u));
    uniforms.cacheCellScale = 2.0f * uniforms.tanHalfFov / std::max(windowRenderRows, 1.0f) * settings.cacheCellPixels;
    uniforms.cacheMinWeight = settings.cacheMinSamples;
    uniforms.cacheUpdateTile = static_cast<uint32_t>(std::max(settings.cacheUpdateTile, 1));
    uniforms.cacheFog = settings.cacheFog ? 1 : 0;

    // The cache holds light that is no longer right after the lighting or the cells change: start it over.
    // (Moving objects and edited materials are followed within its history instead; see cacheHistoryFrames.)
    bool lightingChanged =
            std::memcmp(&uniforms.sunDirection, &previousUniforms.sunDirection,
                        offsetof(SceneUniforms, samplesPerPixel) - offsetof(SceneUniforms, sunDirection)) != 0 ||
            uniforms.maxBounces != previousUniforms.maxBounces || uniforms.shadowsEnabled != previousUniforms.shadowsEnabled ||
            uniforms.maxPortalCrossings != previousUniforms.maxPortalCrossings ||
            uniforms.emissiveSampling != previousUniforms.emissiveSampling ||
            uniforms.blurCaustics != previousUniforms.blurCaustics ||
            uniforms.cacheCellScale != previousUniforms.cacheCellScale || uniforms.cacheFog != previousUniforms.cacheFog;
    if (lightingChanged) {
        cache.cacheClearPending = true;
    }

    // Anything that changes the image (camera moved, slider dragged, ...) restarts accumulation.
    // frameIndex and accumulatedFrames change every frame by design, so those are filled in after comparing.
    // (Exposure and tone mapping aren't in here: they only affect the display pass, see recordDisplay.)
    if (resetAccumulation || std::memcmp(&uniforms, &previousUniforms, sizeof(SceneUniforms)) != 0) {
        accumulatedFrames = 0;
        accumulationRestarts++;
        resetAccumulation = false;
    }
    previousUniforms = uniforms;

    uniforms.frameIndex = frameIndex;
    uniforms.accumulatedFrames = accumulatedFrames;
    // ReSTIR history (after the comparison above: these change every frame). Last frame's reservoirs are usable if
    // ReSTIR ran last frame, at this size (createRayTracingImages), and no light was added or removed: their samples
    // name lights by index in lists that are then rebuilt.
    // (Nor after the camera went through a portal: pixels seen through one have no reservoirs, and their surfaces
    // are now seen directly.)
    uniforms.restirHistoryValid = trace.restirHistoryAvailable && !trace.restirLightsChanged && !teleported ? 1 : 0;
    uniforms.restirCurrentHalf = trace.restirCurrentHalf;
    // Shuffle of last frame's pixels (see restirTemporal): on (bit 4) and this frame's random offset (bits 0-3)
    uint32_t permutationHash = frameIndex * 0x9E3779B9u;
    permutationHash = (permutationHash ^ (permutationHash >> 16)) * 0x85EBCA6Bu;
    permutationHash ^= permutationHash >> 13;
    uniforms.restirPermutation = (settings.restirPermutation ? 16u : 0u) | (permutationHash & 15u);
    // GI samples carry the light they saw (Lo): after the lighting changed (as for the radiance cache, above) it is
    // out of date, so drop them, as when lights are added or removed. Lights that move rely on the age limit.
    uniforms.restirGiHistoryValid =
            trace.restirGiHistoryAvailable && !trace.restirLightsChanged && !lightingChanged && !teleported ? 1 : 0;
    // Bounce light clamp (DLSS modes, see clampBounceLight in the shader). The limit is set on screen, so it is
    // divided by the exposure: the GPU's from a few frames ago (exposureInfo), or the manual one. After the
    // comparison above: it changes every frame with the auto exposure.
    float shownExposure = settings.autoExposure
                          ? (exposure.exposureInfoValid ? exposure.exposureInfo.exposure * std::exp2(settings.exposureCompensation) : 0.0f)
                          : settings.exposure;
    uniforms.bounceLightClamp = settings.clampBounceLight && dlssActive() && shownExposure > 0.0f
                                ? settings.bounceLightClampLevel / shownExposure : 0.0f;
    // ReSTIR GI's Lo clamp, the same way (any mode)
    uniforms.restirGiRadianceClamp = settings.restirGiClampRadiance && shownExposure > 0.0f
                                     ? settings.restirGiRadianceClampLevel / shownExposure : 0.0f;
    bool restirPass = uniforms.restirDi != 0 || uniforms.restirGi != 0;
    if (restirPass) {
        trace.restirCurrentHalf = 1 - trace.restirCurrentHalf;  // this frame's half is last frame's half next frame
    }
    trace.restirHistoryAvailable = restirPass;
    trace.restirGiHistoryAvailable = uniforms.restirGi != 0;
    // Jitter (DLSS mode): a different sub-pixel offset of the whole image every frame, so DLSS sees every part of
    // each pixel over a few frames. In pixels, -0.5..0.5. DLSS SR's guide asks for 8 phases per output pixel (more
    // when each render pixel covers several output pixels); RR's for "at least 32", as it has no reason to limit them.
    frameJitter = glm::vec2(0.0f);
    if (dlssActive()) {
        float scale = static_cast<float>(outputExtent.width) / static_cast<float>(renderExtent.width);
        uint32_t sequenceLength = std::max(32u, static_cast<uint32_t>(std::lround(8.0f * scale * scale)));
        jitterIndex = jitterIndex % sequenceLength + 1;
        frameJitter = glm::vec2(halton(jitterIndex, 2), halton(jitterIndex, 3)) - 0.5f;
    }
    uniforms.jitter = frameJitter;
    if (settings.accumulate && !dlssActive()) {
        accumulatedFrames++;
    }
    return uniforms;
}

// Trace into the radiance image (render size), DLSS (DLSS modes), measure the exposure, tone map into the display
// image (output size), then blit that into the swapchain image. Afterwards the swapchain image is in TRANSFER_DST_OPTIMAL,
// which is what the render pass expects.
void Renderer::recordFrame(VkCommandBuffer commandBuffer, uint32_t imageIndex) {
    VkImageSubresourceRange colorRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    readTimestamps();
    selection.readPick();  // (this slot's fence was waited: a pick it traced last time is in)
    vkCmdResetQueryPool(commandBuffer, timestampQueryPool, currentFrame * timestampsPerFrame, timestampsPerFrame);

    // Lights uploaded this frame may have new indices: ReSTIR can't reuse last frame's samples then. Glowing
    // triangles only when their indices changed (lightIndicesChanged): moved ones keep theirs, and a reused sample is
    // evaluated where its triangle is now.
    trace.restirLightsChanged = scene.lightIndicesChanged || scene.environmentUploadPending || scene.punctualLightsUploadPending;
    scene.lightIndicesChanged = false;

    // Objects moved (or the scene changed): rebuild the TLAS first, then the other uploads. May set resetAccumulation,
    // so it has to happen before the uniforms are built.
    scene.recordUploads(commandBuffer);
    applySceneChanges();

    // 0: upload this frame's uniforms. vkCmdUpdateBuffer copies the data into the command buffer
    //    itself, so a single uniform buffer is enough even with several frames in flight.
    //    It must not overwrite the buffer while the previous frame's shader still reads it.
    VkBufferMemoryBarrier uniformBarrier{};
    uniformBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    uniformBarrier.srcAccessMask = VK_ACCESS_UNIFORM_READ_BIT;
    uniformBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    uniformBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    uniformBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    uniformBarrier.buffer = trace.uniformBuffer;
    uniformBarrier.offset = 0;
    uniformBarrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 1, &uniformBarrier, 0, nullptr);

    SceneUniforms uniforms = buildSceneUniforms();
    vkCmdUpdateBuffer(commandBuffer, trace.uniformBuffer, 0, sizeof(uniforms), &uniforms);
    frameIndex++;
    scene.recordInstanceUpload(commandBuffer);

    uniformBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    uniformBarrier.dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT;

    // 1: make the output images writable. All images the trace shader writes stay in GENERAL; the previous
    //    frame's reads and writes of them (trace, exposure, display, the history copy) must finish first.
    VkMemoryBarrier shaderImagesBarrier{};
    shaderImagesBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    shaderImagesBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    shaderImagesBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

    // Written by the display pass later in this frame. The previous frame may still be blitting from it,
    // so wait for transfers. Its old contents are not needed, hence UNDEFINED.
    VkImageMemoryBarrier preBarriers[1]{};
    preBarriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    preBarriers[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    preBarriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    preBarriers[0].srcAccessMask = 0;
    preBarriers[0].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    preBarriers[0].image = display.displayImage;

    for (auto& barrier : preBarriers) {
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.subresourceRange = colorRange;
    }
    // (the uniform barrier rides along: uniform upload -> shader read; the motion upload has its own)
    vkCmdPipelineBarrier(commandBuffer,
                         VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &shaderImagesBarrier, 1, &uniformBarrier, 1, preBarriers);

    // 2: radiance cache: update paths fill it, then the new samples are averaged in
    writeTimestamp(commandBuffer, 0);
    cache.recordRadianceCache(commandBuffer);
    writeTimestamp(commandBuffer, 1);

    // 3: ReSTIR: the camera rays first (their hits go to the primary hit buffer), and (DI) a light sample per pixel
    trace.recordRestir(commandBuffer, uniforms);
    writeTimestamp(commandBuffer, 2);

    // 4: trace (and ReSTIR GI's passes after it)
    trace.recordImagePaths(commandBuffer, uniforms);
    writeTimestamp(commandBuffer, 3);

    // 5: DLSS mode: denoise + upscale the noisy frame into the DLSS output image
    dlss.recordDlss(commandBuffer);
    writeTimestamp(commandBuffer, 4);

    // 6: measure the image for the exposure, then exposure + tone mapping into the display image
    exposure.recordExposure(commandBuffer);
    noise.recordNoise(commandBuffer);  // noise measurement, if one is running (not timed: part of "exposure")
    writeTimestamp(commandBuffer, 5);
    selection.record(commandBuffer, renderList->camera, frameViewProjection);  // the editor's pick and outline
    display.recordDisplay(commandBuffer);
    writeTimestamp(commandBuffer, 6);
    timestampsWritten[currentFrame] = true;

    // 7: display image -> blit source, swapchain image -> blit destination.
    //    The swapchain image only becomes available at the TRANSFER stage (see the semaphore wait in drawFrame).
    VkImageMemoryBarrier blitBarriers[2]{};
    blitBarriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    blitBarriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    blitBarriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    blitBarriers[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    blitBarriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    blitBarriers[0].image = display.displayImage;

    blitBarriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    blitBarriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    blitBarriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    blitBarriers[1].srcAccessMask = 0;
    blitBarriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    blitBarriers[1].image = swapChainImages[imageIndex];

    for (auto& barrier : blitBarriers) {
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.subresourceRange = colorRange;
    }
    vkCmdPipelineBarrier(commandBuffer,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 2, blitBarriers);

    // 8: blit: a copy plus format conversion, or shrunk to fit the window (output bigger than it, filtered)
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {(int32_t) outputExtent.width, (int32_t) outputExtent.height, 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[1] = {(int32_t) swapChainExtent.width, (int32_t) swapChainExtent.height, 1};
    vkCmdBlitImage(commandBuffer,
                   display.displayImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   swapChainImages[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &blit, outputExtent.width == swapChainExtent.width && outputExtent.height == swapChainExtent.height
                             ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);

    // 9: the "Capture" button: the display image (without the UI) into a buffer for a PNG
    capture.recordCapture(commandBuffer);
}

void Renderer::createTimestampQueries() {
    VkQueryPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = timestampsPerFrame * Configuration::maxFramesInFlight;
    if (vkCreateQueryPool(gpu.device, &info, nullptr, &timestampQueryPool) != VK_SUCCESS) {
        throw std::runtime_error("failed to create timestamp query pool!");
    }
    timestampsWritten.assign(Configuration::maxFramesInFlight, false);
}

// The current frame slot's fence has been waited on: the timestamps it wrote last time are available.
void Renderer::readTimestamps() {
    if (!timestampsWritten[currentFrame]) {
        return;
    }
    uint64_t ticks[timestampsPerFrame];
    if (vkGetQueryPoolResults(gpu.device, timestampQueryPool, currentFrame * timestampsPerFrame, timestampsPerFrame,
                              sizeof(ticks), ticks, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) {
        return;
    }
    float msPerTick = gpu.physicalDeviceProperties.limits.timestampPeriod * 1e-6f;  // the period is in nanoseconds
    for (uint32_t i = 0; i + 1 < timestampsPerFrame; i++) {
        float ms = static_cast<float>(ticks[i + 1] - ticks[i]) * msPerTick;
        lastPassTimes[i] = ms;
        gpuPassTimes[i] = gpuPassTimes[i] == 0.0f ? ms : gpuPassTimes[i] * 0.95f + ms * 0.05f;
    }
    timestampFramesRead++;
}

// Bottom of pipe: the moment everything recorded before has finished
void Renderer::writeTimestamp(VkCommandBuffer commandBuffer, uint32_t index) {
    vkCmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestampQueryPool,
                        currentFrame * timestampsPerFrame + index);
}

void Renderer::cleanupRayTracing() {
    trace.destroyTracePipelines();
    cache.destroyRadianceCache();
    vkDestroyPipelineLayout(gpu.device, trace.rtPipelineLayout, nullptr);
    vkDestroyDescriptorPool(gpu.device, trace.rtDescriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(gpu.device, trace.rtDescriptorSetLayout, nullptr);

    scene.destroy();
    trace.destroySpecularAlbedoTable();
    trace.destroyBlueNoiseTexture();
    exposure.destroyExposureResources();
    noise.destroyNoiseResources();
    display.destroyDisplayResources();
    selection.destroyResources();
    vkDestroyQueryPool(gpu.device, timestampQueryPool, nullptr);

    vkDestroyBuffer(gpu.device, trace.uniformBuffer, nullptr);
    vmaFreeMemory(gpu.allocator, trace.uniformBufferMemory);
}
