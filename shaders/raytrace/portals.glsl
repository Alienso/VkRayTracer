// raytrace.comp, part: Portals.
// Included by raytrace.comp in order: uses what the parts before it define.

// ---------------------------------------------------------------------------------------------
// Portals
//
// Pairs of flat openings (rectangles or ellipses) placed in the editor. A ray that crosses one, from either side,
// goes on from the other one, as if both were the same window: entering the front of one, it leaves the front of
// the other, entering the back, the back. Each end is one entry, the pair's ends are entries 2k and 2k + 1;
// toPartner takes points and directions from an end to its partner. Rigid (no scaling), so a hit's distance along
// the whole bent ray is the sum of its parts.
// They are not in the TLAS: a ray is traced up to the nearest opening it crosses (tested here; there are only a
// few), and if it hits nothing before that, on from the partner. Two portals facing each other (or one that sees
// itself through the other) would send a ray back and forth forever: after scene.maxPortalCrossings crossings
// ("Max portal crossings") it is lost, and no light comes back along it (black).
// Rays that must go in a straight line to a point (shadow rays, ReSTIR's visibility and reconnections) count an
// opening as blocking. The sun and the punctual lights also shine through portals: light sampling aims at their
// images, where they appear through each end (portalLightImage). Other light through a portal (glowing surfaces,
// the sky, the sun after two portals or more) is found by bounce rays, part of which aim through the openings
// (PORTAL_BOUNCE_SHARE).
struct Portal {
    vec3 center;
    float halfWidth;
    vec3 normal;         // the side it faces
    float halfHeight;
    vec3 right;          // along its width; its height is along cross(normal, right)
    uint shape;          // 0 rectangle, PORTAL_ELLIPSE (PortalShape in Portal.h)
    mat4x3 toPartner;
};
layout(binding = 31, scalar) readonly buffer Portals { Portal portals[]; };
const uint PORTAL_ELLIPSE = 1;
// A ray stops this far before an opening and starts this far after its partner: geometry in the opening's plane
// is left out, so a portal set flush into a wall shows what is behind its partner, not the wall.
const float PORTAL_GAP = 1e-3;

// Whether a ray crosses end i's opening before maxT, and where (t)
bool portalOpeningHit(uint i, vec3 origin, vec3 direction, float maxT, out float t) {
    vec3 normal = portals[i].normal;
    float facing = dot(direction, normal);
    t = dot(portals[i].center - origin, normal) / facing;
    if (abs(facing) < 1e-8 || !(t > 0.0 && t < maxT)) {
        return false;
    }
    vec3 offset = origin + direction * t - portals[i].center;
    vec3 right = portals[i].right;
    vec2 p = vec2(dot(offset, right) / portals[i].halfWidth, dot(offset, cross(normal, right)) / portals[i].halfHeight);
    return portals[i].shape == PORTAL_ELLIPSE ? dot(p, p) <= 1.0 : max(abs(p.x), abs(p.y)) <= 1.0;
}

// The nearest opening a ray crosses before maxT, and its distance t; -1: none. skip: an end to leave out (the one
// the ray just came out of: it starts on that plane).
int nearestPortal(vec3 origin, vec3 direction, float maxT, int skip, out float t) {
    t = maxT;
    int nearest = -1;
    for (uint i = 0; i < portalCount(); i++) {
        float distance;
        if (int(i) != skip && portalOpeningHit(i, origin, direction, t, distance)) {
            nearest = int(i);
            t = distance;
        }
    }
    return nearest;
}

// Whether a straight ray crosses an opening before distance (it is then blocked, see above)
bool portalBetween(vec3 origin, vec3 direction, float distance) {
    float t;
    return portalCount() > 0 && nearestPortal(origin, direction, distance, -1, t) >= 0;
}

