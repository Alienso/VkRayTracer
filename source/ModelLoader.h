//
// Loads glTF 2.0 models (.gltf / .glb) into plain CPU-side meshes, materials and textures, using cgltf.
//

#ifndef VULKANDEMO_MODELLOADER_H
#define VULKANDEMO_MODELLOADER_H

#include <glm/glm.hpp>
#include <glm/mat3x2.hpp>
#include <cstdint>
#include <string>
#include <vector>

// Must match Vertex in shaders/raytrace.comp (scalar layout)
struct SceneVertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 uv;
    glm::vec4 tangent;  // xyz: surface direction in which U grows; w: +-1, which way the bitangent points (mirrored UVs)
};

enum class TextureWrap : int { Repeat = 0, ClampToEdge = 1, MirroredRepeat = 2 };

// Decoded image, always 8-bit RGBA.
struct TextureData {
    std::string name;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> pixels;
    bool srgb = false;     // colors (base color, emissive) are stored sRGB-encoded; data (roughness, ...) is linear
    bool nearest = false;  // magnification filter: nearest instead of linear
    TextureWrap wrapU = TextureWrap::Repeat;
    TextureWrap wrapV = TextureWrap::Repeat;
};

enum class AlphaMode : uint32_t { Opaque = 0, Mask = 1, Blend = 2 };

// glTF metallic/roughness material. Every property is a constant, multiplied by its texture if it has one.
// Texture indices are -1 when there is no texture.
// The defaults are our "no material" look: grey matte.
struct Material {
    std::string name = "default";
    glm::vec4 baseColor = {0.8f, 0.8f, 0.8f, 1.0f};  // linear RGB + alpha
    float metallic = 0.0f;
    float roughness = 1.0f;
    float transmission = 0.0f;  // KHR_materials_transmission: 0 = opaque, 1 = fully see-through (glass)
    float ior = 1.5f;           // KHR_materials_ior: index of refraction, sets the reflection strength of non-metals
    // KHR_materials_volume. Without it, a transmissive surface is "thin" (a window pane, a bubble): light passes
    // straight through. Solid: the mesh is a closed volume of glass, light bends when entering and leaving.
    bool solid = false;
    glm::vec3 attenuationColor = glm::vec3(1.0f);  // color white light takes on after attenuationDistance inside
    float attenuationDistance = 0.0f;              // in the model's own units; 0 = no absorption (infinite)
    glm::vec3 emissiveColor = glm::vec3(0.0f);
    float emissiveStrength = 1.0f;  // KHR_materials_emissive_strength: lets emission go above 1
    AlphaMode alphaMode = AlphaMode::Opaque;
    float alphaCutoff = 0.5f;

    int baseColorTexture = -1;          // RGB: color, A: alpha
    int metallicRoughnessTexture = -1;  // G: roughness, B: metallic (R is often ambient occlusion, which we ignore)
    int transmissionTexture = -1;       // R: transmission
    int emissiveTexture = -1;           // RGB: emitted color
    int normalTexture = -1;             // RGB: surface direction in tangent space, see the shader
    float normalScale = 1.0f;           // strength of the normal map's bumps

    // KHR_texture_transform: where each texture is read, texture UV = transform * (mesh UV, 1). Lets a file tile,
    // move or rotate a texture without changing the mesh. Identity when the file doesn't have one.
    glm::mat3x2 baseColorUvTransform = glm::mat3x2(1.0f);
    glm::mat3x2 metallicRoughnessUvTransform = glm::mat3x2(1.0f);
    glm::mat3x2 transmissionUvTransform = glm::mat3x2(1.0f);
    glm::mat3x2 emissiveUvTransform = glm::mat3x2(1.0f);
    glm::mat3x2 normalUvTransform = glm::mat3x2(1.0f);
};

// One glTF "primitive": a list of triangles that share one material.
// Positions and normals are in the mesh's own (object) space; instances place it in the world.
struct Mesh {
    std::string name;
    std::vector<SceneVertex> vertices;
    std::vector<uint32_t> indices;
    uint32_t materialIndex = 0;  // into Model::materials
    bool hasNormals = false;     // false = the shader uses flat normals from the triangle edges
    bool hasTangents = false;    // needed for normal maps; from the file, or generated from the UVs
    // Per triangle: the average color of the material's emissive texture over it (linear RGB). Lets light
    // sampling skip the dark parts of a glowing texture. Empty if the material has no emissive texture.
    std::vector<glm::vec3> emissiveTextureAverage;
    glm::vec3 boundsMin = glm::vec3(0.0f);
    glm::vec3 boundsMax = glm::vec3(0.0f);
};

// A placed copy of a mesh. glTF nodes can reuse the same mesh several times.
struct MeshInstance {
    uint32_t meshIndex;
    glm::mat4 transform;
    bool hiddenByDefault = false;  // new objects start with this part switched off (e.g. the Cornell box's front wall)
};

struct Model {
    std::string name;
    std::vector<Mesh> meshes;
    std::vector<MeshInstance> instances;
    std::vector<Material> materials;
    std::vector<TextureData> textures;      // material texture indices point in here
    glm::vec3 boundsMin = glm::vec3(0.0f);  // world space, over all instances
    glm::vec3 boundsMax = glm::vec3(0.0f);
};

// Throws std::runtime_error if the file can't be loaded.
Model loadGltfModel(const std::string& path);

#endif //VULKANDEMO_MODELLOADER_H
