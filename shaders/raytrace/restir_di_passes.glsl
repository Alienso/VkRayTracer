// raytrace.comp, part: ReSTIR DI passes: candidates and temporal reuse, spatial reuse.
// Included by raytrace.comp in order: uses what the parts before it define.

bool restirVisible(Reservoir r, Hit hit, inout uint seed) {
    float toSolidAngle;
    LightSample s = restirLightSample(r.light, r.point, hit.position, toSolidAngle);
    bool behind = dot(hit.geometricNormal, s.direction) < 0.0;
    vec3 shadowOrigin = shadowRayStart(hit, behind, s);
    bool crossedGlass, crossedSolidGlass;
    vec3 transmittance = shadowTransmittance(shadowOrigin, s.direction, s.distance, -1, seed, OUTSIDE, crossedGlass,
                                             crossedSolidGlass);
    return any(greaterThan(transmittance, vec3(0.0)));
}

// The same surface point in last frame's image (ReSTIR DI's and GI's temporal reuse): where it was (the object's
// motion, last frame's camera), and a stored surface there that matches. gi: look for a GI reservoir instead of a DI
// one. False if there is none.
bool restirFindPrevious(Hit hit, vec4 previousPosition, bool gi, bool shuffle, inout uint seed, out uint previousIndex,
                        out Hit previousHit, out vec3 previousV) {
    previousIndex = 0;
    previousV = vec3(0.0, 0.0, 1.0);
    if ((scene.previousViewProjection * previousPosition).w <= 0.0) {
        return false;  // behind last frame's camera
    }
    ivec2 reprojected = ivec2(floor(projectToPixel(scene.previousViewProjection, previousPosition)));
    // Last frame's view depth of this point (clip w), to compare with the stored surfaces'
    float expectedDepth = (scene.previousViewProjection * previousPosition).w;
    ivec2 size = imageSize(radianceImage);

    // A matching surface of last frame: the reprojected pixel, or if that one doesn't match (something else was
    // there, or a different part of a bumpy surface), one of up to 8 random pixels within 2 of it (as RTXDI).
    // Matching: shading normals within 60 degrees, view depths within 10%, as RTXDI. A stricter test rejects
    // history in a scattered pattern on normal-mapped surfaces, and pixels with long and short histories next to
    // each other look patchy.
    for (int attempt = 0; attempt < 9; attempt++) {
        ivec2 candidate = reprojected;
        if (attempt == 0) {
            // Permutation sampling (as RTXDI): every frame the whole image takes its history from pixels shuffled
            // within 4 x 4 blocks (shift by this frame's offset, mirror within the block, shift back), so every
            // reservoir is still used once. Without it a pixel keeps its sample for many frames, and so its error,
            // moving with the surface: a denoiser takes error that doesn't change for detail. DLSS painted it
            // into the image (smudges like brush strokes, trails behind moving edges).
            if (shuffle) {
                ivec2 offset = ivec2(scene.restirPermutation & 3u, (scene.restirPermutation >> 2) & 3u);
                candidate = ((candidate + offset) ^ ivec2(3)) - offset;
            }
        } else {
            candidate += ivec2(round((vec2(randomFloat(seed), randomFloat(seed)) - 0.5) * 4.0));
        }
        if (any(lessThan(candidate, ivec2(0))) || any(greaterThanEqual(candidate, size))) {
            continue;  // off last frame's image
        }
        uint index = restirIndex(candidate, true);
        if (gi ? giReservoirs[index].M == 0.0 : reservoirs[index].light == RESTIR_NOT_USED) {
            continue;
        }
        RestirSurface stored = restirSurfaces[index];
        Hit surface = unpackRestirSurface(stored);
        float depth = (scene.previousViewProjection * vec4(stored.seenAt, 1.0)).w;
        if (dot(surface.normal, hit.normal) >= 0.5 && abs(depth - expectedDepth) <= 0.1 * max(depth, expectedDepth)) {
            previousIndex = index;
            previousHit = surface;
            previousV = restirSurfaceView(stored);  // last frame's view
            return true;
        }
    }
    return false;
}

