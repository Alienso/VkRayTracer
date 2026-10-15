// raytrace.comp, part: Light sampling at a surface (next event estimation).
// Included by raytrace.comp in order: uses what the parts before it define.

// ---------------------------------------------------------------------------------------------
// Light sampling ("next event estimation"): at every surface, one shadow ray towards each kind of light (the sun,
// the sky, the glowing triangles, the punctual lights). Each kind only picks its sample here: a direction, how far
// the shadow ray goes, and how much light arrives from there divided by the chance of picking it. The rest is the
// same for all of them and done once, in tracePath's light loop (the surface's response, the shadow ray, MIS).
// Keeping that part in one place keeps the shader small: GLSL inlines every call, and four copies of the shadow
// ray and the BRDF made it so large that it ran much slower, even where those lights were switched off.

const uint MIS_NONE = 0;          // nothing else finds this light (punctual lights, the sun without size)
const uint MIS_UNLESS_GLASS = 1;  // bounce rays find it too, except through glass or when the path ends in the cache
const uint MIS_UNLESS_SOLID = 2;  // bounce rays find it too, also through thin panes, except through solid glass (sky)

struct LightSample {
    vec3 direction;   // world space, from the surface towards the light
    float distance;   // how far the shadow ray goes
    vec3 light;       // light arriving at a surface facing it, divided by the pdf of picking this sample; 0 = none
    float pdf;        // per solid angle, for MIS
    uint mis;         // MIS_*
    bool shadowed;    // false: no shadow ray (the sun with "Shadows" off)
};

// Where a shadow ray towards light sample s starts: just off the surface (offsetRay), on the side the light is
// (behind: the far side, light passing through a thin surface). s.distance is measured from the hit itself, so the
// ray gets shorter by how much closer to the light it starts.
vec3 shadowRayStart(Hit hit, bool behind, inout LightSample s) {
    vec3 origin = offsetRay(hit.position, behind ? -hit.geometricNormal : hit.geometricNormal);
    s.distance -= dot(origin - hit.position, s.direction);
    return origin;
}

LightSample noLightSample() {
    LightSample s;
    s.direction = vec3(0.0, 1.0, 0.0);
    s.distance = 0.0;
    s.light = vec3(0.0);
    s.pdf = 0.0;
    s.mis = MIS_NONE;
    s.shadowed = false;
    return s;
}

// Picks a glowing triangle for the point p (normal n), with the light tree (by what it could send to this
// point), or by its share of all light: binary search for the first one whose running sum exceeds a random
// number. Triangles with a bigger share cover a bigger stretch of [0, 1], so get picked more.
// -1: none (all glowing triangles are behind the surface, or a triangle with probability 0, see below).
int pickEmissiveTriangle(vec3 p, vec3 n, bool twoSided, float pick, out float pickProbability) {
    int lightIndex;
    if (scene.lightTree != 0) {
        lightIndex = pickFromLightTree(p, n, twoSided, pick, pickProbability);
        if (lightIndex < 0) {
            return -1;
        }
    } else {
        uint low = 0, high = scene.emissiveTriangleCount - 1;
        while (low < high) {
            uint middle = (low + high) / 2;
            if (emissiveTriangles[middle].cdf < pick) {
                low = middle + 1;
            } else {
                high = middle;
            }
        }
        lightIndex = int(low);
        pickProbability = emissiveTriangles[low].probability;
    }
    // Triangles with probability 0 (a black part of an emissive texture) have no stretch of [0, 1] of their own,
    // but the search can still land on one when the random number is exactly a boundary (e.g. 0). Dividing by
    // its pdf of 0 would give infinity, which would stick in the pixel's running average forever.
    return pickProbability > 0.0 ? lightIndex : -1;
}

