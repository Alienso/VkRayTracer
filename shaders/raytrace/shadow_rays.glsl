// raytrace.comp, part: Shadow rays.
// Included by raytrace.comp in order: uses what the parts before it define.

// ---------------------------------------------------------------------------------------------
// Shadow rays

// How much light gets from origin to origin + direction * maxDistance: 0 if something opaque is in the way,
// otherwise the tint of the glass on the way. Glass is passed in a straight line, without bending: following
// the bent path to a light is what caustics would need, which we skip on purpose. crossedGlass tells the caller
// that this light can't also be found by a bounce ray (those are bent by the glass), so no MIS for it.
// crossedSolidGlass: some of it was solid. Thin panes don't bend rays, so for the sky the bounce rays through
// them do find the same light, and MIS stays on (see tracePath).
//
// Candidates on glass come in no particular order. Absorption inside a solid depends on the length inside,
// which is (sum of the distances where the ray leaves) - (sum where it enters) for any number of crossings,
// so it can be summed in any order.
// A portal's opening on the way blocks the light, except with throughEnd >= 0: a light image (portalLightImage),
// whose ray must go through that end's opening first and then on from its partner, maxDistance being the whole way.
// Fog (see "Fog"): invisible surfaces let the light through; its length inside even fog counts like the absorption,
// plus the fog it starts in (media) and ends in. Not knowing it started in fog, it learns it where it leaves it. Varying
// fog: varyingTransmittance, after the surfaces have let the light through.
vec3 shadowTransmittance(vec3 origin, vec3 direction, float maxDistance, int throughEnd, inout uint seed, uvec2 media,
                         out bool crossedGlass, out bool crossedSolidGlass) {
    crossedGlass = false;
    crossedSolidGlass = false;
    vec3 tint = vec3(1.0);
    int skip = -1;
    float tMin = 0.0;
    // The even fog's density where the part starts (and, after the loop, where it ends)
    float fogInside = 0.0;
    for (uvec2 rest = media; FOG != 0 && firstMedium(rest) != NO_MEDIUM; rest = otherMedia(rest)) {
        uint material = mediumMaterial(firstMedium(rest));
        fogInside += fogVaries(material) ? 0.0 : materials[material].fogDensity;
    }
    // (One part without portals. Cache update paths too have light images: once they made them take 168 registers
    // instead of 128, no more since; the cache pass is ~0.1 ms slower for them in Sponza, which has no portals)
    [[dont_unroll]] for (uint part = 0; part < (!FEATURE_PORTALS ? 1u : 2u); part++) {
        // Up to the opening it must go through (part 0 of a light image), or to the end; any other opening blocks
        float portalT = maxDistance;
        int portal = portalCount() > 0 ? nearestPortal(origin, direction, maxDistance, skip, portalT) : -1;
        if (portal != (part == 0 ? throughEnd : -1)) {
            return vec3(0.0);
        }
        rayQueryEXT rayQuery;
        rayQueryInitializeEXT(rayQuery, tlas, gl_RayFlagsTerminateOnFirstHitEXT | SHADOW_RAY_FLAGS, 0xFF, origin, tMin,
                              direction, portal >= 0 ? max(portalT - PORTAL_GAP, tMin) : max(maxDistance, tMin));
        vec3 opticalDepth = vec3(0.0);
        float fogDepth = 0.0;  // even fog: density * (leaving distance - entering distance) over its surfaces
        uvec2 crossed = OUTSIDE;  // varying fog volumes whose surfaces it crosses
        while (rayQueryProceedEXT(rayQuery)) {
            if (SHADOW_RAY_FLAGS != gl_RayFlagsNoneEXT ||
                rayQueryGetIntersectionTypeEXT(rayQuery, false) != gl_RayQueryCandidateIntersectionTriangleEXT ||
                cutAway(rayQuery)) {
                continue;
            }
            // Only what the next test needs is read at each step (the UVs first, the triangle's shape only for glass and
            // even fog): whatever is read early stays in registers through the tests, on top of the whole path's state.
            MeshInfo mesh = meshInfos[rayQueryGetIntersectionInstanceCustomIndexEXT(rayQuery, false)];
            uint firstIndex = mesh.indexOffset + uint(rayQueryGetIntersectionPrimitiveIndexEXT(rayQuery, false)) * 3;
            uvec3 corner = mesh.vertexOffset + uvec3(indices[firstIndex + 0], indices[firstIndex + 1], indices[firstIndex + 2]);
            vec2 bary = rayQueryGetIntersectionBarycentricsEXT(rayQuery, false);
            vec3 weights = vec3(1.0 - bary.x - bary.y, bary.x, bary.y);
            vec2 uv = vertices[corner.x].uv * weights.x + vertices[corner.y].uv * weights.y + vertices[corner.z].uv * weights.z;

            // through a hole of a cut-out
            float opacity = opacityAt(mesh.materialIndex, uv);
            if (opacity < 1.0 && (opacity <= 0.0 || randomFloat(seed) >= opacity)) {
                continue;
            }

            Material material = materials[mesh.materialIndex];
            if (FOG != 0 && (material.fogDensity > 0.0 || material.invisible != 0)) {
                if (fogVaries(mesh.materialIndex)) {
                    // Varying fog: only which volume (see varyingTransmittance, after this)
                    crossed = enterMedium(crossed, rayQueryGetIntersectionInstanceIdEXT(rayQuery, false));
                } else if (material.fogDensity > 0.0) {
                    bool entering = entersMesh(mesh, corner, weights,
                                               mat3(rayQueryGetIntersectionWorldToObjectEXT(rayQuery, false)), direction);
                    float t = rayQueryGetIntersectionTEXT(rayQuery, false);
                    fogDepth += material.fogDensity * (entering ? -t : t);
                    fogInside += entering ? material.fogDensity : -material.fogDensity;
                }
                if (material.invisible != 0) {
                    continue;
                }
            }
            float transmission = FEATURE_GLASS ? material.transmission : 0.0;
            if (transmission > 0.0 && material.transmissionTexture >= 0) {
                transmission *= sampleTextureFull(material.transmissionTexture, material.transmissionUvTransform, uv).r;
            }
            float metallic = material.metallic;
            if (transmission > 0.0 && material.metallicRoughnessTexture >= 0) {
                metallic *= sampleTextureFull(material.metallicRoughnessTexture, material.metallicRoughnessUvTransform, uv).b;
            }
            if (transmission <= 0.0 || metallic >= 1.0) {
                rayQueryConfirmIntersectionEXT(rayQuery);  // opaque: in shadow (and with TerminateOnFirstHit, done)
                break;
            }

            // Glass: the share that passes is what neither the reflection nor the opaque part takes,
            // tinted by the base color when entering (or crossing a thin pane).
            crossedGlass = true;
            mat3 worldToObject = mat3(rayQueryGetIntersectionWorldToObjectEXT(rayQuery, false));
            vec3 p0 = vertices[corner.x].position;
            vec3 outward = cross(vertices[corner.y].position - p0, vertices[corner.z].position - p0) * worldToObject;
            if (mesh.hasNormals != 0) {
                vec3 smoothNormal = vertexNormal(corner.x) * weights.x + vertexNormal(corner.y) * weights.y
                                    + vertexNormal(corner.z) * weights.z;
                if (dot(outward, smoothNormal * worldToObject) < 0.0) {
                    outward = -outward;  // as in hitFromIntersection: the vertex normals define the outside
                }
            }
            float cosTheta = dot(normalize(outward), direction);
            bool entering = cosTheta < 0.0;
            vec3 color = material.baseColor.rgb;
            if (material.baseColorTexture >= 0) {
                color *= sampleTextureFull(material.baseColorTexture, material.baseColorUvTransform, uv).rgb;
            }
            float fresnel = fresnelSchlick(dielectricF0(material.ior), abs(cosTheta));
            tint *= (1.0 - metallic) * transmission * (1.0 - fresnel);
            if (entering || material.solid == 0) {
                tint *= color;
            }
            if (material.solid != 0) {
                crossedSolidGlass = true;
                float distance = rayQueryGetIntersectionTEXT(rayQuery, false) * length(worldToObject * direction) * mesh.unitScale;
                opticalDepth += material.absorption * (entering ? -distance : distance);
            }
            if (all(lessThan(tint, vec3(1e-4)))) {
                rayQueryConfirmIntersectionEXT(rayQuery);  // practically black: no need to look further
                break;
            }
        }
        if (rayQueryGetIntersectionTypeEXT(rayQuery, true) != gl_RayQueryCommittedIntersectionNoneEXT) {
            return vec3(0.0);
        }
        tint *= exp(-max(opticalDepth, vec3(0.0)));
        // The fog it is in at the end goes on to there (inside < 0: it left fog it didn't know it was in)
        fogInside = max(fogInside, 0.0);
        tint *= exp(-max(fogDepth + fogInside * (portal >= 0 ? portalT : maxDistance), 0.0));
        if (FOG >= 2 && any(greaterThan(tint, vec3(0.0)))) {
            // (after a portal: from the media it started in, as if the partner's side were the same place)
            tint *= varyingTransmittance(origin, direction, portal >= 0 ? portalT : maxDistance, media, crossed, seed);
        }
        if (portal < 0) {
            break;
        }
        // On from the partner, for the rest of the way
        mat4x3 toPartner = portals[portal].toPartner;
        origin = toPartner * vec4(origin + direction * portalT, 1.0);
        direction = normalize(mat3(toPartner) * direction);
        maxDistance -= portalT;
        skip = portal ^ 1;
        tMin = PORTAL_GAP;
    }
    return tint;
}
