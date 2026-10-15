// raytrace.comp, part: Shared by the passes: debug colors, the cache update pass, the camera ray of a pixel.
// Included by raytrace.comp in order: uses what the parts before it define.

// A stable, distinct color per number
vec3 randomColor(uint value) {
    uint h = pcgHash(value + 1u);
    return vec3(h & 0xFFu, (h >> 8) & 0xFFu, (h >> 16) & 0xFFu) / 255.0;
}

// Shows one property of the first surface hit, to check that the mesh and material data arrive correctly.
vec3 debugColor(vec3 direction, inout uint seed) {
    Hit hit = traceClosest(scene.cameraPosition.xyz, direction, seed, 0.0, scene.pixelSpreadAngle);
    if (!hit.found) {
        return vec3(0.0);
    }
    switch (scene.debugView) {
        case 1: return hit.normal * 0.5 + 0.5;  // x -> red, y -> green, z -> blue
        case 2: return vec3(fract(hit.uv), 0.0);
        case 3: return randomColor(hit.meshIndex);
        case 4: return randomColor(hit.materialIndex);
        // linear values, shown without exposure or tone mapping; the blit to the sRGB swapchain makes them look right
        case 5: return hit.albedo;
        case 6: return vec3(hit.metallic);
        case 7: return vec3(hit.roughness);
        case 8: return vec3(hit.transmission);
        case 9: return hit.emissive;
        case 10: return hit.tangent * 0.5 + 0.5;
        case 11: {
            // The cached (indirect) light where the camera looks (cells of surfaces that can't be cached are black)
            if (!cacheableSurface(hit)) {
                return vec3(0.0);
            }
            uint slot, fingerprint;
            float cellSize;
            cacheCell(hit, slot, fingerprint, cellSize);
            int entry = cacheFind(slot, fingerprint);
            if (entry < 0) {
                return vec3(1.0, 0.0, 1.0);
            }
            vec4 cached = cacheValues[entry];
            return cached.w >= scene.cacheMinWeight ? cached.rgb : vec3(0.3, 0.0, 0.0);
        }
    }
    return vec3(1.0, 0.0, 1.0);
}

// Radiance cache update pass: one full-length path from a random pixel of each tile; only the cache gets written.
void updateCache() {
    ivec2 size = imageSize(radianceImage);
    uint tile = scene.cacheUpdateTile;
    uint seed = pcgHash(gl_GlobalInvocationID.x + pcgHash(gl_GlobalInvocationID.y + pcgHash(scene.frameIndex ^ 0x5bd1e995u)));
    vec2 position = vec2(gl_GlobalInvocationID.xy * tile) + vec2(randomFloat(seed), randomFloat(seed)) * float(tile);
    if (position.x >= float(size.x) || position.y >= float(size.y)) {
        return;
    }
    tracePath(scene.cameraPosition.xyz, cameraRayDirection(position, size), seed, false, false);
}

// The start of a pixel's random numbers in this frame
uint pixelSeed(ivec2 pixel) {
    return pcgHash(uint(pixel.x) + pcgHash(uint(pixel.y) + pcgHash(scene.frameIndex)));
}

// Spread samples randomly over the pixel (anti-aliasing). A single sample without accumulation
// goes through the pixel center, otherwise edges would flicker from frame to frame.
vec3 sampleCameraRay(ivec2 pixel, ivec2 size, inout uint seed) {
    bool randomOffset = scene.samplesPerPixel > 1 || scene.accumulate != 0;
    vec2 offset = randomOffset ? pathSample(0u, SAMPLE_CAMERA, seed).xy - 0.5 : vec2(0.0);
    return cameraRayDirection(vec2(pixel) + 0.5 + offset, size);
}

// Whether a reservoir's sample is seen from a surface (a shadow ray; through glass counts as seen)
