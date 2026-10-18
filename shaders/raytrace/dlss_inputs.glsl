// raytrace.comp, part: DLSS inputs.
// Included by raytrace.comp in order: uses what the parts before it define.

// ---------------------------------------------------------------------------------------------

// DLSS inputs

// What DLSS needs to know about the first surface a camera ray hits (see the images at the top). Stored as soon as
// they are known, not at the end of the path: carried along, they would take registers through every bounce.

// Pixel position of a world-space point (w = 1) or direction (w = 0, a point infinitely far away) on the image
vec2 projectToPixel(mat4 viewProjection, vec4 point) {
    vec4 clip = viewProjection * point;
    vec2 ndc = clip.xy / clip.w;
    return (ndc * 0.5 + 0.5) * vec2(imageSize(radianceImage));
}

// Motion vector: where the point was on the image last frame minus where it is now, both without jitter.
// A point behind last frame's camera has no sensible position: no motion then. One just in front of it can be far
// off the image: kept within what a half float holds (the image's format).
vec2 motionVector(vec4 current, vec4 previous) {
    if ((scene.previousViewProjection * previous).w <= 0.0) {
        return vec2(0.0);
    }
    vec2 motion = projectToPixel(scene.previousViewProjection, previous) - projectToPixel(scene.viewProjection, current);
    return clamp(motion, vec2(-65504.0), vec2(65504.0));
}

// Where a point seen through `crossings` openings along the camera ray appeared last frame: point (where it was last
// frame) taken back through those openings as they were then. The camera ray is followed through the openings
// again (their tests only, no ray queries): the same ones the trace went through. Through ends that stood still this
// is where the point appears now, unless the point itself moved.
vec4 seenThroughPortalsLastFrame(vec3 cameraDirection, uint crossings, vec4 point) {
    vec3 origin = scene.cameraPosition.xyz;
    vec3 direction = cameraDirection;
    int skip = -1;
    mat4 back = mat4(1.0);
    for (uint i = 0; i < crossings; i++) {
        float t;
        int portal = nearestPortal(origin, direction, RAY_FAR, skip, t);
        if (portal < 0) {
            break;
        }
        back = back * mat4(portals[portal].lastFromPartner);
        mat4x3 toPartner = portals[portal].toPartner;
        origin = toPartner * vec4(origin + direction * t, 1.0);
        direction = normalize(mat3(toPartner) * direction);
        skip = portal ^ 1;
    }
    return back * point;
}

// Rays that hit nothing: far away, moving only with the camera's rotation. The other guides as DLSS RR's guide
// recommends for the sky: albedo 0.5, no specular, normal and roughness 0, the largest half float as hit distance.
void storeSkyGuides(ivec2 pixel, vec3 direction) {
    imageStore(linearDepthImage, pixel, vec4(scene.farDistance));
    imageStore(motionVectorImage, pixel, vec4(motionVector(vec4(direction, 0.0), vec4(direction, 0.0)), 0.0, 0.0));
    imageStore(diffuseAlbedoImage, pixel, vec4(0.5, 0.5, 0.5, 1.0));
    imageStore(specularAlbedoImage, pixel, vec4(0.0, 0.0, 0.0, 1.0));
    imageStore(normalRoughnessImage, pixel, vec4(0.0));
    imageStore(specularHitDistanceImage, pixel, vec4(65504.0));
}

