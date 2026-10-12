//
// The Cornell box: the classic test scene for global illumination (Cornell University, 1984).
// A room lit only by a panel in the ceiling, with a red left wall and a green right wall. Since the
// walls are the only colored things, any red or green on the white surfaces must have arrived by
// light bouncing off them (color bleeding), which makes indirect lighting easy to see and to check.
//
// Ours is 4 x 4 x 4 units, open at the front (+Z) like the original. The front wall is a part of the
// model that starts hidden; show it under "Parts" to close the box and fly inside.
// The point light variant replaces the ceiling panel by a point light (KHR_lights_punctual's kind) just below
// it, as bright straight down as the panel: the same room with sharp shadows and a lit ceiling.
//
// The mirror maze: a few hallways where every surface is a clean mirror (walls, floor and ceiling), open only at
// the entrance and the exit, lit by many small lamps. Every lamp and every wall is seen again and again in the
// mirrors, so it's hard to tell what is real: a test for reflections (DLSS guides, noise in reflected surfaces).
//
// The light orb: a small glowing ball floating at the height of a fog box's middle, to light fog from inside.
//
// The cloud: a flattened ball of invisible fog, patchy, with soft ragged edges and the scattering of cloud droplets.
//
// The portal rim: two glowing cubes (blue for the A ends, orange for the B ends) that the Scene stretches into the
// bars of the portals' frames.
//

#include "assets/BuiltInModels.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/constants.hpp>

static const char* cornellBoxName = "Cornell box";
static const char* cornellBoxWithBlocksName = "Cornell box (classic, with blocks)";
static const char* cornellBoxPointLightName = "Cornell box (blocks, point light)";
static const char* cubeName = "Cube";
static const char* mirrorMazeName = "Mirror maze";
static const char* fogBoxName = "Fog box";
static const char* lightOrbName = "Light orb";
static const char* cloudName = "Cloud";

const std::vector<std::string>& builtInModelNames() {
    static const std::vector<std::string> names = {cubeName, cornellBoxName, cornellBoxWithBlocksName, cornellBoxPointLightName, mirrorMazeName, fogBoxName, lightOrbName, cloudName};
    return names;
}

const std::string& portalRimModelName() {
    static const std::string name = "Portal rim";
    return name;
}

bool isBuiltInModel(const std::string& name) {
    const auto& names = builtInModelNames();
    return std::find(names.begin(), names.end(), name) != names.end() || name == portalRimModelName();
}

static Material matte(const std::string& name, glm::vec3 color) {
    Material material;
    material.name = name;
    material.baseColor = glm::vec4(color, 1.0f);
    material.metallic = 0.0f;
    material.roughness = 1.0f;
    return material;
}

static void computeBounds(Mesh& mesh) {
    mesh.boundsMin = glm::vec3(std::numeric_limits<float>::max());
    mesh.boundsMax = glm::vec3(std::numeric_limits<float>::lowest());
    for (const SceneVertex& vertex : mesh.vertices) {
        mesh.boundsMin = glm::min(mesh.boundsMin, vertex.position);
        mesh.boundsMax = glm::max(mesh.boundsMax, vertex.position);
    }
}

// A rectangle from 4 corners in order around its edge; the normal follows from the order (right-hand rule).
static void addQuad(Mesh& mesh, glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d) {
    glm::vec3 normal = glm::normalize(glm::cross(b - a, d - a));
    uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
    mesh.vertices.push_back({a, normal, {0.0f, 0.0f}, glm::vec4(0.0f)});
    mesh.vertices.push_back({b, normal, {1.0f, 0.0f}, glm::vec4(0.0f)});
    mesh.vertices.push_back({c, normal, {1.0f, 1.0f}, glm::vec4(0.0f)});
    mesh.vertices.push_back({d, normal, {0.0f, 1.0f}, glm::vec4(0.0f)});
    mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
}

static Mesh quadMesh(const std::string& name, uint32_t material, glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d) {
    Mesh mesh;
    mesh.name = name;
    mesh.materialIndex = material;
    mesh.hasNormals = true;
    addQuad(mesh, a, b, c, d);
    computeBounds(mesh);
    return mesh;
}

