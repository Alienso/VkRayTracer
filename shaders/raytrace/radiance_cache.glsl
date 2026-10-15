// raytrace.comp, part: Radiance cache.
// Included by raytrace.comp in order: uses what the parts before it define.

// ---------------------------------------------------------------------------------------------
// Radiance cache
//
// In a scene lit mostly indirectly (Sponza's shade: sunlight bouncing off the courtyard floor and walls), a path
// needs several random bounces to find the light, and most don't: noise. The cache remembers, for small cells on
// the surfaces, how much indirect light leaves them. A path whose bounce spread out (diffuse or rough) lights the
// surface it lands on directly (shadow rays to the sun and the lamps, as usual), adds the cell's indirect light
// and stops there: one bounce ray instead of many, and an average of many paths instead of one.
// The cells are filled by a few full-length "update paths" per frame (one per cacheUpdateTile^2 pixels): every
// surface they hit gets the light they found from there on, except its own direct light. Direct light has sharp
// shadow edges, and a cell lying across one would spread sunlight into the shade (a path from a dark corner
// mostly lands on the shaded part of such a cell); indirect light is smooth enough for cells.
// Averaged over recent frames, so the cache follows changes (moving objects, the sun) with a short lag. Biased:
// a cell averages its whole area and all directions, which blurs indirect light a little.
//
// Cells: about scene.cacheCellScale * distance wide (a few pixels on screen wherever they are), rounded to a power
// of two so that nearby points agree, and separate per side a surface faces (floor and wall meeting at a corner
// don't mix). Hash table with up to CACHE_PROBES slots tried per cell (linear probing).
//
// Fog (scene.cacheFog): scatter points have cells too, cubes facing no side. In thick fog a path scatters many times,
// and past the first bounce each scatter point costs about as much as a surface (its bounce ray and shadow rays
// through the scene), however few steps the tracking takes (LIMITATIONS.md, "Fog"). Image paths stop at their second
// scatter point (the first one after a bounce that spread out, as at surfaces): its direct light by light sampling,
// the rest from the cell. A cell holds the light leaving it without the fog's color (that is in the throughput up to
// it, see fogHit), averaged over all directions: biased where the fog scatters mostly forward, which matters little
// for light that has already scattered more than once.

const uint CACHE_PROBES = 8;
// Update paths fill the cells of their first 4 surfaces (and scatter points). 8 cost registers: the update pass got
// 168 instead of 128, 2x slower in thick fog (21 ms instead of 10), and Sponza converged no faster.
const uint CACHE_MAX_VERTICES = 4;
const float MIRROR_ROUGHNESS = 0.01;    // smoother reflections count as mirrors: no Russian roulette after them
const float CACHE_ROUGHNESS = 0.25;     // smoother surfaces (and glass) reflect too sharply to be cached
// Smoother glass doesn't light-sample what is behind it (see glassLit): its lobe is so narrow that bounce rays find the
// lights about as well, and every shadow ray would be wasted
const float GLASS_LIGHT_ROUGHNESS = 0.1;
const float CACHE_MAX_RADIANCE = 256.0; // one sample's light is clamped to this (rare glints of the sun)
// A cell averages its whole area, so in a corner it holds more light than the corner itself gets (the walls
// further out are brighter). A path that reached the cell with a short ray, i.e. from right next to it (inside
// a corner, under an object), would show that average as light leaking in; only rays longer than this many
// cell sizes end in the cache, the others trace on.
const float CACHE_MIN_RAY_CELLS = 2.0;

bool cacheableSurface(Hit hit) {
    if (isFog(hit)) {
        return scene.cacheFog != 0;
    }
    return hit.transmission <= 0.0 && hit.roughness >= CACHE_ROUGHNESS;
}

// The cell of a surface point (or a scatter point in fog): where the table search starts (slot) and the cell's id
// (fingerprint, never 0). Two different hashes of the same cell coordinates, so two cells sharing a slot are told apart.
void cacheCell(Hit hit, out uint slot, out uint fingerprint, out float cellSize) {
    float distance = max(length(hit.position - scene.cameraPosition.xyz), 1e-4);
    int level = clamp(int(floor(log2(distance * scene.cacheCellScale))), -14, 10);
    cellSize = exp2(float(level));
    ivec3 cell = ivec3(floor(hit.position / cellSize));
    vec3 a = abs(hit.geometricNormal);
    uint axis = a.x >= a.y && a.x >= a.z ? 0u : (a.y >= a.z ? 1u : 2u);
    // which of the 6 main directions it faces; fog faces none (6): its light comes from all sides
    uint facing = isFog(hit) ? 6u : axis * 2u + (hit.geometricNormal[axis] < 0.0 ? 1u : 0u);
    uint extra = uint(level + 16) | (facing << 8);
    slot = pcgHash(uint(cell.x) + pcgHash(uint(cell.y) + pcgHash(uint(cell.z) + pcgHash(extra))));
    fingerprint = pcgHash((uint(cell.x) * 0x27d4eb2du) ^ pcgHash((uint(cell.y) * 0x165667b1u)
                  ^ pcgHash((uint(cell.z) * 0x85ebca6bu) ^ (extra * 0x9e3779b9u)))) | 1u;
}

int cacheFind(uint slot, uint fingerprint) {
    uint mask = scene.cacheCapacity - 1u;
    for (uint i = 0; i < CACHE_PROBES; i++) {
        uint index = (slot + i) & mask;
        if (cacheKeys[index] == fingerprint) {
            return int(index);
        }
    }
    return -1;
}

// Finds the cell's entry or claims the first empty slot for it. -1: all slots taken by other cells.
int cacheFindOrInsert(uint slot, uint fingerprint) {
    int found = cacheFind(slot, fingerprint);
    if (found >= 0) {
        return found;
    }
    uint mask = scene.cacheCapacity - 1u;
    for (uint i = 0; i < CACHE_PROBES; i++) {
        uint index = (slot + i) & mask;
        uint previous = atomicCompSwap(cacheKeys[index], 0u, fingerprint);
        if (previous == 0u || previous == fingerprint) {  // claimed it, or another path just did for the same cell
            return int(index);
        }
    }
    return -1;
}
