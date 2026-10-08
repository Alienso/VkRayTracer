//
// The environment: what rays see when they leave the scene (sky gradient, HDR map, black or white), and light
// sampling for it.
//
// Before, the sky was only found by bounce rays that happened to leave the scene. Outdoors that works: half of all
// bounces go up. Indoors only the few that happen to fly out through a window or an arcade do, and those carry
// all the sky light: noise. Now every surface also aims a shadow ray at the sky, like at the sun and the glowing
// triangles, in a direction picked in proportion to the sky's brightness (an HDR map's sun or bright clouds get
// most samples). Bounce rays still find the sky as well; MIS weights the two (see tracePath in the shader).
//
// Picking a direction in proportion to brightness: the sphere of directions is cut into cells, rows of equal
// angle from straight up to straight down, each row cut into equal columns (for a map: its pixels, a few at a
// time). Each cell gets the share brightness * solid angle of the total. The shader picks a row from the rows'
// running sums (CDF), then a column from that row's running sums, then a uniformly random direction inside the
// cell. Its pdf is then the cell's share / the cell's solid angle.
//

#include <vulkan/vulkan.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <glm/gtc/constants.hpp>
#include <imgui/imgui.h>
#include <stb_image.h>
#include "renderer/Renderer.h"

static const VkFormat environmentFormat = VK_FORMAT_R32G32B32A32_SFLOAT;  // half floats would clip a bright sun
static const uint32_t maxEnvironmentColumns = 1024;  // a 4k map is sampled in cells of 4 x 4 pixels
static const uint32_t gradientRows = 512;            // the gradient only changes with the height

static float luminance(glm::vec3 color) {
    return glm::dot(color, glm::vec3(0.2126f, 0.7152f, 0.0722f));
}

