// raytrace.comp, part: ReSTIR GI: reservoirs and samples.
// Included by raytrace.comp in order: uses what the parts before it define.

// ---------------------------------------------------------------------------------------------
// ReSTIR GI (Ouyang et al. 2021; its shortcuts and measurements: LIMITATIONS.md)
//
// The light that reaches a ReSTIR surface x1 (the first one, restirSurface) via another surface x2. The image path
// bounces off x1 as usual and finds x2; at a surface that can be reused (restirGiSampleSurface) everything the path
// collects from x2 on (x2's direct light, the radiance cache, deeper bounces) is the light leaving x2 towards x1,
// Lo. That sample (x2, its normal, Lo) goes into the pixel's GI reservoir with W = 1 / pdf of the bounce direction,
// instead of into the image. x2's own glow and what the bounce ray finds without hitting a surface (the sky, the
// sun) stay x1's: they depend on x1 (MIS with its light sampling), and ReSTIR DI or the light sampling count them.
// After the image pass, the samples are reused like ReSTIR DI's (restirGiTemporal), and a last pass (restirGiShade)
// adds BRDF(x1) * cos * Lo * W of each pixel's kept sample.
//
// A sample moved to another first surface keeps its x2 and Lo ("reconnection", Ouyang et al. 2021): only the
// direction and distance from x1 change. Its W is per solid angle at the first surface it came from, so it is
// scaled by the Jacobian, how much larger x2 looks from the new surface: J = G(new) / G(old) with
// G = cos at x2 / distance^2 (restirGiGeometry). Next to x2, J grows without bound: a sample found from far away,
// reused right next to its surface, would get a huge weight. As RTXDI, a reuse with J outside 1/10..10 is skipped,
// and J is clamped to 1/3..3 (a bias: some light lost near corners, for fewer fireflies).

// Must match GpuGiReservoir in Renderer.h
struct GiReservoir {
    vec3 position;   // x2
    uint normal;     // x2's geometric normal, towards x1 (octahedral)
    vec3 radiance;   // Lo: the light leaving x2 towards x1
    float W;         // the sample's weight: BRDF(x1) * cos * Lo * W estimates the light (0: no sample)
    float M;         // how many samples it stands for (0: not a GI pixel)
    uint age;        // frames since the sample was found (bits 0-15); GI_SEEN: found by this pixel's own path
};
const uint GI_SEEN = 0x10000u;  // x1 is known to see x2: shading needs no visibility ray
// 3 parts of one per render pixel: this frame's and last frame's (as restirIndex), and the image pass's fresh samples
layout(binding = 29, scalar) buffer GiReservoirs { GiReservoir giReservoirs[]; };

// Where the image pass leaves a pixel's fresh GI sample
uint restirGiFreshIndex(ivec2 pixel) {
    return 2u * restirPixelCount() + uint(pixel.y) * uint(imageSize(radianceImage).x) + uint(pixel.x);
}

// Second surfaces whose light can be reused from another first surface: not glass (its light depends on where it
// is seen from through it), and not too smooth (likewise; restirGiMinRoughness, 0 = any).
bool restirGiSampleSurface(Hit hit) {
    return hit.transmission <= 0.0 && !hit.solid && hit.roughness >= scene.restirGiMinRoughness && !isFog(hit);
}

// A sky direction in 32 bits: the cell of the sky's sampling (see environmentCellPdf) and 32 x 32 places within
// it. The cell is kept exactly: rounded across a cell's edge, a direction would be weighted with the wrong cell's
// pdf, a bias where the sky's brightness jumps (a sun in the map). Evenly spread directions within a cell stay
// evenly spread over its places, so the cell's pdf stays right for them.
uint encodeSkyPoint(vec3 direction) {
    uint height = scene.environmentHeight, width = scene.environmentWidth;
    vec2 uv = environmentUv(direction);
    uint row = min(uint(uv.y * float(height)), height - 1);
    uint column = min(uint(uv.x * float(width)), width - 1);
    // within the cell: around evenly, and evenly in cos (as sampleEnvironment picks)
    float cos0 = cos(PI * float(row) / float(height));
    float cos1 = cos(PI * float(row + 1) / float(height));
    vec2 within = clamp(vec2(uv.x * float(width) - float(column), (direction.y - cos0) / (cos1 - cos0)), 0.0, 1.0);
    uvec2 place = min(uvec2(within * 32.0), uvec2(31));
    return ((row * width + column) << 10) | (place.x << 5) | place.y;
}

