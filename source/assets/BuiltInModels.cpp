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

#include "assets/BuiltInModels.h"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <glm/gtc/matrix_transform.hpp>

static const char* cornellBoxName = "Cornell box";
static const char* cornellBoxWithBlocksName = "Cornell box (classic, with blocks)";
static const char* cornellBoxPointLightName = "Cornell box (blocks, point light)";

const std::vector<std::string>& builtInModelNames() {
    static const std::vector<std::string> names = {cornellBoxName, cornellBoxWithBlocksName, cornellBoxPointLightName};
    return names;
}

bool isBuiltInModel(const std::string& name) {
    const auto& names = builtInModelNames();
    return std::find(names.begin(), names.end(), name) != names.end();
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

Model createBuiltInModel(const std::string& name) {
    if (name == cornellBoxName) {
        return createCornellBox(false, false);
    }
    if (name == cornellBoxWithBlocksName) {
        return createCornellBox(true, false);
    }
    if (name == cornellBoxPointLightName) {
        return createCornellBox(true, true);
    }
    throw std::runtime_error("unknown built-in model: " + name);
}
