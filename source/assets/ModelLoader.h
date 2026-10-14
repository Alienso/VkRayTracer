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

// How TextureData::pixels is stored. Block-compressed formats (from .dds files) stay compressed on the GPU too:
// 4x4 texels per block of 8 (BC1, BC4) or 16 bytes. BC5 has two channels only (normal maps: X and Y, Z is rebuilt),
// BC4 one (made by the loader, see Material::normalVarianceTexture).
enum class TextureFormat : int { RGBA8 = 0, BC1, BC2, BC3, BC5, BC7, BC4 };

// An image as it goes to the GPU: 8-bit RGBA, or the blocks of a compressed format.
// Mip levels: each half the size of the one before (rounded down, at least 1), down to 1x1, for distant or blurry
// hits (see the shader's ray cones). pixels holds them one after the other, the full-size level first. A .dds file
// brings its own; for 8-bit images the loader makes them.
struct TextureData {
    std::string name;
    uint32_t width = 0;   // of level 0
    uint32_t height = 0;
    uint32_t mipLevels = 1;
    TextureFormat format = TextureFormat::RGBA8;
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
    // KHR_materials_diffuse_transmission: thin surfaces that let light through and scatter it (leaves, paper,
    // fabric). This share of the diffuse layer leaves on the other side instead, tinted by its own color.
    float diffuseTransmission = 0.0f;
    glm::vec3 diffuseTransmissionColor = glm::vec3(1.0f);
    // Fog inside: the closed mesh is filled with fog of this thickness (per unit of distance in the world: light goes
    // ~1 / fogDensity before it scatters; 0 = none). See "Fog" in shaders/raytrace.comp. Not from glTF (yet).
    float fogDensity = 0.0f;
    glm::vec3 fogColor = glm::vec3(1.0f);  // its color in white light (the share of the light it scatters that goes on)
    float fogForward = 0.3f;               // -1..1: how much scattered light keeps going forward (0: every way alike)
    bool invisibleSurface = false;         // rays pass the surface unchanged: only the fog inside shows
    // Fog that varies through the volume (see "Varying density" in shaders/raytrace.comp). Its thickness is then the
    // most there is anywhere; noise and the edge fade only take away.
    bool fogNoise = false;     // patchy: a 3D noise in the mesh's own space (moves, turns and scales with the object)
    float fogNoiseSize = 1.0f; // the size of its blobs, in the mesh's units
    float fogCoverage = 0.5f;  // 0..1: about the share of the volume that has fog
    int fogNoiseSeed = 0;      // another part of the noise, so objects sharing the numbers still differ
    float fogEdge = 0.0f;      // fades out over this distance towards the mesh's surface (mesh units); 0 = sharp
    glm::vec3 fogDrift = glm::vec3(0.0f);  // patchy fog: the blobs move this far per second (mesh units, the mesh's axes)
    glm::vec3 emissiveColor = glm::vec3(0.0f);
    float emissiveStrength = 1.0f;  // KHR_materials_emissive_strength: lets emission go above 1
    AlphaMode alphaMode = AlphaMode::Opaque;
    float alphaCutoff = 0.5f;

    int baseColorTexture = -1;          // RGB: color, A: alpha
    int metallicRoughnessTexture = -1;  // G: roughness, B: metallic (R is often ambient occlusion, which we ignore)
    int transmissionTexture = -1;       // R: transmission
    int emissiveTexture = -1;           // RGB: emitted color
    int diffuseTransmissionTexture = -1;       // A: diffuse transmission
    int diffuseTransmissionColorTexture = -1;  // RGB: color of the light that goes through
    int normalTexture = -1;             // RGB (or RG, see TextureFormat::BC5): surface direction in tangent space, see the shader
    float normalScale = 1.0f;           // strength of the normal map's bumps
    // Made by the loader from the normal map (Toksvig): R = how much the bumps that a smaller mip level averages away
    // widen the reflection (as GGX alpha). Half the normal map's size: its level k belongs to the normal map's level
    // k + 1 (the full size averages nothing). -1 without a normal map, or one in a format we can't decode.
    int normalVarianceTexture = -1;

    // KHR_texture_transform: where each texture is read, texture UV = transform * (mesh UV, 1). Lets a file tile,
    // move or rotate a texture without changing the mesh. Identity when the file doesn't have one.
    glm::mat3x2 baseColorUvTransform = glm::mat3x2(1.0f);
    glm::mat3x2 metallicRoughnessUvTransform = glm::mat3x2(1.0f);
    glm::mat3x2 transmissionUvTransform = glm::mat3x2(1.0f);
    glm::mat3x2 emissiveUvTransform = glm::mat3x2(1.0f);
    glm::mat3x2 normalUvTransform = glm::mat3x2(1.0f);
    glm::mat3x2 diffuseTransmissionUvTransform = glm::mat3x2(1.0f);
    glm::mat3x2 diffuseTransmissionColorUvTransform = glm::mat3x2(1.0f);
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

// KHR_lights_punctual: a light without size, placed by a node. It shines from the node's position (point,
// spot) or along its direction (directional), down the node's -Z axis. Only shadow rays can find such a light.
enum class LightType : uint32_t { Directional = 0, Point = 1, Spot = 2 };
struct PunctualLight {
    std::string name;
    LightType type = LightType::Point;
    glm::vec3 color = glm::vec3(1.0f);  // linear RGB
    float intensity = 1.0f;          // point and spot: candela (lm/sr), directional: lux (lm/m^2)
    float range = 0.0f;              // distance where the light fades out completely; 0 = no limit
    float innerConeAngle = 0.0f;     // spot: full brightness inside this angle from the axis (radians)
    float outerConeAngle = 0.7853982f;  // spot: dark outside it
    glm::mat4 transform = glm::mat4(1.0f);  // the node's transform in the model (only position and rotation count)
    bool enabled = true;             // UI
};

struct Model {
    std::string name;
    std::vector<Mesh> meshes;
    std::vector<MeshInstance> instances;
    std::vector<PunctualLight> lights;
    std::vector<Material> materials;
    std::vector<TextureData> textures;      // material texture indices point in here
    glm::vec3 boundsMin = glm::vec3(0.0f);  // world space, over all instances
    glm::vec3 boundsMax = glm::vec3(0.0f);
};

// Throws std::runtime_error if the file can't be loaded.
Model loadGltfModel(const std::string& path);

#endif //VULKANDEMO_MODELLOADER_H