vec3 decodeSkyPoint(uint point) {
    uint height = scene.environmentHeight, width = scene.environmentWidth;
    uint cell = point >> 10;
    uint row = cell / width, column = cell % width;
    vec2 within = (vec2((point >> 5) & 31u, point & 31u) + 0.5) / 32.0;
    float cos0 = cos(PI * float(row) / float(height));
    float cos1 = cos(PI * float(row + 1) / float(height));
    return environmentDirection((float(column) + within.x) / float(width), mix(cos0, cos1, within.y));
}

// The sample a stored light and point stand for, seen from surfacePoint. s.light: the light arriving at
// a surface facing it per unit of the light's measure (see above; not divided by any pdf). s.pdf: per unit of that
// measure, the chance of picking this point once the light is picked (triangles: by solid angle cos / (distance^2 *
// solid angle), or by area 1 / area, see pointOnTriangle;
// the sky: its sampling; punctual lights: 1). toSolidAngle: solid angle per unit of the measure (triangles:
// cos / distance^2, else 1), to turn a pdf per solid angle (BRDF sampling) into one per unit of the measure.
LightSample restirLightSample(uint light, uint point, vec3 surfacePoint, out float toSolidAngle) {
    toSolidAngle = 1.0;
    uint kind = light >> 30;
    uint index = light & 0x3FFFFFFFu;
    LightSample s = noLightSample();
    if (kind == LIGHT_KIND_TRIANGLE) {
        EmissiveTriangle triangle = emissiveTriangles[index];
        vec2 bary = unpackUnorm2x16(point);
        vec3 weights = vec3(max(1.0 - bary.x - bary.y, 0.0), bary);
        vec3 lightPoint = triangle.p0 * weights.x + triangle.p1 * weights.y + triangle.p2 * weights.z;
        vec2 lightUv = triangle.uv0 * weights.x + triangle.uv1 * weights.y + triangle.uv2 * weights.z;
        vec3 toLight = lightPoint - surfacePoint;
        float distance = length(toLight);
        s.direction = toLight / distance;
        vec3 lightNormal = normalize(cross(triangle.p1 - triangle.p0, triangle.p2 - triangle.p0));
        float cosLight = abs(dot(lightNormal, s.direction));
        if (cosLight <= 1e-6) {
            return noLightSample();  // seen exactly edge-on
        }
        s.distance = distance - stopShortOf(lightPoint, lightNormal, cosLight);  // the light itself would block it
        toSolidAngle = cosLight / (distance * distance);
        float solidAngle = sphericalSolidAngle(triangle, surfacePoint);
        s.pdf = solidAngle > 0.0 ? toSolidAngle / solidAngle : 1.0 / max(triangle.area, 1e-12);
        s.light = emissionAt(triangle.materialIndex, lightUv) * toSolidAngle;
    } else if (kind == LIGHT_KIND_PUNCTUAL) {
        s = punctualLightSample(punctualLights[index], surfacePoint);
        s.pdf = 1.0;
    } else if (kind == LIGHT_KIND_SKY) {
        s.direction = decodeSkyPoint(point);
        s.distance = SKY_DISTANCE;
        s.pdf = environmentPdf(s.direction);
        s.light = environmentLight(s.direction);
    }
    s.mis = MIS_NONE;
    s.shadowed = true;
    return s;
}

// The target: how bright the sample would make the surface (seen from direction v) if nothing blocked it.
// Same rules as the light loop: in front of the surface, or behind one that lets light through.
// brdfPdf: the chance (per solid angle) of BRDF sampling picking the sample's direction.
float restirTarget(Hit hit, vec3 v, LightSample s, out float brdfPdf) {
    brdfPdf = 0.0;
    float cosLight = dot(hit.normal, s.direction);
    float side = dot(hit.geometricNormal, s.direction);
    bool behind = cosLight < 0.0 && side < 0.0 && hit.diffuseTransmission > 0.0;
    if (!((cosLight > 0.0 && side > 0.0) || behind)) {
        return 0.0;
    }
    return luminance(s.light * evaluateBrdfWorld(hit, hit.normal, v, s.direction, brdfPdf) * abs(cosLight));
}

bool isGlassAt(uint materialIndex, vec2 uv);
uint primaryHitIndex(ivec2 pixel);