static float smoothstep(float edge0, float edge1, float x) {
    float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Must match environmentLight in shaders/raytrace.comp (the gradient part). y: the direction's height.
static glm::vec3 gradientSky(const SceneEnvironment& environment, float y) {
    float skyT = std::pow(smoothstep(0.0f, 0.4f, y), 0.35f);
    glm::vec3 sky = glm::mix(environment.skyHorizonColor, environment.skyZenithColor, skyT);
    float groundToSky = smoothstep(-0.01f, 0.0f, y);
    return glm::mix(environment.groundColor, sky, groundToSky);
}

// brightness: columns x rows cells, row 0 straight up. Writes the row CDF (rows entries), then per row its column
// CDF (columns entries each), as the shader expects. False if everything is black (nothing to sample).
static bool buildDistribution(const std::vector<float>& brightness, uint32_t columns, uint32_t rows,
                              std::vector<float>& distribution) {
    distribution.assign(rows + rows * columns, 0.0f);
    std::vector<double> rowWeights(rows);
    double total = 0.0;
    for (uint32_t r = 0; r < rows; r++) {
        // Rows near the poles cover less of the sphere: cells are weighted by their solid angle
        double cos0 = std::cos(glm::pi<double>() * r / rows);
        double cos1 = std::cos(glm::pi<double>() * (r + 1) / rows);
        double solidAngle = 2.0 * glm::pi<double>() / columns * (cos0 - cos1);
        float* rowCdf = &distribution[rows + r * columns];
        double sum = 0.0;
        for (uint32_t c = 0; c < columns; c++) {
            sum += brightness[r * columns + c] * solidAngle;
            rowCdf[c] = static_cast<float>(sum);
        }
        for (uint32_t c = 0; c < columns; c++) {
            // A black row is never picked; give it a valid CDF anyway
            rowCdf[c] = sum > 0.0 ? static_cast<float>(rowCdf[c] / sum) : static_cast<float>(c + 1) / columns;
        }
        rowCdf[columns - 1] = 1.0f;  // no rounding gap at the end
        rowWeights[r] = sum;
        total += sum;
    }
    if (!(total > 0.0)) {
        return false;
    }
    double running = 0.0;
    for (uint32_t r = 0; r < rows; r++) {
        running += rowWeights[r];
        distribution[r] = static_cast<float>(running / total);
    }
    distribution[rows - 1] = 1.0f;
    return true;
}

// environmentTexture <- a new RGBA32F image of these pixels (sampler excluded: environmentSampler)
void Renderer::uploadEnvironmentImage(uint32_t width, uint32_t height, const float* rgba) {
    createImage(width, height, environmentFormat, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                environmentTexture.image, environmentTexture.memory, environmentTexture.view);
    VkDeviceSize size = static_cast<VkDeviceSize>(width) * height * 4 * sizeof(float);
    VkBuffer stagingBuffer;
    VkDeviceMemory stagingMemory;
    createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stagingBuffer, stagingMemory);
    void* mapped;
    vkMapMemory(device, stagingMemory, 0, size, 0, &mapped);
    std::memcpy(mapped, rgba, size);
    vkUnmapMemory(device, stagingMemory);
    immediateSubmit([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = environmentTexture.image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {width, height, 1};
        vkCmdCopyBufferToImage(cmd, stagingBuffer, environmentTexture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
    });
    vkDestroyBuffer(device, stagingBuffer, nullptr);
    vkFreeMemory(device, stagingMemory, nullptr);
}

void Renderer::destroyEnvironmentImage() {
    vkDestroyImageView(device, environmentTexture.view, nullptr);
    vkDestroyImage(device, environmentTexture.image, nullptr);
    vkFreeMemory(device, environmentTexture.memory, nullptr);
    environmentTexture = {};
}

void Renderer::createEnvironment() {
    // Wraps around horizontally, not over the poles. Linear filtering of 32-bit floats is optional in Vulkan.
    VkFormatProperties properties;
    vkGetPhysicalDeviceFormatProperties(physicalDevice, environmentFormat, &properties);
    bool linear = (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    samplerInfo.minFilter = samplerInfo.magFilter;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0.0f;
    if (vkCreateSampler(device, &samplerInfo, nullptr, &environmentSampler) != VK_SUCCESS) {
        throw std::runtime_error("failed to create environment sampler!");
    }

    // The descriptors always need an image and a buffer, so start with a black pixel and a small buffer
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    uploadEnvironmentImage(1, 1, black);
    createStreamedBuffer(environmentDistributionBuffer, 16 * 1024 * sizeof(float));  // grows for a map
}

void Renderer::destroyEnvironment() {
    destroyEnvironmentImage();
    vkDestroySampler(device, environmentSampler, nullptr);
    destroyStreamedBuffer(environmentDistributionBuffer);
}

// The scene's map (from the asset store) into environmentTexture, and its brightness per cell. Once per map: the
// pixels are freed on the CPU afterwards.
void Renderer::uploadEnvironmentMap(EnvironmentMap& map) {
    uploadedEnvironmentId = map.id;
    environmentMapSize = glm::uvec2(0);
    environmentMapLuminance.clear();
    if (map.pixels.empty()) {
        return;  // nothing to show (can't happen: each map is uploaded once)
    }
    const float* pixels = map.pixels.data();
    uint32_t width = map.width;
    uint32_t height = map.height;

    // Brightness per cell of cellSize x cellSize pixels: the average, so a cell's share of the light is right
    uint32_t cellSize = (width + maxEnvironmentColumns - 1) / maxEnvironmentColumns;
    environmentMapCells = {(width + cellSize - 1) / cellSize, (height + cellSize - 1) / cellSize};
    environmentMapLuminance.assign(static_cast<size_t>(environmentMapCells.x) * environmentMapCells.y, 0.0f);
    std::vector<uint32_t> counts(environmentMapLuminance.size(), 0);
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            const float* p = &pixels[(static_cast<size_t>(y) * width + x) * 4];
            size_t cell = static_cast<size_t>(y / cellSize) * environmentMapCells.x + x / cellSize;
            environmentMapLuminance[cell] += luminance({p[0], p[1], p[2]});
            counts[cell]++;
        }
    }
    for (size_t i = 0; i < environmentMapLuminance.size(); i++) {
        environmentMapLuminance[i] /= static_cast<float>(std::max(counts[i], 1u));
    }

    // Replace the image. Rare (picked in the UI), so simply wait for the GPU.
    vkp.deviceWaitIdle(device);
    destroyEnvironmentImage();
    uploadEnvironmentImage(width, height, pixels);
    map.pixels.clear();
    map.pixels.shrink_to_fit();  // the GPU has its copy now

    environmentMapSize = {width, height};
    if (rtDescriptorSet != VK_NULL_HANDLE) {
        updateRayTracingDescriptorSet();
    }
    cacheClearPending = true;  // the cached light came from the old sky
    resetAccumulation = true;  // the uniforms may not change (same size, same settings)
}

// The scene shows a map and it is the one on the GPU
bool Renderer::environmentMapReady() const {
    const SceneEnvironment& environment = renderList->environment;
    return environment.background == static_cast<int>(Background::Map) && assets.isLoaded(environment.map) &&
           assets.environmentMap(environment.map).id == uploadedEnvironmentId && environmentMapSize.x > 0;
}

