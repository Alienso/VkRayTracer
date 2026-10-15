// raytrace.comp, part: ReSTIR GI passes: temporal reuse, spatial reuse, shading.
// Included by raytrace.comp in order: uses what the parts before it define.

float restirGiGeometry(vec3 x1, GiReservoir r) {
    vec3 toFirst = x1 - r.position;
    float distanceSquared = dot(toFirst, toFirst);
    return max(dot(decodeDirection(r.normal), toFirst), 0.0) * inversesqrt(distanceSquared) / distanceSquared;
}

// The target of a GI sample at a first surface seen from direction v: luminance(BRDF * cos * Lo), with the light
// loop's rules (restirTarget). 0 if the surface would see x2 from behind.
float restirGiTarget(Hit hit, vec3 v, GiReservoir r) {
    vec3 toSample = r.position - hit.position;
    LightSample s = noLightSample();
    s.direction = normalize(toSample);
    if (dot(decodeDirection(r.normal), s.direction) >= 0.0) {
        return 0.0;
    }
    s.light = r.radiance;
    float brdfPdf;
    return restirTarget(hit, v, s, brdfPdf);
}

// Last frame's GI reservoir of the same surface point merged into r, this frame's fresh sample, as ReSTIR DI's
// (restirTemporal): generalized balance heuristic, each target taken at the other surface and converted to this
// surface's measure with the Jacobian (see "ReSTIR GI"). Without visibility in the targets (RTXDI's default,
// "basic"), a fresh sample last frame's surface can't see still gives it a share of its MIS weight: light lost
// where the two see different things. Last frame's surface is the same point but up to 3 pixels away (shuffle,
// fallback search), so near edges and far away they often do: Sponza's side aisle, lit through gaps, came out 6%
// too dark (whole image, Accumulation). With restirGiTemporalVisibility, as DI's: a ray from last frame's surface
// to the fresh sample. Last frame's own sample needs none: shading dropped it (W = 0) if its surface didn't see it.
bool restirGiVisible(Hit hit, vec3 x2, uint x2Normal, inout uint seed);
GiReservoir restirGiTemporal(GiReservoir r, ivec2 pixel) {
    RestirSurface own = restirSurfaces[restirIndex(pixel, false)];
    Hit hit = unpackRestirSurface(own);
    uint seed = pcgHash(pixelSeed(pixel) ^ 0x68e31da4u);
    uint previousIndex;
    Hit previousHit;
    vec3 previousV;
    if (!restirFindPrevious(hit, restirPreviousPoint(own, primaryHits[primaryHitIndex(pixel)].instanceIndex), true,
                            scene.restirGiPermutation != 0, seed, previousIndex, previousHit, previousV)) {
        return r;
    }
    GiReservoir previous = giReservoirs[previousIndex];
    if ((previous.age & 0xFFFFu) >= scene.restirGiMaxAge) {
        return r;  // held long enough (its Lo may be out of date too)
    }
    vec3 v = restirSurfaceView(own);
    float previousM = min(previous.M, scene.restirGiMaxHistory * r.M);

    // Targets of both samples at both surfaces, per solid angle at this one
    float currentHere = 0.0, currentThere = 0.0;
    if (r.W > 0.0) {
        currentHere = restirGiTarget(hit, v, r);
        float geometry = restirGiGeometry(hit.position, r);
        currentThere = geometry > 0.0
                       ? restirGiTarget(previousHit, previousV, r) * restirGiGeometry(previousHit.position, r) / geometry
                       : 0.0;
        if (scene.restirGiTemporalVisibility != 0 && currentThere > 0.0
                && !restirGiVisible(previousHit, r.position, r.normal, seed)) {
            currentThere = 0.0;
        }
    }
    float previousHere = 0.0, previousThere = 0.0, jacobian = 1.0;
    if (previous.W > 0.0) {
        float geometry = restirGiGeometry(previousHit.position, previous);
        jacobian = geometry > 0.0 ? restirGiGeometry(hit.position, previous) / geometry : 0.0;
        if (jacobian >= 0.1 && jacobian <= 10.0) {
            previousHere = restirGiTarget(hit, v, previous);
            previousThere = restirGiTarget(previousHit, previousV, previous) / jacobian;
        }
    }
    float currentSum = r.M * currentHere + previousM * currentThere;
    float previousSum = r.M * previousHere + previousM * previousThere;
    float currentWeight = currentSum > 0.0 ? r.M * currentHere / currentSum * currentHere * r.W : 0.0;
    float previousWeight = previousSum > 0.0 ? previousM * previousThere / previousSum * previousHere * previous.W
                                               * clamp(jacobian, 1.0 / 3.0, 3.0) : 0.0;

    float weightSum = currentWeight + previousWeight;
    float keptTarget = currentHere;
    if (randomFloat(seed) * weightSum < previousWeight) {
        r.position = previous.position;
        r.normal = previous.normal;
        r.radiance = previous.radiance;
        r.age = (previous.age & 0xFFFFu) + 1u;
        keptTarget = previousHere;
    }
    r.M += previousM;
    r.W = keptTarget > 0.0 ? weightSum / keptTarget : 0.0;
    return r;
}

