// raytrace.comp, part: The light tree.
// Included by raytrace.comp in order: uses what the parts before it define.

// ---------------------------------------------------------------------------------------------
// The light tree. Picking by power alone sends most shadow rays of a point under a street lamp to lamps a block
// away. The tree groups nearby triangles; walking down from the root, each step goes into one of the two children
// with a chance proportional to a rough guess of how much light it could send to the point (lightTreeImportance).
// The chance of picking a triangle is the product of the chances along its way down. The guess only steers where
// the samples go: every triangle that could light the point keeps a chance above 0, so the result stays correct.
//
// p, n: the shading point and its (shading) normal. twoSided: light from behind counts too (diffuse transmission).
// These must be the same when sampling and when computing the pdf of a triangle a bounce ray hit (for MIS).

// Power / distance^2, times the largest cosine any point of the node's bounding sphere could have with the normal.
// The distance: the distance to the box's center underrates a long box (a string of lamps along the street) for a
// point near one of its ends, where the light from the nearest lamp is strong: those rare samples come out very
// bright (fireflies). Instead, distance^2 = (to the box's nearest point) * (to its farthest corner). Inside the box (or very
// close) the nearest distance is clamped to a twentieth of its radius, the light can't grow without limit there.
float lightTreeImportance(LightTreeNode node, vec3 p, vec3 n, bool twoSided) {
    vec3 center = 0.5 * (node.boundsMin + node.boundsMax);
    float radius2 = 0.25 * dot(node.boundsMax - node.boundsMin, node.boundsMax - node.boundsMin);
    vec3 toCenter = center - p;
    float distance2 = dot(toCenter, toCenter);
    vec3 nearest = clamp(p, node.boundsMin, node.boundsMax);
    vec3 farthest = mix(node.boundsMax, node.boundsMin, step(center, p));  // per axis, the side away from p
    float nearestDistance = max(length(nearest - p), 0.05 * sqrt(radius2) + 1e-4);
    float importance = node.power / (nearestDistance * length(farthest - p));
    if (twoSided || distance2 <= radius2) {
        return importance;  // every direction may bring light / the sphere surrounds the point
    }
    // The sphere is seen within an angle thetaU of its center's direction (at thetaI from the normal); the
    // closest direction to the normal is at max(0, thetaI - thetaU). Nothing below the horizon reaches the point.
    float cosI = dot(n, toCenter) * inversesqrt(distance2);
    float sinU2 = radius2 / distance2;
    float cosU = sqrt(1.0 - sinU2);
    if (cosI >= cosU) {
        return importance;  // the normal points into the sphere
    }
    float sinI = sqrt(max(1.0 - cosI * cosI, 0.0));
    float cosBound = cosI * cosU + sinI * sqrt(sinU2);  // cos(thetaI - thetaU)
    return importance * max(cosBound, 0.0);
}

// Chance of going into the left child of `node` (a node with children), -1 if neither child can light the point
float lightTreeLeftChance(LightTreeNode node, vec3 p, vec3 n, bool twoSided) {
    float left = lightTreeImportance(lightTreeNodes[node.child], p, n, twoSided);
    float right = lightTreeImportance(lightTreeNodes[node.child + 1], p, n, twoSided);
    return left + right > 0.0 ? left / (left + right) : -1.0;
}

// Walks down the tree with one random number (rescaled at every step to stay evenly distributed).
// Returns the triangle, or -1 when nothing can light the point.
int pickFromLightTree(vec3 p, vec3 n, bool twoSided, float u, out float probability) {
    probability = 1.0;
    LightTreeNode node = lightTreeNodes[0];
    while (node.child >= 0) {
        float leftChance = lightTreeLeftChance(node, p, n, twoSided);
        if (leftChance < 0.0) {
            return -1;
        }
        if (u < leftChance) {
            u /= leftChance;
            probability *= leftChance;
            node = lightTreeNodes[node.child];
        } else {
            u = (u - leftChance) / (1.0 - leftChance);
            probability *= 1.0 - leftChance;
            node = lightTreeNodes[node.child + 1];
        }
        u = min(u, 0.99999994);  // rounding could reach 1
    }
    return -node.child - 1;
}