// The closest hit along a ray (cut-outs decided with seed, see traverse), through portals and invisible surfaces:
// origin and direction become the last part's (the hit's t is along it). false: nothing hit, the ray leaves the
// scene along direction; or, with travelled = -1, it was lost in portals; or, with inFog, it scattered in fog first.
// media: the fog volumes it starts in (see "Fog"), changed by the invisible surfaces it goes through and, for rays
// that may scatter, looked up again past each portal (mediumAt). fogU >= 0: it may scatter in fog (the first step is
// picked with fogU, later ones with seed); -1: it passes fog unchanged.
bool intersectClosest(inout vec3 origin, inout vec3 direction, inout uint seed, inout uvec2 media, float fogU,
                      out Intersection hit) {
    hit.travelled = 0.0;
    hit.crossings = 0;
    hit.inFog = false;
    hit.fogMedium = NO_MEDIUM;
    hit.lampSpan = vec2(0.0);
    hit.lampMedia = OUTSIDE;
    hit.lampMajorant = 0.0;
    hit.lampWeight = 0.0;
    bool fog = fogU >= 0.0;
    int skip = -1;
    float tMin = 0.0;
    uint volumes = 0;  // invisible surfaces gone through
    uvec4 passed = uvec4(~0u);  // the last two (see traverse)
    // Where it last went through invisible surfaces (-1: none), and the media before them. A surface at that same
    // distance (a floor under a fog box) counts as in front of them: it is in the fog the ray came through, so its
    // shadow and bounce rays start in that fog. (Before, the floor under a fog box was lit as if outside it: its
    // shadow rays to a light inside crossed no fog surface.)
    float passedT = -1.0;
    uvec2 mediaBefore = media;
    for (uint part = 0; part <= scene.maxPortalCrossings + MAX_VOLUME_CROSSINGS; part++) {
        float portalT = RAY_FAR;
        int portal = portalCount() > 0 ? nearestPortal(origin, direction, RAY_FAR, skip, portalT) : -1;
        float tMax = portal >= 0 ? max(portalT - PORTAL_GAP, tMin) : RAY_FAR;
        bool uneven = false;
        float majorant = FOG != 0 && fog ? mediaMajorant(media, uneven) : 0.0;
        // The first tentative step. In even fog it is where the ray scatters, so the ray query only needs to look that
        // far; in varying fog the query finds the surface first, and the steps go on up to it (deltaTrack).
        float fogT = RAY_FAR;
        if (majorant > 0.0) {
            fogT = tMin - log(1.0 - (fogU >= 0.0 ? fogU : randomFloat(seed))) / majorant;
            fogU = -1.0;
        }
        rayQueryEXT rayQuery;
        rayQueryInitializeEXT(rayQuery, tlas, PATH_RAY_FLAGS, 0xFF, origin, tMin, direction,
                              uneven ? tMax : min(tMax, fogT));
        traverse(rayQuery, seed, volumes < MAX_VOLUME_CROSSINGS, passed);
        bool surface = rayQueryGetIntersectionTypeEXT(rayQuery, true) == gl_RayQueryCommittedIntersectionTriangleEXT;
        float end = surface ? rayQueryGetIntersectionTEXT(rayQuery, true) : tMax;
        if (FOG != 0 && fogT < end) {
            uint medium;
            fogT = deltaTrack(media, origin, direction, fogT, end, majorant, seed, medium);
            if (medium != NO_MEDIUM) {
                addLampSpan(hit, tMin, fogT, media, majorant, seed);
                hit.t = fogT;
                hit.inFog = true;
                hit.fogMedium = mediumMaterial(medium);
                return false;
            }
        }
        if (surface) {
            hit.meshIndex = rayQueryGetIntersectionInstanceCustomIndexEXT(rayQuery, true);
            hit.instanceIndex = rayQueryGetIntersectionInstanceIdEXT(rayQuery, true);
            hit.primitive = rayQueryGetIntersectionPrimitiveIndexEXT(rayQuery, true);
            hit.barycentrics = rayQueryGetIntersectionBarycentricsEXT(rayQuery, true);
            hit.t = end;
            MeshInfo mesh = meshInfos[hit.meshIndex];
            uint material = mesh.materialIndex;
            float boundaryDensity = materials[material].fogDensity;
            bool invisible = materials[material].invisible != 0;
            if (FOG != 0 && (invisible || boundaryDensity > 0.0)) {
                // A volume's surface: into it or out of it
                uint firstIndex = mesh.indexOffset + uint(hit.primitive) * 3;
                uvec3 corner = mesh.vertexOffset + uvec3(indices[firstIndex + 0], indices[firstIndex + 1], indices[firstIndex + 2]);
                vec3 weights = vec3(1.0 - hit.barycentrics.x - hit.barycentrics.y, hit.barycentrics.x, hit.barycentrics.y);
                bool entering = entersMesh(mesh, corner, weights, instanceWorldToObject(hit.instanceIndex), direction);
                if (!entering && boundaryDensity > 0.0 && !inMedium(media, hit.instanceIndex)) {
                    // Out of fog it didn't know it was in: the stretch before was inside it too (see "Fog"). The others
                    // let the ray through up to here, so only this one's steps decide.
                    media = enterMedium(media, hit.instanceIndex);
                    if (fog) {
                        uint caught;
                        float catchUp = deltaTrack(enterMedium(OUTSIDE, hit.instanceIndex), origin, direction,
                                                   tMin - log(1.0 - (fogU >= 0.0 ? fogU : randomFloat(seed))) / boundaryDensity,
                                                   hit.t, boundaryDensity, seed, caught);
                        fogU = -1.0;
                        addLampSpan(hit, tMin, catchUp, media, majorant + boundaryDensity, seed);
                        if (caught != NO_MEDIUM) {
                            hit.t = catchUp;
                            hit.inFog = true;
                            hit.fogMedium = material;
                            return false;
                        }
                    }
                } else {
                    addLampSpan(hit, tMin, hit.t, media, majorant, seed);
                }
                if (invisible) {
                    // On through it, along the same ray
                    if (passedT < 0.0 || hit.t > passedT + max(1e-4, passedT * 1e-5)) {
                        passedT = hit.t;
                        mediaBefore = media;
                    }
                    if (boundaryDensity > 0.0) {
                        media = entering ? enterMedium(media, hit.instanceIndex) : leaveMedium(media, hit.instanceIndex);
                    }
                    tMin = pastSurface(tMin, hit.t);
                    passed = uvec4(hit.instanceIndex, uint(hit.primitive), passed.xy);
                    volumes++;
                    continue;
                }
            } else {
                addLampSpan(hit, tMin, hit.t, media, majorant, seed);
            }
            if (passedT >= 0.0 && hit.t <= passedT + max(1e-4, passedT * 1e-5)) {
                media = mediaBefore;
            }
            return true;
        }
        if (portal < 0) {
            return false;
        }
        if (hit.crossings >= scene.maxPortalCrossings) {
            break;  // lost (see "Portals")
        }
        hit.travelled += portalT;
        hit.crossings++;
        mat4x3 toPartner = portals[portal].toPartner;
        origin = toPartner * vec4(origin + direction * portalT, 1.0);
        direction = normalize(mat3(toPartner) * direction);
        skip = portal ^ 1;
        tMin = PORTAL_GAP;
        passedT = -1.0;
        hit.lampWeight = 0.0;  // the lamps' light: only in the last part (see fogLampLight)
        if (FOG != 0 && fog) {
            // The partner may be in other fog (or none): the fog the ray goes on in is the fog there. (Rays that only
            // look pass fog unchanged and skip it.)
            media = mediumAt(origin);
        }
    }
    hit.travelled = -1.0;
    return false;
}

