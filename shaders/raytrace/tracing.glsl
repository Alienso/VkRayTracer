// raytrace.comp, part: Tracing: rays against the scene, hits and what they found.
// Included by raytrace.comp in order: uses what the parts before it define.

// ---------------------------------------------------------------------------------------------
// Tracing

// Where a new ray leaves a surface point p: just off the surface, on the side n points to (Wachter & Binder, "A
// Fast and Robust Method for Avoiding Self-Intersection", Ray Tracing Gems 2019). p, computed in floats, is off the
// real surface by some rounding error, which grows with p's size: the step is a fixed number of float steps (ulps)
// of each coordinate, so it is as small as it can safely be anywhere: tiny near the origin, larger far out. Near
// the origin, where float steps get extremely small, a fixed tiny distance instead. A fixed distance everywhere
// would be too small far out (the ray hits its own surface: shadow acne) or too big near (it starts behind a
// nearby surface: light leaks through corners and thin walls). The rays then start at t = 0.
// p must come from the triangle (barycentrics, see hitFromIntersection), not origin + t * direction, whose error
// grows with the ray's length too.
vec3 offsetRay(vec3 p, vec3 n) {
    const float nearOrigin = 1.0 / 32.0;
    const float floatScale = 1.0 / 65536.0;
    const float intScale = 256.0;
    ivec3 steps = ivec3(intScale * n);
    vec3 stepped = intBitsToFloat(floatBitsToInt(p) + ivec3(p.x < 0.0 ? -steps.x : steps.x,
                                                            p.y < 0.0 ? -steps.y : steps.y,
                                                            p.z < 0.0 ? -steps.z : steps.z));
    return vec3(abs(p.x) < nearOrigin ? p.x + floatScale * n.x : stepped.x,
                abs(p.y) < nearOrigin ? p.y + floatScale * n.y : stepped.y,
                abs(p.z) < nearOrigin ? p.z + floatScale * n.z : stepped.z);
}

// How far short of a point p on a surface (normal n) a ray must stop not to hit that surface, coming in at cosine
// cosTheta to it: p's rounding error as in offsetRay, along the ray. (Nearly edge-on, at most 100 times that.)
float stopShortOf(vec3 p, vec3 n, float cosTheta) {
    return dot(offsetRay(p, n) - p, n) / max(abs(cosTheta), 0.01);
}

struct Hit {
    bool found;
    float t;                // distance along the ray
    vec3 position;          // world space
    vec3 normal;            // world space, facing the ray. Smooth (interpolated) if the mesh has normals: use for lighting.
    vec3 geometricNormal;   // world space, facing the ray. The triangle's real plane: use to offset new rays.
    vec3 tangent;           // world space, only for the debug view
    vec2 uv;
    vec2 uvFootprintX;      // the hit's footprint in UV space: two axes of the ellipse (see uvFootprint)
    vec2 uvFootprintY;
    uint meshIndex;
    uint instanceIndex;     // which TLAS instance (for its motion)
    uint materialIndex;
    int lightIndex;         // this triangle in emissiveTriangles, -1 if it doesn't glow
    bool frontFace;         // hit from the outside (the side the normals point to)

    // Material at this point (constants * textures)
    vec3 albedo;            // base color
    float alpha;
    float metallic;
    float roughness;
    float transmission;
    float ior;
    bool solid;
    float diffuseTransmission;
    vec3 diffuseTransmissionColor;
    vec3 emissive;
    // Set by tracePath: light sampling here also looks through the glass (see "Light through rough glass")
    bool lightThrough;
};

// Texture reads at a hit, filtered over the hit's footprint (see uvFootprint): the GPU picks the mip level and
// filters along the footprint's long axis (anisotropic filtering). A footprint of 0 reads the full-size level.
// The texture transform scales and rotates the footprint like the UVs.
vec4 sampleTexture(int index, mat3x2 uvTransform, vec2 uv, vec2 footprintX, vec2 footprintY) {
    mat2 linear = mat2(uvTransform[0], uvTransform[1]);
    return textureGrad(textures[nonuniformEXT(index)], uvTransform * vec3(uv, 1.0), linear * footprintX, linear * footprintY);
}