// Called every frame before drawFrame (after updateScene: it reads the render list's environment): uploads the
// scene's map when it is a new one, and rebuilds the sampling table when what it depends on changed (not for
// intensity or rotation: the table is relative and in the map's own directions).
void Renderer::updateEnvironment() {
    const SceneEnvironment& environment = renderList->environment;
    Background mode = static_cast<Background>(environment.background);
    if (mode == Background::Map && assets.isLoaded(environment.map)) {
        EnvironmentMap& map = assets.environmentMap(environment.map);
        if (map.id != uploadedEnvironmentId) {
            uploadEnvironmentMap(map);
        }
    }
    bool mapReady = environmentMapReady();

    std::vector<float> key = {static_cast<float>(environment.background), static_cast<float>(uploadedEnvironmentId),
                              mapReady ? 1.0f : 0.0f, static_cast<float>(settings.skySampling),
                              environment.skyIntensity > 0.0f ? 1.0f : 0.0f, environment.mapIntensity > 0.0f ? 1.0f : 0.0f};
    for (const glm::vec3& color : {environment.skyZenithColor, environment.skyHorizonColor, environment.groundColor}) {
        key.insert(key.end(), {color.r, color.g, color.b});
    }
    if (key == environmentDistributionKey) {
        return;
    }
    environmentDistributionKey = key;

    // Nothing to sample: a black sky, a map that isn't there, or sampling switched off (bounce rays only). Auto
    // samples maps only: measured, the gradient sky gained little (about 1.2x less noise per unit of time in the
    // Bistro interior, even in Sponza) for the extra ray per bounce, while a map's sun gained 3x to 150x.
    environmentCells = glm::uvec2(0);
    auto skySampling = static_cast<SkySampling>(settings.skySampling);
    bool wanted = skySampling == SkySampling::On || (skySampling == SkySampling::Auto && mode == Background::Map);
    bool sample = wanted && ((mode == Background::Sky && environment.skyIntensity > 0.0f) ||
                             (mapReady && environment.mapIntensity > 0.0f) ||
                             mode == Background::White);
    if (!sample) {
        return;
    }

    glm::uvec2 cells;
    std::vector<float> brightness;
    if (mode == Background::Map) {
        cells = environmentMapCells;
        brightness = environmentMapLuminance;
    } else {
        // The gradient (and white) only change with the height: one column. Each row gets the average over its
        // height range (in cos, i.e. evenly over its solid angle).
        cells = {1, gradientRows};
        brightness.resize(gradientRows);
        for (uint32_t r = 0; r < gradientRows; r++) {
            float cos0 = std::cos(glm::pi<float>() * r / gradientRows);
            float cos1 = std::cos(glm::pi<float>() * (r + 1) / gradientRows);
            const int steps = 16;
            float sum = 0.0f;
            for (int s = 0; s < steps; s++) {
                float y = cos0 + (cos1 - cos0) * (s + 0.5f) / steps;
                sum += mode == Background::White ? 1.0f : luminance(gradientSky(environment, y));
            }
            brightness[r] = sum / steps;
        }
    }
    if (!buildDistribution(brightness, cells.x, cells.y, environmentDistribution)) {
        return;  // all black
    }
    environmentCells = cells;

    VkDeviceSize size = sizeof(float) * environmentDistribution.size();
    if (size > environmentDistributionBuffer.capacity) {
        vkp.deviceWaitIdle(device);  // the buffer is replaced, no frame may use it
        ensureStreamedCapacity(environmentDistributionBuffer, size);
        if (rtDescriptorSet != VK_NULL_HANDLE) {
            updateRayTracingDescriptorSet();
        }
    }
    environmentUploadPending = true;
}

void Renderer::recordEnvironmentUpload(VkCommandBuffer commandBuffer) {
    if (environmentCells.x > 0) {
        recordStreamedUpload(commandBuffer, environmentDistributionBuffer, environmentDistribution.data(),
                             sizeof(float) * environmentDistribution.size());
    }
    environmentUploadPending = false;
}

// How the renderer samples the sky (part of the editor's Lighting tab, shown unless the background is black)
void Renderer::buildSkySamplingUi() {
    ImGui::Combo("Sample the sky", &settings.skySampling, "Auto (HDR maps only)\0On\0Off\0");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("On: every surface aims a shadow ray at the sky (bright parts more often), combined with\n"
                          "the bounce rays (MIS). Off: the sky is only found by bounce rays that happen to leave\n"
                          "the scene. Both give the same image. Sampling costs a ray per bounce: it pays off for an\n"
                          "HDR map with a sun (much less noise), hardly for the smooth gradient sky. Auto: maps only.");
    }
}
