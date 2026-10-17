// raytrace.comp, part: Fog.
// Included by raytrace.comp in order: uses what the parts before it define.

// ---------------------------------------------------------------------------------------------
// Fog
//
// Fog is a material: a closed mesh whose material has a fog thickness (Material.fogDensity) is filled with fog. Of
// light going a distance d through fog of density s, exp(-s * d) gets through; the rest scatters: the share fogColor
// of it goes on in a new direction (fogPhase), the rest is absorbed. Its surface is either invisible (Material.invisible:
// rays pass it unchanged, only the fog shows: haze, smoke, a cloud) or the material's own (solid glass with fog inside:
// murky water, milky glass; thin glass: a bubble of smoke). The built-in "Fog box" is a cube of invisible fog. The
// density is the same throughout ("even"), or varies from point to point (see "Varying density" below).
//
// Media. A path keeps which fog volumes it is in (its "media": up to MAX_MEDIA instances, packed: the first slots taken,
// the free ones after them, so loops over them stop at the first free one; see enterMedium), and changes
// them where it goes through a volume's surface: invisible ones in intersectClosest, others where the path passes
// through the surface (tracePath; thin glass too), and at a portal's partner (mediumAt, in intersectClosest). A volume
// is an instance, not a material: the same fog material on two objects is two volumes, each with
// its own part of the noise, and where volumes overlap their densities add up. Where the camera is: mediumAt. A ray that
// leaves fog it didn't know it was in (one too many at once, or one mediumAt didn't see) catches up: the stretch before
// was inside it.
// Where a ray scatters: delta tracking (Woodcock et al. 1965; Novak et al. 2018). It takes tentative steps of random
// length -log(1 - u) / majorant, the majorant being at least the density anywhere (the media's thicknesses added up),
// and at each it scatters with chance density / majorant, else steps on ("null collision"). In even fog the density is
// the majorant, so the first step scatters: the ray query only needs to look that far. The ray's chance of getting past
// a stretch is exactly the share of light that does, so what it hits behind the fog needs no weight. The steps have no
// memory: at every surface it passes (an invisible one, a portal) they start over, as if the ray started there. Of
// overlapping volumes, the one it scatters in is picked by its share of the density there. Where it scatters is a vertex
// of the path like a surface (fogHit): lit by light sampling, the phase function in place of the BRDF, and the path goes
// on from it. Shadow rays add up their length inside even fog, in whatever order its surfaces come
// (shadowTransmittance); varying fog needs its stretches in order (varyingTransmittance).
// The density is the same for every color, so that chance and the light's loss always agree; the color is in the
// scattered light only, not in what passes straight through.
// Not in the fog's light: ReSTIR (its scatter points aren't surfaces). The radiance cache has cells in fog too (see
// there, scene.cacheFog).
// Lamps (the punctual lights) are the exception to the light sampling at scatter points. Their light falls off as
// 1 / distance^2, and the points the fog picks land near one only rarely, then very bright: the glow around a lamp
// would sparkle forever. Instead every stretch of a path takes their light along its whole length, at a point picked
// near the lamp (fogLampLight), and scatter points leave them out.
//
// Varying density (FOG = 2). A fog material with noise (Material.fogNoise) or an edge fade (fogEdge) has a density that
// changes from point to point (fogDensityAt): its thickness, times the noise (a tiling 3D texture read in the mesh's own
// space, so it moves, turns and scales with the object; cut down by the coverage), times the fade (from the mesh's
// signed distance field: how far inside its surface). Both are 0..1, so the thickness is the volume's majorant. The
// textures: volumeTextures (Volumes.cpp). Outside the mesh the distance field says so, and the density is 0: a stretch
// of a ray through the volume's bounding box holds all its fog, whatever the mesh's shape. Shadow rays use that
// (varyingTransmittance): ratio tracking (Novak et al. 2014) along the box, the same tentative steps, the light
// multiplied by 1 - density / majorant at each, with no random end; unbiased, and smooth where the fog is thin. Thin
// parts under a high majorant cost steps that find nothing (null collisions): the price of varying fog.

const uint NO_MEDIUM = 0xFFFFu;
const uvec2 OUTSIDE = uvec2(0xFFFFFFFFu);  // media: in no fog (4 slots of 16 bits, each an instance or NO_MEDIUM)
const uint MAX_MEDIA = 4;                   // volumes a path knows it is in at once (more: forgotten, see above)
const uint MAX_VOLUME_CROSSINGS = 8;        // invisible surfaces a ray goes through (beyond: it stops at the next one)
const uint FOG_MASK = 0x02u;                // TLAS groups of the fog volumes (mediumAt): even ones
const uint VARYING_FOG_MASK = 0x04u;        // varying ones
// Tentative steps along one stretch at most. Only a safety net against a stuck GPU: reached, the rest of the stretch
// is passed as if clear. Thickness 10 over 100 units takes ~1000.
const uint MAX_FOG_STEPS = 4096;

