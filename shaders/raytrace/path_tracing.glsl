// raytrace.comp, part: Path tracing: one path from the camera (tracePath).
// Included by raytrace.comp in order: uses what the parts before it define.

// ---------------------------------------------------------------------------------------------

// The current path's camera ray scattered in fog (see radianceImage)
float cameraRayFog = 0.0;

// Follows one path from the camera through up to maxBounces surfaces and returns the light it carries back.
// storeGuides: write the DLSS inputs of this path (the first hit, and the distance to the second if the first
// bounce reflected) for this thread's pixel. usePrimaryHit: the camera ray's hit is this pixel's stored one
// (see "Primary hits"), its direct light comes from the pixel's ReSTIR reservoir (ReSTIR DI), and its first
// bounce's light becomes a GI sample (ReSTIR GI); direction must be the same camera ray.
//
// Keep in mind when adding to this loop: every value that lives from one bounce to the next, or across the light
// loop below (whose shadow ray needs many registers of its own), takes a register in every pixel. The shader is
// near the point where the GPU fits fewer pixels at a time, or moves values out to slow memory (the Debug tab
// shows both: register count and local memory). Recomputing something cheap, reading it from the uniforms, or
// storing it right away is usually faster than keeping it.
vec3 tracePath(vec3 origin, vec3 direction, inout uint seed, bool storeGuides, bool usePrimaryHit) {
    // The sun is a small glowing disc in the sky. Its total light (PI * sunColor, see below) is spread evenly
    // over the disc's solid angle. Like glowing triangles, it is found both by shadow rays aimed at it and by
    // bounce rays that happen to hit it, combined with MIS: a mirror finds it through its bounce ray, a matte
    // surface through the shadow ray (scene.sunMis). The sky: found by bounce rays that leave the scene, and (if
    // on) by shadow rays aimed at it, again with MIS (scene.environmentSampling).

    vec3 radiance = vec3(0.0);    // light collected along this path
    vec3 throughput = vec3(1.0);  // fraction of light found further along that still reaches the camera
    // The guides cost a little at the first hit (projections, albedos); skipped when nobody reads them.
    bool wantGuides = storeGuides && scene.writeGuides != 0 && CACHE_UPDATE_PASS == 0;
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);

    // Update paths: the cache entries of the surfaces hit so far, and per surface the light found after it,
    // as seen from that surface (divided by the throughput up to it). Adding to these as the light is found
    // (ADD_LIGHT) keeps dim light found late accurate, instead of subtracting large sums at the end.
    // Each surface takes the light of as many bounces after it as an image path that ends in its cell would have
    // traced (Max bounces - 1, as from the second surface): the update path goes on that far past its last cached
    // surface (lastBounce), and light found later than that for an earlier surface stays out of its cell. Without
    // this, surfaces found late got fewer bounces: thick fog, which keeps most of its light from one scatter to the
    // next, came out 10-16% too dark. Entries: the table index in bits 0-23, the bounce it was hit at above.
    uint cacheEntries[CACHE_MAX_VERTICES];
    vec3 cacheInverseThroughput[CACHE_MAX_VERTICES];
    vec3 cacheLight[CACHE_MAX_VERTICES];
    uint cacheVertices = 0;
    uint cacheDepth = max(scene.maxBounces, 1u) - 1u;
    uint lastBounce = scene.maxBounces;
    // The bounce of the light being added: what this bounce's ray sees counts as found at this bounce, the light
    // sampled at its surface as found at the next (as the next ray would have)
    uint lightBounce = 0;
    bool previousLobeRough = false;  // the bounce that led to this hit spread out (diffuse or rough)
    // The update path's entry of the surface hit in this bounce and in the one before (-1: none): their direct
    // light (shadow rays from there, and the light-sampled lights their bounce ray hits) stays out of their cells.
    int currentVertex = -1;
    int previousVertex = -1;
    bool endInCache = false;     // image paths: the cell of this surface has its indirect light
    vec3 cachedLight = vec3(0.0);
    // Reflections so far (passes through glass don't count): light found at a hit has made this many, light
    // reflected by the current surface one more (clampBounceLight)
    uint reflections = 0;
#define ADD_LIGHT_EXCEPT(light, skipped) { vec3 light_ = (light); radiance += clampBounceLight(light_, clampDepth); \
        if (CACHE_UPDATE_PASS != 0) { for (int i_ = 0; i_ < int(cacheVertices); i_++) { \
            if (i_ != (skipped) && lightBounce - (cacheEntries[i_] >> 24u) <= cacheDepth) { \
                cacheLight[i_] += light_ * cacheInverseThroughput[i_]; } } } }