// Last frame's reservoir of the same surface point (where it was on last frame's image), merged into r, which holds
// this frame's candidates. Both reservoirs' samples are resampled for this surface with the generalized balance
// heuristic: a sample's MIS weight is its reservoir's M times its target there, over the sum of that over both
// surfaces (Lin et al. 2022). Last frame's surface comes from its stored copy, and its view direction from last
// frame's camera. Targets ignore what blocks the light. That's a bias: a sample this frame's surface sees but last
// frame's doesn't (last frame's ray hit elsewhere in the pixel, close to a shadow's edge) gets a share of its MIS
// weight for last frame's surface, which could never have produced it: too dark near shadow edges (Sponza with the
// sky: 3.7%, taking history from up to 3 pixels away). With restirTemporal = 2 (the default), both targets at last
// frame's surface include whether it sees the sample. This frame's sample needs a shadow ray from there. Last
// frame's own sample doesn't: it may come from older frames, but the image pass tested it from that surface and
// set W = 0 if blocked (without that test the merge came out too bright: Sponza +2.6%). Then the weights add up to
// 1 for every sample either surface can produce: no bias.
Reservoir restirTemporal(Reservoir r, Hit hit, vec3 v, vec4 previousPoint, inout uint seed) {
    if (scene.restirTemporal == 0 || scene.restirHistoryValid == 0) {
        return r;
    }
    uint previousIndex;
    Hit previousHit;
    vec3 previousV;
    if (!restirFindPrevious(hit, previousPoint, false, (scene.restirPermutation & 16u) != 0u, seed, previousIndex,
                            previousHit, previousV)) {
        return r;
    }
    Reservoir previous = reservoirs[previousIndex];
    float previousM = min(previous.M, scene.restirMaxHistory * r.M);

    // Targets of both samples at both surfaces
    float currentHere = restirTargetOf(r, hit, v);
    float currentThere = restirTargetOf(r, previousHit, previousV);
    if (scene.restirTemporal == 2 && currentThere > 0.0 && r.W > 0.0 && !restirVisible(r, previousHit, seed)) {
        currentThere = 0.0;
    }
    float previousHere = restirTargetOf(previous, hit, v);
    // Last frame's own sample needs no ray: the image pass tested it from that surface and set W = 0 if blocked
    float previousThere = previous.W > 0.0 ? restirTargetOf(previous, previousHit, previousV) : 0.0;
    float currentSum = r.M * currentHere + previousM * currentThere;
    float previousSum = r.M * previousHere + previousM * previousThere;
    float currentWeight = currentSum > 0.0 ? r.M * currentHere / currentSum * currentHere * r.W : 0.0;
    float previousWeight = previousSum > 0.0 ? previousM * previousThere / previousSum * previousHere * previous.W : 0.0;

    float weightSum = currentWeight + previousWeight;
    float keptTarget = currentHere;
    if (randomFloat(seed) * weightSum < previousWeight) {
        r.light = previous.light;
        r.point = previous.point;
        keptTarget = previousHere;
    }
    r.M += previousM;
    r.W = keptTarget > 0.0 ? weightSum / keptTarget : 0.0;
    return r;
}