// A box standing on the floor: `size` = width, height, depth, turned by `angle` degrees around its vertical axis.
static Mesh blockMesh(const std::string& name, uint32_t material, glm::vec3 center, glm::vec3 size, float angle) {
    glm::mat4 m = glm::translate(glm::mat4(1.0f), center) * glm::rotate(glm::mat4(1.0f), glm::radians(angle), glm::vec3(0.0f, 1.0f, 0.0f));
    auto corner = [&](int x, int y, int z) {
        glm::vec3 local = {(x - 0.5f) * size.x, y * size.y, (z - 0.5f) * size.z};
        return glm::vec3(m * glm::vec4(local, 1.0f));
    };
    Mesh mesh;
    mesh.name = name;
    mesh.materialIndex = material;
    mesh.hasNormals = true;  // per face (4 vertices each), so the edges stay sharp
    addQuad(mesh, corner(0, 0, 1), corner(1, 0, 1), corner(1, 1, 1), corner(0, 1, 1));  // front (+z)
    addQuad(mesh, corner(1, 0, 0), corner(0, 0, 0), corner(0, 1, 0), corner(1, 1, 0));  // back
    addQuad(mesh, corner(0, 0, 0), corner(0, 0, 1), corner(0, 1, 1), corner(0, 1, 0));  // left (-x)
    addQuad(mesh, corner(1, 0, 1), corner(1, 0, 0), corner(1, 1, 0), corner(1, 1, 1));  // right
    addQuad(mesh, corner(0, 1, 1), corner(1, 1, 1), corner(1, 1, 0), corner(0, 1, 0));  // top
    computeBounds(mesh);
    return mesh;
}

// A cube from -0.5 to 0.5, all six faces
static Mesh cubeMesh(const std::string& name, uint32_t material) {
    auto corner = [](int x, int y, int z) { return glm::vec3(x - 0.5f, y - 0.5f, z - 0.5f); };
    Mesh mesh;
    mesh.name = name;
    mesh.materialIndex = material;
    mesh.hasNormals = true;
    addQuad(mesh, corner(0, 0, 1), corner(1, 0, 1), corner(1, 1, 1), corner(0, 1, 1));  // front (+z)
    addQuad(mesh, corner(1, 0, 0), corner(0, 0, 0), corner(0, 1, 0), corner(1, 1, 0));  // back
    addQuad(mesh, corner(0, 0, 0), corner(0, 0, 1), corner(0, 1, 1), corner(0, 1, 0));  // left (-x)
    addQuad(mesh, corner(1, 0, 1), corner(1, 0, 0), corner(1, 1, 0), corner(1, 1, 1));  // right
    addQuad(mesh, corner(0, 1, 1), corner(1, 1, 1), corner(1, 1, 0), corner(0, 1, 0));  // top
    addQuad(mesh, corner(0, 0, 0), corner(1, 0, 0), corner(1, 0, 1), corner(0, 0, 1));  // bottom
    computeBounds(mesh);
    return mesh;
}

static Model createCube() {
    Model model;
    model.name = cubeName;

    const glm::vec3 color = {1.0f, 1.0f, 1.0f};
    Material material = matte("white", color);
    material.roughness = 1.0f;
    material.emissiveColor = color;
    material.emissiveStrength = 0.0f;

    model.materials.push_back(material);
    model.meshes.push_back(cubeMesh("mesh", 0));
    model.instances.push_back({0, glm::mat4(1.0f)});

    model.boundsMin = glm::vec3(-0.5f);
    model.boundsMax = glm::vec3(0.5f);

    return model;
}

// A box of fog, 4 x 3 x 4 standing on the ground: an invisible surface around fog (see Material::fogDensity). Scale
// and turn it like any object; its material is shared by all fog boxes.
static Model createFogBox() {
    Model model;
    model.name = fogBoxName;
    Material material = matte("fog", glm::vec3(1.0f));
    material.fogDensity = 0.3f;
    material.invisibleSurface = true;
    model.materials.push_back(material);
    Mesh mesh = cubeMesh("mesh", 0);
    for (SceneVertex& vertex : mesh.vertices) {
        vertex.position = vertex.position * glm::vec3(4.0f, 3.0f, 4.0f) + glm::vec3(0.0f, 1.5f, 0.0f);
    }
    computeBounds(mesh);
    model.boundsMin = mesh.boundsMin;
    model.boundsMax = mesh.boundsMax;
    model.meshes.push_back(mesh);
    model.instances.push_back({0, glm::mat4(1.0f)});
    return model;
}

