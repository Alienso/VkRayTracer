// raytrace.comp, part: Punctual lights.
// Included by raytrace.comp in order: uses what the parts before it define.

// ---------------------------------------------------------------------------------------------
// Punctual lights
//
// A point light of intensity I (candela) lights a surface facing it from distance d with I / d^2 (lux): the light
// spreads over a sphere. A spot does the same inside its cone, fading out between the inner and the outer cone; a
// directional light gives its intensity (lux) everywhere, like the sun. They have no size: bounce rays never hit
// them (no MIS needed), and their shadows are sharp.

LightSample punctualLightSample(PunctualLight light, vec3 position) {
    LightSample s = noLightSample();
    s.shadowed = true;
    if (light.type == LIGHT_DIRECTIONAL) {
        s.direction = -light.direction;
        s.distance = SKY_DISTANCE;
        s.light = light.intensity;
        return s;
    }
    vec3 toLight = light.position - position;
    float distanceSquared = dot(toLight, toLight);
    float distance = sqrt(distanceSquared);
    if (distance < 1e-4) {
        return noLightSample();
    }
    s.direction = toLight / distance;
    s.distance = distance * 0.999;
    s.light = light.intensity / distanceSquared;
    if (light.range > 0.0) {
        // glTF's recommended fade-out: smooth, and exactly 0 at the range
        float ratio = distance / light.range;
        float window = clamp(1.0 - ratio * ratio * ratio * ratio, 0.0, 1.0);
        s.light *= window * window;
    }
    if (light.type == LIGHT_SPOT) {
        float cosAxis = dot(-s.direction, light.direction);
        float t = clamp((cosAxis - light.cosOuterCone) / (light.cosInnerCone - light.cosOuterCone), 0.0, 1.0);
        s.light *= t * t;
    }
    return s;
}

// Up to MAX_PUNCTUAL_LIGHTS_PER_HIT lights: a shadow ray to each (exact). More: one picked at random, counted
// punctualLightCount times (correct on average, noisy with many lights).
const uint MAX_PUNCTUAL_LIGHTS_PER_HIT = 8;
uint punctualLightSamples() {
    uint count = punctualLightCount();
    return count <= MAX_PUNCTUAL_LIGHTS_PER_HIT ? count : 1;
}
LightSample samplePunctualLight(uint index, vec3 position, inout uint seed) {
    uint count = punctualLightCount();
    if (count <= MAX_PUNCTUAL_LIGHTS_PER_HIT) {
        return punctualLightSample(punctualLights[index], position);
    }
    uint picked = min(uint(randomFloat(seed) * float(count)), count - 1);
    LightSample s = punctualLightSample(punctualLights[picked], position);
    s.light *= float(count);
    return s;
}

// The part of [span.x, span.y] along a ray (origin, direction) inside a spot light's cone (x >= y: none). The cone
// (under 90 degrees wide) is convex, so that part is one stretch: its ends are among the span's ends and where the
// ray crosses the cone's surface, a quadratic's roots; between two of them it is all in or all out.
vec2 spotConeSpan(PunctualLight light, vec3 origin, vec3 direction, vec2 span) {
    vec3 fromApex = origin - light.position;
    float along = dot(direction, light.direction), startAlong = dot(fromApex, light.direction);
    float c2 = light.cosOuterCone * light.cosOuterCone;
    // inside: dot(p - apex, axis) >= cos * |p - apex|, i.e. a t^2 + b t + c >= 0 with dot(p - apex, axis) >= 0
    float a = along * along - c2 * dot(direction, direction);
    float b = 2.0 * (along * startAlong - c2 * dot(direction, fromApex));
    float c = startAlong * startAlong - c2 * dot(fromApex, fromApex);
    float ends[4] = float[4](span.x, span.y, span.y, span.y);
    float discriminant = b * b - 4.0 * a * c;
    if (discriminant >= 0.0 && abs(a) > 1e-12) {
        float root = sqrt(discriminant);
        ends[1] = clamp((-b - root) / (2.0 * a), span.x, span.y);
        ends[2] = clamp((-b + root) / (2.0 * a), span.x, span.y);
        if (ends[1] > ends[2]) {
            float swap = ends[1]; ends[1] = ends[2]; ends[2] = swap;
        }
    }
    vec2 inside = vec2(span.y, span.x);
    for (int i = 0; i < 3; i++) {
        float t = 0.5 * (ends[i] + ends[i + 1]);
        vec3 p = fromApex + direction * t;
        float axial = dot(p, light.direction);
        if (ends[i + 1] > ends[i] && axial >= 0.0 && axial * axial >= c2 * dot(p, p)) {
            inside = vec2(min(inside.x, ends[i]), max(inside.y, ends[i + 1]));
        }
    }
    return inside;
}