// The same for rays that only look (no fog of their own)
bool intersectClosest(inout vec3 origin, inout vec3 direction, inout uint seed, out Intersection hit) {
    uvec2 media = OUTSIDE;
    return intersectClosest(origin, direction, seed, media, -1.0, hit);
}

// The surface at an intersection of a ray along direction: position, normals, material.
// coneWidth, coneSpread: the ray's cone at its origin (see uvFootprint); 0, 0 reads every texture at full size.
Hit hitFromIntersection(Intersection intersection, vec3 direction, float coneWidth, float coneSpread) {
    Hit hit;
    hit.found = true;
    hit.meshIndex = intersection.meshIndex;
    hit.instanceIndex = intersection.instanceIndex;
    MeshInfo mesh = meshInfos[hit.meshIndex];
    int primitive = intersection.primitive;
    uint firstIndex = mesh.indexOffset + uint(primitive) * 3;
    int firstLight = instanceFirstLight[hit.instanceIndex];
    hit.lightIndex = firstLight >= 0 ? firstLight + primitive : -1;
    // The corners' attributes are read where they are needed, not all at once: normals and tangents read before
    // the material's texture reads would sit in registers meanwhile.
    uvec3 corner = mesh.vertexOffset + uvec3(indices[firstIndex + 0], indices[firstIndex + 1], indices[firstIndex + 2]);

    vec2 bary = intersection.barycentrics;
    vec3 weights = vec3(1.0 - bary.x - bary.y, bary.x, bary.y);

    hit.t = intersection.travelled + intersection.t;  // through portals: the whole way
    vec2 uv0 = vertices[corner.x].uv;
    vec2 uv1 = vertices[corner.y].uv;
    vec2 uv2 = vertices[corner.z].uv;
    hit.uv = uv0 * weights.x + uv1 * weights.y + uv2 * weights.z;
    hit.materialIndex = mesh.materialIndex;

    // Normals are transformed with the inverse transpose of the object's matrix;
    // n * M == transpose(M) * n, and M (world -> object) is already the inverse.
    // The matrices are read from the instance buffer (and inverted) where they are used: kept in registers from the
    // start, they would take 18 of them across the texture reads below.

    // The position from the triangle: as exact as floats allow (offsetRay counts on it), unlike origin + t *
    // direction. Flat normal from the triangle's edges.
    vec3 p0 = vertices[corner.x].position;
    vec3 edge1 = vertices[corner.y].position - p0;
    vec3 edge2 = vertices[corner.z].position - p0;
    hit.position = instances[hit.instanceIndex].objectToWorld * vec4(p0 + edge1 * bary.x + edge2 * bary.y, 1.0);
    hit.geometricNormal = normalize(cross(edge1, edge2) * instanceWorldToObject(hit.instanceIndex));

    float footprintWidth = scene.textureMipmaps != 0 ? coneWidth + coneSpread * hit.t : 0.0;
    mat3 objectToWorld = mat3(instances[hit.instanceIndex].objectToWorld);
    uvFootprint(objectToWorld * edge1, objectToWorld * edge2, uv1 - uv0, uv2 - uv0, direction, footprintWidth,
                hit.uvFootprintX, hit.uvFootprintY);
    evaluateMaterial(hit);

    // Smooth normal: the per-vertex normals from the file, blended across the triangle. Makes a coarse
    // mesh look round. Without them, fall back to the flat one.
    hit.tangent = vec3(0.0);
    if (mesh.hasNormals != 0) {
        vec3 smoothNormal = vertexNormal(corner.x) * weights.x + vertexNormal(corner.y) * weights.y
                            + vertexNormal(corner.z) * weights.z;
        hit.normal = normalize(smoothNormal * instanceWorldToObject(hit.instanceIndex));

        // Normal map: a texture of small surface directions (bumps, scratches, seams) that don't exist in the
        // geometry. They are stored relative to the surface: X along the tangent, Y along the bitangent, Z along
        // the normal. Tangents are directions ON the surface, so they transform like positions, not like normals.
        Material material = materials[mesh.materialIndex];
        if (mesh.hasTangents != 0) {
            vec4 tangent = vertexTangent(corner.x) * weights.x + vertexTangent(corner.y) * weights.y
                           + vertexTangent(corner.z) * weights.z;
            mat3 objectToWorld = mat3(instances[hit.instanceIndex].objectToWorld);
            vec3 t = objectToWorld * tangent.xyz;
            t -= hit.normal * dot(hit.normal, t);  // interpolation bends it slightly off the surface
            // Broken tangents in the file (along the normal, or corners pointing opposite ways so the blend
            // cancels out) leave nothing to normalize: NaN, which would ruin the pixel for good. Any direction
            // on the surface will do there; the normal map is then just rotated on that tiny spot.
            if (dot(t, t) < 1e-12) {
                t = cross(abs(hit.normal.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0), hit.normal);
            }
            t = normalize(t);
            hit.tangent = t;

            if (material.normalTexture >= 0) {
                // A mirrored instance (negative scale) flips the bitangent as well
                float sign = (tangent.w < 0.0 ? -1.0 : 1.0) * (determinant(objectToWorld) < 0.0 ? -1.0 : 1.0);
                vec3 b = cross(hit.normal, t) * sign;
                vec3 mapped = sampleTexture(material.normalTexture, material.normalUvTransform, hit).rgb * 2.0 - 1.0;  // [0,1] -> [-1,1]
                if (material.normalTwoChannel != 0) {
                    mapped.z = sqrt(max(1.0 - dot(mapped.xy, mapped.xy), 0.0));
                }
                mapped.xy *= material.normalScale;
                // A rotated (or mirrored) texture turns its bumps with it: the map's X/Y follow the texture's axes,
                // the tangent frame follows the mesh UVs. X/Y are a slope (the normal leans away from where the
                // surface rises), and a slope goes from texture to mesh UVs through the transposed matrix. In UV
                // terms the map's Y is -V. The length is kept, so tiling a texture doesn't make its bumps steeper.
                mat2 toTexture = mat2(material.normalUvTransform[0], material.normalUvTransform[1]);
                vec2 slope = transpose(toTexture) * vec2(mapped.x, -mapped.y);
                float slopeLength = length(slope);
                if (slopeLength > 1e-8) {
                    mapped.xy = vec2(slope.x, -slope.y) * (length(mapped.xy) / slopeLength);
                }
                hit.normal = normalize(t * mapped.x + b * mapped.y + hit.normal * mapped.z);

                // Toksvig: the bumps this level averages away widen the reflection by the alpha stored for it
                // (see makeNormalVarianceTexture), added to the material's alpha^2 (as two independent spreads).
                // That texture is half the size: read with the same footprint it gives the right level, except
                // between the normal map's levels 0 and 1, where it fades out (the full size averages nothing).
                if (scene.normalMapFiltering != 0 && material.normalVarianceTexture >= 0 && hit.uvFootprintX != vec2(0.0)) {
                    vec2 texels = vec2(textureSize(textures[nonuniformEXT(material.normalTexture)], 0));
                    // The level the GPU reads: from the footprint's short axis (filtered along the long one)
                    float level = log2(min(length(toTexture * hit.uvFootprintX * texels),
                                           length(toTexture * hit.uvFootprintY * texels)));
                    float bumps = sampleTexture(material.normalVarianceTexture, material.normalUvTransform, hit).r
                                  * clamp(level, 0.0, 1.0) * material.normalScale;
                    float alpha = hit.roughness * hit.roughness;
                    hit.roughness = sqrt(sqrt(alpha * alpha + bumps * bumps));
                }
            }
        }
        // The vertex normals define which side is "outside"; the winding order may disagree.
        if (dot(hit.geometricNormal, hit.normal) < 0.0) {
            hit.geometricNormal = -hit.geometricNormal;
        }
    } else {
        hit.normal = hit.geometricNormal;
    }

    // The normals still point outwards here: tells whether the ray enters or leaves a solid.
    hit.frontFace = dot(hit.geometricNormal, direction) < 0.0;

    // Triangles are double-sided: if we hit the back, flip both normals towards the ray.
    if (!hit.frontFace) {
        hit.geometricNormal = -hit.geometricNormal;
        hit.normal = -hit.normal;
    }
    return hit;
}

