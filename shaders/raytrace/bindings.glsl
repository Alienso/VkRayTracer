// raytrace.comp, part: Bindings: the images, the scene uniforms, and the buffers with the structs they hold.
// Included by raytrace.comp in order: uses what the parts before it define.


layout(binding = 0) uniform accelerationStructureEXT tlas;
// Linear HDR result: the running average over frames while accumulating, otherwise this frame (or a debug
// view). Exposure and tone mapping happen later, in display.comp.
// Alpha: the fog amount, the share of camera rays that scattered in fog before any reflection (through glass counts),
// averaged like the light: on average 1 - the fog's transmittance up to the first surface. For the fog grain
// (display.comp).
layout(binding = 1, rgba32f) uniform image2D radianceImage;
// DLSS inputs ("guides"), all about the first surface the camera ray hits; written only if scene.writeGuides
layout(binding = 2, r32f) uniform writeonly image2D linearDepthImage;          // view-space depth, sky = farDistance
layout(binding = 13, rg16f) uniform writeonly image2D motionVectorImage;       // pixels: last frame's position - this
layout(binding = 14, rgba16f) uniform writeonly image2D diffuseAlbedoImage;
layout(binding = 15, rgba16f) uniform writeonly image2D specularAlbedoImage;
layout(binding = 16, rgba16f) uniform writeonly image2D normalRoughnessImage;  // world-space normal, roughness in alpha
layout(binding = 17, r16f) uniform writeonly image2D specularHitDistanceImage; // 0 = unknown

