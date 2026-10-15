// raytrace.comp, part: Light sampling for glowing surfaces, and MIS.
// Included by raytrace.comp in order: uses what the parts before it define.

// ---------------------------------------------------------------------------------------------
// Light sampling for glowing surfaces, and multiple importance sampling (MIS)
//
// There are two ways to find the light a glowing triangle sends to a surface point:
//  a) bounce sampling: the bounce ray happens to hit it. Good for big lights, bad for small ones (rarely hit).
//  b) light sampling: pick a random point on a random glowing triangle and send a shadow ray there.
//     Good for small lights, bad for big ones close by (points near the edge get picked at grazing angles).
// Both are correct on their own. MIS uses both and weights each sample by how likely its own method was to
// produce it compared to the other one, so each method mostly contributes where it is strong, and the
// light is still counted exactly once in total.
//
// A "pdf" (probability density) below is: how likely is a method to pick this exact direction. Comparing them
// needs the same unit for both, so both are expressed per solid angle (per unit of direction).

// Power heuristic: the usual MIS weight. Gives the sample almost entirely to the method that was much more
// likely to produce it.
float misWeight(float pdfThis, float pdfOther) {
    float a = pdfThis * pdfThis;
    float b = pdfOther * pdfOther;
    return a + b > 0.0 ? a / (a + b) : 0.0;
}

// Where on a picked glowing triangle the shadow ray goes. Evenly by area is simple, but a large triangle close to
// the point looks very different from there: its near part fills most of the point's view of it, its far part is a
// sliver, and yet the far part gets most of the samples (more area). Picking directions evenly over the triangle's
// solid angle (the patch of the point's sky it covers) instead puts the samples where the light comes from (Arvo
// 1995, "Stratified Sampling of Spherical Triangles"; as in pbrt-v4). Its pdf is 1 / solid angle, the same in every
// direction. (It ignores the cosine at the surface, which Peters 2021 also accounts for, at more cost.)
// Tiny triangles (most of them, seen from afar) are sampled by area: the same result there, and the spherical
// formulas lose precision; so are huge ones (nearly around the point). The limits are pbrt-v4's.
const float SPHERICAL_MIN = 3e-4;  // steradians
const float SPHERICAL_MAX = 6.22;

// The solid angle of a triangle seen from p, if it is to be sampled by solid angle; 0: by area
float sphericalSolidAngle(EmissiveTriangle light, vec3 p) {
    if (scene.emissiveSolidAngle == 0) {
        return 0.0;
    }
    vec3 a = light.p0 - p, b = light.p1 - p, c = light.p2 - p;
    float la = length(a), lb = length(b), lc = length(c);
    if (min(la, min(lb, lc)) <= 0.0) {
        return 0.0;
    }
    a /= la;
    b /= lb;
    c /= lc;
    // Van Oosterom & Strackee 1983
    float solidAngle = abs(2.0 * atan(dot(a, cross(b, c)), 1.0 + dot(a, b) + dot(a, c) + dot(b, c)));
    return solidAngle > SPHERICAL_MIN && solidAngle < SPHERICAL_MAX ? solidAngle : 0.0;
}

// The angle between unit vectors, precise near 0 and PI too (unlike acos of the dot product)
float angleBetween(vec3 x, vec3 y) {
    return dot(x, y) < 0.0 ? PI - 2.0 * asin(min(length(x + y) * 0.5, 1.0)) : 2.0 * asin(min(length(y - x) * 0.5, 1.0));
}