// The reflection formulas need the viewer above the shading normal. A smooth or normal-mapped normal can tilt
// away from the viewer near silhouettes; tilt it back just far enough.
void tiltTowardsViewer(inout Hit hit, vec3 direction) {
    float cosViewer = dot(hit.normal, -direction);
    if (cosViewer < 1e-3) {
        hit.normal = normalize(hit.normal - direction * (1e-3 - cosViewer));
    }
}

// A path's scatter point in fog (see "Fog") as a Hit, so the path's loop lights it and bounces from it like a
// surface: marked by its material (isFog), the fog's material in meshIndex, facing back along the ray, light from
// every side (two-sided, as diffuse transmission), no ReSTIR (restirSurface), cache cells of their own (cacheCell).
// Its color is in the throughput.
const uint FOG_MATERIAL = 0xFFFFFFFFu;

Hit fogHit(vec3 position, vec3 direction, float t, uint medium) {
    Hit hit;
    hit.found = true;
    hit.t = t;
    hit.position = position;
    hit.normal = -direction;
    hit.geometricNormal = -direction;
    hit.tangent = vec3(0.0);
    hit.uv = vec2(0.0);
    hit.uvFootprintX = vec2(0.0);
    hit.uvFootprintY = vec2(0.0);
    hit.meshIndex = medium;
    hit.instanceIndex = 0;
    hit.materialIndex = FOG_MATERIAL;
    hit.lightIndex = -1;
    hit.frontFace = true;
    hit.albedo = materials[medium].fogColor;
    hit.alpha = 1.0;
    hit.metallic = 0.0;
    hit.roughness = 1.0;
    hit.transmission = 0.0;
    hit.ior = 1.0;
    hit.solid = false;
    hit.diffuseTransmission = 1.0;
    hit.diffuseTransmissionColor = vec3(1.0);
    hit.emissive = vec3(0.0);
    hit.lightThrough = false;
    return hit;
}