uint mediaSlot(uvec2 media, uint slot) {
    return ((slot < 2u ? media.x : media.y) >> ((slot & 1u) * 16u)) & 0xFFFFu;
}

void setMediaSlot(inout uvec2 media, uint slot, uint instance) {
    uint shift = (slot & 1u) * 16u;
    uint word = ((slot < 2u ? media.x : media.y) & ~(0xFFFFu << shift)) | (instance << shift);
    if (slot < 2u) {
        media.x = word;
    } else {
        media.y = word;
    }
}

// Packed media, one after the other: the first, and the others moved down a slot (a loop over them:
// for (uvec2 rest = media; firstMedium(rest) != NO_MEDIUM; rest = otherMedia(rest)), as few steps as there are volumes)
uint firstMedium(uvec2 media) {
    return media.x & 0xFFFFu;
}

uvec2 otherMedia(uvec2 media) {
    return uvec2((media.x >> 16u) | (media.y << 16u), (media.y >> 16u) | 0xFFFF0000u);
}

bool inMedium(uvec2 media, uint instance) {
    for (uvec2 rest = media; firstMedium(rest) != NO_MEDIUM; rest = otherMedia(rest)) {
        if (firstMedium(rest) == instance) {
            return true;
        }
    }
    return false;
}

// Into the volume of an instance: in front of the others (already in it: unchanged; all slots taken: the one entered
// longest ago is forgotten; instances past 16 bits can't be media)
uvec2 enterMedium(uvec2 media, uint instance) {
    if (instance < NO_MEDIUM && !inMedium(media, instance)) {
        media = uvec2((media.x << 16u) | instance, (media.y << 16u) | (media.x >> 16u));
    }
    return media;
}

// Out of it: the others stay, packed
uvec2 leaveMedium(uvec2 media, uint instance) {
    uvec2 kept = OUTSIDE;
    uint count = 0;
    for (uvec2 rest = media; firstMedium(rest) != NO_MEDIUM; rest = otherMedia(rest)) {
        if (firstMedium(rest) != instance) {
            setMediaSlot(kept, count++, firstMedium(rest));
        }
    }
    return kept;
}

uint mediumMaterial(uint instance) {
    return meshInfos[instances[instance].meshIndex].materialIndex;
}

// Glass volumes. Solid glass (a transmissive material on a closed mesh, KHR_materials_volume) is a medium too, kept
// in the same list as the fog: a path knows which glass it is in, so it can start inside glass (the camera under
// water: mediumAt), pass objects inside glass, and go from one glass into another (water in a glass, ice in the
// water). The glass it is in is the one of its media it entered last; it absorbs along the way (Beer's law), and at
// a surface the light bends by the ratio of the indices of refraction on both sides (tracePath).
// Liquids are usually modelled overlapping their container: the water's outer surface lies inside the glass wall,
// not on its inner surface (Bistro's glasses). Entering the water there counts (the glass to water boundary is then
// there, a few mm early); leaving the glass while in the water that came later changes nothing, and the ray passes
// on unchanged. (Renderers that do this exactly give each volume a priority, which glTF doesn't have.)
// Only in the pipelines with FOG: a scene with solid glass uses those (the plain ones assume there is none).
const uint GLASS_MASK = 0x08u;  // TLAS group of the glass volumes (mediumAt)
// Passes through glass a path may make besides its bounces (see glassPasses in tracePath): a glass of water with ice
// seen through takes about ten
const uint MAX_GLASS_PASSES = 16;

bool glassMaterial(uint material) {
    return FEATURE_GLASS && materials[material].transmission > 0.0 && materials[material].solid != 0;
}

// The glass the path is in: the last of its media that is glass (NO_MEDIUM: none, it is in air or fog only)
uint currentGlass(uvec2 media) {
    for (uvec2 rest = media; FOG != 0 && FEATURE_GLASS && firstMedium(rest) != NO_MEDIUM; rest = otherMedia(rest)) {
        if (glassMaterial(mediumMaterial(firstMedium(rest)))) {
            return firstMedium(rest);
        }
    }
    return NO_MEDIUM;
}

