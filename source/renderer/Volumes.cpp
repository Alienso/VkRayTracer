//
// The fog's 3D textures (see "Varying density" in shaders/raytrace/fog.glsl), one bindless array of sampler3D
// (binding 32):
//
// 0: the fog noise. 128^3 texels that tile, made once at startup: Perlin-Worley noise as in game cloud renderers
//    (Schneider & Vos 2015): smooth Perlin noise, eroded into billows by inverted Worley (cell) noise. Its values are
//    then spread evenly over 0..1 (by rank), so a coverage of c leaves fog in about the share c of the volume, and
//    the density's average follows the slider in a straight line.
// 1..: one signed distance field per mesh with varying fog: over its bounding box, the distance to the mesh's surface,
//    negative inside. The fog fades out towards the surface by it, and is 0 outside (shadow rays track a volume along
//    its bounding box, see varyingTransmittance there). Baked on the CPU the first time a mesh needs one (a material
//    becomes varying fog), and kept by mesh id like the BLASes.
//
// Both are 16 bits per texel: hardware filtering of 8-bit texels shows as steps in smooth gradients close up.
//

#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <glm/gtc/packing.hpp>
#include "renderer/Renderer.h"

namespace {

// Runs body(begin, end) on parts of [0, count) on all cores
template <typename Body>
void parallelFor(uint32_t count, Body body) {
    uint32_t threads = std::max(1u, std::min(std::thread::hardware_concurrency(), count));
    std::vector<std::thread> workers;
    for (uint32_t t = 0; t < threads; t++) {
        uint32_t begin = count * t / threads, end = count * (t + 1) / threads;
        workers.emplace_back([=] { body(begin, end); });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
}

uint32_t hash(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

uint32_t hashCell(glm::ivec3 cell, uint32_t salt) {
    return hash(static_cast<uint32_t>(cell.x) + hash(static_cast<uint32_t>(cell.y) + hash(static_cast<uint32_t>(cell.z) + salt)));
}

float toUnit(uint32_t h) {
    return static_cast<float>(h >> 8) / 16777216.0f;
}

glm::ivec3 wrap(glm::ivec3 cell, int period) {
    return ((cell % period) + period) % period;
}

// Perlin gradient noise at p (in cells), repeating every `period` cells: about -1..1
float perlin(glm::vec3 p, int period, uint32_t salt) {
    glm::ivec3 base = glm::ivec3(glm::floor(p));
    glm::vec3 f = p - glm::vec3(base);
    glm::vec3 fade = f * f * f * (f * (f * 6.0f - 15.0f) + 10.0f);
    float corners[8];
    for (int i = 0; i < 8; i++) {
        glm::ivec3 offset(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        uint32_t h = hashCell(wrap(base + offset, period), salt);
        // One of 12 edge directions of a cube, as in improved Perlin noise
        static const glm::vec3 gradients[12] = {{1, 1, 0}, {-1, 1, 0}, {1, -1, 0}, {-1, -1, 0}, {1, 0, 1}, {-1, 0, 1},
                                                {1, 0, -1}, {-1, 0, -1}, {0, 1, 1}, {0, -1, 1}, {0, 1, -1}, {0, -1, -1}};
        corners[i] = glm::dot(gradients[h % 12], f - glm::vec3(offset));
    }
    float x00 = glm::mix(corners[0], corners[1], fade.x), x10 = glm::mix(corners[2], corners[3], fade.x);
    float x01 = glm::mix(corners[4], corners[5], fade.x), x11 = glm::mix(corners[6], corners[7], fade.x);
    return glm::mix(glm::mix(x00, x10, fade.y), glm::mix(x01, x11, fade.y), fade.z);
}

// Inverted Worley noise: 1 at a cell's random point, falling off to 0 about halfway to the next; repeats every
// `period` cells
float worley(glm::vec3 p, int period, uint32_t salt) {
    glm::ivec3 base = glm::ivec3(glm::floor(p));
    float nearest = 1e9f;
    for (int z = -1; z <= 1; z++) {
        for (int y = -1; y <= 1; y++) {
            for (int x = -1; x <= 1; x++) {
                glm::ivec3 cell = base + glm::ivec3(x, y, z);
                uint32_t h = hashCell(wrap(cell, period), salt);
                glm::vec3 point = glm::vec3(cell) + glm::vec3(toUnit(h), toUnit(hash(h)), toUnit(hash(h + 1)));
                glm::vec3 d = point - p;
                nearest = std::min(nearest, glm::dot(d, d));
            }
        }
    }
    return std::max(0.0f, 1.0f - std::sqrt(nearest));
}

// The noise texture's texels (size^3), 16-bit, spread evenly over 0..1
std::vector<uint16_t> makeFogNoise(uint32_t size) {
    const int blobs = static_cast<int>(fogNoiseBlobs);  // the lowest frequency: this many cells across
    std::vector<float> values(static_cast<size_t>(size) * size * size);
    parallelFor(size, [&](uint32_t zBegin, uint32_t zEnd) {
        for (uint32_t z = zBegin; z < zEnd; z++) {
            for (uint32_t y = 0; y < size; y++) {
                for (uint32_t x = 0; x < size; x++) {
                    glm::vec3 p = (glm::vec3(x, y, z) + 0.5f) / static_cast<float>(size);  // 0..1 across the tile
                    float smooth = 0.0f, amplitude = 0.5f;
                    for (int octave = 0; octave < 4; octave++) {
                        int period = blobs << octave;
                        smooth += amplitude * perlin(p * static_cast<float>(period), period, 17u + octave);
                        amplitude *= 0.5f;
                    }
                    smooth = glm::clamp(smooth * 0.9f + 0.5f, 0.0f, 1.0f);
                    float cells = 0.625f * worley(p * static_cast<float>(blobs), blobs, 101u)
                                  + 0.25f * worley(p * static_cast<float>(2 * blobs), 2 * blobs, 102u)
                                  + 0.125f * worley(p * static_cast<float>(4 * blobs), 4 * blobs, 103u);
                    // Perlin-Worley: the smooth noise, remapped from (cells - 1)..1 to 0..1: where the cells are weak
                    // the smooth noise thins out, which gives the billowy shapes
                    float low = cells - 1.0f;
                    values[(static_cast<size_t>(z) * size + y) * size + x] = (smooth - low) / (1.0f - low);
                }
            }
        }
    });
    // Each value becomes its rank among all, as 0..1 (through a fine histogram)
    const uint32_t bins = 1 << 16;
    std::vector<uint32_t> histogram(bins + 1, 0);
    auto binOf = [&](float v) { return std::min(static_cast<uint32_t>(glm::clamp(v, 0.0f, 1.0f) * bins), bins - 1); };
    for (float v : values) {
        histogram[binOf(v) + 1]++;
    }
    for (uint32_t i = 1; i <= bins; i++) {
        histogram[i] += histogram[i - 1];  // now: how many values lie below bin i
    }
    std::vector<uint16_t> texels(values.size());
    double total = static_cast<double>(values.size());
    for (size_t i = 0; i < values.size(); i++) {
        uint32_t bin = binOf(values[i]);
        double rank = (histogram[bin] + 0.5 * (histogram[bin + 1] - histogram[bin])) / total;
        texels[i] = static_cast<uint16_t>(std::lround(glm::clamp(rank, 0.0, 1.0) * 65535.0));
    }
    return texels;
}

// The point of triangle abc nearest to p (Ericson, "Real-Time Collision Detection", 5.1.5)
glm::vec3 closestOnTriangle(glm::vec3 p, glm::vec3 a, glm::vec3 b, glm::vec3 c) {
    glm::vec3 ab = b - a, ac = c - a, ap = p - a;
    float d1 = glm::dot(ab, ap), d2 = glm::dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return a;
    glm::vec3 bp = p - b;
    float d3 = glm::dot(ab, bp), d4 = glm::dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return b;
    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) return a + ab * (d1 / (d1 - d3));
    glm::vec3 cp = p - c;
    float d5 = glm::dot(ab, cp), d6 = glm::dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return c;
    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) return a + ac * (d2 / (d2 - d6));
    float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    float denominator = 1.0f / (va + vb + vc);
    return a + ab * (vb * denominator) + ac * (vc * denominator);
}

// Whether points (yc, zc) of the y-z plane lie in triangle abc's shadow on it, and where along x the triangle is there.
// A point on an edge that two triangles share counts for one of them only (the top-left rule of rasterizers: an edge
// counts where it runs one way, not the other), so a closed mesh is crossed exactly once per surface.
bool rowCrossesTriangle(glm::vec2 p, glm::vec3 a, glm::vec3 b, glm::vec3 c, float& x) {
    glm::vec2 pa(a.y, a.z), pb(b.y, b.z), pc(c.y, c.z);
    float area = (pb.x - pa.x) * (pc.y - pa.y) - (pb.y - pa.y) * (pc.x - pa.x);
    if (area == 0.0f) {
        return false;  // edge on
    }
    if (area < 0.0f) {
        std::swap(pb, pc);
        std::swap(b, c);
        area = -area;
    }
    // Each edge's side test is computed from its lower end (in doubles), whichever triangle asks: the two triangles that
    // share an edge then always get exactly opposite answers. (Computed from either end, rounding could make both say
    // "outside" for a point a hair off the edge: a row on an ellipsoid's equator came out empty.)
    auto weight = [&](glm::vec2 from, glm::vec2 to) {
        bool flip = to.x < from.x || (to.x == from.x && to.y < from.y);
        glm::dvec2 f = flip ? to : from, t = flip ? from : to, q = p;
        double w = (t.x - f.x) * (q.y - f.y) - (t.y - f.y) * (q.x - f.x);
        if (flip) {
            w = -w;
        }
        glm::vec2 d = to - from;
        bool counts = d.y > 0.0f || (d.y == 0.0f && d.x < 0.0f);
        return w > 0.0 || (w == 0.0 && counts) ? static_cast<float>(w) : -1.0f;
    };
    float wa = weight(pb, pc), wb = weight(pc, pa), wc = weight(pa, pb);
    if (wa < 0.0f || wb < 0.0f || wc < 0.0f) {
        return false;
    }
    x = (wa * a.x + wb * b.x + wc * c.x) / area;
    return true;
}

// A mesh's signed distance field: per texel of `dims` over its bounding box (texel centers), the distance to its
// surface in mesh units, negative inside, as half floats. Near the triangles exactly; from there spread by jump
// flooding (Rong & Tan 2006): each texel takes the nearest of the surface points its neighbors at halving steps know.
// Off by a fraction of a texel in places, which the fade can't show. Inside or outside: along each row of texels
// (along x), the surfaces its line crosses; a texel is inside after an odd number of them (closed meshes only, which
// the material panel checks).
std::vector<uint16_t> bakeDistanceField(const Mesh& mesh, glm::vec3 lower, glm::vec3 upper, glm::uvec3 dims) {
    glm::vec3 extent = glm::max(upper - lower, glm::vec3(1e-6f));
    glm::vec3 texel = extent / glm::vec3(dims);
    auto center = [&](glm::ivec3 i) { return lower + (glm::vec3(i) + 0.5f) * texel; };
    size_t count = static_cast<size_t>(dims.x) * dims.y * dims.z;
    auto index = [&](glm::ivec3 i) { return (static_cast<size_t>(i.z) * dims.y + i.y) * dims.x + i.x; };
    const glm::vec3 none(1e30f);
    std::vector<glm::vec3> nearest(count, none);  // per texel: the nearest surface point found so far

    // Exact near the surface: each triangle's box, widened by two texels. In slabs of z, so threads don't share texels.
    float reach = 2.0f * std::max(texel.x, std::max(texel.y, texel.z));
    parallelFor(dims.z, [&](uint32_t zBegin, uint32_t zEnd) {
        for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
            glm::vec3 a = mesh.vertices[mesh.indices[t]].position;
            glm::vec3 b = mesh.vertices[mesh.indices[t + 1]].position;
            glm::vec3 c = mesh.vertices[mesh.indices[t + 2]].position;
            glm::vec3 boxMin = glm::min(a, glm::min(b, c)) - reach, boxMax = glm::max(a, glm::max(b, c)) + reach;
            glm::ivec3 from = glm::max(glm::ivec3(glm::floor((boxMin - lower) / texel)), glm::ivec3(0));
            glm::ivec3 to = glm::min(glm::ivec3(glm::floor((boxMax - lower) / texel)), glm::ivec3(dims) - 1);
            from.z = std::max(from.z, static_cast<int>(zBegin));
            to.z = std::min(to.z, static_cast<int>(zEnd) - 1);
            for (int z = from.z; z <= to.z; z++) {
                for (int y = from.y; y <= to.y; y++) {
                    for (int x = from.x; x <= to.x; x++) {
                        glm::ivec3 i(x, y, z);
                        glm::vec3 p = center(i);
                        glm::vec3 q = closestOnTriangle(p, a, b, c);
                        glm::vec3& known = nearest[index(i)];
                        if (glm::dot(q - p, q - p) < glm::dot(known - p, known - p)) {
                            known = q;
                        }
                    }
                }
            }
        }
    });

    // Jump flooding: steps of half the largest size, then halving, then 1 once more
    std::vector<glm::vec3> next(count);
    int step = static_cast<int>(std::max(dims.x, std::max(dims.y, dims.z))) / 2;
    std::vector<int> steps;
    for (; step >= 1; step /= 2) {
        steps.push_back(step);
    }
    steps.push_back(1);
    for (int jump : steps) {
        parallelFor(dims.z, [&](uint32_t zBegin, uint32_t zEnd) {
            for (int z = static_cast<int>(zBegin); z < static_cast<int>(zEnd); z++) {
                for (int y = 0; y < static_cast<int>(dims.y); y++) {
                    for (int x = 0; x < static_cast<int>(dims.x); x++) {
                        glm::ivec3 i(x, y, z);
                        glm::vec3 p = center(i);
                        glm::vec3 best = nearest[index(i)];
                        float bestDistance = glm::dot(best - p, best - p);
                        for (int n = 0; n < 27; n++) {
                            glm::ivec3 j = i + glm::ivec3(n % 3 - 1, (n / 3) % 3 - 1, n / 9 - 1) * jump;
                            if (glm::any(glm::lessThan(j, glm::ivec3(0))) || glm::any(glm::greaterThanEqual(j, glm::ivec3(dims)))) {
                                continue;
                            }
                            glm::vec3 candidate = nearest[index(j)];
                            float d = glm::dot(candidate - p, candidate - p);
                            if (d < bestDistance) {
                                best = candidate;
                                bestDistance = d;
                            }
                        }
                        next[index(i)] = best;
                    }
                }
            }
        });
        nearest.swap(next);
    }

    // Where each row's line crosses the surface. In slabs of z again: each row belongs to one thread.
    std::vector<std::vector<float>> rowCrossings(static_cast<size_t>(dims.y) * dims.z);
    parallelFor(dims.z, [&](uint32_t zBegin, uint32_t zEnd) {
        for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
            glm::vec3 a = mesh.vertices[mesh.indices[t]].position;
            glm::vec3 b = mesh.vertices[mesh.indices[t + 1]].position;
            glm::vec3 c = mesh.vertices[mesh.indices[t + 2]].position;
            glm::vec3 boxMin = glm::min(a, glm::min(b, c)), boxMax = glm::max(a, glm::max(b, c));
            // the rows whose centers lie in the triangle's box, and one more on each side: a row exactly on the box's
            // edge (an ellipsoid's equator) can round out of it, and the crossing test decides anyway
            glm::ivec3 from = glm::max(glm::ivec3(glm::ceil((boxMin - lower) / texel - 0.5f)) - 1, glm::ivec3(0));
            glm::ivec3 to = glm::min(glm::ivec3(glm::floor((boxMax - lower) / texel - 0.5f)) + 1, glm::ivec3(dims) - 1);
            from.z = std::max(from.z, static_cast<int>(zBegin));
            to.z = std::min(to.z, static_cast<int>(zEnd) - 1);
            for (int z = from.z; z <= to.z; z++) {
                for (int y = from.y; y <= to.y; y++) {
                    float x;
                    glm::vec3 p = center(glm::ivec3(0, y, z));
                    if (rowCrossesTriangle(glm::vec2(p.y, p.z), a, b, c, x)) {
                        rowCrossings[static_cast<size_t>(z) * dims.y + y].push_back(x);
                    }
                }
            }
        }
    });