// Must match SceneUniforms in Renderer.h
layout(binding = 3) uniform SceneUniforms {
    mat4 viewProjection;          // this frame, without jitter: world -> clip (Vulkan: y down)
    mat4 previousViewProjection;  // last frame's
    vec4 cameraPosition;
    vec4 cameraForward;
    vec4 cameraRight;
    vec4 cameraUp;
    vec2 jitter;            // sub-pixel offset in pixels, (0,0) until DLSS
    float tanHalfFov;       // tan(verticalFov / 2)
    float aspectRatio;      // width / height
    vec4 sunDirection;      // xyz: normalized direction TOWARDS the sun, w: cos(angular radius of the sun disc)
    vec4 sunColor;          // rgb: color * intensity (light arriving at a surface facing the sun)
    vec4 skyZenithColor;    // rgb, already multiplied by the sky intensity
    vec4 skyHorizonColor;
    vec4 groundColor;       // environment below the horizon
    uint environmentMap;    // 1 = the environment is the HDR map (environmentMap below) instead of the gradient
    uint environmentSampling;  // 1 = shadow rays are aimed at the sky too (environmentCdf below)
    uint environmentWidth;  // cells of environmentCdf
    uint environmentHeight;
    float environmentRotation;   // radians around the vertical axis
    float environmentIntensity;  // multiplies the HDR map
    uint punctualLightCount;     // entries in punctualLights
    uint samplesPerPixel;
    uint maxBounces;        // number of surfaces a path may hit
    uint frameIndex;        // changes every frame, seeds the random numbers
    uint shadowsEnabled;
    uint accumulatedFrames; // frames already averaged into radianceImage (0 = start over)
    uint accumulate;        // 0 = show only this frame
    uint debugView;         // 0 = path traced, otherwise one property of the first hit, see DebugView in Renderer.h
    uint emissiveTriangleCount;
    uint emissiveSampling;  // 0 = bounces only, 1 = light sampling only, 2 = both with MIS
    uint blurCaustics;      // 1 = path regularization, see tracePath
    float farDistance;      // depth written for rays that hit nothing
    uint writeGuides;       // 1 = write the DLSS input images
    uint cacheQuery;        // 1 = paths end in the radiance cache where they can
    uint cacheCapacity;     // entries in the cache (a power of two)
    float cacheCellScale;   // cache cell size per unit of distance from the camera
    float cacheMinWeight;   // samples a cache entry needs before paths use it
    uint cacheUpdateTile;   // update pass: one path per cacheUpdateTile x cacheUpdateTile pixels
    uint lightTree;         // 1 = light sampling picks glowing triangles by walking the light tree (binding 24)
    uint sunOn;             // 1 = the sun gives light (its color isn't 0)
    uint sunMis;            // 1 = the sun has a size: bounce rays find it too, shared with its shadow rays by MIS
    float sunPdf;           // pdf of a shadow ray towards the sun: 1 / the disc's solid angle
    float sunRadianceScale; // sunColor * this = the light of one point of the disc
    uint textureMipmaps;    // 1 = textures are read at the level that fits the hit's footprint (ray cones, see uvFootprint)
    float pixelSpreadAngle; // angle between neighboring camera rays (radians): how fast a pixel's footprint grows
    uint normalMapFiltering; // 1 = bumps that a normal map's smaller levels average away widen the reflection (Toksvig)
    uint restirDi;          // 1 = ReSTIR DI: the RESTIR_PASS traced the camera rays and picked a light sample per pixel
    uint restirTriangleCandidates;  // candidates per kind of light (0 when there are none of that kind)
    uint restirPunctualCandidates;
    uint restirSkyCandidates;
    uint restirBrdfCandidates;
    float restirMinRoughness;       // rougher surfaces use ReSTIR (restirSurface)
    uint restirTemporal;            // 1 = reuse last frame's reservoirs (restirTemporal); 2 = also with a shadow ray
    float restirMaxHistory;         // last frame's reservoir counts for at most this many times this frame's candidates
    uint restirHistoryValid;        // 0: last frame's reservoirs are unusable (first frame, lights changed, ...)
    uint restirCurrentHalf;         // which half of the reservoir and surface buffers is this frame's (0/1)
    uint restirPermutation;         // shuffle of last frame's pixels: bit 4 = on, this frame's offset x in bits 0-1, y in 2-3
    uint restirSpatialNeighbors;    // neighbors' reservoirs each pixel resamples (restirSpatial); 0 = no spatial pass
    float restirSpatialRadius;      // within this many render pixels
    uint restirDisocclusionNeighbors;  // as many for pixels that found no history
    uint restirSpatialVisibility;   // 1 = the spatial reuse is unbiased (shadow rays from the neighbors, see restirSpatial)
    float bounceLightClamp;         // 0 = off; else the most one sample of bounce light may add (luminance, see tracePath)
    uint restirGi;                  // 1 = ReSTIR GI: the first bounce's light at ReSTIR surfaces is a sample (see "ReSTIR GI")
    float restirGiMinRoughness;     // second surfaces at least this rough give a sample
    uint restirGiTemporal;          // 1 = reuse last frame's GI reservoirs (restirGiTemporal)
    float restirGiMaxHistory;       // last frame's reservoir counts for at most this many times this frame's samples
    uint restirGiMaxAge;            // samples older than this many frames are dropped
    uint restirGiHistoryValid;      // 0: last frame's GI reservoirs are unusable (first frame, lights changed, ...)
    uint restirGiPermutation;       // 1 = shuffle last frame's pixels (offset in restirPermutation's bits 0-3)
    uint restirGiSpatialNeighbors;  // neighbors' GI reservoirs each pixel resamples; 0 = no spatial pass
    float restirGiSpatialRadius;    // within this many render pixels
    uint restirGiDisocclusionNeighbors;  // as many for pixels that found no history
    uint restirGiSpatialVisibility; // 1 = rays from the neighbors' surfaces for the kept sample (see restirGiSpatial)
    float restirGiBoilingFilter;    // 0 = off; else its strength (see restirGiTemporalPass)
    float restirGiRadianceClamp;    // 0 = off; else the most a GI sample's Lo may be (luminance)
    uint restirGiTemporalVisibility;  // 1 = the temporal reuse is unbiased (a ray from last frame's surface)
    uint pathSampling;              // the path's main decisions (pathSample): 0 = white noise, 1 = Sobol, 2 = + blue noise
    uint emissiveSolidAngle;        // 1 = points on glowing triangles by solid angle (see sphericalSolidAngle), 0 = by area
    uint portalCount;               // entries in the portal list (binding 31): two per pair
    float fogTime;                  // seconds of fog drift (Material.fogDrift); 0 while no fog drifts
    uint lightCandidates;           // glowing triangles picked by power to keep one of (sampleEmissiveLight); 1 = tree
    uint cacheFog;                  // 1 = scatter points in fog have cache cells too (see "Radiance cache")
    uint maxPortalCrossings;        // portals a ray may go through (see "Portals"); at least 1
} scene;

// (0 in a pipeline without the feature, so its code drops out)
uint portalCount() {
    return FEATURE_PORTALS ? scene.portalCount : 0u;
}

uint punctualLightCount() {
    return FEATURE_LAMPS ? scene.punctualLightCount : 0u;
}

// Must match GpuVertex in GpuAssets.h (a SceneVertex, packed). Scalar layout = tightly packed like in C++.
struct Vertex {
    vec3 position;
    uint normal;    // decodeDirection
    uint tangent;   // decodeTangent: xyz the direction in which U grows, w: +-1 bitangent sign
    vec2 uv;
};