// Whether ReSTIR GI's spatial pass runs
bool restirGiSpatialOn() {
    return scene.restirGiSpatialNeighbors + scene.restirGiDisocclusionNeighbors > 0;
}

shared float giBoilingWeights[64];  // one per thread of the 8 x 8 group (restirGiTemporalPass)

// RESTIR_PASS = 4: ReSTIR GI's temporal reuse, from the image pass's fresh samples to this frame's reservoirs (or,
// for the spatial pass, back in place of the fresh ones: each pixel only reads its own). inside: the thread has a
// pixel (the group's other threads still take part in the boiling filter).
//
// Boiling filter (RTXDI's): a reservoir whose sample brings far more light (target * W) than the average of the
// group's (8 x 8 pixels, those with light) is emptied. Without it, one lucky bright sample spreads over its
// neighbors and is passed on for many frames: a blotch that grows and then vanishes ("boiling"). Dropped when it
// brings more than 10 / strength - 9 times the average (strength 0.2: 41 x). Takes light away: little where light
// is spread evenly (0.1-0.2%), a lot where it all comes as rare bright samples (Sponza's side aisle, lit through
// gaps: 9%, and the dropping made it flicker). Off by default.
void restirGiTemporalPass(ivec2 pixel, bool inside) {
    uint freshIndex = restirGiFreshIndex(pixel);
    GiReservoir r;
    r.W = 0.0;
    r.M = 0.0;
    if (inside) {
        r = giReservoirs[freshIndex];
        if (r.M > 0.0 && scene.restirGiTemporal != 0 && scene.restirGiHistoryValid != 0) {
            r = restirGiTemporal(r, pixel);
        }
    }
    if (scene.restirGiBoilingFilter > 0.0) {
        float weight = 0.0;
        if (r.W > 0.0) {
            RestirSurface own = restirSurfaces[restirIndex(pixel, false)];
            weight = restirGiTarget(unpackRestirSurface(own), restirSurfaceView(own), r) * r.W;
        }
        giBoilingWeights[gl_LocalInvocationIndex] = weight;
        barrier();
        float sum = 0.0;
        float count = 0.0;
        for (uint i = 0; i < 64u; i++) {
            float w = giBoilingWeights[i];
            sum += w;
            count += w > 0.0 ? 1.0 : 0.0;
        }
        if (weight > sum / max(count, 1.0) * (10.0 / clamp(scene.restirGiBoilingFilter, 1e-6, 1.0) - 9.0)) {
            r.W = 0.0;
        }
    }
    if (inside) {
        giReservoirs[restirGiSpatialOn() ? freshIndex : restirIndex(pixel, false)] = r;
    }
}