    std::vector<uint16_t> texels(count);
    for (int z = 0; z < static_cast<int>(dims.z); z++) {
        for (int y = 0; y < static_cast<int>(dims.y); y++) {
            std::vector<float>& crossings = rowCrossings[static_cast<size_t>(z) * dims.y + y];
            std::sort(crossings.begin(), crossings.end());
            size_t passed = 0;  // crossings before this texel
            for (int x = 0; x < static_cast<int>(dims.x); x++) {
                glm::ivec3 i(x, y, z);
                glm::vec3 p = center(i);
                while (passed < crossings.size() && crossings[passed] < p.x) {
                    passed++;
                }
                float distance = nearest[index(i)] == none ? 65504.0f : glm::length(nearest[index(i)] - p);
                distance = std::min(distance, 65504.0f);
                texels[index(i)] = glm::packHalf1x16(passed % 2 == 1 ? -distance : distance);
            }
        }
    }
    return texels;
}

// Texels along each side of a distance field: 64 along the box's longest side, the others in proportion
glm::uvec3 distanceFieldSize(glm::vec3 lower, glm::vec3 upper) {
    glm::vec3 extent = glm::max(upper - lower, glm::vec3(1e-6f));
    float longest = std::max(extent.x, std::max(extent.y, extent.z));
    return glm::max(glm::uvec3(glm::ceil(extent / longest * 64.0f)), glm::uvec3(4));
}

}  // namespace