// The first surface. The albedos split its response the way evaluateBrdf does: the diffuse layer gets what the
// (energy compensated) reflection doesn't return; light going through glass counts as specular (it stays sharp,
// like a reflection). The specular hit distance is 0 (unknown) unless the bounce from here reflects (tracePath).
// direction: the ray's last part (after portals), cameraDirection: the camera ray's own. Seen through a portal, the
// surface is where it appears: `distance` (the whole length) along the camera ray, not where it is; last frame it
// appeared where the openings it went through (portalCrossings, straight from the camera) showed it then, with its
// object's motion (a moving end, or a moving object seen through one, moves on the image). Seen through mirrors
// (storeMirrorGuides) likewise, as if they stood still (portalCrossings 0), its normal turned the way the mirrors turn
// it (toCamera) and its albedos tinted by their color (tint).
void storeSurfaceGuides(ivec2 pixel, Hit hit, vec3 direction, vec3 cameraDirection, bool virtualPosition,
                        uint portalCrossings, float distance, mat3 toCamera, vec3 tint) {
    // A point on an object that moved is reprojected with the object's own motion as well
    vec4 position = vec4(virtualPosition ? scene.cameraPosition.xyz + cameraDirection * distance : hit.position, 1.0);
    vec4 previousPosition = !virtualPosition ? instanceMotion(hit.instanceIndex) * position
                            : FEATURE_PORTALS && portalCrossings > 0
                                ? seenThroughPortalsLastFrame(cameraDirection, portalCrossings,
                                                              instanceMotion(hit.instanceIndex) * vec4(hit.position, 1.0))
                                : position;
    imageStore(linearDepthImage, pixel, vec4(distance * dot(cameraDirection, scene.cameraForward.xyz)));
    imageStore(motionVectorImage, pixel, vec4(motionVector(position, previousPosition), 0.0, 0.0));
    ReflectionEnergy energy = reflectionEnergy(hit, dot(hit.normal, -direction));
    vec3 base = (1.0 - hit.metallic) * (1.0 - energy.dielectricAlbedo) * hit.albedo;
    vec3 diffuseAlbedo = (1.0 - hit.metallic) * (1.0 - energy.dielectricAlbedo) * (1.0 - hit.transmission)
                         * mix(hit.albedo, hit.diffuseTransmissionColor, hit.diffuseTransmission);
    vec3 specularAlbedo = mix(vec3(energy.dielectricAlbedo), energy.metalAlbedo, hit.metallic) + base * hit.transmission;
    // Glowing surfaces: white, no specular. DLSS divides the light by the albedo and multiplies it back; a lamp's
    // own light divided by its (often dark) material color became a huge, noisy value. Bistro at night (DLAA):
    // 54% less error after 8 frames, 23% after 64, the fairy lights less smeared in motion.
    if (luminance(hit.emissive) > 0.0) {
        diffuseAlbedo = vec3(1.0);
        specularAlbedo = vec3(0.0);
    }
    imageStore(diffuseAlbedoImage, pixel, vec4(diffuseAlbedo * tint, 1.0));
    imageStore(specularAlbedoImage, pixel, vec4(specularAlbedo * tint, 1.0));
    imageStore(normalRoughnessImage, pixel, vec4(toCamera * hit.normal, hit.roughness));
    imageStore(specularHitDistanceImage, pixel, vec4(0.0));
}

// A perfect mirror for the guides: smooth (roughness 0) metal. A smooth non-metal isn't one: what it shows is
// mostly its own color (or what is behind it, glass), the reflection only a few percent.
bool guideMirror(Hit hit) {
    return hit.roughness <= 0.0 && hit.metallic >= 0.99 && hit.transmission <= 0.0 && luminance(hit.emissive) <= 0.0;
}

// Clear glass for the guides: smooth and mostly see-through. What it shows is mostly what is behind it, so the guides
// are those of that (storeMirrorGuides), as RTX Remix does for glass. With the glass's own (a smooth specular surface
// at the glass's depth, its hit distance the few mm to the glass's back), DLSS had no edges or texture to keep and
// reprojected the view through it wrongly: glasses in Bistro turned flat grey under DLSS, worse in motion. Only the
// guides: the image pass shades the glass itself (its reflection is a good part of what it shows at grazing angles).
bool guideClear(Hit hit) {
    return FEATURE_GLASS && hit.roughness <= 0.0 && hit.transmission >= 0.5 && hit.metallic < 0.5
           && luminance(hit.emissive) <= 0.0;
}