// RESTIR_PASS: the first camera ray of each pixel, with the same random numbers the image pass will use, then
// (ReSTIR DI) the candidates of its first surface. Runs for ReSTIR DI or GI; with GI only, the reservoirs say
// "not used" and the image pass samples the lights as usual.
void restirPass(ivec2 pixel, ivec2 size) {
    uint seed = pixelSeed(pixel);
    startPathSamples(pixel, 0u);
    vec3 cameraDirection = sampleCameraRay(pixel, size, seed);
    vec3 direction = cameraDirection;
    Intersection intersection;
    Hit hit;
    vec3 tint;
    float travelled;
    bool throughPortal;
    bool found = traceCameraRay(pixel, direction, seed, intersection, hit, tint, travelled, throughPortal);
    storePrimaryHit(pixel, found, intersection, seed, throughPortal, travelled, direction, tint);
    found = found && !throughPortal;  // seen through a portal: no ReSTIR (see PRIMARY_THROUGH_PORTAL)

    Reservoir reservoir;
    reservoir.light = RESTIR_NOT_USED;
    reservoir.point = 0;
    reservoir.W = 0.0;
    reservoir.M = 0.0;
    if (found) {
        // The same surface the image pass will see (see tracePath's first bounce). Where the camera sees it, and
        // where that was last frame (as restirPreviousPoint).
        vec3 seenAt = travelled > 0.0 ? scene.cameraPosition.xyz + cameraDirection * (travelled + hit.t) : hit.position;
        vec4 previousPoint = travelled > 0.0 ? vec4(seenAt, 1.0)
                                             : instanceMotion(hit.instanceIndex) * vec4(hit.position, 1.0);
        if (restirSurface(hit) && scene.restirDi != 0) {
            // Random numbers of their own: the image pass goes on with the pixel's (its bounce directions must
            // not depend on which light was picked)
            uint candidateSeed = pcgHash(seed ^ 0x9e3779b9u);
            reservoir = restirCandidates(hit, -direction, candidateSeed);
            // Visibility reuse: a blocked sample gets W = 0, so it isn't passed on to the next frame (or neighbors).
            // Before the merge, not after: tested only after it, a blocked fresh candidate could win over a visible
            // history sample, and that history would be lost (part C3: 1.7-3.7x the single-frame error)
            if (reservoir.W > 0.0) {
                vec3 surfacePoint = hit.position;
                float toSolidAngle;
                LightSample s = restirLightSample(reservoir.light, reservoir.point, surfacePoint, toSolidAngle);
                bool behind = dot(hit.geometricNormal, s.direction) < 0.0;
                vec3 shadowOrigin = shadowRayStart(hit, behind, s);
                bool crossedGlass, crossedSolidGlass;
                vec3 transmittance = shadowTransmittance(shadowOrigin, s.direction, s.distance, -1, candidateSeed, OUTSIDE,
                                                         crossedGlass, crossedSolidGlass);
                if (all(equal(transmittance, vec3(0.0)))) {
                    reservoir.W = 0.0;
                }
            }
            reservoir = restirTemporal(reservoir, hit, -direction, previousPoint, candidateSeed);
        }
        if (restirSurface(hit)) {
            restirSurfaces[restirIndex(pixel, false)] = packRestirSurface(hit, seenAt, -direction);
        }
    }
    reservoirs[restirSpatialOn() ? restirTemporalIndex(pixel) : restirIndex(pixel, false)] = reservoir;
}

// The number of candidates a reservoir starts with (its M before any reuse)
float restirFreshCount() {
    return float(scene.restirTriangleCandidates + scene.restirPunctualCandidates + scene.restirSkyCandidates
                 + scene.restirBrdfCandidates);
}

// A neighbor of the spatial pass: the n-th point of the R2 sequence (Roberts 2018) in a disc of the radius,
// shifted by the pixel's random start (per pixel and frame). Evenly spread, few duplicates; (0, 0) for none.
ivec2 restirNeighborOffset(uint n, vec2 start, float radius) {
    vec2 u = fract(start + float(n + 1) * vec2(0.7548776662, 0.5698402910));
    float angle = 2.0 * PI * u.y;
    return ivec2(round(radius * sqrt(u.x) * vec2(cos(angle), sin(angle))));
}

// The canonical sample's MIS weight (see restirSpatial) with visibility: a neighbor whose surface doesn't see the
// sample could never have produced it (its own sample was dropped if blocked), so its comparison goes wholly to the
// canonical reservoir. One shadow ray per neighbor whose target there isn't 0.
float restirSpatialCanonicalMis(Reservoir canonical, RestirLightPoint canonicalPoint, float canonicalHere,
                                float canonicalM, float mSum, ivec2 pixel, vec2 start, uint used, uint wanted,
                                inout uint seed) {
    float mis = 0.0;
    for (uint n = 0; n < wanted; n++) {
        if ((used & (1u << n)) == 0u) {
            continue;
        }
        ivec2 neighbor = pixel + restirNeighborOffset(n, start, scene.restirSpatialRadius);
        float M = reservoirs[restirTemporalIndex(neighbor)].M;
        RestirSurface stored = restirSurfaces[restirIndex(neighbor, false)];
        Hit surface = unpackRestirSurface(stored);
        float canonicalThere = restirTargetAt(canonicalPoint, surface, restirSurfaceView(stored));
        if (canonicalThere > 0.0 && !restirVisible(canonical, surface, seed)) {
            canonicalThere = 0.0;
        }
        mis += (M + canonicalM) / mSum * canonicalM * canonicalHere / (M * canonicalThere + canonicalM * canonicalHere);
    }
    return mis;
}