GpuTexture Renderer::uploadVolumeTexture(uint32_t width, uint32_t height, uint32_t depth, VkFormat format,
                                         const void* data, size_t size, VkSampler sampler) {
    GpuTexture texture;
    texture.sampler = sampler;
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_3D;
    imageInfo.extent = {width, height, depth};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    if (vmaCreateImage(allocator, &imageInfo, &allocationInfo, &texture.image, &texture.memory, nullptr) != VK_SUCCESS) {
        throw std::runtime_error("failed to create volume texture!");
    }
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = texture.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_3D;
    viewInfo.format = format;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device, &viewInfo, nullptr, &texture.view) != VK_SUCCESS) {
        throw std::runtime_error("failed to create volume texture view!");
    }

    VkBuffer stagingBuffer;
    VmaAllocation stagingMemory;
    createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stagingBuffer, stagingMemory);
    void* mapped;
    vmaMapMemory(allocator, stagingMemory, &mapped);
    std::memcpy(mapped, data, size);
    vmaUnmapMemory(allocator, stagingMemory);
    immediateSubmit([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = texture.image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {width, height, depth};
        vkCmdCopyBufferToImage(cmd, stagingBuffer, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
    });
    vkDestroyBuffer(device, stagingBuffer, nullptr);
    vmaFreeMemory(allocator, stagingMemory);
    return texture;
}