// Whether a candidate hit stops a ray the way it stops shadow rays (see shadowTransmittance): holes of cut-outs
// (by chance, for see-through ones) and glass let it through.
bool blocksLight(uint meshIndex, int primitive, vec2 bary, inout uint seed) {
    MeshInfo mesh = meshInfos[meshIndex];
    uint firstIndex = mesh.indexOffset + uint(primitive) * 3;
    vec2 uv0 = vertices[mesh.vertexOffset + indices[firstIndex + 0]].uv;
    vec2 uv1 = vertices[mesh.vertexOffset + indices[firstIndex + 1]].uv;
    vec2 uv2 = vertices[mesh.vertexOffset + indices[firstIndex + 2]].uv;
    vec2 uv = uv0 * (1.0 - bary.x - bary.y) + uv1 * bary.x + uv2 * bary.y;
    float opacity = opacityAt(mesh.materialIndex, uv);
    if (opacity < 1.0 && (opacity <= 0.0 || randomFloat(seed) >= opacity)) {
        return false;
    }
    return !isGlassAt(mesh.materialIndex, uv);
}

// Glass: lets light through without being metal (shadow rays and the BRDF candidates' rays pass it)
bool isGlassAt(uint materialIndex, vec2 uv) {
    Material material = materials[materialIndex];
    if (FOG != 0 && material.invisible != 0) {
        return true;  // (fog: invisible surfaces)
    }
    float transmission = FEATURE_GLASS ? material.transmission : 0.0;
    if (transmission > 0.0 && material.transmissionTexture >= 0) {
        transmission *= sampleTextureFull(material.transmissionTexture, material.transmissionUvTransform, uv).r;
    }
    float metallic = material.metallic;
    if (transmission > 0.0 && material.metallicRoughnessTexture >= 0) {
        metallic *= sampleTextureFull(material.metallicRoughnessTexture, material.metallicRoughnessUvTransform, uv).b;
    }
    return transmission > 0.0 && metallic < 1.0;
}

bool isGlassHit(Intersection hit) {
    MeshInfo mesh = meshInfos[hit.meshIndex];
    if ((!FEATURE_GLASS || materials[mesh.materialIndex].transmission <= 0.0)
        && (FOG == 0 || materials[mesh.materialIndex].invisible == 0)) {
        return false;  // almost every hit: no vertex or texture reads
    }
    uint firstIndex = mesh.indexOffset + uint(hit.primitive) * 3;
    vec2 uv0 = vertices[mesh.vertexOffset + indices[firstIndex + 0]].uv;
    vec2 uv1 = vertices[mesh.vertexOffset + indices[firstIndex + 1]].uv;
    vec2 uv2 = vertices[mesh.vertexOffset + indices[firstIndex + 2]].uv;
    vec2 uv = uv0 * (1.0 - hit.barycentrics.x - hit.barycentrics.y) + uv1 * hit.barycentrics.x
              + uv2 * hit.barycentrics.y;
    return isGlassAt(mesh.materialIndex, uv);
}

// The image path's first bounce at this pixel's ReSTIR surface (see BounceHit). bounces = false: the path ends.
void storeBounceHit(bool bounces, bool found, Intersection hit, vec3 direction, vec3 weight, float pdf,
                    float lobeRoughness, bool transmitted, bool specularLobe, bool throughPortal) {
    BounceHit stored;
    stored.meshIndex = !bounces ? BOUNCE_NONE : found ? hit.meshIndex : ~0u;
    stored.instanceIndex = hit.instanceIndex;
    stored.primitive = hit.primitive;
    stored.barycentrics = hit.barycentrics;
    stored.t = hit.t;
    stored.direction = direction;
    stored.weightRG = packHalf2x16(weight.rg);
    stored.weightBRoughness = (packHalf2x16(vec2(weight.b, 0.0)) & 0xFFFFu)
                              | (uint(round(clamp(lobeRoughness, 0.0, 1.0) * 255.0)) << 16)
                              | ((transmitted ? 1u : 0u) << 24) | ((specularLobe ? 2u : 0u) << 24)
                              | (throughPortal ? BOUNCE_RETRACE : 0u);
    stored.pdf = pdf;
    bounceHits[primaryHitIndex(ivec2(gl_GlobalInvocationID.xy))] = stored;
}