// RESTIR_PASS = 2: spatial reuse. The pixel's reservoir (after the temporal merge) and those of a few pixels nearby,
// resampled for this pixel's surface. A neighbor is used if its surface is like this one: as in restirTemporal
// (shading normals within 60 degrees, view depths within 10%), plus roughness within 0.25 (a glossy and a rough
// surface want different lights). A rejected one isn't replaced: the cost stays fixed. Pixels that found no history
// (just uncovered) take more neighbors (restirDisocclusionNeighbors).
//
// MIS: pairwise (Bitterli 2022; Lin et al. 2022). Instead of comparing every reservoir with every other one (the
// generalized balance heuristic of restirTemporal: (k + 1)^2 targets), each of the k neighbors is compared only with
// the pixel's own ("canonical") reservoir c. With p_j the target at reservoir j's surface and M_j its count:
//     neighbor i's sample x:  m_i(x) = s_i * M_i p_i(x) / (M_i p_i(x) + M_c/k p_c(x))
//     the canonical sample:   m_c(x) = sum over i of s_i * M_c/k p_c(x) / (M_i p_i(x) + M_c/k p_c(x))
// with s_i = (M_i + M_c/k) / (M_c + the sum of the M_i). They add up to 1 for every x, and with equal targets each
// reservoir's weight is its share of the M, as with the balance heuristic. 3k + 1 targets (of k + 1 samples, each
// looked up once: restirLightPoint). All surfaces are this
// frame's, seen by this frame's camera.
//
// Targets ignore what blocks the light. That's a bias near shadows: a neighbor that can't see the canonical sample
// still takes a share of its MIS weight, as if it could have produced it (Sponza with the sky: 1.6% too dark with
// neighbors within 3 pixels, 4.7% within 10, 9.7% within 30). With restirSpatialVisibility the kept sample's MIS
// weight includes visibility, and W is scaled by its weight with visibility / without. Picking with the weights
// without visibility is fine: the result's expected value is the sum over the reservoirs of (weight with
// visibility) * (light) / (target), whichever way they are picked, as long as no weight with visibility is > 0 where
// the one without is 0. The weights with visibility: each target times whether its surface sees the sample. A kept
// neighbor sample needs no ray from here (if this surface doesn't see it, the image pass's shadow ray finds no light
// whatever its weight), but one from the neighbor's surface: a sample from that pixel's history was tested from last
// frame's surface, not this one (assuming it seen made Sponza 1.9% too bright). The kept canonical sample needs a ray
// from each neighbor (restirSpatialCanonicalMis).
void restirSpatial(ivec2 pixel, ivec2 size) {
    uint ownIndex = restirIndex(pixel, false);
    Reservoir canonical = reservoirs[restirTemporalIndex(pixel)];
    if (canonical.light == RESTIR_NOT_USED) {
        reservoirs[ownIndex] = canonical;
        return;
    }
    RestirSurface own = restirSurfaces[ownIndex];
    Hit hit = unpackRestirSurface(own);
    vec3 v = restirSurfaceView(own);
    float depth = restirSurfaceDepth(own);
    uint seed = pcgHash(pixelSeed(pixel) ^ 0x2545f491u);
    vec2 start = vec2(randomFloat(seed), randomFloat(seed));
    bool disoccluded = canonical.M <= restirFreshCount() + 0.5;
    uint wanted = min(disoccluded ? max(scene.restirDisocclusionNeighbors, scene.restirSpatialNeighbors)
                                  : scene.restirSpatialNeighbors, 16u);

    // First which neighbors are used, and their M: the weights need k and the sum of the M
    uint used = 0;
    float k = 0.0;
    float mSum = canonical.M;
    for (uint n = 0; n < wanted; n++) {
        ivec2 neighbor = pixel + restirNeighborOffset(n, start, scene.restirSpatialRadius);
        if (neighbor == pixel || any(lessThan(neighbor, ivec2(0))) || any(greaterThanEqual(neighbor, size))) {
            continue;
        }
        Reservoir r = reservoirs[restirTemporalIndex(neighbor)];
        if (r.light == RESTIR_NOT_USED) {
            continue;
        }
        RestirSurface stored = restirSurfaces[restirIndex(neighbor, false)];
        Hit surface = unpackRestirSurface(stored);
        float neighborDepth = restirSurfaceDepth(stored);
        if (dot(surface.normal, hit.normal) >= 0.5 && abs(neighborDepth - depth) <= 0.1 * max(neighborDepth, depth)
                && abs(surface.roughness - hit.roughness) <= 0.25) {
            used |= 1u << n;
            k += 1.0;
            mSum += r.M;
        }
    }
    if (k == 0.0) {
        reservoirs[ownIndex] = canonical;
        return;
    }

    float canonicalM = canonical.M / k;  // the canonical reservoir's count in each comparison
    RestirLightPoint canonicalPoint = restirLightPoint(canonical);
    float canonicalHere = canonical.W > 0.0 ? restirTargetAt(canonicalPoint, hit, v) : 0.0;
    float canonicalMis = 0.0;
    float weightSum = 0.0;
    Reservoir result = canonical;
    float keptTarget = canonicalHere;
    uint keptNeighbor = 0xFFFFFFFFu;
    for (uint n = 0; n < wanted; n++) {
        if ((used & (1u << n)) == 0u) {
            continue;
        }
        ivec2 neighbor = pixel + restirNeighborOffset(n, start, scene.restirSpatialRadius);
        Reservoir r = reservoirs[restirTemporalIndex(neighbor)];
        RestirSurface stored = restirSurfaces[restirIndex(neighbor, false)];
        Hit surface = unpackRestirSurface(stored);
        vec3 neighborV = restirSurfaceView(stored);
        float share = (r.M + canonicalM) / mSum;
        // The canonical sample's weight for this comparison
        if (canonicalHere > 0.0) {
            float canonicalThere = restirTargetAt(canonicalPoint, surface, neighborV);
            canonicalMis += share * canonicalM * canonicalHere / (r.M * canonicalThere + canonicalM * canonicalHere);
        }
        // The neighbor's sample
        if (r.W > 0.0) {
            RestirLightPoint point = restirLightPoint(r);
            float here = restirTargetAt(point, hit, v);
            if (here > 0.0) {
                float there = restirTargetAt(point, surface, neighborV);
                float denominator = r.M * there + canonicalM * here;
                float weight = denominator > 0.0 ? share * r.M * there / denominator * here * r.W : 0.0;
                weightSum += weight;
                if (randomFloat(seed) * weightSum < weight) {
                    result.light = r.light;
                    result.point = r.point;
                    keptTarget = here;
                    keptNeighbor = n;
                }
            }
        }
    }
    float canonicalWeight = canonicalMis * canonicalHere * canonical.W;
    weightSum += canonicalWeight;
    float correction = 1.0;  // the kept sample's MIS weight with visibility / without
    if (randomFloat(seed) * weightSum < canonicalWeight) {
        result.light = canonical.light;
        result.point = canonical.point;
        keptTarget = canonicalHere;
        if (scene.restirSpatialVisibility != 0) {
            correction = restirSpatialCanonicalMis(canonical, canonicalPoint, canonicalHere, canonicalM, mSum, pixel,
                                                   start, used, wanted, seed) / canonicalMis;
        }
    } else if (scene.restirSpatialVisibility != 0 && keptNeighbor != 0xFFFFFFFFu) {
        // A kept neighbor sample: its own surface may not see it. Then that reservoir's weight for it is 0.
        ivec2 neighbor = pixel + restirNeighborOffset(keptNeighbor, start, scene.restirSpatialRadius);
        if (!restirVisible(result, unpackRestirSurface(restirSurfaces[restirIndex(neighbor, false)]), seed)) {
            correction = 0.0;
        }
    }
    result.W = keptTarget > 0.0 && weightSum > 0.0 ? weightSum * correction / keptTarget : 0.0;
    result.M = mSum;
    reservoirs[ownIndex] = result;
}

// How large x2 looks from x1, per unit of its area: cos at x2 / distance^2 (0 if x1 is behind x2)
