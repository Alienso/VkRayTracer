// raytrace.comp, part: Random numbers: PCG hash, Owen-scrambled Sobol points.
// Included by raytrace.comp in order: uses what the parts before it define.

// ---------------------------------------------------------------------------------------------
// Random numbers (PCG hash)

uint pcgHash(uint value) {
    uint state = value * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float randomFloat(inout uint seed) {
    seed = pcgHash(seed);
    return float(seed) / 4294967295.0;
}

// ---------------------------------------------------------------------------------------------
// Low-discrepancy samples (Owen-scrambled Sobol points, Burley 2020: "Practical Hash-based Owen Scrambling")
//
// Independent random numbers clump and leave gaps by chance, and a pixel's average only settles as those even out.
// The main decisions of a path take their numbers from a Sobol sequence instead, whose first 2, 4, 8, ... points
// always fill [0,1)^4 evenly: the pixel's samples (one per frame, or several per frame) cover the light, the sun's
// disc, the sky and the bounce directions without clumps, and its average settles faster. Each decision (SAMPLE_*)
// at each bounce takes its own 4D point. Owen scrambling with a seed per decision randomizes the points without
// losing that evenness, and makes the decisions independent of each other ("padding"); the sample index is
// shuffled the same way. Everything else (cut-outs, the punctual lights, ReSTIR's candidates and reuse, the cache's
// update paths) keeps randomFloat.
// Neighboring pixels: with "Sobol", each pixel scrambles with its own seed too, so their errors are unrelated (white
// noise over the screen). With "+ blue noise" all pixels share the scrambled points, each shifted (modulo 1) by
// its texel of a blue-noise texture (Georgiev & Fajardo 2016): in a frame, neighboring pixels then take samples that
// differ evenly, and their errors too, fine-grained noise that DLSS filters more easily than blotches. The shift
// is the same in every frame, so a pixel's own samples stay just as evenly spread.
const uvec4 SOBOL_DIRECTIONS[32] = uvec4[](  // dimensions 0-3: van der Corput, then Joe & Kuo's 2-4
    uvec4(0x80000000u, 0x80000000u, 0x80000000u, 0x80000000u),
    uvec4(0x40000000u, 0xc0000000u, 0xc0000000u, 0xc0000000u),
    uvec4(0x20000000u, 0xa0000000u, 0x60000000u, 0x20000000u),
    uvec4(0x10000000u, 0xf0000000u, 0x90000000u, 0x50000000u),
    uvec4(0x08000000u, 0x88000000u, 0xe8000000u, 0xf8000000u),
    uvec4(0x04000000u, 0xcc000000u, 0x5c000000u, 0x74000000u),
    uvec4(0x02000000u, 0xaa000000u, 0x8e000000u, 0xa2000000u),
    uvec4(0x01000000u, 0xff000000u, 0xc5000000u, 0x93000000u),
    uvec4(0x00800000u, 0x80800000u, 0x68800000u, 0xd8800000u),
    uvec4(0x00400000u, 0xc0c00000u, 0x9cc00000u, 0x25400000u),
    uvec4(0x00200000u, 0xa0a00000u, 0xee600000u, 0x59e00000u),
    uvec4(0x00100000u, 0xf0f00000u, 0x55900000u, 0xe6d00000u),
    uvec4(0x00080000u, 0x88880000u, 0x80680000u, 0x78080000u),
    uvec4(0x00040000u, 0xcccc0000u, 0xc09c0000u, 0xb40c0000u),
    uvec4(0x00020000u, 0xaaaa0000u, 0x60ee0000u, 0x82020000u),
    uvec4(0x00010000u, 0xffff0000u, 0x90550000u, 0xc3050000u),
    uvec4(0x00008000u, 0x80008000u, 0xe8808000u, 0x208f8000u),
    uvec4(0x00004000u, 0xc000c000u, 0x5cc0c000u, 0x51474000u),
    uvec4(0x00002000u, 0xa000a000u, 0x8e606000u, 0xfbea2000u),
    uvec4(0x00001000u, 0xf000f000u, 0xc5909000u, 0x75d93000u),
    uvec4(0x00000800u, 0x88008800u, 0x6868e800u, 0xa0858800u),
    uvec4(0x00000400u, 0xcc00cc00u, 0x9c9c5c00u, 0x914e5400u),
    uvec4(0x00000200u, 0xaa00aa00u, 0xeeee8e00u, 0xdbe79e00u),
    uvec4(0x00000100u, 0xff00ff00u, 0x5555c500u, 0x25db6d00u),
    uvec4(0x00000080u, 0x80808080u, 0x8000e880u, 0x58800080u),
    uvec4(0x00000040u, 0xc0c0c0c0u, 0xc0005cc0u, 0xe54000c0u),
    uvec4(0x00000020u, 0xa0a0a0a0u, 0x60008e60u, 0x79e00020u),
    uvec4(0x00000010u, 0xf0f0f0f0u, 0x9000c590u, 0xb6d00050u),
    uvec4(0x00000008u, 0x88888888u, 0xe8006868u, 0x800800f8u),
    uvec4(0x00000004u, 0xccccccccu, 0x5c009c9cu, 0xc00c0074u),
    uvec4(0x00000002u, 0xaaaaaaaau, 0x8e00eeeeu, 0x200200a2u),
    uvec4(0x00000001u, 0xffffffffu, 0xc5005555u, 0x50050093u));
const uint SAMPLE_CAMERA = 0u;    // the offset in the pixel
const uint SAMPLE_SUN = 1u;       // a point on the sun's disc
const uint SAMPLE_SKY = 2u;       // a direction picked by the sky's brightness
const uint SAMPLE_EMISSIVE = 3u;  // a glowing triangle, and a point on it
const uint SAMPLE_BOUNCE = 4u;    // the bounce: lobe, direction, Russian roulette
const uint SAMPLE_FOG = 5u;       // where the ray scatters in fog, and the lamps' light in it (fogLampLight)
uint sobolPixelSeed = 0u;  // this pixel's scrambling seed (blue noise: the same for all); 0 = off (white noise)
uint sobolIndex = 0u;      // this path's number among its pixel's samples
ivec2 sobolPixel = ivec2(0);

// 64 x 64 texels, tiled; per texel 4 channels of 16 bits, each a blue-noise mask of its own (BlueNoise.cpp)
const uint BLUE_NOISE_SIZE = 64u;
layout(binding = 30, scalar) readonly buffer BlueNoise { uvec2 blueNoise[]; };

// The texture at this pixel, moved by an offset of its own per decision: neighbors in one channel differ evenly,
// and the decisions' shifts are unrelated to each other
vec4 blueNoiseAt(ivec2 pixel, uint seed) {
    uvec2 texel = (uvec2(pixel) + uvec2(seed, seed >> 16u)) & (BLUE_NOISE_SIZE - 1u);
    uvec2 v = blueNoise[texel.y * BLUE_NOISE_SIZE + texel.x];
    return vec4(v.x & 0xffffu, v.x >> 16u, v.y & 0xffffu, v.y >> 16u) / 65536.0;
}

// One random-looking permutation of the 32-bit values that only mixes each bit with lower ones (Laine & Karras;
// the constants are Vegdahl's improved version). Applied to the reversed bits, that is Owen scrambling.
uint laineKarrasPermutation(uint x, uint seed) {
    x ^= x * 0x3d20adeau;
    x += seed;
    x *= (seed >> 16u) | 1u;
    x ^= x * 0x05526c56u;
    x ^= x * 0x53a22864u;
    return x;
}

uint nestedUniformScramble(uint x, uint seed) {
    return bitfieldReverse(laineKarrasPermutation(bitfieldReverse(x), seed));
}

// The pixel's sample number: one sample per frame (several with samplesPerPixel), counted from the start of the
// accumulation, or from the program's start otherwise (DLSS keeps a history of its own)
void startPathSamples(ivec2 pixel, uint sampleInFrame) {
    uint frame = scene.accumulate != 0 ? scene.accumulatedFrames : scene.frameIndex;
    sobolIndex = frame * scene.samplesPerPixel + sampleInFrame;
    sobolPixel = pixel;
    sobolPixelSeed = scene.pathSampling == 0 ? 0u
                   : scene.pathSampling == 2 ? 0x2545f491u  // all pixels alike, shifted by the blue noise
                   : pcgHash(uint(pixel.x) + pcgHash(uint(pixel.y) ^ 0x9e3779b9u)) | 1u;
}

// Four numbers in [0, 1) for one decision of the path (SAMPLE_*) at a bounce: the coordinates of this path's
// Sobol point, or white noise when off
vec4 pathSample(uint bounce, uint decision, inout uint seed) {
    if (sobolPixelSeed == 0u) {
        return vec4(randomFloat(seed), randomFloat(seed), randomFloat(seed), randomFloat(seed));
    }
    uint dimensionSeed = pcgHash(sobolPixelSeed ^ pcgHash(bounce * 8u + decision));
    // Each bit of the scrambled index depends only on the index's bits from it up, so the bits above the index's
    // highest one are those of sample 0's. XORed out (the same for every sample: a constant XORed into all points,
    // which the scramble below randomizes anyway), the loop runs over ~log2(samples) bits instead of all 32.
    uint index = nestedUniformScramble(sobolIndex, dimensionSeed) ^ nestedUniformScramble(0u, dimensionSeed);
    uvec4 x = uvec4(0u);
    for (; index != 0u; index &= index - 1u) {
        x ^= SOBOL_DIRECTIONS[findLSB(index)];  // only the set bits
    }
    x = uvec4(nestedUniformScramble(x.x, pcgHash(dimensionSeed + 1u)), nestedUniformScramble(x.y, pcgHash(dimensionSeed + 2u)),
              nestedUniformScramble(x.z, pcgHash(dimensionSeed + 3u)), nestedUniformScramble(x.w, pcgHash(dimensionSeed + 4u)));
    vec4 u = vec4(x >> 8u) / 16777216.0;  // 24 bits: exactly representable, below 1
    if (scene.pathSampling == 2) {
        u = min(fract(u + blueNoiseAt(sobolPixel, dimensionSeed)), vec4(0.99999994));
    }
    return u;
}

// Any two vectors perpendicular to n (and to each other).
void buildBasis(vec3 n, out vec3 tangent, out vec3 bitangent) {
    vec3 helper = abs(n.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    tangent = normalize(cross(helper, n));
    bitangent = cross(n, tangent);
}

// Uniformly distributed direction inside a cone around `axis`. cosMaxAngle = 1 gives `axis` itself.
vec3 randomDirectionInCone(vec3 axis, float cosMaxAngle, vec2 u) {
    float cosTheta = mix(1.0, cosMaxAngle, u.x);
    float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));
    float phi = u.y * 2.0 * PI;
    vec3 tangent, bitangent;
    buildBasis(axis, tangent, bitangent);
    return normalize(tangent * (sinTheta * cos(phi)) + bitangent * (sinTheta * sin(phi)) + axis * cosTheta);
}