// Whether a first surface sees a GI sample's x2 (geometric normal x2Normal, octahedral). Glass blocks it: a bounce
// ray that hits glass gives no sample (the path goes on through it and counts that light itself), so x2 behind glass
// is not one this surface could have.
bool restirGiVisible(Hit hit, vec3 x2, uint x2Normal, inout uint seed) {
    float side = dot(x2 - hit.position, hit.geometricNormal);
    vec3 origin = offsetRay(hit.position, side < 0.0 ? -hit.geometricNormal : hit.geometricNormal);
    vec3 toSample = x2 - origin;
    float distance = length(toSample);
    vec3 direction = toSample / distance;
    vec3 n2 = decodeDirection(x2Normal);
    float stop = distance - stopShortOf(x2, n2, dot(n2, direction));
    if (stop <= 0.0) {
        return true;  // closer than the rounding error: nothing fits in between
    }
    if (portalBetween(origin, direction, stop)) {
        return false;
    }
    rayQueryEXT rayQuery;
    rayQueryInitializeEXT(rayQuery, tlas, gl_RayFlagsTerminateOnFirstHitEXT | PATH_RAY_FLAGS, 0xFF, origin, 0.0, direction, stop);
    traverse(rayQuery, seed, false, uvec4(~0u));
    return rayQueryGetIntersectionTypeEXT(rayQuery, true) == gl_RayQueryCommittedIntersectionNoneEXT;
}

// A neighbor's surface for the GI spatial pass: as DI's test (normals within 60 degrees, view depths within 10%,
// roughness within 0.25), and as RTXDI's, diffuse albedos (luminance) within 0.25: the target weighs a sample by
// the surface's BRDF, so a dark and a bright surface prefer different ones.
bool restirGiSimilar(Hit hit, float depth, RestirSurface stored) {
    Hit surface = unpackRestirSurface(stored);
    float surfaceDepth = restirSurfaceDepth(stored);
    return dot(surface.normal, hit.normal) >= 0.5 && abs(surfaceDepth - depth) <= 0.1 * max(surfaceDepth, depth)
           && abs(surface.roughness - hit.roughness) <= 0.25
           && abs(luminance(surface.albedo) - luminance(hit.albedo)) <= 0.25;
}

// A GI sample's area-measure target at a surface: target * G. Pairwise MIS compares these: x2 is the same point
// whichever surface it is seen from, so per unit of x2's area all surfaces' targets are in the same measure.
float restirGiAreaTarget(Hit hit, vec3 v, GiReservoir r) {
    float geometry = restirGiGeometry(hit.position, r);
    return geometry > 0.0 ? restirGiTarget(hit, v, r) * geometry : 0.0;
}

// The canonical sample's MIS weight (see restirGiSpatial) with visibility: a neighbor whose surface doesn't see x2
// could never have produced it. A ray from each neighbor whose target isn't 0.
float restirGiSpatialCanonicalMis(GiReservoir canonical, float canonicalArea, float canonicalM, float mSum,
                                  ivec2 pixel, vec2 start, uint used, uint wanted, inout uint seed) {
    float mis = 0.0;
    for (uint n = 0; n < wanted; n++) {
        if ((used & (1u << n)) == 0u) {
            continue;
        }
        ivec2 neighbor = pixel + restirNeighborOffset(n, start, scene.restirGiSpatialRadius);
        float M = giReservoirs[restirGiFreshIndex(neighbor)].M;
        RestirSurface stored = restirSurfaces[restirIndex(neighbor, false)];
        Hit surface = unpackRestirSurface(stored);
        float there = restirGiAreaTarget(surface, restirSurfaceView(stored), canonical);
        if (there > 0.0 && !restirGiVisible(surface, canonical.position, canonical.normal, seed)) {
            there = 0.0;
        }
        mis += (M + canonicalM) / mSum * canonicalM * canonicalArea / (M * there + canonicalM * canonicalArea);
    }
    return mis;
}

