// raytrace.comp, part: ReSTIR DI: reservoirs and samples.
// Included by raytrace.comp in order: uses what the parts before it define.

// ---------------------------------------------------------------------------------------------
// ReSTIR DI (Bitterli et al. 2020; its shortcuts and measurements: LIMITATIONS.md)
//
// Direct light at the first surface the camera sees, if it is rough enough (restirSurface). Instead of a shadow
// ray per kind of light, the RESTIR_PASS draws several candidate light samples per pixel (glowing triangles,
// punctual lights, the sky) and keeps one of them, picked with a chance proportional to how much light it would
// bring if nothing blocked it (its "target", restirTarget): resampled importance sampling (RIS, Talbot et al.
// 2005). The kept sample and its weight W go into the pixel's reservoir. The image pass traces one shadow ray to
// it and counts its light times W. W = (sum of the candidates' weights) / (the kept sample's target) keeps the
// result correct on average, whatever the target is; the closer the target is to the real light (shadows aside),
// the less noise. The sun is not in it: it keeps its own shadow ray (slot 0 of the light loop).
//
// The candidates come from two kinds of strategies: light sampling (each kind of light has its own candidates) and
// BRDF sampling (a direction picked by the surface's reflection, whatever light its ray finds). The light ones
// find small and distant lights; the BRDF ones the light a glossy surface reflects in a narrow cone, or a large
// light very close by. A candidate's weight is target / (sum over the strategies that could have produced it of
// their number of candidates * their pdf of it): generalized RIS with the balance heuristic (Lin et al. 2022).
// A light the BRDF rays can't find (punctual lights, or one whose triangles are left out of light sampling, which
// the bounce ray counts instead) only has its light-sampling term.
//
// A sample is stored as which light and where on it, so that another pixel or the next frame can use it too. Its
// light, target and W are per unit of the light's own measure: per area of a glowing triangle, per solid angle of
// the sky, per punctual light. These don't depend on the point being lit, so a sample moves between pixels without
// a change of variables (a Jacobian). The light loop's solid-angle quantities differ from them by cos / distance^2.
//
// Temporal reuse (restirTemporal): last frame's reservoir of the same surface point is merged in. With a still
// scene, a pixel's reservoir soon stands for the candidates of many frames (up to restirMaxHistory times this
// frame's), and the shadow ray in the RESTIR_PASS (visibility reuse) keeps blocked samples from being passed on.
//
// Spatial reuse (restirSpatial): each pixel also resamples a few nearby pixels' reservoirs, in a pass of its own.
//
// Buffers: reservoirs and surfaces have two halves, this frame's and last frame's, swapped every frame. Reservoirs
// have a third part with the spatial pass: the RESTIR_PASS's result (restirTemporalIndex), which the spatial pass
// reads, writing this frame's half.

const uint RESTIR_NOT_USED = 0xFFFFFFFFu;  // Reservoir.light: this pixel's first surface does its own light sampling
const uint LIGHT_KIND_NONE = 0;            // nothing could light the surface
const uint LIGHT_KIND_TRIANGLE = 1;        // a glowing triangle; point: barycentrics (2 x 16 bits)
const uint LIGHT_KIND_PUNCTUAL = 2;        // a punctual light
const uint LIGHT_KIND_SKY = 3;             // the sky; point: see encodeSkyPoint

// Must match GpuReservoir in Renderer.h
struct Reservoir {
    uint light;   // kind << 30 | index
    uint point;   // where on the light
    float W;      // the kept sample's weight: its light * W estimates the direct light (0: nothing)
    float M;      // how many candidates it stands for
};
layout(binding = 26, scalar) buffer Reservoirs { Reservoir reservoirs[]; };  // 3 parts of one per render pixel

// What the reuse needs to know about a pixel's first surface: enough for its target (restirTarget) without its
// textures. 48 bytes; the material in 8 bits per value. Must match GpuRestirSurface in Renderer.h.
struct RestirSurface {
    vec3 position;
    uint normal;             // octahedral, 2 x 16 bits
    uint geometricNormal;
    uint albedoRoughness;    // 4 x 8 bits
    uint material;           // metallic, (ior - 1) / 2, diffuse transmission (8 bits each)
    uint diffuseTransmissionColor;
    // Where the camera sees it (its position, or in mirrors its image: see "Mirrors and ReSTIR") and the way
    // towards the viewer (octahedral): for view directions and depths, and to find it on last frame's image
    vec3 seenAt;
    uint view;
};
layout(binding = 27, scalar) buffer RestirSurfaces { RestirSurface restirSurfaces[]; };  // 2 halves, as above