#define ADD_LIGHT(light) ADD_LIGHT_EXCEPT(light, -1)
    bool firstBounceSpecular = false;  // the bounce ray leaving the first hit came from a reflection/transmission
    float bouncePdf = 0.0;        // pdf of the bounce direction that led to the current hit (for MIS)
    float pathRoughness = 0.0;    // roughest reflection along the path so far (for blurCaustics)
    // The ray cone (see uvFootprint): its width where the current ray starts, and how fast it grows per unit
    float coneWidth = 0.0;
    float coneSpread = scene.pixelSpreadAngle;
    bool sampleLights = scene.emissiveTriangleCount > 0 && scene.emissiveSampling != EMISSIVE_BOUNCES_ONLY;

    // Light behind glass. Shadow rays pass glass in a straight line (tinted), so the light sampling at a surface
    // already counts lights behind glass, completely (no MIS, see shadowTransmittance). A bounce ray that went
    // through glass and then hits a light must not count it again. That needs two facts about the path:
    bool throughGlass = false;    // went through glass since the last reflection
    bool reflectedOnce = false;   // there was a reflection, i.e. a surface that did light sampling
    // Seen without any reflection before (the camera looking through a window at the sun or a lamp), nothing
    // counted it yet: then it counts fully.
    // The sky (when light-sampled) is the exception for thin panes: they don't bend the ray, so the bounce ray
    // through a window and the shadow ray through it find the same light, and share it by MIS like without glass.
    // Otherwise a glossy table reflecting the window would only get the shadow rays, which rarely hit its narrow
    // reflection: fireflies. So the sky needs to know about solid glass, and the last reflection's pdf.
    bool throughSolidGlass = false;  // went through solid glass since the last reflection
    // Light through rough glass. Seen through frosted glass, a lamp or the sun is found only when the spread-out ray
    // happens to hit it: fireflies. So where there was no reflection yet, rough glass light-samples what lies behind it
    // (hit.lightThrough: a thin pane, or solid glass where the path leaves it; evaluateGlassTransmission), and shares
    // the light its bounce ray finds by MIS, like a reflection. Its shadow rays stop at more glass: that light is the
    // next glass's (if rough), or its bounce rays' (counted fully then). After a reflection the reflection's straight
    // shadow ray already counted the light behind the glass (see above), so glass doesn't.
    bool glassLit = false;  // the last light sampling was at rough glass (no reflection yet)
    // Portals gone through since the last reflection. Light sampling reaches the sun and the punctual lights through
    // one of them (light images, see "Portals"), nothing else through any: whatever else such a ray finds (a glowing
    // surface, the sky, the sun after two) counts fully (the bounce rays aimed through openings are part of the bounce
    // sampling, see PORTAL_BOUNCE_SHARE), and isn't that surface's direct light for the radiance cache either (paths
    // that end in the cache rely on shadow rays for that).
    uint portalCrossings = 0;
    float reflectionPdf = 0.0;       // bouncePdf of the last reflection (glass passes don't change it)
    // Where the last reflection was: the light tree's chance of picking a glowing triangle depends on the point
    vec3 reflectionPosition = vec3(0.0);
    vec3 reflectionNormal = vec3(0.0, 0.0, 1.0);
    bool reflectionTwoSided = false;
    // The last reflection was the first surface with ReSTIR DI: its reservoir counted all the light that light
    // sampling can find (glowing triangles, punctual lights, its share of the sampled sky), behind glass or not. Its
    // bounce ray then counts only what light sampling can't pick, and the sky's MIS share of the rest.
    bool restirReflection = false;
    // The first bounce was picked and traced by the RESTIR_PASS (its BRDF candidate, see BounceHit)
    bool sharedBounce = false;
    // ReSTIR GI: 1 = the first surface takes a GI sample, 2 = it did at this path's second surface: from there on
    // radiance collects that sample's Lo (with the throughput starting over at 1), and giImageLight holds what the
    // pixel collected before. 3 = it found no surface that can be one.
    uint giState = 0;
    vec3 giImageLight = vec3(0.0);
    // The fog and glass the path is in (see "Fog", "Glass volumes")
    uvec2 media = FOG != 0 ? mediumAt(origin) : OUTSIDE;
    // Passes through glass (in or out, or reflected inside) don't count as bounces: light that gets trapped inside
    // glass (total internal reflection) would otherwise run out of bounces and be lost, and a window would cost two.
    // They have a budget of their own (MAX_GLASS_PASSES). "bounce" below counts both (its random numbers, the first
    // surface's special cases), depth only the bounces (Max bounces, the radiance cache's windows, Russian roulette).
    uint glassPasses = 0;

    // maxBounces surfaces get lit (by light sampling and their bounce ray). The bounce ray leaving the last one
    // is still traced, but only to collect what it sees directly (sky or a glowing surface): light sampling
    // already counts direct light at that last surface, so without this ray, sky and glowing surfaces found
    // by bouncing would be one bounce short, and the methods wouldn't agree. (Update paths: lastBounce, see above.)
    for (uint bounce = 0; bounce - glassPasses <= lastBounce; bounce++) {
        uint depth = bounce - glassPasses;
        // light found at this hit (the sky, the sun, a glowing surface). Not in a GI sample's Lo (see "ReSTIR GI").
        uint clampDepth = giState == 2 ? 0u : reflections;
        lightBounce = depth;
        // (not traceClosest: one copy of the code that builds a Hit, called once here, keeps the shader smaller)
        Intersection intersection;
        bool found;
        vec3 rayDirection = direction;  // before any portal (the camera ray's, for the guides)
        float fogU = FOG != 0 ? pathSample(bounce, SAMPLE_FOG, seed).x : -1.0;
        bool storedHit = bounce == 0 && usePrimaryHit && primaryHitStored(pixel);
        bool storedBounce = bounce == 1 && sharedBounce
                            && (bounceHits[primaryHitIndex(pixel)].weightBRoughness & BOUNCE_RETRACE) == 0u;
        if (storedHit) {
            vec3 tint;
            found = loadPrimaryHit(pixel, seed, intersection, direction, tint);
            throughput = tint;
        } else if (storedBounce) {
            BounceHit stored = bounceHits[primaryHitIndex(pixel)];
            found = stored.meshIndex != ~0u;
            intersection.meshIndex = stored.meshIndex;
            intersection.instanceIndex = stored.instanceIndex;
            intersection.primitive = stored.primitive;
            intersection.barycentrics = stored.barycentrics;
            intersection.t = stored.t;
            intersection.travelled = 0.0;
            intersection.crossings = 0;
            intersection.inFog = false;
            intersection.lampWeight = 0.0;
        }
        // The ray; with fog also the camera ray to a stored hit again: the RESTIR_PASS's hit is the surface, this decides
        // whether it scatters in fog before it (with fog, no mirrors on the way: see traceCameraRay). One call for both:
        // the compiled shader has one copy of it (two took the varying fog's from 25 ms to 36 in a Cornell box).
        if (!storedBounce && (FOG != 0 || !storedHit)) {
            vec3 tracedOrigin = origin;
            vec3 tracedDirection = direction;
            Intersection traced;
            bool tracedFound = intersectClosest(tracedOrigin, tracedDirection, seed, media, fogU, traced);
            if (FOG == 0 || !storedHit) {
                found = tracedFound;
                intersection = traced;
                origin = tracedOrigin;
                direction = tracedDirection;
            } else if (traced.inFog) {
                found = false;
                intersection = traced;
            } else {
                intersection.lampSpan = traced.lampSpan;
                intersection.lampMedia = traced.lampMedia;
                intersection.lampMajorant = traced.lampMajorant;
                intersection.lampWeight = traced.lampWeight;
            }
        }
        // The lamps' light fog scatters into this stretch (not the final ray's: as at its end, see below). Not in
        // update paths: one more shadow ray took them from 128 registers to 168; their scatter points light-sample
        // the lamps instead (noisier, but the cache averages it).
        if (CACHE_UPDATE_PASS == 0 && intersection.lampWeight > 0.0 && depth < scene.maxBounces) {
            ADD_LIGHT(throughput * fogLampLight(origin, direction, intersection, bounce, seed));
        }
        bool crossedPortal = intersection.travelled != 0.0;
        portalCrossings += intersection.crossings;
        Hit hit;
        hit.found = found;
        if (found) {
            hit = hitFromIntersection(intersection, direction, coneWidth, coneSpread);
        }
        if (FOG != 0 && intersection.inFog) {
            // Scattered in fog before anything: the path's vertex is there. Its color: the share that goes on (in
            // glass, what the glass absorbed on the way: below)
            uint medium = intersection.fogMedium;
            if (reflections == 0 && giState != 2) {
                cameraRayFog = 1.0;  // the camera sees this fog (see radianceImage)
            }
            vec3 scatterPoint = origin + direction * intersection.t;
            // The new direction comes out of a frame built around this one, so its rounding error would grow with
            // every scatter: after a few dozen (high Max bounces), cosines above 1 made the phase function NaN
            direction = normalize(direction);
            hit = fogHit(scatterPoint, direction, intersection.travelled + intersection.t, medium);
            throughput *= materials[medium].fogColor;
            if (bounce == 0 && wantGuides) {
                guidesRetrace = true;  // the guides: of what is behind the fog
            }
        }
        previousVertex = currentVertex;
        currentVertex = -1;
        if (bounce == 1 && firstBounceSpecular) {
            imageStore(specularHitDistanceImage, pixel, vec4(hit.found ? hit.t : 65504.0));  // the sky: as above
        }

        if (!hit.found) {
            if (bounce == 0 && wantGuides) {
                storeSkyGuides(pixel, rayDirection);  // through portals too: they turn the sky with the camera
            }
            if (intersection.travelled < 0.0) {
                break;  // lost in portals: no light
            }
            vec3 skyLight = throughput * environmentLight(direction);
            bool lightSampled = reflectedOnce || (glassLit && !throughGlass);  // where this ray started, or before
            if (scene.environmentSampling == 0 || !lightSampled || portalCrossings > 0) {
                ADD_LIGHT(skyLight);  // nothing else finds it (the camera sees the sky, maybe through glass)
            } else if (!throughSolidGlass) {
                // The bounce-sampling share of the last surface's direct skylight, also through thin panes. Unlike
                // the sun's, it goes into that surface's cache cell as well: a path ending there takes it from the
                // cache (see the shadow ray below). A smooth sky is mostly found this way, so its light comes
                // averaged; a bright spot (a map's sun) is almost entirely the shadow rays' share, so its shadows
                // stay sharp. After a ReSTIR surface: of the part its reservoir leaves (RESTIR_SKY_SHARE).
                float share = restirReflection ? 1.0 - RESTIR_SKY_SHARE : 1.0;
                ADD_LIGHT(skyLight * misWeight(reflectionPdf, environmentPdf(direction)) * share);
            }
            // The sun's disc: always visible to the camera; for bounce rays, the MIS share of the bounce method
            if (scene.sunOn != 0 && dot(direction, scene.sunDirection.xyz) >= scene.sunDirection.w) {
                vec3 sunRadiance = scene.sunColor.rgb * scene.sunRadianceScale;
                // Its image through a portal is light-sampled only through one, and only with "Shadows" (as there)
                bool sunImaged = portalCrossings == 1 && scene.shadowsEnabled != 0;
                if (!lightSampled || (portalCrossings > 0 && !sunImaged)) {
                    ADD_LIGHT(throughput * sunRadiance);  // the camera sees it (maybe through glass), or portals
                } else if (!throughGlass && scene.sunMis != 0) {
                    // the bounce-sampling share of the last surface's direct sunlight
                    ADD_LIGHT_EXCEPT(throughput * sunRadiance * misWeight(bouncePdf, scene.sunPdf), previousVertex);
                }
            }
            break;
        }
        tiltTowardsViewer(hit, direction);
        // Seen in mirrors through the RESTIR_PASS's stored hit: that pass wrote the guides (traceCameraRay)
        if (bounce == 0 && wantGuides && !(crossedPortal && intersection.crossings == 0) && !isFog(hit)) {
            storeSurfaceGuides(pixel, hit, direction, rayDirection, crossedPortal, hit.t, mat3(1.0), vec3(1.0));
            guidesRetrace = guideMirror(hit) || guideClear(hit);
        }

        // Glass volumes (see there). The ray just travelled hit.t through the glass it is in, losing light (Beer's law).
        // Leaving glass it didn't know it was in (more than the media can hold), it was in that one.
        bool glassSurface = FOG != 0 && FEATURE_GLASS && hit.solid && glassMaterial(hit.materialIndex);  // (fog's isn't solid)
        bool leavingUnknown = glassSurface && !hit.frontFace && !inMedium(media, hit.instanceIndex);
        uint glassHere = currentGlass(media);
        if (leavingUnknown && glassHere == NO_MEDIUM) {
            glassHere = hit.instanceIndex;
        }
        if (glassHere != NO_MEDIUM) {
            throughput *= glassTransmittance(glassHere, direction, hit.t);
        }
        // At a glass surface: which glass is on either side. From here on hit.ior is the ratio of their indices of
        // refraction. Into glass from air (or fog) as n_glass / n_air, which sampleBrdf expects; otherwise the ray is
        // in glass here (insideGlass), and it is n_here / n_there for sampleGlassExit: out into air, or into
        // another glass (from the glass into the water in it). passThrough: out of a glass while in one entered
        // after it (the water's surface inside the glass's wall): nothing changes, the ray goes on.
        bool insideGlass = false;
        bool passThrough = false;
        bool outIntoAir = false;
        if (glassSurface) {
            if (hit.frontFace) {
                insideGlass = glassHere != NO_MEDIUM;
                hit.ior = insideGlass ? glassIor(glassHere) / hit.ior : hit.ior;
            } else {
                uint glassThere = currentGlass(leaveMedium(media, hit.instanceIndex));
                insideGlass = true;
                passThrough = glassHere != hit.instanceIndex;
                outIntoAir = glassThere == NO_MEDIUM;
                hit.ior /= glassIor(glassThere);
            }
        }

        // Light given off by the surface itself. Seen directly by the camera it always counts. Found by a bounce
        // ray, it is the "bounce sampling" half: with light sampling on, it shares the job (MIS) or leaves it
        // entirely to light sampling.
        if (any(greaterThan(hit.emissive, vec3(0.0)))) {
            float weight = 1.0;
            bool lightSampled = sampleLights && hit.lightIndex >= 0 && portalCrossings == 0;
            if (!lightSampled) {
                // only bounce rays find it (or the camera does)
            } else if (reflectedOnce && restirReflection) {
                weight = lightPickProbability(hit.lightIndex, reflectionPosition, reflectionNormal,
                                              reflectionTwoSided) > 0.0 ? 0.0 : 1.0;
            } else if (reflectedOnce && throughGlass) {
                weight = 0.0;
            } else if (reflectedOnce || (glassLit && !throughGlass)) {
                if (scene.emissiveSampling == EMISSIVE_LIGHT_SAMPLING) {
                    weight = 0.0;
                } else {
                    float cosLight = abs(dot(hit.geometricNormal, direction));
                    float pickProbability = lightPickProbability(hit.lightIndex, reflectionPosition, reflectionNormal,
                                                                 reflectionTwoSided);
                    weight = misWeight(bouncePdf, lightPdf(hit.lightIndex, pickProbability, reflectionPosition, hit.t,
                                                           cosLight));
                }
            }
            // Found by the bounce of a surface that also light-sampled it: part of that surface's direct light
            ADD_LIGHT_EXCEPT(throughput * hit.emissive * weight, reflectedOnce && lightSampled ? previousVertex : -1);
        }
        // The final ray: only what it sees counts, also through glass (a pass is no bounce: it goes on, unless the
        // glass reflects it). No light sampling at glass then.
        bool finalRay = depth == lastBounce;
        if (finalRay && !(FEATURE_GLASS && hit.transmission > 0.0)) {
            break;
        }

        // ReSTIR GI: the second surface of a GI pixel. Its glow (above) stays the first surface's; everything after
        // it is the sample's Lo.
        if (giState == 1) {
            giState = 3;
            if (restirGiSampleSurface(hit) && !crossedPortal) {  // reconnecting to it needs a straight line
                uint index = restirGiFreshIndex(pixel);
                giReservoirs[index].position = hit.position;
                giReservoirs[index].normal = encodeDirection(hit.geometricNormal);
                giReservoirs[index].W = 1.0 / bouncePdf;
                giReservoirs[index].M = 1.0;
                giReservoirs[index].age = GI_SEEN;
                giImageLight = radiance;
                radiance = vec3(0.0);
                throughput = vec3(1.0);
                giState = 2;
            }
        }

        // Path regularization ("blur caustics"). A caustic is light that reaches a matte surface via a shiny one,
        // like the sun reflected by the helmet onto the floor. From the floor it is only found when a random
        // bounce hits the shiny surface AND its mirror direction hits the tiny sun: very rarely, but then
        // hundreds of times brighter than the floor, so the image shows bright dots (fireflies) for a long time.
        // After a rough bounce, the exact shape of what a shiny surface reflects barely matters, so treat it as
        // at least half as rough as the roughest bounce before. The reflected light becomes a wide glow that
        // light sampling finds easily. Brightness stays the same (energy compensation); only sharp caustic
        // patterns are lost. What the camera sees directly or through mirrors is unaffected.
        if (scene.blurCaustics != 0) {
            hit.roughness = max(hit.roughness, 0.5 * pathRoughness);
        }

        // Radiance cache (see above). Update paths remember this surface; image paths that got here by a bounce
        // that spread out take the cell's indirect light, add this surface's direct light below and stop.
        if (cacheableSurface(hit)) {
            if (CACHE_UPDATE_PASS != 0) {
                if (cacheVertices < CACHE_MAX_VERTICES) {
                    uint slot, fingerprint;
                    float cellSize;
                    cacheCell(hit, slot, fingerprint, cellSize);
                    int entry = cacheFindOrInsert(slot, fingerprint);
                    if (entry >= 0) {
                        currentVertex = int(cacheVertices);
                        cacheEntries[cacheVertices] = uint(entry) | (min(depth, 255u) << 24u);
                        lastBounce = depth + cacheDepth;
                        // Channels that carry (almost) nothing can't tell how bright the light behind them is
                        cacheInverseThroughput[cacheVertices] = mix(vec3(0.0), 1.0 / max(throughput, vec3(1e-8)),
                                                                    greaterThan(throughput, vec3(1e-6)));
                        cacheLight[cacheVertices] = vec3(0.0);
                        cacheVertices++;
                    }
                }
            } else if (scene.cacheQuery != 0 && bounce >= 1 && previousLobeRough && !(giState == 2 && bounce == 1)) {
                // Not at a GI sample's second surface: its Lo would hold the cell's value of this frame, kept and
                // shared by the reuse for many frames and pixels. Where cells are noisy (light through openings),
                // the reuse also favours samples whose cell was momentarily bright: patches that came and went
                // (Sponza's side aisle). The path goes on and may end in the cache one surface later.
                // In fog the steps between scatter points are short where it is thick, and there are no corners for
                // a cell's average to leak into: any length will do.
                uint slot, fingerprint;
                float cellSize;
                cacheCell(hit, slot, fingerprint, cellSize);
                int entry = isFog(hit) || hit.t >= CACHE_MIN_RAY_CELLS * cellSize ? cacheFind(slot, fingerprint) : -1;
                if (entry >= 0) {
                    vec4 cached = cacheValues[entry];
                    endInCache = cached.w >= scene.cacheMinWeight;
                    cachedLight = cached.rgb;
                }
            }
        }

        // Inside glass there is nothing to light-sample: lights are outside, behind the glass surface. Unless it is rough,
        // the path had no reflection yet, and air is behind it: then the lights behind it (see glassLit).
        bool leavingGlass = insideGlass;
        hit.lightThrough = FEATURE_GLASS && !reflectedOnce && hit.transmission > 0.0 && hit.metallic < 1.0
                           && hit.roughness >= GLASS_LIGHT_ROUGHNESS
                           && (glassSurface ? outIntoAir && !passThrough : !hit.solid);
        // The first surface with ReSTIR DI: slot 2 takes the reservoir's sample instead of a glowing triangle,
        // the sky slot is left out, and the punctual ones if the reservoir has them (more than a few lights)
        bool restirVertex = bounce == 0 && usePrimaryHit && !isFog(hit)
                            && reservoirs[restirIndex(pixel, false)].light != RESTIR_NOT_USED;
        if (CACHE_UPDATE_PASS == 0 && bounce == 0 && usePrimaryHit && scene.restirGi != 0 && restirSurface(hit)
                && intersection.crossings == 0) {  // through portals no, in mirrors yes (see "Mirrors and ReSTIR")
            giState = 1;
        }
        // Some of the bounce rays aim through portals (see PORTAL_BOUNCE_SHARE): at rough opaque surfaces that see an
        // opening, and go on (not ending in the cache)
        float portalShare = CACHE_UPDATE_PASS == 0 && FEATURE_PORTALS && portalCount() > 0 && !isFog(hit)
                            && !endInCache && !finalRay && hit.transmission <= 0.0 && hit.diffuseTransmission <= 0.0
                            && hit.roughness >= CACHE_ROUGHNESS && portalAimTotal(hit.position, hit.normal) > 0.0
                            ? PORTAL_BOUNCE_SHARE : 0.0;
        if ((!leavingGlass || hit.lightThrough) && !finalRay) {
            // Direct light: one shadow ray per kind of light (see LightSample). Slot 0: the sun, 1: the sky, 2: the
            // glowing triangles, 3 and up: the punctual lights, then the sun's and the punctual lights' images through
            // each portal end (portalLightImage). One loop, so the shadow ray and the BRDF exist only
            // once in the compiled shader. In world space: a shading frame would take 9 more registers across the
            // shadow rays. Shadow rays start slightly off the surface so they don't hit the surface they start on,
            // along the real triangle plane's normal (the smooth normal can point a bit into the surface).
            clampDepth = giState == 2 ? 0u : reflections + 1;  // light this surface reflects
            lightBounce = depth + 1;
            // (none at a scatter point in the fog: the stretches before take the lamps' light, see fogLampLight)
            uint lightSlots = (restirVertex && scene.restirPunctualCandidates > 0) || (isFog(hit) && CACHE_UPDATE_PASS == 0)
                              ? 3 : 3 + punctualLightSamples();
            uint imageSlots = portalCount() * (1 + punctualLightSamples());
            [[dont_unroll]] for (uint slot = 0; slot < lightSlots + imageSlots; slot++) {
                LightSample s = noLightSample();
                int imageEnd = -1;  // the portal end this slot's light is seen through
                if (slot == 0) {
                    // A random point on the sun's disc. Points near a shadow edge see only part of the disc, so
                    // averaged over many samples the shadow edge becomes soft (penumbra). The disc delivers
                    // PI * sunColor in total, so a white matte surface (BRDF 1 / PI) facing the sun looks exactly
                    // sunColor bright. (none when the sun is switched off: its color is then 0)
                    if (scene.sunOn != 0) {
                        s.direction = randomDirectionInCone(scene.sunDirection.xyz, scene.sunDirection.w,
                                                            pathSample(bounce, SAMPLE_SUN, seed).xy);
                        s.distance = SKY_DISTANCE;
                        s.light = PI * scene.sunColor.rgb;
                        s.pdf = scene.sunPdf;
                        s.mis = scene.sunMis != 0 ? MIS_UNLESS_GLASS : MIS_NONE;
                        s.shadowed = scene.shadowsEnabled != 0;  // "Shadows" is about the sun
                    }
                } else if (slot == 1) {
                    // A direction picked by the sky's brightness. Behind solid glass no bounce ray counts it (as for
                    // the sun); behind thin panes they do. Ending in the cache, the cache has the bounce rays' share
                    // (see the miss above), so this stays the MIS share. At a ReSTIR surface, of the part its
                    // reservoir leaves (RESTIR_SKY_SHARE).
                    if (scene.environmentSampling != 0) {
                        float skyPdf;
                        s.direction = sampleEnvironment(pathSample(bounce, SAMPLE_SKY, seed), skyPdf);
                        if (skyPdf > 0.0) {
                            s.distance = SKY_DISTANCE;
                            s.light = environmentLight(s.direction) / skyPdf
                                      * (restirVertex ? 1.0 - RESTIR_SKY_SHARE : 1.0);
                            s.pdf = skyPdf;
                            s.mis = MIS_UNLESS_SOLID;
                            s.shadowed = true;
                        }
                    }
                } else if (slot == 2) {
                    if (restirVertex) {
                        s = restirReservoirSample(pixel, hit.position);
                    } else if (sampleLights) {
                        s = sampleEmissiveLight(hit, hit.normal, hit.position,
                                                pathSample(bounce, SAMPLE_EMISSIVE, seed), seed);
                    }
                } else if (slot < lightSlots) {
                    s = samplePunctualLight(slot - 3, hit.position, seed);
                } else {
                    uint image = slot - lightSlots;
                    imageEnd = int(image % portalCount());
                    s = portalLightImage(uint(imageEnd), image / portalCount(), hit.position, seed);
                }
                if (all(equal(s.light, vec3(0.0)))) {
                    continue;
                }

                // In front of the surface, or behind a thin one that lets light through (diffuse transmission, rough
                // glass). Leaving glass, only behind: in front is inside the glass. The shading normal and the real
                // triangle must agree on the side.
                float cosLight = dot(hit.normal, s.direction);
                float side = dot(hit.geometricNormal, s.direction);
                bool behind = cosLight < 0.0 && side < 0.0 && (hit.diffuseTransmission > 0.0 || hit.lightThrough);
                if (!((cosLight > 0.0 && side > 0.0 && !leavingGlass) || behind)) {
                    continue;
                }
                float pdfBounce;  // how likely the bounce sampling would have picked this direction
                vec3 contribution;
                if (isFog(hit)) {
                    pdfBounce = fogPhase(dot(direction, s.direction), materials[hit.meshIndex].fogForward);
                    contribution = s.light * pdfBounce;
                } else {
                    contribution = s.light * evaluateBrdfWorld(hit, hit.normal, -direction, s.direction, pdfBounce)
                                   * abs(cosLight);
                }
                if (all(equal(contribution, vec3(0.0)))) {
                    continue;  // a dark spot of an emissive texture, or a direction the surface doesn't reflect into
                }
                bool crossedGlass = false, crossedSolidGlass = false;
                vec3 transmittance = vec3(1.0);
                if (s.shadowed) {
                    vec3 shadowOrigin = shadowRayStart(hit, behind, s);
                    transmittance = shadowTransmittance(shadowOrigin, s.direction, s.distance, imageEnd, seed, media,
                                                        crossedGlass, crossedSolidGlass);
                    // The reservoir's sample is blocked: kept with W = 0, so every sample handed on to the next
                    // frame is known to be seen from its own surface (restirTemporal counts on that)
                    if (restirVertex && slot == 2 && all(equal(transmittance, vec3(0.0)))) {
                        reservoirs[restirIndex(pixel, false)].W = 0.0;
                    }
                    if (behind && hit.lightThrough && crossedGlass) {
                        continue;  // through rough glass and more glass: the bounce rays' (see glassLit)
                    }
                }
                // Behind glass, bounce rays don't find the light (they are bent), so the shadow ray counts it fully.
                // Ending in the cache, no bounce ray looks for it either. Bounce rays partly aimed through portals:
                // their pdf the mix (here, past the shadow ray: most lights it reaches only after it got through).
                if (portalShare > 0.0 && s.mis != MIS_NONE && any(greaterThan(transmittance, vec3(0.0)))) {
                    pdfBounce = mix(pdfBounce, portalDirectionPdf(hit.position, hit.normal, s.direction), portalShare);
                }
                float lightWeight = 1.0;
                if (s.mis == MIS_UNLESS_GLASS && !crossedGlass && !endInCache) {
                    lightWeight = misWeight(s.pdf, pdfBounce);
                } else if (s.mis == MIS_UNLESS_SOLID && !crossedSolidGlass) {
                    lightWeight = misWeight(s.pdf, pdfBounce);
                }
                ADD_LIGHT_EXCEPT(throughput * transmittance * contribution * lightWeight, currentVertex);
            }

            if (endInCache) {
                // all the light reflected here that came via other surfaces
                radiance += clampBounceLight(throughput * cachedLight, giState == 2 ? 0u : reflections + 2);
                break;
            }
        }

        ShadingFrame frame = makeShadingFrame(hit.normal);
        vec3 v = toLocal(frame, -direction);
        vec3 l;
        vec3 weight = vec3(1.0);
        float lobeRoughness = hit.roughness;
        bool transmitted = false;
        bool specularLobe = true;
        vec4 bounceRandom = pathSample(bounce, SAMPLE_BOUNCE, seed);  // xyz: the direction, w: Russian roulette
        if (isFog(hit)) {
            // On from the fog in a direction picked by its phase function: weight 1 (phase / pdf)
            l = toLocal(frame, sampleFogDirection(direction, materials[hit.meshIndex].fogForward, bounceRandom.xy, bouncePdf));
            lobeRoughness = 1.0;
            specularLobe = false;
        } else if (passThrough) {
            l = -v;  // on unchanged (see insideGlass)
            lobeRoughness = 0.0;
            transmitted = true;
        } else if (leavingGlass) {
            if (!sampleGlassExit(hit, v, bounceRandom.xyz, l, bouncePdf)) {
                break;
            }
            transmitted = true;  // in or out, the path stays "through glass" for the light rules above
        } else if (restirVertex && scene.restirBrdfCandidates > 0 && scene.accumulate == 0 && FOG == 0
                   && portalShare == 0.0) {
            // Picked and traced already: the ReSTIR pass's BRDF candidate (same sampling, its own random numbers).
            // Not when accumulating: the bounce then shares its direction with the light choice, which changes how
            // the noise averages out over frames (Sponza: the same single-frame error, but 29% more after 8 frames,
            // 13% after 64, equal after 512; no bias). Under DLSS it is as good or better, and saves a ray.
            BounceHit stored = bounceHits[primaryHitIndex(pixel)];
            if (stored.meshIndex == BOUNCE_NONE) {
                break;
            }
            sharedBounce = true;
            l = toLocal(frame, stored.direction);
            weight = vec3(unpackHalf2x16(stored.weightRG), unpackHalf2x16(stored.weightBRoughness).x);
            bouncePdf = stored.pdf;
            lobeRoughness = float((stored.weightBRoughness >> 16) & 0xFFu) / 255.0;
            transmitted = (stored.weightBRoughness & (1u << 24)) != 0u;
            specularLobe = (stored.weightBRoughness & (2u << 24)) != 0u;
        } else {
            // Indirect light: continue the path in a direction picked by the surface's reflection (mostly where
            // it reflects the most light), or through it. The throughput is multiplied by BRDF * cos / pdf.
            // Or through a portal (portalShare), the pdf then the mix of both (see PORTAL_BOUNCE_SHARE).
            if (portalShare > 0.0 && randomFloat(seed) < portalShare) {
                vec3 aimed = samplePortalDirection(hit.position, hit.normal,
                                                   vec3(randomFloat(seed), randomFloat(seed), randomFloat(seed)));
                l = toLocal(frame, aimed);
                if (l.z <= 0.0) {
                    break;  // behind the surface (part of the opening is): nothing is reflected that way
                }
                float brdfPdf;
                vec3 brdf = evaluateBrdfWorld(hit, hit.normal, -direction, aimed, brdfPdf);
                bouncePdf = mix(brdfPdf, portalDirectionPdf(hit.position, hit.normal, aimed), portalShare);
                weight = brdf * l.z / bouncePdf;
                lobeRoughness = 1.0;  // spread out as a diffuse bounce
                transmitted = false;
                specularLobe = false;
            } else {
                if (!sampleBrdf(hit, v, bounceRandom.xyz, l, weight, bouncePdf, lobeRoughness, transmitted, specularLobe)) {
                    break;
                }
                if (portalShare > 0.0) {
                    float mixed = mix(bouncePdf, portalDirectionPdf(hit.position, hit.normal, toWorld(frame, l)),
                                      portalShare);
                    weight *= bouncePdf / mixed;
                    bouncePdf = mixed;
                }
            }
        }
        pathRoughness = max(pathRoughness, lobeRoughness);
        // The cone at the new ray's start: as wide as it was when it got here; a rough lobe spreads it by about
        // its width (2 * GGX alpha; the diffuse layer counts as roughness 1, so after it textures read small levels)
        coneWidth += coneSpread * hit.t;
        coneSpread += 2.0 * ggxAlpha(lobeRoughness);
        previousLobeRough = !transmitted && lobeRoughness >= CACHE_ROUGHNESS;
        if (bounce == 0 && wantGuides) {
            // (a mirror or clear glass: storeMirrorGuides wrote it)
            firstBounceSpecular = specularLobe && !guideMirror(hit) && !guideClear(hit);
        }
        direction = sharedBounce && bounce == 0 ? bounceHits[primaryHitIndex(pixel)].direction : toWorld(frame, l);
        // Around the smooth normal, a direction can still end up on the wrong side of the actual triangle.
        // Such a path ends: on the real surface, this light would be blocked.
        bool throughSurface = l.z < 0.0;
        float side = dot(direction, hit.geometricNormal);
        if (throughSurface ? side >= 0.0 : side <= 0.0) {
            break;
        }
        // Through glass that light-sampled what is behind it (not reflected back inside: nothing to find there).
        // bouncePdf is then the pdf for MIS (see glassLit).
        bool glassSampled = transmitted && hit.lightThrough && throughSurface;
        // Through the surface of a mesh with fog: into its fog or out of it (see "Fog"). Solid glass, and thin glass
        // too (a bubble of smoke: it doesn't bend the ray, but the fog is still inside). Solid glass without fog too
        // (see "Glass volumes").
        if (throughSurface && (glassSurface || (FOG != 0 && materials[hit.materialIndex].fogDensity > 0.0))) {
            media = hit.frontFace ? enterMedium(media, hit.instanceIndex) : leaveMedium(media, hit.instanceIndex);
        }
        throughput *= weight;
        // Continue from just off the surface, on the side the new ray goes to.
        origin = offsetRay(hit.position, throughSurface ? -hit.geometricNormal : hit.geometricNormal);
        if (transmitted && !glassSampled) {
            throughGlass = true;
            throughSolidGlass = throughSolidGlass || hit.solid;
        } else {
            throughGlass = false;
            throughSolidGlass = false;
            portalCrossings = 0;
            reflectedOnce = reflectedOnce || !transmitted;
            glassLit = transmitted;
            reflections += transmitted ? 0u : 1u;
            restirReflection = restirVertex;
            reflectionPdf = bouncePdf;
            reflectionPosition = hit.position;
            reflectionNormal = hit.normal;
            reflectionTwoSided = hit.diffuseTransmission > 0.0 || hit.lightThrough;
        }

        // Russian roulette: after a few bounces, randomly stop paths that carry little light and
        // boost the survivors by the same factor, so on average the result stays the same.
        // A throughput above 1 (sample weights can exceed 1) always survives: dividing by a chance above 1 would
        // take light away.
        // Not after a mirror reflection: such a path is cheap and goes the same way every frame, so stopping it at
        // random would be the only noise in what the mirror shows. Between colored mirrors (a red and a green wall
        // facing each other) the throughput drops fast, and the few survivors, boosted 10x and more, made the
        // nested reflections speckled. Max bounces still ends such paths.
        bool mirrorBounce = specularLobe && lobeRoughness < MIRROR_ROUGHNESS;
        if (depth >= 2 && !mirrorBounce) {
            float survive = min(max(throughput.r, max(throughput.g, throughput.b)), 1.0);
            if (bounceRandom.w >= survive) {
                break;
            }
            throughput /= survive;
        }
        if (transmitted) {
            glassPasses++;  // no bounce (see glassPasses)
            if (glassPasses > MAX_GLASS_PASSES) {
                break;
            }
        }
    }

    // ReSTIR GI: the sample's Lo, or no sample. Every pixel writes its fresh reservoir every frame; a GI pixel that
    // found no sample still counts one try (M = 1).
    if (CACHE_UPDATE_PASS == 0 && usePrimaryHit && scene.restirGi != 0) {
        uint index = restirGiFreshIndex(pixel);
        if (giState == 2) {
            // Optionally limited (RTXDI does): a rare very bright sample, reused, would stand out over a patch
            float brightness = luminance(radiance);
            if (scene.restirGiRadianceClamp > 0.0 && brightness > scene.restirGiRadianceClamp) {
                radiance *= scene.restirGiRadianceClamp / brightness;
            }
            giReservoirs[index].radiance = radiance;
            radiance = giImageLight;
        } else {
            giReservoirs[index].W = 0.0;
            giReservoirs[index].M = giState == 0 ? 0.0 : 1.0;
            giReservoirs[index].age = 0;
        }
    }

    // Update paths: hand each surface's light to its cache entry (averaged in cache_resolve.comp)
    if (CACHE_UPDATE_PASS != 0) {
        for (uint i = 0; i < cacheVertices; i++) {
            vec3 light = min(cacheLight[i], vec3(CACHE_MAX_RADIANCE));
            if (any(isnan(light)) || any(isinf(light))) {
                continue;
            }
            uint entry = cacheEntries[i] & 0xFFFFFFu;
            atomicAdd(cacheAccumulators[entry].x, light.r);
            atomicAdd(cacheAccumulators[entry].y, light.g);
            atomicAdd(cacheAccumulators[entry].z, light.b);
            atomicAdd(cacheAccumulators[entry].w, 1.0);
        }
    }
#undef ADD_LIGHT
#undef ADD_LIGHT_EXCEPT
    return radiance;
}

vec3 cameraRayDirection(vec2 pixelPosition, ivec2 size) {
    vec2 uv = (pixelPosition + scene.jitter) / vec2(size);
    vec2 ndc = uv * 2.0 - 1.0;  // [-1, 1]; image y goes down, camera up goes up, hence the minus below
    return normalize(scene.cameraForward.xyz
                     + ndc.x * scene.tanHalfFov * scene.aspectRatio * scene.cameraRight.xyz
                     - ndc.y * scene.tanHalfFov * scene.cameraUp.xyz);
}