// RESTIR_PASS = 5: ReSTIR GI's spatial reuse, as ReSTIR DI's (restirSpatial: pairwise MIS, the same neighbor
// pattern, more neighbors for pixels without history), with the targets compared per unit of x2's area
// (restirGiAreaTarget) and a neighbor's sample moved to this surface with its Jacobian (skipped outside 1/10..10,
// clamped to 1/3..3, as in restirGiTemporal). Targets ignore visibility (RTXDI's "basic"); the kept sample gets a
// visibility ray when shaded. With restirGiSpatialVisibility, as DI's: the kept sample's MIS weight with
// visibility / without (rays from the neighbors' surfaces).
void restirGiSpatial(ivec2 pixel, ivec2 size) {
    uint ownIndex = restirIndex(pixel, false);
    GiReservoir canonical = giReservoirs[restirGiFreshIndex(pixel)];
    if (canonical.M == 0.0) {
        giReservoirs[ownIndex] = canonical;
        return;
    }
    RestirSurface own = restirSurfaces[ownIndex];
    Hit hit = unpackRestirSurface(own);
    vec3 v = restirSurfaceView(own);
    float depth = restirSurfaceDepth(own);
    uint seed = pcgHash(pixelSeed(pixel) ^ 0x3c6ef372u);
    vec2 start = vec2(randomFloat(seed), randomFloat(seed));
    bool disoccluded = canonical.M <= 1.5;
    uint wanted = min(disoccluded ? max(scene.restirGiDisocclusionNeighbors, scene.restirGiSpatialNeighbors)
                                  : scene.restirGiSpatialNeighbors, 16u);

    // First which neighbors are used, and their M
    uint used = 0;
    float k = 0.0;
    float mSum = canonical.M;
    for (uint n = 0; n < wanted; n++) {
        ivec2 neighbor = pixel + restirNeighborOffset(n, start, scene.restirGiSpatialRadius);
        if (neighbor == pixel || any(lessThan(neighbor, ivec2(0))) || any(greaterThanEqual(neighbor, size))) {
            continue;
        }
        float M = giReservoirs[restirGiFreshIndex(neighbor)].M;
        if (M == 0.0) {
            continue;
        }
        if (restirGiSimilar(hit, depth, restirSurfaces[restirIndex(neighbor, false)])) {
            used |= 1u << n;
            k += 1.0;
            mSum += M;
        }
    }
    if (k == 0.0) {
        giReservoirs[ownIndex] = canonical;
        return;
    }

    float canonicalM = canonical.M / k;
    float canonicalHere = canonical.W > 0.0 ? restirGiTarget(hit, v, canonical) : 0.0;
    float canonicalArea = canonicalHere * restirGiGeometry(hit.position, canonical);
    float canonicalMis = 0.0;
    float weightSum = 0.0;
    GiReservoir result = canonical;
    float keptTarget = canonicalHere;
    uint keptNeighbor = 0xFFFFFFFFu;
    for (uint n = 0; n < wanted; n++) {
        if ((used & (1u << n)) == 0u) {
            continue;
        }
        ivec2 neighbor = pixel + restirNeighborOffset(n, start, scene.restirGiSpatialRadius);
        GiReservoir r = giReservoirs[restirGiFreshIndex(neighbor)];
        RestirSurface stored = restirSurfaces[restirIndex(neighbor, false)];
        Hit surface = unpackRestirSurface(stored);
        vec3 neighborV = restirSurfaceView(stored);
        float share = (r.M + canonicalM) / mSum;
        if (canonicalArea > 0.0) {
            float canonicalThere = restirGiAreaTarget(surface, neighborV, canonical);
            canonicalMis += share * canonicalM * canonicalArea / (r.M * canonicalThere + canonicalM * canonicalArea);
        }
        if (r.W > 0.0) {
            float geometryThere = restirGiGeometry(surface.position, r);
            float geometryHere = restirGiGeometry(hit.position, r);
            float jacobian = geometryThere > 0.0 ? geometryHere / geometryThere : 0.0;
            if (jacobian >= 0.1 && jacobian <= 10.0) {
                float here = restirGiTarget(hit, v, r);
                if (here > 0.0) {
                    float hereArea = here * geometryHere;
                    float thereArea = restirGiTarget(surface, neighborV, r) * geometryThere;
                    float denominator = r.M * thereArea + canonicalM * hereArea;
                    float weight = denominator > 0.0 ? share * r.M * thereArea / denominator * here * r.W
                                                       * clamp(jacobian, 1.0 / 3.0, 3.0) : 0.0;
                    weightSum += weight;
                    if (randomFloat(seed) * weightSum < weight) {
                        result.position = r.position;
                        result.normal = r.normal;
                        result.radiance = r.radiance;
                        result.age = r.age & 0xFFFFu;
                        keptTarget = here;
                        keptNeighbor = (r.age & GI_SEEN) != 0u ? 0xFFFFFFFEu : n;  // seen there: no ray needed
                    }
                }
            }
        }
    }
    float canonicalWeight = canonicalMis * canonicalHere * canonical.W;
    weightSum += canonicalWeight;
    float correction = 1.0;
    if (randomFloat(seed) * weightSum < canonicalWeight) {
        result.position = canonical.position;
        result.normal = canonical.normal;
        result.radiance = canonical.radiance;
        result.age = canonical.age;
        keptTarget = canonicalHere;
        if (scene.restirGiSpatialVisibility != 0) {
            correction = restirGiSpatialCanonicalMis(canonical, canonicalArea, canonicalM, mSum, pixel, start, used,
                                                     wanted, seed) / canonicalMis;
        }
    } else if (scene.restirGiSpatialVisibility != 0 && keptNeighbor < 0xFFFFFFFEu) {
        // A kept neighbor sample: its own surface may not see it (from that pixel's history)
        ivec2 neighbor = pixel + restirNeighborOffset(keptNeighbor, start, scene.restirGiSpatialRadius);
        if (!restirGiVisible(unpackRestirSurface(restirSurfaces[restirIndex(neighbor, false)]), result.position,
                             result.normal, seed)) {
            correction = 0.0;
        }
    }
    result.W = keptTarget > 0.0 && weightSum > 0.0 ? weightSum * correction / keptTarget : 0.0;
    result.M = mSum;
    giReservoirs[ownIndex] = result;
}