// A sphere of `radius` around `center`: rings of latitude, smooth normals (the same position on the seam and at the
// poles is several vertices, with their own UVs)
static Mesh sphereMesh(const std::string& name, uint32_t material, glm::vec3 center, float radius) {
    const uint32_t rings = 24, segments = 48;
    Mesh mesh;
    mesh.name = name;
    mesh.materialIndex = material;
    mesh.hasNormals = true;
    // The seam's and the poles' copies get exactly the same positions (sin(pi) isn't 0 in floats): the mesh is closed
    // exactly, which the fog's distance field needs
    for (uint32_t ring = 0; ring <= rings; ring++) {
        float theta = glm::pi<float>() * static_cast<float>(ring) / rings;  // from straight up
        float sinTheta = ring == 0 || ring == rings ? 0.0f : std::sin(theta);
        float cosTheta = ring == 0 ? 1.0f : ring == rings ? -1.0f : std::cos(theta);
        for (uint32_t segment = 0; segment <= segments; segment++) {
            float phi = 2.0f * glm::pi<float>() * static_cast<float>(segment % segments) / segments;
            glm::vec3 normal(sinTheta * std::cos(phi), cosTheta, -sinTheta * std::sin(phi));
            mesh.vertices.push_back({center + normal * radius, normal,
                                     {static_cast<float>(segment) / segments, static_cast<float>(ring) / rings},
                                     glm::vec4(0.0f)});
        }
    }
    for (uint32_t ring = 0; ring < rings; ring++) {
        for (uint32_t segment = 0; segment < segments; segment++) {
            uint32_t a = ring * (segments + 1) + segment, b = a + segments + 1;  // this ring, the one below
            if (ring > 0) {
                mesh.indices.insert(mesh.indices.end(), {a, b, a + 1});
            }
            if (ring + 1 < rings) {
                mesh.indices.insert(mesh.indices.end(), {a + 1, b, b + 1});
            }
        }
    }
    computeBounds(mesh);
    return mesh;
}

// A glowing ball, 0.3 across, floating 1.5 above the ground: the middle of a fog box (which is 3 high)
static Model createLightOrb() {
    Model model;
    model.name = lightOrbName;
    Material material = matte("orb", glm::vec3(1.0f));
    material.emissiveColor = {1.0f, 0.8f, 0.55f};  // warm
    material.emissiveStrength = 40.0f;
    model.materials.push_back(material);
    Mesh mesh = sphereMesh("mesh", 0, glm::vec3(0.0f, 1.5f, 0.0f), 0.15f);
    model.boundsMin = mesh.boundsMin;
    model.boundsMax = mesh.boundsMax;
    model.meshes.push_back(mesh);
    model.instances.push_back({0, glm::mat4(1.0f)});
    return model;
}

// A cumulus-like cloud, 8 x 3.6 x 5.6, its bottom 3 above the ground: a flattened ball of invisible fog (only the fog
// shows). Its shape comes from the noise and the edge fade; the thickness is that of a small real cloud seen
// close up. Its material is shared by all clouds: give one another Variation, or turn it, so two clouds differ.
static Model createCloud() {
    Model model;
    model.name = cloudName;
    Material material = matte("cloud", glm::vec3(1.0f));
    material.invisibleSurface = true;
    material.fogDensity = 8.0f;            // half of it passes on straight (the droplets' forward peak, see Material)
    material.fogColor = glm::vec3(0.99f);  // droplets absorb almost nothing
    material.fogDroplets = 20.0f;
    material.fogNoise = true;
    material.fogNoiseSize = 1.6f;
    material.fogCoverage = 0.7f;
    material.fogDetail = 0.5f;
    material.fogEdge = 1.0f;
    model.materials.push_back(material);
    const glm::vec3 radii(4.0f, 1.8f, 2.8f), center(0.0f, 4.8f, 0.0f);
    Mesh mesh = sphereMesh("mesh", 0, glm::vec3(0.0f), 1.0f);
    for (SceneVertex& vertex : mesh.vertices) {
        vertex.position = center + vertex.position * radii;
        vertex.normal = glm::normalize(vertex.normal / radii);  // an ellipsoid's normal
    }
    computeBounds(mesh);
    model.boundsMin = mesh.boundsMin;
    model.boundsMax = mesh.boundsMax;
    model.meshes.push_back(mesh);
    model.instances.push_back({0, glm::mat4(1.0f)});
    return model;
}