vec4 sampleTexture(int index, mat3x2 uvTransform, Hit hit) {
    return sampleTexture(index, uvTransform, hit.uv, hit.uvFootprintX, hit.uvFootprintY);
}

// The full-size level, for points that are tested rather than seen: the cut-out test during traversal (averaged
// mip levels would let leaves fade into holes at a distance), and points picked on glowing triangles.
vec4 sampleTextureFull(int index, mat3x2 uvTransform, vec2 uv) {
    return textureLod(textures[nonuniformEXT(index)], uvTransform * vec3(uv, 1.0), 0.0);
}

void evaluateMaterial(inout Hit hit) {
    Material material = materials[hit.materialIndex];

    vec4 baseColor = material.baseColor;
    if (material.baseColorTexture >= 0) {
        baseColor *= sampleTexture(material.baseColorTexture, material.baseColorUvTransform, hit);
    }
    hit.albedo = baseColor.rgb;
    hit.alpha = baseColor.a;

    hit.metallic = material.metallic;
    hit.roughness = material.roughness;
    if (material.metallicRoughnessTexture >= 0) {
        vec4 metallicRoughness = sampleTexture(material.metallicRoughnessTexture, material.metallicRoughnessUvTransform, hit);
        hit.roughness *= metallicRoughness.g;
        hit.metallic *= metallicRoughness.b;
    }

    hit.transmission = FEATURE_GLASS ? material.transmission : 0.0;
    if (FEATURE_GLASS && material.transmissionTexture >= 0) {
        hit.transmission *= sampleTexture(material.transmissionTexture, material.transmissionUvTransform, hit).r;
    }
    hit.ior = material.ior;
    hit.solid = material.solid != 0;
    hit.lightThrough = false;

    hit.diffuseTransmission = FEATURE_DIFFUSE_TRANSMISSION ? material.diffuseTransmission : 0.0;
    if (FEATURE_DIFFUSE_TRANSMISSION && material.diffuseTransmissionTexture >= 0) {
        hit.diffuseTransmission *= sampleTexture(material.diffuseTransmissionTexture, material.diffuseTransmissionUvTransform, hit).a;
    }
    hit.diffuseTransmissionColor = material.diffuseTransmissionColor;
    if (hit.diffuseTransmission > 0.0 && material.diffuseTransmissionColorTexture >= 0) {
        hit.diffuseTransmissionColor *= sampleTexture(material.diffuseTransmissionColorTexture,
                                                      material.diffuseTransmissionColorUvTransform, hit).rgb;
    }

    hit.emissive = material.emissive;
    if (material.emissiveTexture >= 0) {
        hit.emissive *= sampleTexture(material.emissiveTexture, material.emissiveUvTransform, hit).rgb;
    }
}

// How solid a material is at a point: 1 = solid, 0 = a hole. Alpha is the base color's alpha (constant *
// texture). Mask is all or nothing at the cutoff; blend is in between. Opaque materials ignore alpha.
float opacityAt(uint materialIndex, vec2 uv) {
    Material material = materials[materialIndex];
    if (!FEATURE_CUTOUTS || material.alphaMode == ALPHA_OPAQUE) {
        return 1.0;
    }
    float alpha = material.baseColor.a;
    if (material.baseColorTexture >= 0) {
        alpha *= sampleTextureFull(material.baseColorTexture, material.baseColorUvTransform, uv).a;
    }
    if (material.alphaMode == ALPHA_MASK) {
        return alpha >= material.alphaCutoff ? 1.0 : 0.0;
    }
    return alpha;
}