// RESTIR_PASS = 3: ReSTIR GI's shading. Adds the pixel's kept GI sample to the image: BRDF(x1) * cos * Lo * W. The
// first surface is rebuilt from the stored camera ray's hit exactly as the image pass had it (its textures at full
// precision; the packed RestirSurface would round the albedo and roughness). A sample from another pixel or frame
// gets a visibility ray; if blocked, it adds nothing and is dropped (W = 0), so the next frame doesn't reuse it.
void restirGiShade(ivec2 pixel, ivec2 size) {
    uint index = restirIndex(pixel, false);
    GiReservoir r = giReservoirs[index];
    if (!(r.W > 0.0)) {
        return;
    }
    uint seed = pixelSeed(pixel);
    startPathSamples(pixel, 0u);
    vec3 direction = sampleCameraRay(pixel, size, seed);
    Intersection intersection;
    vec3 tint;
    if (!loadPrimaryHit(pixel, seed, intersection, direction, tint)) {
        return;
    }
    Hit hit = hitFromIntersection(intersection, direction, 0.0, scene.pixelSpreadAngle);
    tiltTowardsViewer(hit, direction);
    if ((r.age & GI_SEEN) == 0u && !restirGiVisible(hit, r.position, r.normal, seed)) {
        giReservoirs[index].W = 0.0;
        return;
    }
    // From where the bounce ray started: off the surface, on the side it went to
    float side = dot(r.position - hit.position, hit.geometricNormal);
    vec3 l = normalize(r.position - offsetRay(hit.position, side < 0.0 ? -hit.geometricNormal : hit.geometricNormal));
    float pdf;
    vec3 light = evaluateBrdfWorld(hit, hit.normal, -direction, l, pdf) * abs(dot(hit.normal, l)) * r.radiance * r.W
                 * tint;
    if (FOG != 0) {
        // The image pass's camera ray gets to the surface with the fog's transmittance; scattered, its path has no GI.
        // (Up to just before the surface: a shadow ray would count it as blocking)
        bool crossedGlass, crossedSolidGlass;
        light *= shadowTransmittance(scene.cameraPosition.xyz, direction, intersection.t * 0.999, -1, seed,
                                     mediumAt(scene.cameraPosition.xyz), crossedGlass, crossedSolidGlass);
    }
    // As the image pass's: one of samplesPerPixel paths, and the running average when accumulating
    light /= float(scene.samplesPerPixel);
    if (scene.accumulate != 0 && scene.accumulatedFrames > 0) {
        light /= float(scene.accumulatedFrames + 1);
    }
    if (any(isnan(light)) || any(isinf(light))) {
        return;
    }
    imageStore(radianceImage, pixel, imageLoad(radianceImage, pixel) + vec4(light, 0.0));
}

// Debug view "ReSTIR light": the kind of light each pixel's sample is on. Glowing triangle: orange, punctual light:
// purple, sky: blue, none found: dark red, blocked (W = 0, set by the ReSTIR passes' visibility rays; the image pass
// doesn't run in this view): black. Grey: no ReSTIR.