uint restirPixelCount() {
    uvec2 size = uvec2(imageSize(radianceImage));
    return size.x * size.y;
}

// Where a pixel's reservoir and surface are, in this frame's half (previousFrame: last frame's)
uint restirIndex(ivec2 pixel, bool previousFrame) {
    uint part = previousFrame ? 1u - scene.restirCurrentHalf : scene.restirCurrentHalf;
    return part * restirPixelCount() + uint(pixel.y) * uint(imageSize(radianceImage).x) + uint(pixel.x);
}

// Whether the spatial pass runs (restirSpatial)
bool restirSpatialOn() {
    return scene.restirSpatialNeighbors + scene.restirDisocclusionNeighbors > 0;
}

// Where the RESTIR_PASS leaves a pixel's reservoir for the spatial pass (reservoirs only)
uint restirTemporalIndex(ivec2 pixel) {
    return 2u * restirPixelCount() + uint(pixel.y) * uint(imageSize(radianceImage).x) + uint(pixel.x);
}

// seenAt: where the camera sees it, v: towards the viewer
RestirSurface packRestirSurface(Hit hit, vec3 seenAt, vec3 v) {
    RestirSurface surface;
    surface.position = hit.position;
    surface.seenAt = seenAt;
    surface.view = encodeDirection(v);
    surface.normal = encodeDirection(hit.normal);
    surface.geometricNormal = encodeDirection(hit.geometricNormal);
    surface.albedoRoughness = packUnorm4x8(vec4(hit.albedo, hit.roughness));
    surface.material = packUnorm4x8(vec4(hit.metallic, (hit.ior - 1.0) * 0.5, hit.diffuseTransmission, 0.0));
    surface.diffuseTransmissionColor = packUnorm4x8(vec4(hit.diffuseTransmissionColor, 0.0));
    return surface;
}

// A Hit with what restirTarget reads (the rest stays unset)
Hit unpackRestirSurface(RestirSurface surface) {
    Hit hit;
    hit.found = true;
    hit.position = surface.position;
    hit.normal = decodeDirection(surface.normal);
    hit.geometricNormal = decodeDirection(surface.geometricNormal);
    vec4 albedoRoughness = unpackUnorm4x8(surface.albedoRoughness);
    hit.albedo = albedoRoughness.rgb;
    hit.roughness = albedoRoughness.a;
    vec4 material = unpackUnorm4x8(surface.material);
    hit.metallic = material.x;
    hit.ior = 1.0 + 2.0 * material.y;
    hit.diffuseTransmission = FEATURE_DIFFUSE_TRANSMISSION ? material.z : 0.0;
    hit.diffuseTransmissionColor = unpackUnorm4x8(surface.diffuseTransmissionColor).rgb;
    hit.transmission = 0.0;  // ReSTIR surfaces have none
    hit.lightThrough = false;
    return hit;
}

// The way from a stored surface towards whoever saw it (the camera, or in mirrors its mirror image)
vec3 restirSurfaceView(RestirSurface surface) {
    return decodeDirection(surface.view);
}

// A stored surface's view depth with this frame's camera: of where the camera sees it
float restirSurfaceDepth(RestirSurface surface) {
    return dot(surface.seenAt - scene.cameraPosition.xyz, scene.cameraForward.xyz);
}

// Where a stored surface (this frame's) was on last frame's image: its point moved back by its object's motion,
// or its image in mirrors, which stands still with the mirrors (moving mirrors aren't followed)
vec4 restirPreviousPoint(RestirSurface surface, uint instanceIndex) {
    return surface.seenAt != surface.position ? vec4(surface.seenAt, 1.0)
                                              : instanceMotion(instanceIndex) * vec4(surface.position, 1.0);
}

// Surfaces that use ReSTIR: rough enough that a picked light sample finds the light they reflect. A near-mirror
// reflects a light in a tiny cone that a bounce ray finds far better; those keep the usual light sampling + MIS.
bool restirSurface(Hit hit) {
    return hit.roughness >= scene.restirMinRoughness && hit.transmission <= 0.0 && !isFog(hit);
}