// How much a glowing triangle could light the point p (normal n), per unit of its power: roughly its solid angle
// (facing cosine over the distance squared) times the surface's cosine, both towards its center. Never 0 (each
// cosine at least 0.05, the area added to the distance squared), so every triangle that may light p keeps a chance
// (see sampleEmissiveLight).
float lightCandidateReach(EmissiveTriangle light, vec3 p, vec3 n, bool twoSided) {
    vec3 toCenter = (light.p0 + light.p1 + light.p2) / 3.0 - p;
    float distanceSquared = dot(toCenter, toCenter);
    float cosine = dot(n, toCenter) * inversesqrt(max(distanceSquared, 1e-12));
    vec3 lightNormal = normalize(cross(light.p1 - light.p0, light.p2 - light.p0));
    float cosLight = abs(dot(lightNormal, toCenter)) * inversesqrt(max(distanceSquared, 1e-12));
    return max(twoSided ? abs(cosine) : cosine, 0.05) * max(cosLight, 0.05) / (distanceSquared + light.area);
}

// One randomly picked point on the glowing triangles, seen from surfacePoint. random: x picks the
// triangle, y and z the point, w which candidate is kept.
// Candidates (scene.lightCandidates, resampled importance sampling; picking by power only, 1 with the tree): picking
// by power alone sends many shadow rays to lights that are far away or behind the surface. So pick several, keep one
// with a chance proportional to target / pickProbability, target = its power x lightCandidateReach, and scale its
// light by the mean of those weights over the kept one's: on average exactly the light of all triangles.
// The candidates cost a pick and a read each, no ray. With 1, it is the plain pick.
// MIS (with the bounce rays) still uses the pick's pdf, here and where bounce rays hit the triangle: the two weights
// still add up to 1 everywhere, so nothing is counted twice or lost; they are just less well balanced.
LightSample sampleEmissiveLight(Hit hit, vec3 shadingNormal, vec3 surfacePoint, vec4 random, inout uint seed) {
    LightSample s = noLightSample();
    // 1: pick a triangle
    bool twoSided = hit.diffuseTransmission > 0.0 || hit.lightThrough;
    int lightIndex = -1;
    float pickProbability = 0.0;
    float weightSum = 0.0;
    float keptWeight = 0.0;
    float u = random.w;
    [[dont_unroll]] for (uint i = 0u; i < scene.lightCandidates; i++) {
        float candidateProbability;
        int candidate = pickEmissiveTriangle(hit.position, shadingNormal, twoSided,
                                             i == 0u ? random.x : randomFloat(seed), candidateProbability);
        if (candidate < 0) {
            continue;
        }
        EmissiveTriangle c = emissiveTriangles[candidate];
        float weight = c.probability * lightCandidateReach(c, hit.position, shadingNormal, twoSided)
                       / candidateProbability;
        weightSum += weight;
        float keep = weight / weightSum;  // one random number for all: rescaled after each choice
        if (u < keep) {
            u /= keep;
            lightIndex = candidate;
            pickProbability = candidateProbability;
            keptWeight = weight;
        } else {
            u = (u - keep) / (1.0 - keep);
        }
        u = min(u, 0.99999994);
    }
    if (lightIndex < 0) {
        return s;
    }
    float candidateScale = weightSum / (float(scene.lightCandidates) * keptWeight);
    EmissiveTriangle light = emissiveTriangles[lightIndex];

    // 2: a point on it
    vec3 weights;
    if (!pointOnTriangle(light, surfacePoint, random.yz, weights)) {
        return s;
    }
    vec3 lightPoint = light.p0 * weights.x + light.p1 * weights.y + light.p2 * weights.z;
    vec2 lightUv = light.uv0 * weights.x + light.uv1 * weights.y + light.uv2 * weights.z;

    vec3 toLight = lightPoint - surfacePoint;
    float distance = length(toLight);
    s.direction = toLight / distance;
    vec3 lightNormal = normalize(cross(light.p1 - light.p0, light.p2 - light.p0));
    float cosLight = abs(dot(lightNormal, s.direction));  // glowing triangles shine from both sides, like everything we hit
    if (cosLight <= 1e-6) {
        return s;  // seen exactly edge-on
    }
    s.distance = distance - stopShortOf(lightPoint, lightNormal, cosLight);  // the light itself would count as a blocker
    s.pdf = lightPdf(lightIndex, pickProbability, surfacePoint, distance, cosLight);
    s.light = emissionAt(light.materialIndex, lightUv) / s.pdf * candidateScale;
    s.mis = scene.emissiveSampling == EMISSIVE_MIS ? MIS_UNLESS_GLASS : MIS_NONE;
    s.shadowed = true;
    return s;
}