// The chance that light sampling at p picks this triangle: its share of the light, or the way down the tree
float lightPickProbability(int lightIndex, vec3 p, vec3 n, bool twoSided) {
    EmissiveTriangle light = emissiveTriangles[lightIndex];
    if (scene.lightTree == 0 || light.probability <= 0.0) {
        return light.probability;  // probability 0: not in the tree, never picked
    }
    float probability = 1.0;
    LightTreeNode node = lightTreeNodes[0];
    for (uint depth = 0; node.child >= 0; depth++) {
        float leftChance = lightTreeLeftChance(node, p, n, twoSided);
        if (leftChance < 0.0) {
            return 0.0;
        }
        bool right = (light.treeTrail & (1u << depth)) != 0u;
        probability *= right ? 1.0 - leftChance : leftChance;
        node = lightTreeNodes[node.child + (right ? 1 : 0)];
    }
    return -node.child - 1 == lightIndex ? probability : 0.0;
}

// Light sampling for the sky (see Environment.cpp): the sphere of directions in cells, rows from straight up to
// straight down, each row cut into columns. A cell is picked with a chance proportional to its brightness times
// its solid angle, then a direction evenly inside it.

// The first entry of environmentCdf[first .. first + count) that reaches u
uint searchEnvironmentCdf(uint first, uint count, float u) {
    uint low = 0, high = count - 1;
    while (low < high) {
        uint middle = (low + high) / 2;
        if (environmentCdf[first + middle] < u) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low;
}

// pdf (per solid angle) of a direction in this cell
float environmentCellPdf(uint row, uint column) {
    uint width = scene.environmentWidth, height = scene.environmentHeight;
    uint rowStart = height + row * width;
    float rowChance = environmentCdf[row] - (row > 0 ? environmentCdf[row - 1] : 0.0);
    float columnChance = environmentCdf[rowStart + column] - (column > 0 ? environmentCdf[rowStart + column - 1] : 0.0);
    float cos0 = cos(PI * float(row) / float(height));
    float cos1 = cos(PI * float(row + 1) / float(height));
    float solidAngle = 2.0 * PI / float(width) * (cos0 - cos1);
    return rowChance * columnChance / max(solidAngle, 1e-12);
}

// How likely sampleEnvironment is to pick this direction (for MIS)
float environmentPdf(vec3 direction) {
    vec2 uv = environmentUv(direction);
    uint row = min(uint(uv.y * float(scene.environmentHeight)), scene.environmentHeight - 1);
    uint column = min(uint(uv.x * float(scene.environmentWidth)), scene.environmentWidth - 1);
    return environmentCellPdf(row, column);
}

vec3 sampleEnvironment(vec4 random, out float pdf) {
    uint height = scene.environmentHeight, width = scene.environmentWidth;
    uint row = searchEnvironmentCdf(0, height, random.x);
    uint column = searchEnvironmentCdf(height + row * width, width, random.y);
    // Evenly over the cell's solid angle: evenly in cos(angle from straight up) and around
    float cos0 = cos(PI * float(row) / float(height));
    float cos1 = cos(PI * float(row + 1) / float(height));
    float cosTheta = mix(cos0, cos1, random.z);
    float u = (float(column) + random.w) / float(width);
    pdf = environmentCellPdf(row, column);  // 0 for a black cell hit by a random number exactly on its boundary
    return environmentDirection(u, cosTheta);
}

// Light given off at a point of a glowing triangle. A hole in a cut-out doesn't glow, and a see-through
// surface glows in proportion to how often rays stop on it (a bounce ray that hits it sees it with chance alpha).
vec3 emissionAt(uint materialIndex, vec2 uv) {
    Material material = materials[materialIndex];
    vec3 emission = material.emissive;
    if (material.emissiveTexture >= 0) {
        emission *= sampleTextureFull(material.emissiveTexture, material.emissiveUvTransform, uv).rgb;
    }
    return emission * opacityAt(materialIndex, uv);
}