// The light fog scatters into a ray from the punctual lights along its stretches inside fog (Intersection.lampSpan):
// one stretch, picked by its majorant times its length, one light picked at random, at one point picked by equi-angular sampling
// (Kulla & Fajardo 2012, "Importance Sampling Techniques for Path Tracing in Participating Media"): evenly by the
// angle under which the lamp sees the ray, i.e. as densely as its 1 / distance^2. A directional light: evenly along
// the stretch.
// No loss on the way from origin to that point: the stretch ends where the path scatters, if it does, and a point
// gets onto it exactly as often as light from it would get through (see "Fog"); averaged, that is the loss.
// Portals: only the ray's last part (origin, direction), the one it scattered or hit something in.
vec3 fogLampLight(vec3 origin, vec3 direction, Intersection stretch, uint bounce, inout uint seed) {
    vec2 span = stretch.lampSpan;
    vec4 u = pathSample(bounce, SAMPLE_FOG, seed);  // x: where the ray scatters (intersectClosest)
    uint count = punctualLightCount();
    PunctualLight light = punctualLights[min(uint(u.y * float(count)), count - 1)];
    // A spot light lights only the part inside its cone: the points are picked there (a narrow beam seen from the
    // side got few points in it: slow-fading speckles)
    vec2 lit = light.type == LIGHT_SPOT ? spotConeSpan(light, origin, direction, span) : span;
    if (!(lit.x < lit.y)) {
        return vec3(0.0);
    }
    float t, pdf;
    if (light.type == LIGHT_DIRECTIONAL) {
        t = mix(span.x, span.y, u.z);
        pdf = 1.0 / (span.y - span.x);
    } else {
        float along = dot(light.position - origin, direction);  // the ray's point nearest the lamp
        float height = max(length(light.position - origin - direction * along), 1e-4);  // and its distance
        float angleA = atan((lit.x - along) / height);
        float angleB = atan((lit.y - along) / height);
        float offset = height * tan(mix(angleA, angleB, u.z));
        t = along + offset;
        pdf = height / ((angleB - angleA) * (height * height + offset * offset));
    }
    vec3 point = origin + direction * t;
    LightSample s = punctualLightSample(light, point);
    if (all(equal(s.light, vec3(0.0))) || !(pdf > 0.0)) {
        return vec3(0.0);
    }
    // What the fog there scatters towards the ray, per unit of length: each volume's density, color and phase
    vec3 scattering = vec3(0.0);
    float cosTheta = dot(direction, s.direction);
    for (uvec2 rest = stretch.lampMedia; firstMedium(rest) != NO_MEDIUM; rest = otherMedia(rest)) {
        uint material = mediumMaterial(firstMedium(rest));
        scattering += fogDensityAt(firstMedium(rest), point) * materials[material].fogColor
                      * fogPhase(cosTheta, materials[material].fogForward);
    }
    if (all(equal(scattering, vec3(0.0)))) {
        return vec3(0.0);  // a clear spot of varying fog
    }
    // The stretch's chance was its majorant * length / lampWeight
    float scale = float(count) * stretch.lampWeight / (stretch.lampMajorant * (span.y - span.x) * pdf);
    bool crossedGlass, crossedSolidGlass;
    return s.light * scale * scattering
           * shadowTransmittance(point, s.direction, s.distance, -1, seed, stretch.lampMedia, crossedGlass,
                                 crossedSolidGlass);
}

// A light seen through portal end `end`: where it appears from this side, moved back through the portal. A shadow
// ray towards it goes through the end's opening and on from the partner to the light itself (shadowTransmittance
// with throughEnd), the same distance in all, so falloff and spot cones come out right. which: 0 = the sun, k =
// the punctual light of slot k - 1 (as samplePunctualLight). Most points don't see a light's image through a given
// opening: no light then (s.light = 0), for the cost of one plane test.
LightSample portalLightImage(uint end, uint which, vec3 position, inout uint seed) {
    mat4x3 toPartner = portals[end].toPartner;
    mat3 back = transpose(mat3(toPartner));  // the partner's side -> this side (a rotation: transposed = inverse)
    LightSample s = noLightSample();
    if (which == 0) {
        // Without "Shadows" the sun isn't blocked by the opening either: it already lights everything directly
        if (scene.sunOn == 0 || scene.shadowsEnabled == 0) {
            return s;
        }
        vec2 u = vec2(randomFloat(seed), randomFloat(seed));
        s.direction = back * randomDirectionInCone(scene.sunDirection.xyz, scene.sunDirection.w, u);
        s.distance = SKY_DISTANCE;
        s.light = PI * scene.sunColor.rgb;
        s.pdf = scene.sunPdf;
        s.mis = scene.sunMis != 0 ? MIS_UNLESS_GLASS : MIS_NONE;
        s.shadowed = true;
    } else {
        uint count = punctualLightCount();
        uint index = which - 1;
        float scale = 1.0;
        if (count > MAX_PUNCTUAL_LIGHTS_PER_HIT) {
            index = min(uint(randomFloat(seed) * float(count)), count - 1);
            scale = float(count);
        }
        PunctualLight light = punctualLights[index];
        light.position = back * (light.position - toPartner[3]);
        light.direction = back * light.direction;
        s = punctualLightSample(light, position);
        s.light *= scale;
    }
    float t;
    if (!portalOpeningHit(end, position, s.direction, s.distance, t)) {
        s.light = vec3(0.0);
    }
    return s;
}

