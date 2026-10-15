// raytrace.comp, part: Primary hits.
// Included by raytrace.comp in order: uses what the parts before it define.

// ---------------------------------------------------------------------------------------------
// Primary hits
//
// With ReSTIR DI the camera rays are traced in a pass of their own (RESTIR_PASS), before the image pass: ReSTIR
// needs every pixel's first surface before any pixel gets shaded. The image pass then reads the stored hit instead
// of tracing the ray again.

uint primaryHitIndex(ivec2 pixel) {
    return uint(pixel.y) * uint(imageSize(radianceImage).x) + uint(pixel.x);
}

void storePrimaryHit(ivec2 pixel, bool found, Intersection intersection, uint seed, bool throughPortal,
                     float travelled, vec3 direction, vec3 tint) {
    PrimaryHit stored;
    stored.meshIndex = throughPortal ? PRIMARY_THROUGH_PORTAL : found ? intersection.meshIndex : ~0u;
    stored.travelled = travelled;
    stored.direction = direction;
    stored.tintRG = packHalf2x16(tint.rg);
    stored.tintB = packHalf2x16(vec2(tint.b, 0.0));
    stored.instanceIndex = intersection.instanceIndex;
    stored.primitive = intersection.primitive;
    stored.barycentrics = intersection.barycentrics;
    stored.t = intersection.t;
    stored.seed = seed;
    primaryHits[primaryHitIndex(pixel)] = stored;
}

// false: the camera ray hit nothing. The pixel's random numbers go on from where the RESTIR_PASS left them, so
// the path continues exactly as if the image pass had traced the ray itself. (Not for a ray through a portal: see
// primaryHitStored.) Seen in mirrors: direction becomes the ray's last part, tint the mirrors' color, and the
// intersection's travelled the length before it (crossings stays 0: that tells it from a portal's).
bool loadPrimaryHit(ivec2 pixel, inout uint seed, out Intersection intersection, inout vec3 direction, out vec3 tint) {
    PrimaryHit stored = primaryHits[primaryHitIndex(pixel)];
    seed = stored.seed;
    intersection.travelled = stored.travelled;
    intersection.crossings = 0;
    intersection.inFog = false;
    intersection.lampWeight = 0.0;
    tint = vec3(1.0);
    if (stored.travelled > 0.0) {
        direction = stored.direction;
        tint = vec3(unpackHalf2x16(stored.tintRG), unpackHalf2x16(stored.tintB).x);
    }
    if (stored.meshIndex >= PRIMARY_THROUGH_PORTAL) {
        return false;
    }
    intersection.meshIndex = stored.meshIndex;
    intersection.instanceIndex = stored.instanceIndex;
    intersection.primitive = stored.primitive;
    intersection.barycentrics = stored.barycentrics;
    intersection.t = stored.t;
    return true;
}

// Whether the RESTIR_PASS stored the camera ray's hit: not if it went through a portal
bool primaryHitStored(ivec2 pixel) {
    return primaryHits[primaryHitIndex(pixel)].meshIndex != PRIMARY_THROUGH_PORTAL;
}