static Model createPortalRim() {
    Model model;
    model.name = portalRimModelName();
    const char* names[2] = {"rim A", "rim B"};
    const glm::vec3 colors[2] = {{0.1f, 0.45f, 1.0f}, {1.0f, 0.45f, 0.05f}};
    for (uint32_t i = 0; i < 2; i++) {
        Material material = matte(names[i], colors[i]);
        material.roughness = 0.4f;
        material.emissiveColor = colors[i];
        material.emissiveStrength = 2.0f;
        model.materials.push_back(material);
        model.meshes.push_back(cubeMesh(names[i], i));
        model.instances.push_back({i, glm::mat4(1.0f)});
    }
    model.boundsMin = glm::vec3(-0.5f);
    model.boundsMax = glm::vec3(0.5f);
    return model;
}

static Model createCornellBox(bool withBlocks, bool pointLight) {
    Model model;
    model.name = pointLight ? cornellBoxPointLightName : withBlocks ? cornellBoxWithBlocksName : cornellBoxName;

    // Colors close to the measured paints of the original box (converted to RGB)
    enum { White, Red, Green, Light };
    model.materials.push_back(matte("white", {0.73f, 0.73f, 0.73f}));
    model.materials.push_back(matte("red", {0.65f, 0.05f, 0.05f}));
    model.materials.push_back(matte("green", {0.12f, 0.45f, 0.15f}));
    Material light = matte("light", {0.78f, 0.78f, 0.78f});
    light.emissiveColor = {1.0f, 0.85f, 0.6f};  // warm, like the original's incandescent light
    light.emissiveStrength = 15.0f;
    model.materials.push_back(light);

    const float h = 2.0f;       // half width/depth
    const float top = 4.0f;     // ceiling height
    const float lift = 0.002f;  // the box's floor sits just above the scene's floor, so the two never overlap exactly

    // Corners: x from -h (left, red) to +h (right, green), z from -h (back) to +h (front, open).
    // Quads are wound so their normals point into the room (it doesn't change the rendering, our
    // triangles are double-sided, but it keeps the normals debug view sensible).
    model.meshes.push_back(quadMesh("floor", White, {-h, lift, h}, {h, lift, h}, {h, lift, -h}, {-h, lift, -h}));
    model.meshes.push_back(quadMesh("ceiling", White, {-h, top, -h}, {h, top, -h}, {h, top, h}, {-h, top, h}));
    model.meshes.push_back(quadMesh("back wall", White, {-h, lift, -h}, {h, lift, -h}, {h, top, -h}, {-h, top, -h}));
    model.meshes.push_back(quadMesh("left wall (red)", Red, {-h, lift, h}, {-h, lift, -h}, {-h, top, -h}, {-h, top, h}));
    model.meshes.push_back(quadMesh("right wall (green)", Green, {h, lift, -h}, {h, lift, h}, {h, top, h}, {h, top, -h}));
    model.meshes.push_back(quadMesh("front wall", White, {h, lift, h}, {-h, lift, h}, {-h, top, h}, {h, top, h}));

    // The light: about a quarter of the ceiling's width, hanging a hair below it and facing down.
    const float lw = 0.5f, ld = 0.4f, ly = top - 0.002f;
    if (pointLight) {
        // The panel sends emission * area candela straight down (a flat glowing surface: less to the sides);
        // the point light sends that in every direction.
        PunctualLight point;
        point.name = "point light";
        point.type = LightType::Point;
        point.color = light.emissiveColor;
        point.intensity = light.emissiveStrength * (2.0f * lw) * (2.0f * ld);
        point.transform = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, top - 0.1f, 0.0f));
        model.lights.push_back(point);
    } else {
        model.meshes.push_back(quadMesh("light", Light, {-lw, ly, -ld}, {lw, ly, -ld}, {lw, ly, ld}, {-lw, ly, ld}));
    }

    if (withBlocks) {
        // Positions and angles as in the original: a short cube front right, a tall block back left.
        model.meshes.push_back(blockMesh("short block", White, {0.65f, lift, 0.55f}, {1.2f, 1.2f, 1.2f}, -17.0f));
        model.meshes.push_back(blockMesh("tall block", White, {-0.65f, lift, -0.6f}, {1.2f, 2.4f, 1.2f}, 17.0f));
    }

    for (uint32_t i = 0; i < model.meshes.size(); i++) {
        MeshInstance instance{i, glm::mat4(1.0f)};
        instance.hiddenByDefault = model.meshes[i].name == "front wall";
        model.instances.push_back(instance);
    }

    model.boundsMin = {-h, 0.0f, -h};
    model.boundsMax = {h, top, h};
    return model;
}