// Must match MeshInfo in GpuAssets.h. All meshes share the vertex and index buffers;
// this says where one mesh's part starts. Indices are relative to the mesh's first vertex.
struct MeshInfo {
    uint materialIndex;
    uint vertexOffset;
    uint indexOffset;
    uint vertexCount;
    uint indexCount;
    uint hasNormals;
    uint hasTangents;
    float unitScale;        // file units per mesh unit (the glTF node's scale), for absorption distances
    vec3 boundsMin;         // the mesh's box, in its own space
    vec3 boundsMax;
    int distanceField;      // in volumeTextures: the distance to its surface over that box; -1: none (Volumes.cpp)
};

layout(binding = 4, scalar) readonly buffer Vertices { Vertex vertices[]; };
layout(binding = 5, scalar) readonly buffer Indices { uint indices[]; };
layout(binding = 6, scalar) readonly buffer MeshInfos { MeshInfo meshInfos[]; };

// Unit vectors in 32 bits: the octahedron folded flat (Cigolle et al. 2014)
uint encodeDirection(vec3 n) {
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    vec2 e = n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return packSnorm2x16(e);
}

vec3 decodeOctahedral(vec2 e) {
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    float t = max(-n.z, 0.0);
    n.xy += vec2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t);
    return normalize(n);
}

vec3 decodeDirection(uint packed) {
    return decodeOctahedral(unpackSnorm2x16(packed));
}

// A vertex's tangent (packVertex in SceneGpu.cpp): 16 bits for x, 15 for y (sign-extended), the sign of w on top
vec4 decodeTangent(uint packed) {
    vec2 e = vec2(float(int(packed << 16u) >> 16), float(int(packed << 1u) >> 17)) / vec2(32767.0, 16383.0);
    return vec4(decodeOctahedral(max(e, vec2(-1.0))), (packed & 0x80000000u) != 0u ? -1.0 : 1.0);
}

vec3 vertexNormal(uint vertex) {
    return decodeDirection(vertices[vertex].normal);
}

vec4 vertexTangent(uint vertex) {
    return decodeTangent(vertices[vertex].tangent);
}

// Must match GpuMaterial in Renderer.h. Each value is a constant, multiplied by its texture if the index is >= 0.
struct Material {
    vec4 baseColor;         // linear RGB + alpha
    vec3 emissive;          // light given off, already multiplied by the strength
    float metallic;
    float roughness;
    float transmission;
    float ior;
    float alphaCutoff;
    uint alphaMode;         // 0 opaque, 1 mask, 2 blend
    int baseColorTexture;
    int metallicRoughnessTexture;  // G: roughness, B: metallic
    int transmissionTexture;       // R: transmission
    int emissiveTexture;
    int normalTexture;             // RGB: direction in tangent space (X along U, Y along "up" in the image, Z out)
    float normalScale;
    uint normalTwoChannel;         // 1 = the texture has X and Y only (BC5 from .dds), Z = sqrt(1 - X^2 - Y^2)
    uint solid;                    // transmissive: 1 = closed volume that refracts, 0 = thin (light passes straight)
    vec3 absorption;               // inside a solid: light left after distance d is exp(-absorption * d)
    // KHR_texture_transform: each texture is read at transform * (mesh UV, 1); identity when the file has none
    mat3x2 baseColorUvTransform;
    mat3x2 metallicRoughnessUvTransform;
    mat3x2 transmissionUvTransform;
    mat3x2 emissiveUvTransform;
    mat3x2 normalUvTransform;
    float diffuseTransmission;           // share of the diffuse layer that leaves on the other side (leaves, paper)
    vec3 diffuseTransmissionColor;       // its tint (not multiplied by the base color)
    int diffuseTransmissionTexture;      // A: diffuse transmission
    int diffuseTransmissionColorTexture; // RGB: tint
    mat3x2 diffuseTransmissionUvTransform;
    mat3x2 diffuseTransmissionColorUvTransform;
    int normalVarianceTexture;     // R: the bumps the normal map's level averages away, as GGX alpha (see hitFromIntersection)
    float fogDensity;              // fog inside the closed mesh, per unit of distance; 0 = none (see "Fog")
    vec3 fogColor;                 // the share of the light it scatters that goes on
    float fogForward;              // Henyey-Greenstein g of its scattering (fogPhase)
    uint invisible;                // 1 = rays pass the surface unchanged (only the fog inside shows)
    uint fogNoise;                 // varying fog (see "Varying density"): 1 = patchy, by the noise
    float fogNoiseScale;           // noise texture repeats per mesh unit
    float fogCoverage;             // 0..1: about the share of the volume with fog
    float fogEdge;                 // fades out over this distance towards the surface (mesh units); 0 = sharp
    vec3 fogNoiseOffset;           // where in the noise texture (from the material's seed)
    vec3 fogDrift;                 // how fast the noise moves through the mesh, in noise texture repeats per second
};
layout(binding = 8, scalar) readonly buffer Materials { Material materials[]; };