// The BRDF candidates' ray: the first surface that isn't glass. Through glass like the shadow rays, so the BRDF
// candidates find exactly the lights that light candidates can reach: the MIS weights count on that (a light
// behind a window would otherwise get the BRDF share of its weight but never be found by a BRDF ray).
// Stops at a portal's opening, as shadow rays do: false with travelled = -1 then (no light).
bool intersectTowardsLight(vec3 origin, vec3 direction, inout uint seed, out Intersection hit) {
    float portalT = RAY_FAR;
    int portal = portalCount() > 0 ? nearestPortal(origin, direction, RAY_FAR, -1, portalT) : -1;
    hit.travelled = 0.0;
    hit.crossings = 0;
    rayQueryEXT rayQuery;
    rayQueryInitializeEXT(rayQuery, tlas, SHADOW_RAY_FLAGS, 0xFF, origin, 0.0, direction,
                          portal >= 0 ? max(portalT - PORTAL_GAP, 0.0) : RAY_FAR);
    while (rayQueryProceedEXT(rayQuery)) {
        if (SHADOW_RAY_FLAGS == gl_RayFlagsNoneEXT &&
            rayQueryGetIntersectionTypeEXT(rayQuery, false) == gl_RayQueryCandidateIntersectionTriangleEXT &&
            blocksLight(rayQueryGetIntersectionInstanceCustomIndexEXT(rayQuery, false),
                        rayQueryGetIntersectionPrimitiveIndexEXT(rayQuery, false),
                        rayQueryGetIntersectionBarycentricsEXT(rayQuery, false), seed)) {
            rayQueryConfirmIntersectionEXT(rayQuery);
        }
    }
    if (rayQueryGetIntersectionTypeEXT(rayQuery, true) != gl_RayQueryCommittedIntersectionTriangleEXT) {
        hit.travelled = portal >= 0 ? -1.0 : 0.0;
        return false;
    }
    hit.meshIndex = rayQueryGetIntersectionInstanceCustomIndexEXT(rayQuery, true);
    hit.instanceIndex = rayQueryGetIntersectionInstanceIdEXT(rayQuery, true);
    hit.primitive = rayQueryGetIntersectionPrimitiveIndexEXT(rayQuery, true);
    hit.barycentrics = rayQueryGetIntersectionBarycentricsEXT(rayQuery, true);
    hit.t = rayQueryGetIntersectionTEXT(rayQuery, true);
    return true;
}

