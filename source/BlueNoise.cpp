//
// Blue-noise texture for the path sampler (binding 30, see pathSample in shaders/raytrace.comp): 64 x 64 texels,
// four independent channels, tiled over the screen.
//
// In a blue-noise mask neighboring texels have very different values: no clumps of similar values, no large gaps.
// Used to shift the Sobol points per pixel, it makes neighboring pixels in a frame take different samples in the
// same way, so their errors differ from pixel to pixel instead of forming blotches: fine-grained noise, which the
// eye and DLSS average away more easily than white noise (Georgiev & Fajardo 2016, "Blue-noise dithered sampling").
//
// Made once at startup with Ulichney's void-and-cluster method (1993), each channel from its own random start.
//

#include <vulkan/vulkan.h>
#include <algorithm>
#include <cmath>
#include <random>
#include <vector>
#include "Application.h"

static constexpr int blueNoiseSize = 64;  // must match BLUE_NOISE_SIZE in the shader

// Ranks 0 .. size^2 - 1 of a blue-noise mask: the order in which the texels get "switched on". Each prefix of the order
// is evenly spread; a texel's value is its rank / size^2. The energy of a texel is the sum of a Gaussian around every
// switched-on texel (on a torus, so the mask tiles): high in clusters, low in voids.
static std::vector<int> voidAndCluster(uint32_t seed) {
    const int n = blueNoiseSize * blueNoiseSize;
    const float sigma = 1.9f;  // Ulichney's 1.5 gives slightly clumpier masks at this size
    const int radius = 8;      // exp(-r^2 / (2 sigma^2)) is below 1e-4 beyond
    std::vector<float> kernel((2 * radius + 1) * (2 * radius + 1));
    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            kernel[(dy + radius) * (2 * radius + 1) + dx + radius] = std::exp(-(dx * dx + dy * dy) / (2.0f * sigma * sigma));
        }
    }
    std::vector<float> energy(n, 0.0f);
    std::vector<char> on(n, 0);
    auto toggle = [&](int texel, bool switchOn) {
        on[texel] = switchOn ? 1 : 0;
        int x = texel % blueNoiseSize, y = texel / blueNoiseSize;
        float sign = switchOn ? 1.0f : -1.0f;
        for (int dy = -radius; dy <= radius; dy++) {
            int row = ((y + dy) & (blueNoiseSize - 1)) * blueNoiseSize;
            for (int dx = -radius; dx <= radius; dx++) {
                energy[row + ((x + dx) & (blueNoiseSize - 1))] += sign * kernel[(dy + radius) * (2 * radius + 1) + dx + radius];
            }
        }
    };
    // The switched-on texel in the tightest cluster, or the switched-off one in the largest void
    auto tightestCluster = [&] {
        int best = -1;
        for (int i = 0; i < n; i++) {
            if (on[i] && (best < 0 || energy[i] > energy[best])) {
                best = i;
            }
        }
        return best;
    };
    auto largestVoid = [&] {
        int best = -1;
        for (int i = 0; i < n; i++) {
            if (!on[i] && (best < 0 || energy[i] < energy[best])) {
                best = i;
            }
        }
        return best;
    };

    // 1: a random tenth of the texels, then moved from clusters into voids until that changes nothing
    std::mt19937 random(seed);
    int initialCount = n / 10;
    std::vector<int> order(n);
    for (int i = 0; i < n; i++) {
        order[i] = i;
    }
    std::shuffle(order.begin(), order.end(), random);
    for (int i = 0; i < initialCount; i++) {
        toggle(order[i], true);
    }
    while (true) {
        int cluster = tightestCluster();
        toggle(cluster, false);
        int gap = largestVoid();
        toggle(gap, true);
        if (gap == cluster) {
            break;
        }
    }
    std::vector<char> initialOn = on;
    std::vector<float> initialEnergy = energy;

    // 2: ranks below the initial count, by taking the initial texels away from the tightest cluster on
    std::vector<int> rank(n, 0);
    for (int r = initialCount - 1; r >= 0; r--) {
        int cluster = tightestCluster();
        rank[cluster] = r;
        toggle(cluster, false);
    }
    // 3: the rest, from the initial pattern, by filling the largest void. (Past half, Ulichney switches to the
    //    tightest cluster of the switched-off texels; with a kernel that sums to the same everywhere, that is the
    //    same texel.)
    on = initialOn;
    energy = initialEnergy;
    for (int r = initialCount; r < n; r++) {
        int gap = largestVoid();
        rank[gap] = r;
        toggle(gap, true);
    }
    return rank;
}

void Application::createBlueNoiseTexture() {
    const int n = blueNoiseSize * blueNoiseSize;
    std::vector<int> channels[4];
    for (uint32_t c = 0; c < 4; c++) {
        channels[c] = voidAndCluster(0x1234567u + c * 7919u);
    }
    // Per texel two uints of two 16-bit values each: (rank + 0.5) / n, exact in 16 bits
    std::vector<uint32_t> packed(n * 2);
    for (int i = 0; i < n; i++) {
        uint32_t v[4];
        for (int c = 0; c < 4; c++) {
            v[c] = static_cast<uint32_t>(channels[c][i] * (65536 / n) + (65536 / n) / 2);
        }
        packed[i * 2] = v[0] | (v[1] << 16);
        packed[i * 2 + 1] = v[2] | (v[3] << 16);
    }
    createDeviceLocalBuffer(packed.data(), sizeof(uint32_t) * packed.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                            blueNoiseBuffer, blueNoiseMemory);
}

void Application::destroyBlueNoiseTexture() {
    vkDestroyBuffer(device, blueNoiseBuffer, nullptr);
    vkFreeMemory(device, blueNoiseMemory, nullptr);
}