// The index of refraction in a glass volume (NO_MEDIUM: air, 1)
float glassIor(uint glass) {
    return glass == NO_MEDIUM ? 1.0 : materials[mediumMaterial(glass)].ior;
}

// What the light keeps going a distance t along direction through a glass volume. Its absorption is per unit of the
// model file (the glTF node's scale, MeshInfo.unitScale).
vec3 glassTransmittance(uint glass, vec3 direction, float t) {
    float units = length(instanceWorldToObject(glass) * direction) * meshInfos[instances[glass].meshIndex].unitScale;
    return exp(-materials[mediumMaterial(glass)].absorption * t * units);
}

bool fogVaries(uint material) {
    return FOG >= 2 && (materials[material].fogNoise != 0 || materials[material].fogEdge > 0.0);
}

// The density of the fog of volume `instance` at world point p, inside it (see "Varying density")
float fogDensityAt(uint instance, vec3 p) {
    uint material = mediumMaterial(instance);
    float density = materials[material].fogDensity;
    if (!fogVaries(material)) {
        return density;
    }
    vec3 local = instanceWorldToObject(instance) * (p - instances[instance].objectToWorld[3]);
    if (materials[material].fogNoise != 0) {
        // Drifting fog: the noise is read further back along the drift the longer it has drifted (wrapped: the
        // texture repeats every 1, and the product stays small enough for float precision)
        vec3 drifted = materials[material].fogNoiseOffset - fract(materials[material].fogDrift * scene.fogTime);
        float noise = textureLod(volumeTextures[0], local * materials[material].fogNoiseScale + drifted, 0.0).r;
        // The noise is spread evenly over 0..1 (Volumes.cpp): the top `coverage` of it is fog, rising from 0
        float coverage = materials[material].fogCoverage;
        density *= clamp((noise - (1.0 - coverage)) / max(coverage, 1e-3), 0.0, 1.0);
    }
    // The mesh's signed distance (negative inside): none outside, faded towards the surface inside
    uint mesh = instances[instance].meshIndex;
    int field = meshInfos[mesh].distanceField;
    if (field >= 0 && density > 0.0) {
        vec3 lower = meshInfos[mesh].boundsMin;
        vec3 uvw = (local - lower) / max(meshInfos[mesh].boundsMax - lower, vec3(1e-6));
        float inside = -textureLod(volumeTextures[nonuniformEXT(field)], uvw, 0.0).r;
        float edge = materials[material].fogEdge;
        density *= inside <= 0.0 ? 0.0 : edge > 0.0 ? smoothstep(0.0, 1.0, inside / edge) : 1.0;
    }
    return density;
}

// The media's majorant: their thicknesses added up. uneven: some of them varies.
float mediaMajorant(uvec2 media, out bool uneven) {
    float majorant = 0.0;
    uneven = false;
    for (uvec2 rest = media; firstMedium(rest) != NO_MEDIUM; rest = otherMedia(rest)) {
        uint material = mediumMaterial(firstMedium(rest));
        majorant += materials[material].fogDensity;
        uneven = uneven || fogVaries(material);
    }
    return majorant;
}

// Delta tracking (see "Fog") from the tentative step at t up to end: where the ray scatters, and in which volume
// (medium); end and NO_MEDIUM if it gets through
float deltaTrack(uvec2 media, vec3 origin, vec3 direction, float t, float end, float majorant, inout uint seed,
                 out uint medium) {
    medium = NO_MEDIUM;
    for (uint step = 0; step < MAX_FOG_STEPS && t < end; step++) {
        vec3 p = origin + direction * t;
        float u = randomFloat(seed) * majorant;
        for (uvec2 rest = media; firstMedium(rest) != NO_MEDIUM; rest = otherMedia(rest)) {
            u -= fogDensityAt(firstMedium(rest), p);
            if (u < 0.0) {
                medium = firstMedium(rest);
                return t;
            }
        }
        t -= log(1.0 - randomFloat(seed)) / majorant;
    }
    return end;
}

// The share of light that gets through the fog of one volume from t = a to b along a ray: ratio tracking (see
// "Varying density"). Where little is left, Russian roulette: it goes on with chance transmittance / 0.1, counted as
// 0.1, which keeps the average.
float ratioTrack(uint instance, vec3 origin, vec3 direction, float a, float b, inout uint seed) {
    float majorant = materials[mediumMaterial(instance)].fogDensity;
    float transmittance = 1.0;
    float t = a;
    for (uint step = 0; step < MAX_FOG_STEPS; step++) {
        t -= log(1.0 - randomFloat(seed)) / majorant;
        if (t >= b) {
            break;
        }
        transmittance *= 1.0 - fogDensityAt(instance, origin + direction * t) / majorant;
        if (transmittance < 0.1) {
            if (randomFloat(seed) * 0.1 >= transmittance) {
                return 0.0;
            }
            transmittance = 0.1;
        }
    }
    return transmittance;
}