// Decides whether a ray hits or passes through a cut-out surface at a candidate hit (see traverse).
// A see-through (blend) surface stops a ray with chance alpha. Over many rays, alpha of the light is stopped
// and 1 - alpha passes, which is exactly what blending means: no sorting of transparent layers needed.
bool candidateIsSolid(uint meshIndex, int primitive, vec2 bary, inout uint seed, bool invisibleCounts) {
    MeshInfo mesh = meshInfos[meshIndex];
    if (FOG != 0 && !invisibleCounts && materials[mesh.materialIndex].invisible != 0) {
        return false;
    }
    uint firstIndex = mesh.indexOffset + uint(primitive) * 3;
    vec2 uv0 = vertices[mesh.vertexOffset + indices[firstIndex + 0]].uv;
    vec2 uv1 = vertices[mesh.vertexOffset + indices[firstIndex + 1]].uv;
    vec2 uv2 = vertices[mesh.vertexOffset + indices[firstIndex + 2]].uv;
    vec2 uv = uv0 * (1.0 - bary.x - bary.y) + uv1 * bary.x + uv2 * bary.y;

    float opacity = opacityAt(mesh.materialIndex, uv);
    return opacity >= 1.0 || (opacity > 0.0 && randomFloat(seed) < opacity);
}

// Portal pieces (portals/PortalPieces.h): an object part way through a portal is cut at the opening, and its part
// beyond is shown at the other end by a ghost instance, cut to that end's side. A cut instance is "not opaque" (its
// candidate hits come to the shader) and holds 1 + its clip region as its TLAS instance's shader binding table
// offset (no ray tracing pipeline reads it); 0: whole. A candidate on a part it doesn't keep is passed, like a hole
// of a cut-out (the same test as portals::pieceKeeps).
bool cutAway(rayQueryEXT rayQuery) {
    uint region = FEATURE_PORTALS ? rayQueryGetIntersectionInstanceShaderBindingTableRecordOffsetEXT(rayQuery, false)
                                  : 0u;
    if (region == 0u) {
        return false;
    }
    region -= 1u;
    vec3 p = rayQueryGetWorldRayOriginEXT(rayQuery)
             + rayQueryGetWorldRayDirectionEXT(rayQuery) * rayQueryGetIntersectionTEXT(rayQuery, false);
    uint cuts = clipRegions[region].cutCount;
    for (uint i = 0; i < cuts; i++) {
        vec3 q = clipRegions[region].cuts[i].worldToEnd * vec4(p, 1.0);
        if (clipRegions[region].cuts[i].side * q.z < -1e-5) {
            if (clipRegions[region].cuts[i].keepNear != 0u) {
                return true;  // behind the end it came out of
            }
            vec2 r = q.xy / clipRegions[region].cuts[i].halfSize;
            if (clipRegions[region].cuts[i].ellipse != 0u ? dot(r, r) <= 1.0 : max(abs(r.x), abs(r.y)) <= 1.0) {
                return true;  // gone into an opening
            }
        }
    }
    return false;
}

// The traversal loop. Hits on opaque instances are accepted by the hardware on its own. Instances a ray can pass
// through (cut-outs, glass, fog) are flagged "not opaque" in the TLAS (see updateScene): for those, the traversal stops
// at every hit as a "candidate" and the shader decides whether it counts (unless PATH_RAY_FLAGS makes the ray treat
// everything as opaque: no cut-outs or fog in this pipeline). Confirmed hits shorten the ray,
// so the closest confirmed one ends up committed. invisibleCounts: surfaces of invisible materials are hits too
// (intersectClosest goes on through them itself, following which fog the ray is in); else they are passed.
// passed: the instance and triangle (x, y) the ray has just gone through (intersectClosest), and the one before (z, w),
// ~0u: none. Two: where two volumes share a surface, the ray goes on from that same distance past one and then the
// other, and must not find the first again.
bool notPassed(rayQueryEXT rayQuery, uvec4 passed) {
    uvec2 triangle = uvec2(rayQueryGetIntersectionInstanceIdEXT(rayQuery, false),
                           rayQueryGetIntersectionPrimitiveIndexEXT(rayQuery, false));
    return triangle != passed.xy && triangle != passed.zw;
}