// Radiance cache (RadianceCache.cpp, shaders/cache_resolve.comp): a hash table of small cells on the surfaces,
// each with the light that leaves it (reflected, not its own emission), averaged over recent frames.
layout(binding = 18, scalar) buffer CacheKeys { uint cacheKeys[]; };                  // 0 = empty, else the cell's fingerprint
layout(binding = 19, scalar) buffer CacheAccumulators { vec4 cacheAccumulators[]; }; // this frame's samples: rgb sum, count
layout(binding = 20, scalar) readonly buffer CacheValues { vec4 cacheValues[]; };    // rgb: average light, w: sample weight

const uint ALPHA_OPAQUE = 0;
const uint ALPHA_MASK = 1;
const uint ALPHA_BLEND = 2;

// Must match EmissiveTriangle in Renderer.h. Every triangle of every glowing instance, in world space.
struct EmissiveTriangle {
    vec3 p0, p1, p2;
    vec2 uv0, uv1, uv2;
    uint materialIndex;
    float probability;      // chance that light sampling picks this triangle
    float cdf;              // running sum of the probabilities, for the binary search
    float area;
    uint treeTrail;         // its way down the light tree: bit d = 1 means "right child" at depth d
};
layout(binding = 9, scalar) readonly buffer EmissiveTriangles { EmissiveTriangle emissiveTriangles[]; };
// Per TLAS instance: where its triangles start in emissiveTriangles, or -1 if it doesn't glow.
layout(binding = 10, scalar) readonly buffer InstanceFirstLight { int instanceFirstLight[]; };

// How much light the GGX reflection returns in total (see createSpecularAlbedoTable in Materials.cpp):
// entry [roughness * SIZE + view angle], total for a given F0 = F0 * x + y.
layout(binding = 11, scalar) readonly buffer SpecularAlbedo { vec2 specularAlbedo[]; };

// Per TLAS instance. Must match GpuInstance in Renderer.h.
struct InstanceData {
    mat4x3 objectToWorld;  // used instead of the ray query's matrices, so a stored hit (see "Primary hits") is
                           // rebuilt the same way
    uint meshIndex;        // as its custom index in the TLAS (fog volumes are known by their instance, see "Fog")
    uint motion;           // its object's entry in motions
};
layout(binding = 12, scalar) readonly buffer Instances { InstanceData instances[]; };
// Per object: takes a point on it from this frame's world position to last frame's (object motion); identity for
// objects that didn't move
layout(binding = 33, scalar) readonly buffer Motions { mat4 motions[]; };
// Objects part way through portals (see "Portal pieces" in tracing.glsl): what a cut instance keeps. Must match
// GpuClipCut and GpuClipRegion in RendererTypes.h.
struct ClipCut {
    mat4x3 worldToEnd;  // world -> the end's space: the opening's plane at z = 0
    vec2 halfSize;
    uint ellipse;       // 1: an ellipse, 0: a rectangle
    uint keepNear;      // 1: an exit (only side * z >= 0 kept), 0: an entrance (side * z < 0 in the tube removed)
    float side;
};
struct ClipRegion {
    uint cutCount;
    ClipCut cuts[4];
};
layout(binding = 34, scalar) readonly buffer ClipRegions { ClipRegion clipRegions[]; };

// The inverse of an instance's matrix, without the translation (what normals and directions need). Worked out here
// rather than stored: 36 bytes less per instance (millions in big scenes).
mat3 instanceWorldToObject(uint instance) {
    return inverse(mat3(instances[instance].objectToWorld));
}

mat4 instanceMotion(uint instance) {
    return motions[instances[instance].motion];
}