// Whether a ray along direction crosses a triangle from the outside (the side its vertex normals point to, as in
// hitFromIntersection)
bool entersMesh(MeshInfo mesh, uvec3 corner, vec3 weights, mat3 worldToObject, vec3 direction) {
    vec3 p0 = vertices[corner.x].position;
    vec3 outward = cross(vertices[corner.y].position - p0, vertices[corner.z].position - p0) * worldToObject;
    if (mesh.hasNormals != 0) {
        vec3 smoothNormal = vertexNormal(corner.x) * weights.x + vertexNormal(corner.y) * weights.y
                            + vertexNormal(corner.z) * weights.z;
        if (dot(outward, smoothNormal * worldToObject) < 0.0) {
            outward = -outward;
        }
    }
    return dot(outward, direction) < 0.0;
}

// The next surface of a fog volume along a ray, from tMin up to tMax, among the TLAS groups in mask, leaving out the
// triangles `passed` (the ones the ray just went through, see traverse). false: none. instance, primitive, t: which and
// where; entering: from its outside.
bool nextFogSurface(vec3 origin, vec3 direction, float tMin, float tMax, uint mask, uvec4 passed, out uint instance,
                    out int primitive, out float t, out bool entering) {
    rayQueryEXT rayQuery;
    rayQueryInitializeEXT(rayQuery, tlas, gl_RayFlagsNoneEXT, mask, origin, tMin, direction, tMax);
    while (rayQueryProceedEXT(rayQuery)) {
        // (fog volumes are all "not opaque": for the shadow rays)
        if (notPassed(rayQuery, passed) && !cutAway(rayQuery)) {
            rayQueryConfirmIntersectionEXT(rayQuery);
        }
    }
    if (rayQueryGetIntersectionTypeEXT(rayQuery, true) != gl_RayQueryCommittedIntersectionTriangleEXT) {
        return false;
    }
    instance = rayQueryGetIntersectionInstanceIdEXT(rayQuery, true);
    primitive = rayQueryGetIntersectionPrimitiveIndexEXT(rayQuery, true);
    t = rayQueryGetIntersectionTEXT(rayQuery, true);
    MeshInfo mesh = meshInfos[rayQueryGetIntersectionInstanceCustomIndexEXT(rayQuery, true)];
    uint firstIndex = mesh.indexOffset + uint(primitive) * 3;
    uvec3 corner = mesh.vertexOffset + uvec3(indices[firstIndex + 0], indices[firstIndex + 1], indices[firstIndex + 2]);
    vec2 bary = rayQueryGetIntersectionBarycentricsEXT(rayQuery, true);
    entering = entersMesh(mesh, corner, vec3(1.0 - bary.x - bary.y, bary.x, bary.y), instanceWorldToObject(instance),
                          direction);
    return true;
}

// Going on along a ray from a surface at t: from the same distance (a surface right there, like a floor under a fog
// box, still counts), only that triangle left out (rounding: it may be a hair closer)
float pastSurface(float tMin, float t) {
    return max(tMin, t - max(1e-4, t * 1e-5));
}

// The volumes (fog, glass) p is in: their surfaces straight above it, in order; a volume whose first one is seen from
// the inside holds p. Up to MAX_VOLUME_CROSSINGS surfaces and MAX_MEDIA volumes. (Nested ones come out in the order
// they are seen from p, the innermost first: as if entered from the outside in.)
uvec2 mediumAt(vec3 p) {
    uvec2 media = OUTSIDE;
    uvec2 seen = OUTSIDE;
    float tMin = 0.0;
    uvec4 passed = uvec4(~0u);
    uint mask = FOG_MASK | VARYING_FOG_MASK | (FEATURE_GLASS ? GLASS_MASK : 0u);
    uint held = 0;
    for (uint crossing = 0; crossing < MAX_VOLUME_CROSSINGS; crossing++) {
        uint instance;
        int primitive;
        float t;
        bool entering;
        if (!nextFogSurface(p, vec3(0.0, 1.0, 0.0), tMin, RAY_FAR, mask, passed, instance, primitive, t, entering)) {
            break;
        }
        if (!inMedium(seen, instance)) {
            if (mediaSlot(seen, MAX_MEDIA - 1) != NO_MEDIUM) {
                break;  // no room to remember more
            }
            seen = enterMedium(seen, instance);
            if (!entering) {
                setMediaSlot(media, held++, instance);  // behind the ones found before: those are further in
            }
        }
        tMin = pastSurface(tMin, t);
        passed = uvec4(instance, uint(primitive), passed.xy);
    }
    return media;
}