// Mirrors and the guides (primary surface replacement, as NVIDIA recommends for DLSS RR). DLSS keeps edges and
// texture by the guides of the surface the camera sees; a mirror's are flat and plain, so DLSS can't tell what it
// shows from noise and smudges it. When the camera sees a mirror, this follows the reflection to the first surface
// that isn't one and writes the guides again, for that surface as it appears in the mirrors: at the whole length
// along the camera ray (the reflections are rigid, so the image of a still point stands still), its normal
// mirrored back by each mirror's plane, its albedo tinted by the mirrors' color. The sky: as seen directly.
// A chain longer than maxBounces keeps the first mirror's guides.
// Its own trace from the camera, after the path (see main), rather than following the path's bounces: anything
// the path's loop keeps, or traces in it, costs registers in every pixel (that took the trace shader from 128 to
// 168). The path only notes that the camera sees a mirror (guidesRetrace).
// The same when the camera ray scattered in the fog (see "Fog"): the path never got to the surface, and it changes
// from frame to frame which pixels do; the guides stay those of what the camera sees behind the fog.
bool guidesRetrace = false;

// Clear glass (guideClear) likewise: through it, bent as it bends the light (thin panes: straight on), tinted by
// the share that gets through. Its image isn't rigid like a mirror's: the normal stays as it is. Through thin panes
// the surface is put at the whole length along the camera ray (exact). Solid glass is a lens (a glass of water): what
// is seen through it doesn't move like any point along the camera ray, and the whole length jumps wherever something
// else comes into view behind it (the wall behind a glass, then a chair). Its motion vectors jumped with it, DLSS
// took its history from elsewhere, and the view through the glass shifted while the camera moved. So the depth and
// motion are the first solid glass surface's (smooth across the glass), the rest of the guides what is behind.
// Where the light can't get out (total internal reflection), on along the reflection. Without the media of
// tracePath: from glass into glass (water in a glass) bends as into air, which only moves the guides a little.
void storeMirrorGuides(ivec2 pixel, vec3 cameraDirection, uint seed) {
    mat3 toCamera = mat3(1.0);
    vec3 tint = vec3(1.0);
    float distance = 0.0;
    vec3 origin = scene.cameraPosition.xyz;
    vec3 direction = cameraDirection;
    float coneSpread = scene.pixelSpreadAngle;  // the camera ray's cone, which the mirrors don't widen
    // The first solid glass surface (lensDistance < 0: none yet): its point, seen at that length along the camera ray
    // after mirrors, else where it is (with its object's motion)
    float lensDistance = -1.0;
    vec4 lensPosition = vec4(0.0);
    vec4 lensPrevious = vec4(0.0);
    for (uint i = 0; i <= scene.maxBounces + MAX_GLASS_PASSES; i++) {
        Intersection intersection;
        bool found = intersectClosest(origin, direction, seed, intersection);
        Hit hit;
        if (found) {
            hit = hitFromIntersection(intersection, direction, coneSpread * distance, coneSpread);
            tiltTowardsViewer(hit, direction);
            distance += hit.t;
        }
        bool clear = found && guideClear(hit);
        bool virtualPosition = i > 0 || intersection.travelled != 0.0;
        if (!found || (!guideMirror(hit) && !clear)) {
            if (found) {
                storeSurfaceGuides(pixel, hit, direction, cameraDirection, virtualPosition,
                                   i == 0 ? intersection.crossings : 0u, distance, toCamera, tint);
            } else if (intersection.travelled >= 0.0) {
                storeSkyGuides(pixel, cameraDirection);
            } else {
                return;  // lost in portals
            }
            if (lensDistance >= 0.0) {
                imageStore(linearDepthImage, pixel, vec4(lensDistance * dot(cameraDirection, scene.cameraForward.xyz)));
                imageStore(motionVectorImage, pixel, vec4(motionVector(lensPosition, lensPrevious), 0.0, 0.0));
            }
            return;
        }
        vec3 through = direction;
        if (clear && hit.solid) {
            through = refract(direction, hit.normal, hit.frontFace ? 1.0 / hit.ior : hit.ior);
            if (lensDistance < 0.0) {
                lensDistance = distance;
                lensPosition = vec4(virtualPosition ? scene.cameraPosition.xyz + cameraDirection * distance : hit.position, 1.0);
                lensPrevious = virtualPosition ? lensPosition : instanceMotion(hit.instanceIndex) * lensPosition;
            }
        }
        if (clear && dot(through, through) > 0.0) {
            float cosine = dot(hit.normal, -direction);
            tint *= hit.transmission * (1.0 - fresnelDielectric(cosine, hit.frontFace || !hit.solid ? 1.0 / hit.ior : hit.ior))
                    * (hit.frontFace || !hit.solid ? hit.albedo : vec3(1.0));
            direction = through;
            origin = offsetRay(hit.position, -hit.geometricNormal);
            continue;
        }
        toCamera = toCamera * (mat3(1.0) - 2.0 * outerProduct(hit.normal, hit.normal));
        tint *= clear ? vec3(1.0) : fresnelSchlick(hit.albedo, dot(hit.normal, -direction));
        direction = reflect(direction, hit.normal);
        origin = offsetRay(hit.position, hit.geometricNormal);
    }
}

