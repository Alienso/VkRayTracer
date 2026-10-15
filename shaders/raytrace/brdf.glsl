// raytrace.comp, part: How a surface reflects light (the BRDF).
// Included by raytrace.comp in order: uses what the parts before it define.

// ---------------------------------------------------------------------------------------------
// How a surface reflects light (the BRDF), following the glTF reference model (spec appendix B)
//
// Every surface is a mix of two layers:
//  - diffuse: light enters the material, scatters around and leaves in a random direction, tinted by the
//    base color. Metals don't have it: light that enters a metal is absorbed.
//  - specular: light reflects off the surface itself, like a mirror. A rough surface is modeled as many tiny
//    mirrors (microfacets) with random tilts; the rougher, the more they spread and the blurrier the
//    reflection. GGX is the standard distribution of those tilts.
// How much is reflected by the specular layer is the Fresnel effect: a few percent when looking straight at
// glass or plastic (more for a higher IOR), rising to 100% at grazing angles. Metals reflect a lot at any
// angle, in their own color (gold, copper), which glTF stores as the base color.
//
// Directions here are "local": in a frame where the shading normal is +Z. Both v (towards the viewer) and
// l (towards the light) point away from the surface.

struct ShadingFrame {
    vec3 tangent;
    vec3 bitangent;
    vec3 normal;
};

ShadingFrame makeShadingFrame(vec3 normal) {
    ShadingFrame frame;
    frame.normal = normal;
    buildBasis(normal, frame.tangent, frame.bitangent);
    return frame;
}

vec3 toLocal(ShadingFrame frame, vec3 v) {
    return vec3(dot(v, frame.tangent), dot(v, frame.bitangent), dot(v, frame.normal));
}

vec3 toWorld(ShadingFrame frame, vec3 v) {
    return frame.tangent * v.x + frame.bitangent * v.y + frame.normal * v.z;
}