// The camera rays' hits (RESTIR_PASS), one per render pixel, row by row. Must match GpuPrimaryHit in Renderer.h.
struct PrimaryHit {
    uint meshIndex;      // ~0: the camera ray hit nothing, PRIMARY_THROUGH_PORTAL: it crossed a portal (not stored)
    uint instanceIndex;
    int primitive;
    vec2 barycentrics;
    float t;
    uint seed;           // the pixel's random numbers after the ray (cut-outs use some), for the image pass to go on with
    // Seen in mirrors (see "Mirrors and ReSTIR"): the ray's length before its last part (0: no mirror), the last
    // part's direction, and the mirrors' color (half floats)
    float travelled;
    vec3 direction;
    uint tintRG;
    uint tintB;
};
layout(binding = 25, scalar) buffer PrimaryHits { PrimaryHit primaryHits[]; };
// The camera ray went through a portal: the pixel doesn't use ReSTIR (its first surface is not where the camera
// sees it), and the image pass traces the ray again itself
const uint PRIMARY_THROUGH_PORTAL = 0xFFFFFFFDu;
// The first bounce of the image path at a ReSTIR surface, picked and traced by the RESTIR_PASS as its BRDF
// candidate (see restirCandidates), one per render pixel. Must match GpuBounceHit in Renderer.h.
struct BounceHit {
    uint meshIndex;      // ~0: the ray left the scene, BOUNCE_NONE: no bounce (the path ends at the first surface)
    uint instanceIndex;
    int primitive;
    vec2 barycentrics;
    float t;
    vec3 direction;      // exact: the hit is rebuilt with it (which side was hit, texture footprint)
    uint weightRG;       // the sample's weight (BRDF * cos / pdf), half floats
    uint weightBRoughness;  // weight.b (half), lobe roughness (8 bits), flags (8 bits: 1 transmitted, 2 specular lobe,
                            // 4 the ray crossed a portal: not stored, the image pass traces it itself)
    float pdf;
};
layout(binding = 28, scalar) buffer BounceHits { BounceHit bounceHits[]; };
const uint BOUNCE_NONE = 0xFFFFFFFEu;
const uint BOUNCE_RETRACE = 4u << 24;
const uint SPECULAR_ALBEDO_SIZE = 32;  // must match Renderer::specularAlbedoSize

const uint EMISSIVE_BOUNCES_ONLY = 0;
const uint EMISSIVE_LIGHT_SAMPLING = 1;
const uint EMISSIVE_MIS = 2;

// All textures of all models in one array. Neighboring pixels can hit different materials, so the
// index differs between the threads of a warp: nonuniformEXT tells the compiler to handle that.
layout(binding = 7) uniform sampler2D textures[];
// The fog's 3D textures (Volumes.cpp): 0 the noise, then the distance fields of meshes with varying fog
layout(binding = 32) uniform sampler3D volumeTextures[];

// The HDR environment map: equirectangular (u: around the horizon, v: from straight up to straight down)
layout(binding = 21) uniform sampler2D environmentMap;
// For picking sky directions in proportion to their brightness (Environment.cpp): the running sums (CDF) over the
// environmentHeight rows, then for each row the running sums over its environmentWidth columns.
layout(binding = 22, scalar) readonly buffer EnvironmentDistribution { float environmentCdf[]; };

// Must match GpuPunctualLight in Renderer.h. The lights that came with the models (KHR_lights_punctual), in
// world space: points, spots and directional lights without size, so only shadow rays reach them.
struct PunctualLight {
    vec3 position;
    uint type;              // LIGHT_* below
    vec3 direction;         // the way it shines
    float range;            // 0 = no limit
    vec3 intensity;         // candela (point, spot), lux (directional)
    float cosOuterCone;
    float cosInnerCone;
};
layout(binding = 23, scalar) readonly buffer PunctualLights { PunctualLight punctualLights[]; };
const uint LIGHT_DIRECTIONAL = 0;
const uint LIGHT_POINT = 1;
const uint LIGHT_SPOT = 2;

// Must match LightTreeNode in Renderer.h. A binary tree over the glowing triangles that light sampling can pick,
// node 0 is the root (see buildLightTree in Lights.cpp and pickFromLightTree below).
struct LightTreeNode {
    vec3 boundsMin;
    float power;            // its triangles' summed probability (share of the scene's light)
    vec3 boundsMax;
    int child;              // >= 0: the first of its two children (the second follows); < 0: triangle -child-1
};
layout(binding = 24, scalar) readonly buffer LightTree { LightTreeNode lightTreeNodes[]; };

const float PI = 3.14159265359;
const float RAY_FAR = 1e30;          // how far rays go: no limit, whatever the scene's size
const float SKY_DISTANCE = RAY_FAR;  // shadow rays towards the sun or the sky: anything on the way blocks them