// Mirrors and ReSTIR. ReSTIR works on the first surface the camera sees; in a mirror that is the mirror, too smooth
// for it, and what the mirror shows got none: plain light sampling, far noisier than the same surface seen directly.
// So the RESTIR_PASS follows the camera ray through mirrors (guideMirror) to the first surface that isn't one, and
// that becomes the pixel's surface: for ReSTIR DI and GI, and for the image pass, which goes on from it with the
// last part's direction and the mirrors' color as its throughput (loadPrimaryHit). The mirrors themselves aren't
// shaded: a perfect mirror's light sampling finds next to nothing, its light comes with its reflection.
// For the reuse, such a surface is seen from the camera's mirror image: its view direction is the last part's
// (RestirSurface.view), and its depth and last frame's pixel are those of where the camera sees it (seenAt), which
// stands still with the mirrors. Through a portal anywhere on the way: not stored, as before (PRIMARY_THROUGH_PORTAL).
// It also writes the guides of pixels that see a mirror (as storeMirrorGuides); the image pass leaves them.
// direction: the camera ray's, then its last part's. travelled: the length before the last part (0: no mirror).
// hit: the surface (unset if nothing was hit), its t along the last part.
bool traceCameraRay(ivec2 pixel, inout vec3 direction, inout uint seed, out Intersection intersection, out Hit hit,
                    out vec3 tint, out float travelled, out bool throughPortal) {
    vec3 cameraDirection = direction;
    vec3 origin = scene.cameraPosition.xyz;
    mat3 toCamera = mat3(1.0);
    tint = vec3(1.0);
    travelled = 0.0;
    throughPortal = false;
    float coneSpread = scene.pixelSpreadAngle;  // the camera ray's cone, which the mirrors don't widen
    for (uint i = 0; i <= scene.maxBounces; i++) {
        bool found = intersectClosest(origin, direction, seed, intersection);
        if (intersection.travelled != 0.0) {
            throughPortal = true;
            return found;
        }
        if (found) {
            hit = hitFromIntersection(intersection, direction, coneSpread * travelled, coneSpread);
            tiltTowardsViewer(hit, direction);
        }
        // With fog the image pass decides where the camera ray scatters, along a straight line (tracePath)
        if (!found || !guideMirror(hit) || i == scene.maxBounces || FOG != 0) {
            if (travelled > 0.0 && scene.writeGuides != 0) {
                if (found) {
                    storeSurfaceGuides(pixel, hit, direction, cameraDirection, true, 0u, travelled + hit.t, toCamera,
                                       tint);
                } else {
                    storeSkyGuides(pixel, cameraDirection);
                }
            }
            return found;
        }
        // A mirror: on along the reflection, its image mirrored by the mirror's plane
        toCamera = toCamera * (mat3(1.0) - 2.0 * outerProduct(hit.normal, hit.normal));
        tint *= fresnelSchlick(hit.albedo, dot(hit.normal, -direction));
        travelled += hit.t;
        origin = offsetRay(hit.position, hit.geometricNormal);
        direction = reflect(direction, hit.normal);
    }
    return false;
}