// The RESTIR_PASS's candidates for a surface seen from direction v. Streaming RIS: each candidate replaces the
// kept one with a chance of its weight / the sum of the weights so far.
Reservoir restirCandidates(Hit hit, vec3 v, inout uint seed) {
    vec3 surfacePoint = hit.position;
    bool twoSided = hit.diffuseTransmission > 0.0;
    uint triangles = scene.restirTriangleCandidates;
    uint punctual = scene.restirPunctualCandidates;
    uint sky = scene.restirSkyCandidates;
    uint lightCandidates = triangles + punctual + sky;
    uint total = lightCandidates + scene.restirBrdfCandidates;
    Reservoir r;
    r.light = LIGHT_KIND_NONE << 30;
    r.point = 0;
    r.M = float(total);
    float weightSum = 0.0;
    float keptTarget = 0.0;
    [[dont_unroll]] for (uint i = 0; i < total; i++) {
        uint light;
        uint point = 0;
        float pickProbability = 0.0;  // triangles: the chance that light sampling picks this one
        if (i < triangles) {
            int index = pickEmissiveTriangle(hit.position, hit.normal, twoSided, randomFloat(seed), pickProbability);
            if (index < 0) {
                continue;
            }
            vec3 weights;  // as in sampleEmissiveLight
            if (!pointOnTriangle(emissiveTriangles[index], surfacePoint, vec2(randomFloat(seed), randomFloat(seed)),
                                 weights)) {
                continue;
            }
            point = packUnorm2x16(weights.yz);
            light = (LIGHT_KIND_TRIANGLE << 30) | uint(index);
        } else if (i < triangles + punctual) {
            uint count = punctualLightCount();
            uint index = min(uint(randomFloat(seed) * float(count)), count - 1);
            light = (LIGHT_KIND_PUNCTUAL << 30) | index;
        } else if (i < lightCandidates) {
            float pdf;
            point = encodeSkyPoint(sampleEnvironment(vec4(randomFloat(seed), randomFloat(seed), randomFloat(seed),
                                                          randomFloat(seed)), pdf));
            light = LIGHT_KIND_SKY << 30;
        } else {
            // BRDF sampling: a direction as the bounce ray would pick it, and the light its ray finds (if light
            // sampling could have picked it too; otherwise the bounce ray counts that light, see tracePath)
            ShadingFrame frame = makeShadingFrame(hit.normal);
            vec3 l, weight;
            float pdf, lobeRoughness;
            bool transmitted, specularLobe;
            Intersection none = Intersection(0u, 0u, 0, vec2(0.0), 0.0, 0.0, 0u, false, NO_MEDIUM, vec2(0.0), OUTSIDE, 0.0,
                                             0.0);
            vec3 random = vec3(randomFloat(seed), randomFloat(seed), randomFloat(seed));
            if (!sampleBrdf(hit, toLocal(frame, v), random, l, weight, pdf, lobeRoughness, transmitted, specularLobe)) {
                if (i == lightCandidates) {
                    storeBounceHit(false, false, none, vec3(0.0), vec3(0.0), 0.0, 0.0, false, false, false);
                }
                continue;
            }
            vec3 direction = toWorld(frame, l);
            float side = dot(direction, hit.geometricNormal);
            // The first BRDF candidate's ray is also the image path's first bounce: traced like a path ray (it
            // stops at glass) and stored, so the image pass doesn't trace its own (part C4)
            bool pathBounce = i == lightCandidates;
            if (l.z < 0.0 ? side >= 0.0 : side <= 0.0) {
                if (pathBounce) {
                    storeBounceHit(false, false, none, direction, weight, pdf, lobeRoughness, transmitted, specularLobe,
                                   false);
                }
                continue;  // on the wrong side of the real triangle (see tracePath)
            }
            Intersection found;
            vec3 origin = offsetRay(hit.position, side < 0.0 ? -hit.geometricNormal : hit.geometricNormal);
            bool foundSurface;
            if (pathBounce) {
                vec3 rayOrigin = origin;
                vec3 rayDirection = direction;
                foundSurface = intersectClosest(rayOrigin, rayDirection, seed, found);
                bool throughPortal = found.travelled != 0.0;
                storeBounceHit(true, foundSurface, found, direction, weight, pdf, lobeRoughness, transmitted,
                               specularLobe, throughPortal);
                if (throughPortal) {
                    continue;  // not a light candidate: light sampling can't reach through a portal
                }
                // The candidate goes on through glass, as shadow rays do (rare: one more ray)
                if (foundSurface && isGlassHit(found)) {
                    foundSurface = intersectTowardsLight(origin + direction * found.t, direction, seed, found);
                }
            } else {
                foundSurface = intersectTowardsLight(origin, direction, seed, found);
            }
            if (foundSurface) {
                int firstLight = instanceFirstLight[found.instanceIndex];
                if (triangles == 0 || firstLight < 0) {
                    continue;
                }
                int index = firstLight + found.primitive;
                pickProbability = lightPickProbability(index, hit.position, hit.normal, twoSided);
                if (pickProbability <= 0.0) {
                    continue;
                }
                point = packUnorm2x16(found.barycentrics);
                light = (LIGHT_KIND_TRIANGLE << 30) | uint(index);
            } else {
                if (sky == 0 || found.travelled < 0.0) {
                    continue;  // (or the ray went into a portal)
                }
                point = encodeSkyPoint(direction);
                light = LIGHT_KIND_SKY << 30;
            }
        }
        // Evaluated from the stored (rounded) point, so the pdfs and the target are exactly the kept sample's
        float toSolidAngle;
        LightSample s = restirLightSample(light, point, surfacePoint, toSolidAngle);
        float brdfPdf;
        float target = restirTarget(hit, v, s, brdfPdf);
        uint kind = light >> 30;
        float sourcePdf;  // sum over the strategies of (number of candidates * pdf), per unit of the light's measure
        if (kind == LIGHT_KIND_PUNCTUAL) {
            sourcePdf = float(punctual) / float(punctualLightCount());
        } else {
            sourcePdf = (kind == LIGHT_KIND_TRIANGLE ? float(triangles) * pickProbability : float(sky)) * s.pdf
                        + float(scene.restirBrdfCandidates) * brdfPdf * toSolidAngle;
        }
        if (target <= 0.0 || !(sourcePdf > 0.0)) {
            continue;
        }
        float weight = target / sourcePdf;
        weightSum += weight;
        if (randomFloat(seed) * weightSum < weight) {
            r.light = light;
            r.point = point;
            keptTarget = target;
        }
    }
    r.W = keptTarget > 0.0 ? weightSum / keptTarget : 0.0;
    return r;
}