// Hallways of square cells (cellSize wide, `height` tall) on a grid, x to the east, z to the south. The entrance
// is the south side of the first cell (facing the default camera), the exit the east side of the last.
static Model createMirrorMaze() {
    Model model;
    model.name = mirrorMazeName;

    enum { Mirror, CeilingLamp, WallLamp };
    // Silver: almost white, so a reflection seen through many mirrors fades slowly (and Max bounces ends it)
    Material mirror = matte("mirror", {0.95f, 0.95f, 0.93f});
    mirror.metallic = 1.0f;
    mirror.roughness = 0.0f;
    model.materials.push_back(mirror);
    Material ceilingLamp = matte("ceiling lamps", {0.8f, 0.8f, 0.8f});
    ceilingLamp.emissiveColor = {1.0f, 0.88f, 0.7f};  // warm
    ceilingLamp.emissiveStrength = 12.0f;
    model.materials.push_back(ceilingLamp);
    Material wallLamp = matte("wall lamps", {0.8f, 0.8f, 0.8f});
    wallLamp.emissiveColor = {0.75f, 0.85f, 1.0f};    // cool
    wallLamp.emissiveStrength = 6.0f;
    model.materials.push_back(wallLamp);

    const float cellSize = 2.0f, h = cellSize * 0.5f;
    const float height = 3.0f;
    const float lift = 0.002f;  // above the scene's floor, as the Cornell box's

    // The way through (north is -z), with a dead end branching off it. Cells are linked only along the way and
    // the branch: neighbors that aren't linked have a mirror between them.
    const std::vector<glm::ivec2> way = {{0, 0}, {0, 1}, {0, 2}, {1, 2}, {2, 2}, {2, 3}, {2, 4}, {3, 4}, {4, 4}, {4, 3}, {4, 2}};
    const std::vector<glm::ivec2> branch = {{1, 2}, {1, 3}, {1, 4}};
    std::vector<glm::ivec2> cells = way;
    cells.insert(cells.end(), branch.begin() + 1, branch.end());
    std::vector<std::pair<glm::ivec2, glm::ivec2>> links;
    for (const std::vector<glm::ivec2>* list : {&way, &branch}) {
        for (size_t i = 1; i < list->size(); i++) {
            links.push_back({(*list)[i - 1], (*list)[i]});
        }
    }
    auto linked = [&](glm::ivec2 a, glm::ivec2 b) {
        return std::any_of(links.begin(), links.end(), [&](const auto& l) {
            return (l.first == a && l.second == b) || (l.first == b && l.second == a);
        });
    };
    auto inMaze = [&](glm::ivec2 c) { return std::find(cells.begin(), cells.end(), c) != cells.end(); };
    const glm::ivec2 entrance = way.front(), entranceSide = {0, -1};
    const glm::ivec2 exit = way.back(), exitSide = {1, 0};

    Mesh mirrors, ceilingLamps, wallLamps;
    mirrors.name = "mirrors";
    mirrors.materialIndex = Mirror;
    ceilingLamps.name = "ceiling lamps";
    ceilingLamps.materialIndex = CeilingLamp;
    wallLamps.name = "wall lamps";
    wallLamps.materialIndex = WallLamp;
    const glm::vec3 up = {0.0f, 1.0f, 0.0f};
    for (glm::ivec2 cell : cells) {
        glm::vec3 c = {cell.x * cellSize, 0.0f, -cell.y * cellSize};
        addQuad(mirrors, c + glm::vec3(-h, lift, h), c + glm::vec3(h, lift, h), c + glm::vec3(h, lift, -h),
                c + glm::vec3(-h, lift, -h));  // floor
        addQuad(mirrors, c + glm::vec3(-h, height, -h), c + glm::vec3(h, height, -h), c + glm::vec3(h, height, h),
                c + glm::vec3(-h, height, h));  // ceiling
        // A square lamp in the middle of the ceiling, a hair below it, facing down
        const float lamp = 0.3f, lampY = height - 0.002f;
        addQuad(ceilingLamps, c + glm::vec3(-lamp, lampY, -lamp), c + glm::vec3(lamp, lampY, -lamp),
                c + glm::vec3(lamp, lampY, lamp), c + glm::vec3(-lamp, lampY, lamp));

        for (glm::ivec2 side : {glm::ivec2(1, 0), glm::ivec2(-1, 0), glm::ivec2(0, 1), glm::ivec2(0, -1)}) {
            glm::ivec2 neighbor = cell + side;
            if (linked(cell, neighbor) || (cell == entrance && side == entranceSide) || (cell == exit && side == exitSide)) {
                continue;  // open
            }
            glm::vec3 out = {static_cast<float>(side.x), 0.0f, static_cast<float>(-side.y)};
            glm::vec3 along = glm::cross(out, up);  // wound so the normal points into the cell
            // Between two cells of the maze one mirror serves both: made by the cell with the smaller coordinates
            if (!inMaze(neighbor) || cell.x < neighbor.x || (cell.x == neighbor.x && cell.y < neighbor.y)) {
                glm::vec3 p = c + out * h;
                addQuad(mirrors, p - along * h + up * lift, p + along * h + up * lift, p + along * h + up * height,
                        p - along * h + up * height);
            }
            // A thin lamp strip along the wall at waist height, just off the mirror (on each side of a shared one)
            glm::vec3 q = c + out * (h - 0.01f);
            const float stripLength = h * 0.8f, stripLow = 1.0f, stripHigh = 1.06f;
            addQuad(wallLamps, q - along * stripLength + up * stripLow, q + along * stripLength + up * stripLow,
                    q + along * stripLength + up * stripHigh, q - along * stripLength + up * stripHigh);
        }
    }
    for (Mesh* mesh : {&mirrors, &ceilingLamps, &wallLamps}) {
        mesh->hasNormals = true;
        computeBounds(*mesh);
        model.instances.push_back({static_cast<uint32_t>(model.meshes.size()), glm::mat4(1.0f)});
        model.meshes.push_back(std::move(*mesh));
    }
    model.boundsMin = model.meshes[0].boundsMin;  // the mirrors enclose everything
    model.boundsMax = model.meshes[0].boundsMax;
    return model;
}

Model createBuiltInModel(const std::string& name) {
    if (name == cubeName) {
        return createCube();
    }
    if (name == cornellBoxName) {
        return createCornellBox(false, false);
    }
    if (name == cornellBoxWithBlocksName) {
        return createCornellBox(true, false);
    }
    if (name == cornellBoxPointLightName) {
        return createCornellBox(true, true);
    }
    if (name == mirrorMazeName) {
        return createMirrorMaze();
    }
    if (name == fogBoxName) {
        return createFogBox();
    }
    if (name == lightOrbName) {
        return createLightOrb();
    }
    if (name == cloudName) {
        return createCloud();
    }
    if (name == portalRimModelName()) {
        return createPortalRim();
    }
    throw std::runtime_error("unknown built-in model: " + name);
}