void Renderer::createVolumeTextures() {
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.maxLod = 0.0f;
    if (vkCreateSampler(device, &samplerInfo, nullptr, &volumeRepeatSampler) != VK_SUCCESS) {
        throw std::runtime_error("failed to create volume sampler!");
    }
    samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(device, &samplerInfo, nullptr, &volumeClampSampler) != VK_SUCCESS) {
        throw std::runtime_error("failed to create volume sampler!");
    }

    const uint32_t size = 128;
    std::vector<uint16_t> noise = makeFogNoise(size);
    gpuAssets.fogNoise = uploadVolumeTexture(size, size, size, VK_FORMAT_R16_UNORM, noise.data(),
                                             noise.size() * sizeof(uint16_t), volumeRepeatSampler);
    gpuAssets.volumeTextures = {gpuAssets.fogNoise};
}

void Renderer::destroyVolumeTextures() {
    auto destroy = [&](GpuTexture& texture) {
        vkDestroyImageView(device, texture.view, nullptr);
        vkDestroyImage(device, texture.image, nullptr);
        vmaFreeMemory(allocator, texture.memory);
    };
    destroy(gpuAssets.fogNoise);
    for (auto& entry : gpuAssets.distanceFields) {
        destroy(entry.second);
    }
    gpuAssets.distanceFields.clear();
    gpuAssets.volumeTextures.clear();
    vkDestroySampler(device, volumeRepeatSampler, nullptr);
    vkDestroySampler(device, volumeClampSampler, nullptr);
}