float luminance(vec3 color) {
    return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

// glTF's roughness is perceptual; the GGX formulas use alpha = roughness^2. A perfect mirror (alpha 0) would
// need its own special case, a tiny minimum makes it an extremely sharp but still "normal" lobe.
// Bounce light clamp (DLSS modes): light that reached the camera after reflecting off two or more surfaces (the
// first surface's indirect light) adds at most scene.bounceLightClamp per sample. Such light is found by chance
// (a bounce ray that happens to hit a small bright lamp): rare, very bright samples that DLSS can't tell from
// detail, so it keeps them for many frames (ghosting) and the image settles slowly. Direct light at the first
// surface (ReSTIR, the shadow rays, lamps seen in a glossy reflection) is never clamped. The limit is set on screen
// (RayTracer.cpp), so it means the same at night and by day. Biased: the clamped light is lost.
vec3 clampBounceLight(vec3 light, uint reflections) {
    if (CACHE_UPDATE_PASS == 0 && scene.bounceLightClamp > 0.0 && reflections >= 2) {
        float l = luminance(light);
        if (l > scene.bounceLightClamp) {
            light *= scene.bounceLightClamp / l;
        }
    }
    return light;
}

float ggxAlpha(float roughness) {
    return max(roughness * roughness, 1e-6);
}

// Reflectance at normal incidence of a non-metal, from its index of refraction: 0.04 for IOR 1.5.
float dielectricF0(float ior) {
    float r = (ior - 1.0) / (ior + 1.0);
    return r * r;
}

// Schlick's approximation of Fresnel: f0 looking straight at the surface, rising to 1 at grazing angles.
float fresnelSchlick(float f0, float cosTheta) {
    return f0 + (1.0 - f0) * pow(1.0 - clamp(cosTheta, 0.0, 1.0), 5.0);
}

vec3 fresnelSchlick(vec3 f0, float cosTheta) {
    return f0 + (1.0 - f0) * pow(1.0 - clamp(cosTheta, 0.0, 1.0), 5.0);
}

// GGX distribution D: how many microfacets face the half vector h, from the sine^2 and cosine of its angle to the
// normal. The sine^2 is passed instead of 1 - cos^2, which loses all precision for the very sharp lobes of smooth
// surfaces (cos is then 1 to many digits).
float ggxDistribution(float sinSquared, float cosTheta, float alpha) {
    float a2 = alpha * alpha;
    float d = sinSquared + cosTheta * cosTheta * a2;
    return a2 / (PI * d * d);
}

// Smith masking G1: the fraction of microfacets facing h that are visible from a direction at cosTheta
// (the others are hidden behind neighboring bumps).
float smithG1(float cosTheta, float alpha) {
    float a2 = alpha * alpha;
    return 2.0 * cosTheta / (cosTheta + sqrt(a2 + (1.0 - a2) * cosTheta * cosTheta));
}

// Smith visibility V = G2 / (4 * cos_l * cos_v), height-correlated: masking from v and shadowing towards l
// are not independent (a bump that hides one tends to hide the other).
float smithVisibility(float cosL, float cosV, float alpha) {
    float a2 = alpha * alpha;
    float gv = cosL * sqrt(cosV * cosV * (1.0 - a2) + a2);
    float gl = cosV * sqrt(cosL * cosL * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-12);
}

// Bilinear lookup in the specular albedo table
vec2 lookupSpecularAlbedo(float cosV, float roughness) {
    float last = float(SPECULAR_ALBEDO_SIZE - 1);
    vec2 p = clamp(vec2(cosV, roughness), 0.0, 1.0) * last;
    uvec2 i = uvec2(min(floor(p), vec2(last - 1.0)));
    vec2 f = p - vec2(i);
    uint row0 = i.y * SPECULAR_ALBEDO_SIZE + i.x;
    uint row1 = row0 + SPECULAR_ALBEDO_SIZE;
    return mix(mix(specularAlbedo[row0], specularAlbedo[row0 + 1], f.x),
               mix(specularAlbedo[row1], specularAlbedo[row1 + 1], f.x), f.y);
}

// Energy bookkeeping of the reflection layer for one view direction.
// The GGX reflection only counts light that reflects off ONE microfacet; light that would hit a neighbouring
// facet next is lost (up to ~70% at roughness 1). In reality it bounces on and mostly escapes. Turquin's fix:
// a white reflection returns E instead of 1, so scale it by 1 / E. For a colored or weak reflection the extra
// bounces return less (each bounce reflects only F0), hence the general factor 1 + F0 * (1 - E) / E.
struct ReflectionEnergy {
    float dielectricCompensation;  // multiplier for the reflection of plastics, glass, ...
    vec3 metalCompensation;        // and of metals (colored: gold gets more saturated with each bounce)
    float dielectricAlbedo;        // total light the (compensated) reflection of a non-metal returns
    vec3 metalAlbedo;              // and of a metal
};

ReflectionEnergy reflectionEnergy(Hit hit, float cosV) {
    vec2 table = lookupSpecularAlbedo(cosV, hit.roughness);
    float e = max(table.x + table.y, 1e-3);  // white reflection, single bounce
    float f0 = dielectricF0(hit.ior);
    ReflectionEnergy energy;
    energy.dielectricCompensation = 1.0 + f0 * (1.0 - e) / e;
    energy.metalCompensation = 1.0 + hit.albedo * (1.0 - e) / e;
    energy.dielectricAlbedo = min((f0 * table.x + table.y) * energy.dielectricCompensation, 1.0);
    energy.metalAlbedo = (hit.albedo * table.x + table.y) * energy.metalCompensation;
    return energy;
}

// Transmission (glass): what passes the reflection layer of a non-metal either scatters inside (diffuse) or
// goes through (transmission), in the ratio given by the transmission value. Light that goes through is
// tinted by the base color.
// Diffuse transmission (leaves, paper): of the diffuse layer, this share comes out on the other side (scattered
// evenly, tinted by its own color) instead of back on the side the light came from.
//
// Sampling picks one layer at random, each with its share of the light it returns. The specular one gets a
// minimum so that a highlight on a dark plastic still gets enough samples.
struct LobeProbabilities {
    float diffuse;
    float specular;
    float transmission;
    float diffuseTransmission;
};

LobeProbabilities lobeProbabilities(Hit hit, ReflectionEnergy energy) {
    float specular = mix(energy.dielectricAlbedo, luminance(energy.metalAlbedo), hit.metallic);
    float base = (1.0 - hit.metallic) * (1.0 - energy.dielectricAlbedo);
    float transmission = base * hit.transmission * luminance(hit.albedo);
    float diffuse = base * (1.0 - hit.transmission) * (1.0 - hit.diffuseTransmission) * luminance(hit.albedo);
    float diffuseTransmission = base * (1.0 - hit.transmission) * hit.diffuseTransmission
                                * luminance(hit.diffuseTransmissionColor);

    LobeProbabilities p;
    float total = specular + diffuse + transmission + diffuseTransmission;
    if (total <= 0.0) {
        p.diffuse = p.specular = p.transmission = p.diffuseTransmission = 0.0;
        return p;
    }
    p.specular = specular / total;
    float rest = diffuse + transmission + diffuseTransmission;
    if (specular > 0.0 && rest > 0.0) {
        p.specular = max(p.specular, 0.1);
    }
    float share = rest > 0.0 ? (1.0 - p.specular) / rest : 0.0;
    p.diffuse = diffuse * share;
    p.transmission = transmission * share;
    p.diffuseTransmission = diffuseTransmission * share;
    return p;
}

// Exact Fresnel reflectance of a smooth boundary between two clear media, eta = n(incident side) / n(other side).
// Unlike Schlick's formula it also works from inside glass, including total internal reflection (returns 1):
// beyond a critical angle no light can leave the denser medium.
float fresnelDielectric(float cosIncident, float eta) {
    float sin2Transmitted = eta * eta * (1.0 - cosIncident * cosIncident);
    if (sin2Transmitted >= 1.0) {
        return 1.0;
    }
    float cosTransmitted = sqrt(1.0 - sin2Transmitted);
    float rs = (eta * cosIncident - cosTransmitted) / (eta * cosIncident + cosTransmitted);
    float rp = (cosIncident - eta * cosTransmitted) / (cosIncident + eta * cosTransmitted);
    return 0.5 * (rs * rs + rp * rp);
}

// Reflected light towards v per unit of light arriving from l (the BRDF value), and the pdf with which
// sampleBrdf would pick l. Covers the reflecting layers (cosL > 0) and diffuse transmission (cosL < 0, light
// arriving from the other side of a thin surface). Light arriving through glass (specular transmission) is not
// evaluated here (see shadowTransmittance and tracePath; rough glass: evaluateGlassTransmission).
// Takes only what the formulas need: the cosines of v and l with the shading normal, of v with the half vector h,
// and h's sine^2 and cosine with the normal. These don't depend on the frame, so the light loop in tracePath can
// evaluate it from world-space vectors (evaluateBrdfWorld) without keeping a shading frame in registers.
vec3 evaluateBrdfAngles(Hit hit, float cosV, float cosL, float cosVH, float hSinSquared, float hCos, out float pdf) {
    if (cosL < 0.0) {
        // Diffuse transmission: Lambert on the other side. Only its own lobe produces such directions with a
        // pdf (specular transmission has none, see sampleBrdf).
        ReflectionEnergy energy = reflectionEnergy(hit, cosV);
        LobeProbabilities p = lobeProbabilities(hit, energy);
        pdf = p.diffuseTransmission * -cosL / PI;
        return (1.0 - hit.metallic) * (1.0 - energy.dielectricAlbedo) * (1.0 - hit.transmission)
               * hit.diffuseTransmission * hit.diffuseTransmissionColor / PI;
    }
    float alpha = ggxAlpha(hit.roughness);
    ReflectionEnergy energy = reflectionEnergy(hit, cosV);

    float d = ggxDistribution(hSinSquared, hCos, alpha);
    float specular = d * smithVisibility(cosL, cosV, alpha);
    float fresnelDielectric = fresnelSchlick(dielectricF0(hit.ior), cosVH);
    vec3 fresnelMetal = fresnelSchlick(hit.albedo, cosVH);

    // Dielectric: diffuse below, getting exactly the light the reflection doesn't return. (The glTF reference
    // takes away 1 - F(v.h) instead, which varies with the light direction: at grazing view angles diffuse +
    // reflection then add up to more than 100%, visible as bright rims in the white furnace.)
    vec3 dielectric = (1.0 - energy.dielectricAlbedo) * (1.0 - hit.transmission) * (1.0 - hit.diffuseTransmission)
                      * hit.albedo / PI
                      + fresnelDielectric * specular * energy.dielectricCompensation;
    vec3 metal = fresnelMetal * specular * energy.metalCompensation;
    vec3 brdf = mix(dielectric, metal, hit.metallic);

    // Diffuse: cosine-weighted. Specular: the VNDF pdf of h, converted to a pdf of l (the reflection doubles
    // angles, hence the 1 / (4 cos_vh)); simplifies to G1(v) * D / (4 cos_v). Transmission only produces
    // directions on the other side, so it adds nothing here (nor does diffuse transmission).
    LobeProbabilities p = lobeProbabilities(hit, energy);
    float pdfDiffuse = cosL / PI;
    float pdfSpecular = smithG1(cosV, alpha) * d / (4.0 * cosV);
    pdf = p.diffuse * pdfDiffuse + p.specular * pdfSpecular;
    return brdf;
}

// The same with local directions (the shading normal is +Z), as sampleBrdf has them. v.z > 0.
vec3 evaluateBrdf(Hit hit, vec3 v, vec3 l, out float pdf) {
    vec3 h = normalize(v + l);
    return evaluateBrdfAngles(hit, v.z, l.z, dot(v, h), h.x * h.x + h.y * h.y, h.z, pdf);
}

// Light through rough glass (hit.lightThrough, see tracePath): the transmission lobe as a BTDF, so light sampling can
// look through frosted glass. Value per unit of light from l (per cosine), and the pdf with which the bounce picks l.
// Both ways of going through give the path a fixed weight w (sampleBrdf's thin pane: the transmitted share over the
// lobe's chance; sampleGlassExit: 1), so the BTDF is simply w * pdf / |cos l|, and it agrees with the sampling exactly.
// Thin: the reflection lobe mirrored to the other side. Leaving solid glass: refraction from glass (ior) into air
// through the microfacet h halfway between them (Walter et al. 2007, "Microfacet Models for Refraction through Rough
// Surfaces"), picked among the visible ones and taken with chance 1 - Fresnel, as sampleGlassExit does.
// l is on the other side (dot(n, l) < 0).
vec3 evaluateGlassTransmission(Hit hit, vec3 n, vec3 v, vec3 l, out float pdf) {
    float alpha = ggxAlpha(hit.roughness);
    float cosV = dot(n, v);
    float cosL = dot(n, l);
    if (hit.solid) {
        vec3 h = -(hit.ior * v + l);
        h = normalize(dot(n, h) < 0.0 ? -h : h);
        float cosVH = dot(v, h);
        float cosLH = dot(l, h);
        float spread = hit.ior * cosVH + cosLH;  // dh / dl = |cos_lh| / spread^2
        if (cosVH <= 0.0 || cosLH >= 0.0 || spread <= 0.0) {
            pdf = 0.0;
            return vec3(0.0);
        }
        vec3 hAcross = cross(n, h);
        float visibleNormals = smithG1(cosV, alpha) * cosVH * ggxDistribution(dot(hAcross, hAcross), dot(n, h), alpha) / cosV;
        pdf = visibleNormals * (1.0 - fresnelDielectric(cosVH, hit.ior)) * -cosLH / (spread * spread);
        return vec3(pdf / -cosL);
    }
    vec3 h = normalize(v + l - 2.0 * cosL * n);
    vec3 hAcross = cross(n, h);
    float lobePdf = smithG1(cosV, alpha) * ggxDistribution(dot(hAcross, hAcross), dot(n, h), alpha) / (4.0 * cosV);
    ReflectionEnergy energy = reflectionEnergy(hit, cosV);
    LobeProbabilities p = lobeProbabilities(hit, energy);
    pdf = p.transmission * lobePdf;
    return (1.0 - hit.metallic) * hit.transmission * (1.0 - energy.dielectricAlbedo) * hit.albedo * lobePdf / -cosL;
}

// The same with world-space directions and the shading normal n. dot(n, v) > 0.
// With hit.lightThrough, light from the other side also comes through the glass: leaving solid glass only that way
// (sampleGlassExit has nothing else), on a thin pane besides its diffuse transmission.
vec3 evaluateBrdfWorld(Hit hit, vec3 n, vec3 v, vec3 l, out float pdf) {
    vec3 glass = vec3(0.0);
    float glassPdf = 0.0;
    if (FEATURE_GLASS && hit.lightThrough && dot(n, l) < 0.0) {
        glass = evaluateGlassTransmission(hit, n, v, l, glassPdf);
        if (hit.solid) {
            pdf = glassPdf;
            return glass;
        }
    }
    vec3 h = normalize(v + l);
    vec3 hAcross = cross(n, h);  // its length is the sine
    vec3 brdf = evaluateBrdfAngles(hit, dot(n, v), dot(n, l), dot(v, h), dot(hAcross, hAcross), dot(n, h), pdf);
    pdf += glassPdf;
    return brdf + glass;
}

// Samples a microfacet normal from the facets visible from v (VNDF), so no samples are wasted on facets that
// face away from the viewer. Dupuy & Benyoub 2023, "Sampling Visible GGX Normals with Spherical Caps".
vec3 sampleGgxVisibleNormal(vec3 v, float alpha, vec2 u) {
    // stretch the view direction so the rough surface becomes a hemisphere of normals
    vec3 vStretched = normalize(vec3(v.xy * alpha, v.z));
    float phi = 2.0 * PI * u.x;
    float z = (1.0 - u.y) * (1.0 + vStretched.z) - vStretched.z;
    float sinTheta = sqrt(clamp(1.0 - z * z, 0.0, 1.0));
    vec3 hStretched = vec3(sinTheta * cos(phi), sinTheta * sin(phi), z) + vStretched;
    // and back
    return normalize(vec3(hStretched.xy * alpha, max(hStretched.z, 0.0)));
}

// Picks the direction the path continues in. Returns false if the sample is unusable (points into the
// surface); otherwise l, its pdf, and weight = BRDF * cos / pdf, the factor the path's throughput changes by.
// lobeRoughness: how spread out the picked layer is (the diffuse layer counts as fully rough).
// transmitted: the path went through glass (l.z < 0). Then pdf is meaningless (not used for MIS) and
// weight is simply the transmitted share of the light: the direction follows the visible microfacets.
// Diffuse transmission also gives l.z < 0, but is an ordinary scattering (transmitted = false, with a pdf).
// specularLobe: a reflection or transmission was picked, not the diffuse layer (for the DLSS hit distance).
// random: x picks the lobe, y and z the direction
bool sampleBrdf(Hit hit, vec3 v, vec3 random, out vec3 l, out vec3 weight, out float pdf,
                out float lobeRoughness, out bool transmitted, out bool specularLobe) {
    float u0 = random.x;
    vec2 u = random.yz;
    ReflectionEnergy energy = reflectionEnergy(hit, v.z);
    LobeProbabilities p = lobeProbabilities(hit, energy);
    transmitted = false;
    specularLobe = u0 < p.transmission + p.specular;

    if (u0 >= 1.0 - p.diffuseTransmission) {
        // Out the other side, cosine-weighted around the reversed normal
        lobeRoughness = 1.0;
        float r = sqrt(u.x);
        float phi = 2.0 * PI * u.y;
        l = vec3(r * cos(phi), r * sin(phi), -sqrt(max(1.0 - u.x, 0.0)));
        if (l.z >= -1e-6) {
            return false;
        }
        vec3 btdf = evaluateBrdf(hit, v, l, pdf);
        if (pdf <= 0.0) {
            return false;
        }
        weight = btdf * -l.z / pdf;
        return true;
    }

    if (u0 < p.transmission) {
        // Through the surface. A rough surface spreads the direction like it spreads reflections.
        transmitted = true;
        lobeRoughness = hit.roughness;
        vec3 h = sampleGgxVisibleNormal(v, ggxAlpha(hit.roughness), u);
        if (hit.solid) {
            // Entering glass: bends towards the normal (Snell's law, n_air / n_glass)
            l = refract(-v, h, 1.0 / hit.ior);
            if (l.z >= 0.0) {
                return false;  // also the zero vector refract() gives for total internal reflection
            }
        } else {
            // Thin: no bending, the way a window pane shifts the view slightly but keeps its direction.
            // Mirror the reflected direction to the other side.
            l = reflect(-v, h);
            if (l.z <= 0.0) {
                return false;
            }
            l.z = -l.z;
        }
        pdf = 0.0;
        if (hit.lightThrough) {
            // Light sampling looks through it too: the pdf for MIS (as evaluateGlassTransmission, with the diffuse
            // transmission's share, which also goes this way)
            float alpha = ggxAlpha(hit.roughness);
            pdf = p.transmission * smithG1(v.z, alpha) * ggxDistribution(h.x * h.x + h.y * h.y, h.z, alpha) / (4.0 * v.z)
                  + p.diffuseTransmission * -l.z / PI;
        }
        weight = (1.0 - hit.metallic) * hit.transmission * (1.0 - energy.dielectricAlbedo) * hit.albedo / p.transmission;
        return true;
    }

    if (u0 < p.transmission + p.specular) {
        vec3 h = sampleGgxVisibleNormal(v, ggxAlpha(hit.roughness), u);
        l = reflect(-v, h);
        lobeRoughness = hit.roughness;
    } else {
        lobeRoughness = 1.0;
        // cosine-weighted around the normal: pick a uniform point on a disc and project it up
        float r = sqrt(u.x);
        float phi = 2.0 * PI * u.y;
        l = vec3(r * cos(phi), r * sin(phi), sqrt(max(1.0 - u.x, 0.0)));
    }
    if (l.z <= 1e-6) {
        return false;
    }
    vec3 brdf = evaluateBrdf(hit, v, l, pdf);
    if (pdf <= 0.0) {
        return false;
    }
    weight = brdf * l.z / pdf;
    return true;
}

// Inside solid glass, at the surface on the way out: either reflect back inside or refract out into the air,
// chosen by the exact Fresnel reflectance of the visible microfacet. Light isn't lost or tinted here (the tint
// was applied on the way in; absorption on the way through), so the weight is 1 either way.
// hit.ior: n_glass / n_air (or the other side's, see "Glass volumes"). pdf: refracted out with hit.lightThrough, for
// MIS (as evaluateGlassTransmission); else 0.
// random: x chooses reflection or refraction, y and z the microfacet
bool sampleGlassExit(Hit hit, vec3 v, vec3 random, out vec3 l, out float pdf) {
    float alpha = ggxAlpha(hit.roughness);
    vec3 h = sampleGgxVisibleNormal(v, alpha, random.yz);
    float cosVH = dot(v, h);
    float fresnel = fresnelDielectric(cosVH, hit.ior);
    pdf = 0.0;
    if (random.x < fresnel) {
        l = reflect(-v, h);
        return l.z > 0.0;
    }
    l = refract(-v, h, hit.ior);
    if (hit.lightThrough && l.z < 0.0) {
        float cosLH = dot(l, h);
        float spread = hit.ior * cosVH + cosLH;
        pdf = smithG1(v.z, alpha) * cosVH * ggxDistribution(h.x * h.x + h.y * h.y, h.z, alpha) / v.z
              * (1.0 - fresnel) * -cosLH / max(spread * spread, 1e-12);
    }
    return l.z < 0.0;
}