// Bounce rays through portals. Light sampling finds only the sun's and the punctual lights' images through a portal;
// the sky, glowing surfaces and lit rooms behind one were found only by bounce rays that happened to go through: a
// small window into a dark room lit it with fireflies. So at a rough surface that sees openings, PORTAL_BOUNCE_SHARE
// of the bounce rays aim through one (an end picked by about the solid angle it covers, a point on it picked evenly),
// the others are picked by the surface's reflection as usual. Either way the ray's pdf is the mix of both
// strategies' (one-sample MIS, Veach 1997), so the result stays unbiased, and its weight is at most 1 / (1 -
// PORTAL_BOUNCE_SHARE) times the usual one: no new fireflies. The aim doesn't know what is in the way: an opening
// behind a wall, or in another room, costs its share of the bounce rays for nothing.
const float PORTAL_BOUNCE_SHARE = 0.5;

float portalArea(uint end) {
    return portals[end].halfWidth * portals[end].halfHeight * (portals[end].shape == PORTAL_ELLIPSE ? PI : 4.0);
}

// How much an end is worth aiming at from a surface: about the solid angle it covers (its center's), 0 where it is
// all behind the surface or the surface is in its plane
float portalAimWeight(uint end, vec3 position, vec3 normal) {
    vec3 toCenter = portals[end].center - position;
    float distanceSquared = dot(toCenter, toCenter);
    float facing = abs(dot(toCenter, portals[end].normal));  // cos * distance
    vec3 across = portals[end].right * portals[end].halfWidth;
    vec3 up = cross(portals[end].normal, portals[end].right) * portals[end].halfHeight;
    float above = max(max(dot(toCenter + across + up, normal), dot(toCenter + across - up, normal)),
                      max(dot(toCenter - across + up, normal), dot(toCenter - across - up, normal)));
    if (facing < 1e-4 || above <= 0.0) {
        return 0.0;
    }
    return min(portalArea(end) * facing / (distanceSquared * sqrt(distanceSquared)), 2.0 * PI);
}

float portalAimTotal(vec3 position, vec3 normal) {
    float total = 0.0;
    for (uint i = 0; i < portalCount(); i++) {
        total += portalAimWeight(i, position, normal);
    }
    return total;
}

// A direction through an opening (portalAimTotal > 0): random.x picks the end, yz the point on it
vec3 samplePortalDirection(vec3 position, vec3 normal, vec3 random) {
    float pick = random.x * portalAimTotal(position, normal);
    uint end = 0;
    for (uint i = 0; i < portalCount(); i++) {
        float weight = portalAimWeight(i, position, normal);
        if (weight > 0.0) {
            end = i;  // (the last one with a weight, should rounding leave pick above them all)
            if (pick < weight) {
                break;
            }
            pick -= weight;
        }
    }
    vec2 p = 2.0 * random.yz - 1.0;
    if (portals[end].shape == PORTAL_ELLIPSE) {
        float angle = 2.0 * PI * random.z;
        p = sqrt(random.y) * vec2(cos(angle), sin(angle));
    }
    vec3 up = cross(portals[end].normal, portals[end].right);
    vec3 point = portals[end].center + portals[end].right * (portals[end].halfWidth * p.x)
                 + up * (portals[end].halfHeight * p.y);
    return normalize(point - position);
}

// How likely samplePortalDirection is to pick a direction, per solid angle: over every end whose opening the line
// crosses (whichever comes first: the pick doesn't look)
float portalDirectionPdf(vec3 position, vec3 normal, vec3 direction) {
    float pdf = 0.0;
    float total = 0.0;
    for (uint i = 0; i < portalCount(); i++) {
        float weight = portalAimWeight(i, position, normal);
        float t;
        if (weight > 0.0 && portalOpeningHit(i, position, direction, RAY_FAR, t)) {
            pdf += weight * t * t / (portalArea(i) * abs(dot(direction, portals[i].normal)));
        }
        total += weight;
    }
    return total > 0.0 ? pdf / total : 0.0;
}