// The distance fields the meshes with varying fog need: baked if missing, released when the mesh went, and their
// slots in the descriptor array written into the mesh infos. true: the descriptor set must be updated (the caller).
bool Renderer::syncDistanceFields() {
    const std::vector<Mesh>& meshes = assets.meshes();
    const std::vector<uint64_t>& ids = assets.meshIds();
    std::map<uint64_t, GpuTexture> kept;
    std::vector<int32_t> slots(meshes.size(), -1);
    std::vector<GpuTexture> textures = {gpuAssets.fogNoise};
    bool changed = false;
    for (size_t m = 0; m < meshes.size(); m++) {
        if (m >= gpuAssets.meshInfos.size() || !fogVaries(assets.materials()[meshes[m].materialIndex].material) ||
            meshes[m].indices.empty()) {
            continue;
        }
        auto it = kept.find(ids[m]);  // (a mesh used twice in the list can't happen, but costs nothing)
        if (it == kept.end()) {
            auto old = gpuAssets.distanceFields.find(ids[m]);
            if (old != gpuAssets.distanceFields.end()) {
                it = kept.emplace(ids[m], old->second).first;
                gpuAssets.distanceFields.erase(old);
            } else {
                if (textures.size() >= maxVolumeTextures) {
                    std::cerr << "Too many meshes with varying fog: " << meshes[m].name << " fades without its edges\n";
                    continue;
                }
                const MeshInfo& info = gpuAssets.meshInfos[m];  // its box, as the shader has it
                glm::uvec3 dims = distanceFieldSize(info.boundsMin, info.boundsMax);
                std::vector<uint16_t> texels = bakeDistanceField(meshes[m], info.boundsMin, info.boundsMax, dims);
                vkp.deviceWaitIdle(device);  // (uploads wait anyway)
                it = kept.emplace(ids[m], uploadVolumeTexture(dims.x, dims.y, dims.z, VK_FORMAT_R16_SFLOAT, texels.data(),
                                                              texels.size() * sizeof(uint16_t), volumeClampSampler)).first;
                changed = true;
            }
        }
        slots[m] = static_cast<int32_t>(textures.size());
        textures.push_back(it->second);
    }
    // What is left belonged to meshes that went or no longer have varying fog
    for (auto& entry : gpuAssets.distanceFields) {
        releaseTexture(entry.second);
        changed = true;
    }
    gpuAssets.distanceFields = std::move(kept);
    gpuAssets.volumeTextures = std::move(textures);

    bool slotsChanged = false;
    for (size_t m = 0; m < gpuAssets.meshInfos.size() && m < slots.size(); m++) {
        slotsChanged |= gpuAssets.meshInfos[m].distanceField != slots[m];
        gpuAssets.meshInfos[m].distanceField = slots[m];
    }
    if (changed || slotsChanged) {
        vkp.deviceWaitIdle(device);  // the descriptor set gets rewritten: no frame may use it
    }
    if (slotsChanged && gpuAssets.geometryUploaded) {
        // The mesh infos are small: a new buffer
        vkDestroyBuffer(device, gpuAssets.meshInfoBuffer, nullptr);
        vmaFreeMemory(allocator, gpuAssets.meshInfoBufferMemory);
        createDeviceLocalBuffer(gpuAssets.meshInfos.data(), sizeof(gpuAssets.meshInfos[0]) * gpuAssets.meshInfos.size(),
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, gpuAssets.meshInfoBuffer, gpuAssets.meshInfoBufferMemory);
    }
    return changed || slotsChanged;
}