// A point on the triangle (v0, v1, v2) whose direction from p is spread evenly over the triangle's solid angle
// (Arvo 1995, written as in pbrt-v4's SampleSphericalTriangle), as barycentric weights. u.x picks how much of the
// solid angle lies on one side of the direction (a sub-triangle of that area, sharing the corner at v0), u.y
// where along the sub-triangle's far edge. false: too thin to tell.
bool sampleSphericalTriangle(vec3 v0, vec3 v1, vec3 v2, vec3 p, vec2 u, out vec3 weights) {
    weights = vec3(1.0 / 3.0);
    vec3 a = normalize(v0 - p), b = normalize(v1 - p), c = normalize(v2 - p);
    // the planes through p and each edge, and the triangle's angles between them
    vec3 nab = cross(a, b), nbc = cross(b, c), nca = cross(c, a);
    if (dot(nab, nab) == 0.0 || dot(nbc, nbc) == 0.0 || dot(nca, nca) == 0.0) {
        return false;
    }
    nab = normalize(nab);
    nbc = normalize(nbc);
    nca = normalize(nca);
    float alpha = angleBetween(nab, -nca);
    float beta = angleBetween(nbc, -nab);
    float gamma = angleBetween(nca, -nbc);
    // the sub-triangle's area (+ PI), and its corner c' on the edge from a to c
    float subArea = mix(PI, alpha + beta + gamma, u.x);
    float cosAlpha = cos(alpha), sinAlpha = sin(alpha);
    float sinPhi = sin(subArea) * cosAlpha - cos(subArea) * sinAlpha;
    float cosPhi = cos(subArea) * cosAlpha + sin(subArea) * sinAlpha;
    float k1 = cosPhi + cosAlpha;
    float k2 = sinPhi - sinAlpha * dot(a, b);
    float cosB = clamp((k2 + (k2 * cosPhi - k1 * sinPhi) * cosAlpha) / ((k2 * sinPhi + k1 * cosPhi) * sinAlpha), -1.0, 1.0);
    vec3 cPrime = cosB * a + sqrt(max(1.0 - cosB * cosB, 0.0)) * normalize(c - dot(c, a) * a);
    // a direction between b and c', evenly over the arc's share of the solid angle
    float cosTheta = 1.0 - u.y * (1.0 - dot(cPrime, b));
    vec3 w = cosTheta * b + sqrt(max(1.0 - cosTheta * cosTheta, 0.0)) * normalize(cPrime - dot(cPrime, b) * b);
    // where that direction meets the triangle (Moller-Trumbore)
    vec3 e1 = v1 - v0, e2 = v2 - v0;
    vec3 s1 = cross(w, e2);
    float divisor = dot(s1, e1);
    if (divisor == 0.0) {
        return false;
    }
    vec3 s = p - v0;
    vec2 bary = clamp(vec2(dot(s, s1), dot(w, cross(s, e1))) / divisor, 0.0, 1.0);
    bary /= max(bary.x + bary.y, 1.0);
    if (any(isnan(bary))) {
        return false;
    }
    weights = vec3(1.0 - bary.x - bary.y, bary);
    return true;
}

// A point on glowing triangle `light` for the surface point p: by solid angle or by area (see above). u: two
// random numbers. false: none (a degenerate triangle).
bool pointOnTriangle(EmissiveTriangle light, vec3 p, vec2 u, out vec3 weights) {
    if (sphericalSolidAngle(light, p) > 0.0) {
        return sampleSphericalTriangle(light.p0, light.p1, light.p2, p, u, weights);
    }
    float r1 = sqrt(u.x);  // evenly over the area (the square root keeps points from bunching up in one corner)
    weights = vec3(1.0 - r1, r1 * (1.0 - u.y), r1 * u.y);
    return true;
}

// pdf (per solid angle) of light sampling choosing the point `distance` away from p on triangle `light`, seen at
// `cosLight` (angle between the triangle's normal and the direction towards p). By solid angle it is
// 1 / solid angle. By area it is 1 / area per area; a small patch far away or seen edge-on covers a small solid
// angle, hence distance^2 / cos to convert.
// pickProbability: the chance that this triangle was picked (lightPickProbability).
float lightPdf(int lightIndex, float pickProbability, vec3 p, float distance, float cosLight) {
    EmissiveTriangle light = emissiveTriangles[lightIndex];
    float solidAngle = sphericalSolidAngle(light, p);
    if (solidAngle > 0.0) {
        return pickProbability / solidAngle;
    }
    return pickProbability / max(light.area, 1e-12) * distance * distance / max(cosLight, 1e-6);
}