void traverse(rayQueryEXT rayQuery, inout uint seed, bool invisibleCounts, uvec4 passed) {
    while (rayQueryProceedEXT(rayQuery)) {
        if (PATH_RAY_FLAGS == gl_RayFlagsNoneEXT &&
            rayQueryGetIntersectionTypeEXT(rayQuery, false) == gl_RayQueryCandidateIntersectionTriangleEXT &&
            (FOG == 0 || notPassed(rayQuery, passed)) && !cutAway(rayQuery) &&
            candidateIsSolid(rayQueryGetIntersectionInstanceCustomIndexEXT(rayQuery, false),
                             rayQueryGetIntersectionPrimitiveIndexEXT(rayQuery, false),
                             rayQueryGetIntersectionBarycentricsEXT(rayQuery, false), seed, invisibleCounts)) {
            rayQueryConfirmIntersectionEXT(rayQuery);
        }
    }
}

// Ray cones (Akenine-Moller et al. 2021, "Improved Shader and Texture Level of Detail Using Ray Cones"): every
// ray stands for a thin cone, the part of the scene one pixel sees through it. Its width grows with the distance
// (by the spread angle per unit) and with every bounce (a rough reflection spreads it, see tracePath). Where it
// meets a surface it leaves an ellipse: as wide as the cone across the ray's slant, 1 / cos longer along it.
// Mapped to UV space through the triangle, the ellipse's two axes are what textureGrad needs to pick the mip
// level: a distant or blurry hit reads a small level (fewer texels, which stay in the cache; no shimmering), a
// close one the full size. Ignores how a curved mirror or lens widens or narrows the cone.
// e1, e2: the triangle's edges in world space, uv1, uv2: the same edges in UV space.
void uvFootprint(vec3 e1, vec3 e2, vec2 uv1, vec2 uv2, vec3 direction, float width, out vec2 alongX, out vec2 alongY) {
    alongX = vec2(0.0);
    alongY = vec2(0.0);
    vec3 n = cross(e1, e2);
    float areaSquared = dot(n, n);  // (2 * area)^2
    if (areaSquared < 1e-30 || width <= 0.0) {
        return;
    }
    n *= inversesqrt(areaSquared);
    vec3 across = cross(n, direction);  // on the surface, square to the slant: the ellipse's short axis
    float acrossLength = length(across);
    vec3 a = acrossLength > 1e-6 ? across / acrossLength : normalize(e1);  // straight on: any direction
    vec3 b = cross(n, a);
    vec3 axisA = a * width;
    vec3 axisB = b * (width / max(abs(dot(n, direction)), 0.01));
    // A vector on the triangle as s * e1 + t * e2 (solved with the edges' dot products), then in UV space
    float g11 = dot(e1, e1), g12 = dot(e1, e2), g22 = dot(e2, e2);
    vec2 stA = vec2(g22 * dot(axisA, e1) - g12 * dot(axisA, e2), g11 * dot(axisA, e2) - g12 * dot(axisA, e1)) / areaSquared;
    vec2 stB = vec2(g22 * dot(axisB, e1) - g12 * dot(axisB, e2), g11 * dot(axisB, e2) - g12 * dot(axisB, e1)) / areaSquared;
    alongX = uv1 * stA.x + uv2 * stA.y;
    alongY = uv1 * stB.x + uv2 * stB.y;
}

// What a ray query tells about a hit: WHICH instance and triangle, WHERE on it (barycentrics), and how far.
// Everything else (Hit) we look up in our own buffers, in hitFromIntersection.
struct Intersection {
    uint meshIndex;      // the instance's custom index (set to the mesh index in the TLAS)
    uint instanceIndex;  // which TLAS instance
    int primitive;       // which triangle of the mesh
    vec2 barycentrics;
    float t;             // along the ray's last part (after the last portal it went through)
    float travelled;     // the length of its parts before that (0: no portal). -1: lost in portals (see below)
    uint crossings;      // how many portals it went through
    bool inFog;          // it scattered in fog first, at t along its last part (no hit then, see "Fog")
    uint fogMedium;      // in which (the material)
    // The lamps' light in its fog (fogLampLight): a stretch of the last part inside fog, picked by its majorant times
    // its length among them (lampWeight: theirs in all, 0: none), the fog volumes there and their majorant
    vec2 lampSpan;
    uvec2 lampMedia;
    float lampMajorant;
    float lampWeight;
};