bool isFog(Hit hit) {
    return FOG != 0 && hit.materialIndex == FOG_MATERIAL;
}

Hit traceClosest(vec3 origin, vec3 direction, inout uint seed, float coneWidth, float coneSpread) {
    Intersection intersection;
    if (!intersectClosest(origin, direction, seed, intersection)) {
        Hit hit;
        hit.found = false;
        return hit;
    }
    return hitFromIntersection(intersection, direction, coneWidth, coneSpread);
}

// Where a direction lands on the environment map, and back. u goes around the horizon (the same way as in
// Blender, so a map looks the same there), v from straight up (0) to straight down (1).
vec2 environmentUv(vec3 direction) {
    float angle = atan(-direction.z, -direction.x) + scene.environmentRotation;
    return vec2(fract(angle / (2.0 * PI) + 0.5), acos(clamp(direction.y, -1.0, 1.0)) / PI);
}

vec3 environmentDirection(float u, float cosTheta) {
    float angle = 2.0 * PI * (u - 0.5) - scene.environmentRotation;
    float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));
    return vec3(-sinTheta * cos(angle), cosTheta, -sinTheta * sin(angle));
}

// Light coming from the environment in a direction: the HDR map, or a sky gradient above the horizon and
// ground below. The sun is NOT part of it, it is handled separately with shadow rays (see tracePath).
vec3 environmentLight(vec3 direction) {
    if (scene.environmentMap != 0) {
        return textureLod(environmentMap, environmentUv(direction), 0.0).rgb * scene.environmentIntensity;
    }
    float skyT = pow(smoothstep(0.0, 0.4, direction.y), 0.35);
    vec3 sky = mix(scene.skyHorizonColor.rgb, scene.skyZenithColor.rgb, skyT);
    float groundToSky = smoothstep(-0.01, 0.0, direction.y);
    return mix(scene.groundColor.rgb, sky, groundToSky);
}