// The share of scattered light that goes into a direction at cosine cosTheta to the light's own, per solid angle
// (Henyey-Greenstein: g = 0: every way alike, towards 1: mostly on forward)
float fogPhase(float cosTheta, float g) {
    float d = 1.0 + g * g - 2.0 * g * cosTheta;
    return (1.0 - g * g) / (4.0 * PI * d * sqrt(d));
}

// A direction picked by fogPhase around `forward` (the ray's); pdf = fogPhase of it
vec3 sampleFogDirection(vec3 forward, float g, vec2 u, out float pdf) {
    float cosTheta = 1.0 - 2.0 * u.x;
    if (abs(g) > 1e-3) {
        float s = (1.0 - g * g) / (1.0 - g + 2.0 * g * u.x);
        cosTheta = clamp((1.0 + g * g - s * s) / (2.0 * g), -1.0, 1.0);
    }
    pdf = fogPhase(cosTheta, g);
    float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));
    float phi = 2.0 * PI * u.y;
    vec3 tangent, bitangent;
    buildBasis(forward, tangent, bitangent);
    return normalize(tangent * (sinTheta * cos(phi)) + bitangent * (sinTheta * sin(phi)) + forward * cosTheta);
}

// A stretch [a, b] of a ray inside fog (the volumes media, their majorant), for the lamps' light (see Intersection):
// kept with a chance of its majorant times its length among all so far
void addLampSpan(inout Intersection hit, float a, float b, uvec2 media, float majorant, inout uint seed) {
    float weight = majorant * (b - a);
    if (CACHE_UPDATE_PASS != 0 || punctualLightCount() == 0 || !(weight > 0.0)) {
        return;
    }
    hit.lampWeight += weight;
    if (randomFloat(seed) * hit.lampWeight < weight) {
        hit.lampSpan = vec2(a, b);
        hit.lampMedia = media;
        hit.lampMajorant = majorant;
    }
}

// The stretch of a ray (up to end) inside the bounding box of an instance's mesh; x >= y: none
vec2 boxSpan(uint instance, vec3 origin, vec3 direction, float end) {
    uint mesh = instances[instance].meshIndex;
    // In the mesh's space: the same t along the ray (the direction isn't normalized there)
    mat3 worldToObject = instanceWorldToObject(instance);
    vec3 o = worldToObject * (origin - instances[instance].objectToWorld[3]);
    vec3 d = worldToObject * direction;
    vec3 inverse = 1.0 / mix(d, vec3(1e-30), equal(d, vec3(0.0)));
    vec3 a = (meshInfos[mesh].boundsMin - o) * inverse;
    vec3 b = (meshInfos[mesh].boundsMax - o) * inverse;
    vec3 near = min(a, b), far = max(a, b);
    return vec2(max(max(near.x, near.y), max(near.z, 0.0)), min(min(far.x, far.y), min(far.z, end)));
}

// Varying fog in shadow rays: the share of light that gets through it between origin and origin + direction * end
// (see "Varying density"). Its volumes: the ones the ray starts in (media; the even ones are shadowTransmittance's), and
// the ones whose surfaces it crosses (crossed, noted in shadowTransmittance's traversal). Each is ratio-tracked along the
// ray's stretch through its bounding box: outside the mesh its density is 0, so that holds all its fog, whatever the
// mesh's shape, without knowing where the ray goes in and out.
float varyingTransmittance(vec3 origin, vec3 direction, float end, uvec2 media, uvec2 crossed, inout uint seed) {
    uvec2 volumes = crossed;
    for (uvec2 rest = media; firstMedium(rest) != NO_MEDIUM; rest = otherMedia(rest)) {
        if (fogVaries(mediumMaterial(firstMedium(rest)))) {
            volumes = enterMedium(volumes, firstMedium(rest));
        }
    }
    float transmittance = 1.0;
    for (uvec2 rest = volumes; firstMedium(rest) != NO_MEDIUM; rest = otherMedia(rest)) {
        vec2 span = boxSpan(firstMedium(rest), origin, direction, end);
        if (span.x < span.y) {
            transmittance *= ratioTrack(firstMedium(rest), origin, direction, span.x, span.y, seed);
            if (transmittance <= 0.0) {
                return 0.0;
            }
        }
    }
    return transmittance;
}