// The skylight at a ReSTIR surface: this share from the reservoir, the rest by the usual light sampling (the sky's
// shadow ray and the bounce ray's MIS share). Both are unbiased, so is their mix. With the reservoir alone, a pixel
// in light that comes through openings (Sponza's side aisle) was either fully lit or black, with one shadow ray
// deciding, and DLSS RR takes much of such light for noise: the aisle 6% too dark on DLSS's first frame, still 3%
// after 64. Half and half: 3.4% and 1.4%, the error and the cost (one shadow ray) about the same. 75/25 was no better.
const float RESTIR_SKY_SHARE = 0.5;

// The image pass's light sample at a ReSTIR surface: the reservoir's, its light times W (the sky's times
// RESTIR_SKY_SHARE)
LightSample restirReservoirSample(ivec2 pixel, vec3 surfacePoint) {
    Reservoir r = reservoirs[restirIndex(pixel, false)];
    if (!(r.W > 0.0)) {
        return noLightSample();
    }
    float toSolidAngle;
    LightSample s = restirLightSample(r.light, r.point, surfacePoint, toSolidAngle);
    s.light *= r.W * ((r.light >> 30) == LIGHT_KIND_SKY ? RESTIR_SKY_SHARE : 1.0);
    return s;
}

// The target of a reservoir's sample at a surface (0 for none)
float restirTargetOf(Reservoir r, Hit hit, vec3 v) {
    if ((r.light >> 30) == LIGHT_KIND_NONE) {
        return 0.0;
    }
    float toSolidAngle, brdfPdf;
    LightSample s = restirLightSample(r.light, r.point, hit.position, toSolidAngle);
    return restirTarget(hit, v, s, brdfPdf);
}

// A reservoir's sample without the surface it lights: what restirLightSample looks up (the triangle, its emission
// texture, the sky map), once, for targets at several surfaces (restirTargetAt). The spatial pass needs 3k + 1
// targets of only k + 1 samples.
struct RestirLightPoint {
    uint kind;        // LIGHT_KIND_*
    uint index;       // punctual lights: which
    vec3 position;    // triangles: the point; the sky: the direction towards it
    vec3 normal;      // triangles'
    vec3 emission;    // triangles: the emitted light; the sky: its light
};

RestirLightPoint restirLightPoint(Reservoir r) {
    RestirLightPoint p;
    p.kind = r.light >> 30;
    p.index = r.light & 0x3FFFFFFFu;
    p.position = vec3(0.0);
    p.normal = vec3(0.0, 1.0, 0.0);
    p.emission = vec3(0.0);
    if (p.kind == LIGHT_KIND_TRIANGLE) {
        EmissiveTriangle triangle = emissiveTriangles[p.index];
        vec2 bary = unpackUnorm2x16(r.point);
        vec3 weights = vec3(max(1.0 - bary.x - bary.y, 0.0), bary);
        p.position = triangle.p0 * weights.x + triangle.p1 * weights.y + triangle.p2 * weights.z;
        p.normal = normalize(cross(triangle.p1 - triangle.p0, triangle.p2 - triangle.p0));
        p.emission = emissionAt(triangle.materialIndex,
                                triangle.uv0 * weights.x + triangle.uv1 * weights.y + triangle.uv2 * weights.z);
    } else if (p.kind == LIGHT_KIND_SKY) {
        p.position = decodeSkyPoint(r.point);
        p.emission = environmentLight(p.position);
    }
    return p;
}

// restirTargetOf from a looked-up sample (the same value)
float restirTargetAt(RestirLightPoint p, Hit hit, vec3 v) {
    vec3 surfacePoint = hit.position;
    LightSample s = noLightSample();
    if (p.kind == LIGHT_KIND_TRIANGLE) {
        vec3 toLight = p.position - surfacePoint;
        float distance = length(toLight);
        s.direction = toLight / distance;
        float cosLight = abs(dot(p.normal, s.direction));
        if (cosLight <= 1e-6) {
            return 0.0;
        }
        s.light = p.emission * (cosLight / (distance * distance));
    } else if (p.kind == LIGHT_KIND_PUNCTUAL) {
        s = punctualLightSample(punctualLights[p.index], surfacePoint);
    } else if (p.kind == LIGHT_KIND_SKY) {
        s.direction = p.position;
        s.light = p.emission;
    } else {
        return 0.0;
    }
    float brdfPdf;
    return restirTarget(hit, v, s, brdfPdf);
}
