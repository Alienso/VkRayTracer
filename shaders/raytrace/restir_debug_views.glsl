// raytrace.comp, part: ReSTIR debug views.
// Included by raytrace.comp in order: uses what the parts before it define.

vec3 restirDebugLight(ivec2 pixel) {
    if (scene.restirDi == 0) {
        return vec3(0.05);
    }
    Reservoir r = reservoirs[restirIndex(pixel, false)];
    if (r.light == RESTIR_NOT_USED) {
        return vec3(0.05);
    }
    uint kind = r.light >> 30;
    if (kind == LIGHT_KIND_NONE) {
        return vec3(0.3, 0.0, 0.0);
    }
    if (!(r.W > 0.0)) {
        return vec3(0.0);
    }
    return kind == LIGHT_KIND_TRIANGLE ? vec3(1.0, 0.5, 0.1) : kind == LIGHT_KIND_PUNCTUAL ? vec3(0.7, 0.2, 1.0)
                                                                                         : vec3(0.2, 0.5, 1.0);
}

// Debug view "ReSTIR M": how many candidates each pixel's reservoir stands for, relative to one frame's (log scale):
// red = this frame's only (no history, no neighbors), through yellow and green to blue = 64 x. Grey: no ReSTIR there.
vec3 restirDebugM(ivec2 pixel) {
    if (scene.restirDi == 0) {
        return vec3(0.05);
    }
    Reservoir r = reservoirs[restirIndex(pixel, false)];
    if (r.light == RESTIR_NOT_USED) {
        return vec3(0.05);
    }
    float t = clamp(log2(max(r.M, 1.0) / max(restirFreshCount(), 1.0)) / 6.0, 0.0, 1.0);
    return clamp(abs(fract(t * 0.66 + vec3(0.0, 2.0, 1.0) / 3.0) * 6.0 - 3.0) - 1.0, 0.0, 1.0);  // hue: red to blue
}
