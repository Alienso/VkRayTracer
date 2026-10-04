//
// glTF 2.0 loading with cgltf.
//
// A glTF file is a tree of nodes. Each node has a transform and may point at a mesh; a mesh is a list of
// primitives (triangles + one material). We turn every primitive into one Mesh and every node that uses
// a mesh into instances, with the node's transform multiplied down from its parents.
// Materials and the images they use are loaded along the way; images are decoded with stb_image.
//

#define CGLTF_IMPLEMENTATION
#include "../include/cgltf/cgltf.h"
#include "../include/stb_image.h"

#include "ModelLoader.h"
#include <glm/gtc/type_ptr.hpp>
#include <stdexcept>
#include <iostream>
#include <algorithm>
#include <limits>
#include <map>
#include <cstring>

static const cgltf_accessor* findAttribute(const cgltf_primitive& primitive, cgltf_attribute_type type) {
    for (cgltf_size i = 0; i < primitive.attributes_count; i++) {
        // index 0 = the first set (glTF allows TEXCOORD_0, TEXCOORD_1, ...)
        if (primitive.attributes[i].type == type && primitive.attributes[i].index == 0) {
            return primitive.attributes[i].data;
        }
    }
    return nullptr;
}

// Reads a whole accessor as floats, converting from whatever the file stores (normalized ints, sparse, ...).
static std::vector<float> readFloats(const cgltf_accessor* accessor) {
    std::vector<float> values(accessor->count * cgltf_num_components(accessor->type));
    cgltf_accessor_unpack_floats(accessor, values.data(), values.size());
    return values;
}

// ---------------------------------------------------------------------------------------------
// Textures

static TextureWrap toWrap(cgltf_wrap_mode mode) {
    switch (mode) {
        case cgltf_wrap_mode_clamp_to_edge: return TextureWrap::ClampToEdge;
        case cgltf_wrap_mode_mirrored_repeat: return TextureWrap::MirroredRepeat;
        default: return TextureWrap::Repeat;
    }
}

// Images live either inside the file (.glb, or a base64 "data:" URI) or in a file next to the .gltf.
static TextureData decodeImage(const cgltf_image* image, const std::string& directory) {
    TextureData texture;
    texture.name = image->name ? image->name : (image->uri && std::strncmp(image->uri, "data:", 5) != 0 ? image->uri : "image");

    int width = 0, height = 0, channels = 0;
    stbi_uc* pixels = nullptr;
    if (image->buffer_view) {
        const uint8_t* data = cgltf_buffer_view_data(image->buffer_view);
        pixels = stbi_load_from_memory(data, static_cast<int>(image->buffer_view->size), &width, &height, &channels, STBI_rgb_alpha);
    } else if (image->uri && std::strncmp(image->uri, "data:", 5) == 0) {
        const char* base64 = std::strstr(image->uri, "base64,");
        if (base64) {
            base64 += 7;
            size_t length = std::strlen(base64);
            size_t padding = (length > 0 && base64[length - 1] == '=') + (length > 1 && base64[length - 2] == '=');
            size_t size = length / 4 * 3 - padding;
            cgltf_options options{};
            void* data = nullptr;
            if (cgltf_load_buffer_base64(&options, size, base64, &data) == cgltf_result_success) {
                pixels = stbi_load_from_memory(static_cast<stbi_uc*>(data), static_cast<int>(size), &width, &height, &channels, STBI_rgb_alpha);
                free(data);  // cgltf allocates with malloc by default
            }
        }
    } else if (image->uri) {
        std::string uri = image->uri;
        uri.resize(cgltf_decode_uri(&uri[0]));  // "%20" -> " ", ...
        pixels = stbi_load((directory + uri).c_str(), &width, &height, &channels, STBI_rgb_alpha);
    }

    if (!pixels) {
        throw std::runtime_error("failed to decode image '" + texture.name + "': " +
                                 (stbi_failure_reason() ? stbi_failure_reason() : "unknown format"));
    }
    texture.width = static_cast<uint32_t>(width);
    texture.height = static_cast<uint32_t>(height);
    texture.pixels.assign(pixels, pixels + static_cast<size_t>(width) * height * 4);
    stbi_image_free(pixels);
    return texture;
}

// Loads the texture a material slot points at (once per texture and color space). Returns -1 if there is none.
class TextureLoader {
public:
    TextureLoader(const cgltf_data* data, std::string directory, Model& model)
            : data(data), directory(std::move(directory)), model(model) {}

    int load(const cgltf_texture_view& view, bool srgb, const char* slot, const std::string& materialName) {
        if (!view.texture) {
            return -1;
        }
        if (!view.texture->image) {
            std::cerr << model.name << ": " << materialName << " " << slot
                      << ": image format not supported (KTX2/WebP?), using the constant only" << std::endl;
            return -1;
        }
        if (view.texcoord != 0) {
            std::cerr << model.name << ": " << materialName << " " << slot
                      << " uses a second UV set, which isn't supported; using the first one" << std::endl;
        }

        // The same image can be used as color (sRGB) by one material and as data (linear) by another.
        auto key = std::make_pair(view.texture - data->textures, srgb);
        auto it = cache.find(key);
        if (it != cache.end()) {
            return it->second;
        }

        TextureData texture = decodeImage(view.texture->image, directory);
        texture.srgb = srgb;
        if (const cgltf_sampler* sampler = view.texture->sampler) {
            texture.nearest = sampler->mag_filter == cgltf_filter_type_nearest;
            texture.wrapU = toWrap(sampler->wrap_s);
            texture.wrapV = toWrap(sampler->wrap_t);
        }
        int index = static_cast<int>(model.textures.size());
        model.textures.push_back(std::move(texture));
        cache[key] = index;
        return index;
    }

private:
    const cgltf_data* data;
    std::string directory;
    Model& model;
    std::map<std::pair<ptrdiff_t, bool>, int> cache;
};

// ---------------------------------------------------------------------------------------------
// Materials

static Material loadMaterial(const cgltf_material& gltfMaterial, TextureLoader& textures) {
    Material material;
    material.name = gltfMaterial.name ? gltfMaterial.name : "material";

    // glTF's own defaults (when a value is missing) are white, fully metallic, fully rough; cgltf fills those in.
    if (gltfMaterial.has_pbr_metallic_roughness) {
        const cgltf_pbr_metallic_roughness& pbr = gltfMaterial.pbr_metallic_roughness;
        material.baseColor = glm::make_vec4(pbr.base_color_factor);
        material.metallic = pbr.metallic_factor;
        material.roughness = pbr.roughness_factor;
        material.baseColorTexture = textures.load(pbr.base_color_texture, true, "base color", material.name);
        material.metallicRoughnessTexture = textures.load(pbr.metallic_roughness_texture, false, "metallic/roughness", material.name);
    } else if (gltfMaterial.has_pbr_specular_glossiness) {
        // The old specular/glossiness workflow: at least keep the color.
        material.baseColor = glm::make_vec4(gltfMaterial.pbr_specular_glossiness.diffuse_factor);
        material.baseColorTexture = textures.load(gltfMaterial.pbr_specular_glossiness.diffuse_texture, true, "diffuse", material.name);
    }

    if (gltfMaterial.has_transmission) {
        material.transmission = gltfMaterial.transmission.transmission_factor;
        material.transmissionTexture = textures.load(gltfMaterial.transmission.transmission_texture, false, "transmission", material.name);
    }
    if (gltfMaterial.has_ior) {
        material.ior = gltfMaterial.ior.ior;
    }

    material.emissiveColor = glm::make_vec3(gltfMaterial.emissive_factor);
    if (gltfMaterial.has_emissive_strength) {
        material.emissiveStrength = gltfMaterial.emissive_strength.emissive_strength;
    }
    material.emissiveTexture = textures.load(gltfMaterial.emissive_texture, true, "emissive", material.name);

    material.normalTexture = textures.load(gltfMaterial.normal_texture, false, "normal", material.name);
    material.normalScale = gltfMaterial.normal_texture.scale;

    switch (gltfMaterial.alpha_mode) {
        case cgltf_alpha_mode_mask: material.alphaMode = AlphaMode::Mask; break;
        case cgltf_alpha_mode_blend: material.alphaMode = AlphaMode::Blend; break;
        default: material.alphaMode = AlphaMode::Opaque; break;
    }
    material.alphaCutoff = gltfMaterial.alpha_cutoff;
    return material;
}

// ---------------------------------------------------------------------------------------------
// Meshes

// Tangents for files that have a normal map but no TANGENT attribute.
// A normal map stores directions relative to the surface: X along the texture's U axis, Y along its V axis.
// So per triangle we work out which 3D direction U (and V) grow in, from how the UVs change along the edges,
// and average that over the triangles around each vertex.
// (Exporters use the more elaborate MikkTSpace algorithm; this simple version agrees with it on smooth meshes.)
static void generateTangents(Mesh& mesh) {
    std::vector<glm::vec3> uDirections(mesh.vertices.size(), glm::vec3(0.0f));
    std::vector<glm::vec3> vDirections(mesh.vertices.size(), glm::vec3(0.0f));

    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        uint32_t i0 = mesh.indices[i], i1 = mesh.indices[i + 1], i2 = mesh.indices[i + 2];
        const SceneVertex& v0 = mesh.vertices[i0];
        const SceneVertex& v1 = mesh.vertices[i1];
        const SceneVertex& v2 = mesh.vertices[i2];

        glm::vec3 edge1 = v1.position - v0.position;
        glm::vec3 edge2 = v2.position - v0.position;
        // glTF's V axis points down the image, but a normal map's +Y means "up in the image". Flipping V here
        // makes the bitangent point up, matching what MikkTSpace-generated tangents in glTF files do.
        glm::vec2 duv1 = (v1.uv - v0.uv) * glm::vec2(1.0f, -1.0f);
        glm::vec2 duv2 = (v2.uv - v0.uv) * glm::vec2(1.0f, -1.0f);

        float determinant = duv1.x * duv2.y - duv2.x * duv1.y;
        if (std::abs(determinant) < 1e-12f) {
            continue;  // degenerate UVs, this triangle can't tell us anything
        }
        float r = 1.0f / determinant;
        glm::vec3 uDirection = (edge1 * duv2.y - edge2 * duv1.y) * r;
        glm::vec3 vDirection = (edge2 * duv1.x - edge1 * duv2.x) * r;
        for (uint32_t index : {i0, i1, i2}) {
            uDirections[index] += uDirection;
            vDirections[index] += vDirection;
        }
    }

    for (size_t i = 0; i < mesh.vertices.size(); i++) {
        SceneVertex& vertex = mesh.vertices[i];
        glm::vec3 n = vertex.normal;
        // Make the tangent perpendicular to the normal (Gram-Schmidt)
        glm::vec3 t = uDirections[i] - n * glm::dot(n, uDirections[i]);
        if (glm::dot(t, t) < 1e-20f) {
            // No usable UVs around this vertex: any direction perpendicular to the normal will do
            t = glm::cross(std::abs(n.y) < 0.999f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f), n);
        }
        t = glm::normalize(t);
        // Mirrored UVs: cross(normal, tangent) points opposite to the V direction, then w = -1
        float handedness = glm::dot(glm::cross(n, t), vDirections[i]) < 0.0f ? -1.0f : 1.0f;
        vertex.tangent = glm::vec4(t, handedness);
    }
    mesh.hasTangents = true;
}

static Mesh loadPrimitive(const cgltf_mesh& gltfMesh, const cgltf_primitive& primitive, size_t primitiveIndex) {
    Mesh mesh;
    mesh.name = gltfMesh.name ? gltfMesh.name : "mesh";
    if (gltfMesh.primitives_count > 1) {
        mesh.name += " #" + std::to_string(primitiveIndex);
    }

    const cgltf_accessor* positions = findAttribute(primitive, cgltf_attribute_type_position);
    const cgltf_accessor* normals = findAttribute(primitive, cgltf_attribute_type_normal);
    const cgltf_accessor* uvs = findAttribute(primitive, cgltf_attribute_type_texcoord);
    const cgltf_accessor* tangents = findAttribute(primitive, cgltf_attribute_type_tangent);

    std::vector<float> positionData = readFloats(positions);
    std::vector<float> normalData = normals ? readFloats(normals) : std::vector<float>();
    std::vector<float> uvData = uvs ? readFloats(uvs) : std::vector<float>();
    std::vector<float> tangentData = tangents ? readFloats(tangents) : std::vector<float>();
    mesh.hasNormals = normals != nullptr;
    mesh.hasTangents = tangents != nullptr && normals != nullptr;

    mesh.vertices.resize(positions->count);
    for (size_t i = 0; i < positions->count; i++) {
        SceneVertex& vertex = mesh.vertices[i];
        vertex.position = glm::make_vec3(&positionData[i * 3]);
        vertex.normal = mesh.hasNormals ? glm::make_vec3(&normalData[i * 3]) : glm::vec3(0.0f);
        vertex.uv = uvs ? glm::make_vec2(&uvData[i * 2]) : glm::vec2(0.0f);
        vertex.tangent = mesh.hasTangents ? glm::make_vec4(&tangentData[i * 4]) : glm::vec4(0.0f);
    }

    // Non-indexed primitives: every 3 consecutive vertices form a triangle.
    if (primitive.indices) {
        mesh.indices.resize(primitive.indices->count);
        cgltf_accessor_unpack_indices(primitive.indices, mesh.indices.data(), sizeof(uint32_t), mesh.indices.size());
    } else {
        mesh.indices.resize(positions->count);
        for (uint32_t i = 0; i < mesh.indices.size(); i++) {
            mesh.indices[i] = i;
        }
    }
    mesh.indices.resize(mesh.indices.size() / 3 * 3);  // drop a broken trailing triangle, if any

    bool hasNormalMap = primitive.material && primitive.material->normal_texture.texture;
    if (hasNormalMap && !mesh.hasTangents && normals && uvs) {
        generateTangents(mesh);
    }

    // glTF positions accessors are required to carry min/max, but don't trust that.
    mesh.boundsMin = glm::vec3(std::numeric_limits<float>::max());
    mesh.boundsMax = glm::vec3(std::numeric_limits<float>::lowest());
    for (const SceneVertex& vertex : mesh.vertices) {
        mesh.boundsMin = glm::min(mesh.boundsMin, vertex.position);
        mesh.boundsMax = glm::max(mesh.boundsMax, vertex.position);
    }
    return mesh;
}

static void addNodeInstances(const cgltf_data* data, const cgltf_node* node, const glm::mat4& parentTransform,
                             const std::vector<std::vector<uint32_t>>& meshesOfGltfMesh, Model& model) {
    float local[16];
    cgltf_node_transform_local(node, local);  // column major, like glm
    glm::mat4 transform = parentTransform * glm::make_mat4(local);

    if (node->mesh) {
        size_t gltfMeshIndex = node->mesh - data->meshes;
        for (uint32_t meshIndex : meshesOfGltfMesh[gltfMeshIndex]) {
            model.instances.push_back({meshIndex, transform});
        }
    }
    for (cgltf_size i = 0; i < node->children_count; i++) {
        addNodeInstances(data, node->children[i], transform, meshesOfGltfMesh, model);
    }
}

Model loadGltfModel(const std::string& path) {
    cgltf_options options{};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&options, path.c_str(), &data) != cgltf_result_success) {
        throw std::runtime_error("failed to parse glTF file: " + path);
    }
    // .glb files carry their binary data inside, .gltf files point at separate .bin files.
    if (cgltf_load_buffers(&options, data, path.c_str()) != cgltf_result_success) {
        cgltf_free(data);
        throw std::runtime_error("failed to load glTF buffers: " + path);
    }

    Model model;
    size_t slash = path.find_last_of("/\\");
    model.name = path.substr(slash + 1);
    std::string directory = slash == std::string::npos ? "" : path.substr(0, slash + 1);

    // 1: materials (and the textures they use), in the file's order so primitives can refer to them by index.
    //    One extra default material at the end for primitives without one.
    try {
        TextureLoader textures(data, directory, model);
        for (cgltf_size m = 0; m < data->materials_count; m++) {
            model.materials.push_back(loadMaterial(data->materials[m], textures));
        }
    } catch (...) {
        cgltf_free(data);
        throw;
    }
    uint32_t defaultMaterial = static_cast<uint32_t>(model.materials.size());
    bool defaultMaterialUsed = false;

    // 2: meshes. One glTF mesh can turn into several of ours (one per primitive).
    std::vector<std::vector<uint32_t>> meshesOfGltfMesh(data->meshes_count);
    for (cgltf_size m = 0; m < data->meshes_count; m++) {
        const cgltf_mesh& gltfMesh = data->meshes[m];
        for (cgltf_size p = 0; p < gltfMesh.primitives_count; p++) {
            const cgltf_primitive& primitive = gltfMesh.primitives[p];
            if (primitive.type != cgltf_primitive_type_triangles) {
                continue;  // points and lines can't be ray traced as triangles
            }
            if (primitive.has_draco_mesh_compression || !findAttribute(primitive, cgltf_attribute_type_position)) {
                std::cerr << model.name << ": skipping a compressed or position-less primitive" << std::endl;
                continue;
            }
            Mesh mesh = loadPrimitive(gltfMesh, primitive, p);
            if (mesh.indices.empty()) {
                continue;
            }
            if (primitive.material) {
                mesh.materialIndex = static_cast<uint32_t>(primitive.material - data->materials);
            } else {
                mesh.materialIndex = defaultMaterial;
                defaultMaterialUsed = true;
            }
            meshesOfGltfMesh[m].push_back(static_cast<uint32_t>(model.meshes.size()));
            model.meshes.push_back(std::move(mesh));
        }
    }

    if (defaultMaterialUsed) {
        model.materials.emplace_back();
    }

    // 3: instances, by walking the node tree of the default scene (or all root nodes if there is none)
    const cgltf_scene* scene = data->scene ? data->scene : (data->scenes_count > 0 ? &data->scenes[0] : nullptr);
    if (scene) {
        for (cgltf_size i = 0; i < scene->nodes_count; i++) {
            addNodeInstances(data, scene->nodes[i], glm::mat4(1.0f), meshesOfGltfMesh, model);
        }
    } else {
        for (cgltf_size i = 0; i < data->nodes_count; i++) {
            if (!data->nodes[i].parent) {
                addNodeInstances(data, &data->nodes[i], glm::mat4(1.0f), meshesOfGltfMesh, model);
            }
        }
    }
    cgltf_free(data);

    if (model.instances.empty()) {
        throw std::runtime_error("glTF file contains no triangle meshes: " + path);
    }

    // 4: world space bounds: transform the 8 corners of every instance's box
    model.boundsMin = glm::vec3(std::numeric_limits<float>::max());
    model.boundsMax = glm::vec3(std::numeric_limits<float>::lowest());
    for (const MeshInstance& instance : model.instances) {
        const Mesh& mesh = model.meshes[instance.meshIndex];
        for (int i = 0; i < 8; i++) {
            glm::vec3 corner = {(i & 1) ? mesh.boundsMax.x : mesh.boundsMin.x,
                                (i & 2) ? mesh.boundsMax.y : mesh.boundsMin.y,
                                (i & 4) ? mesh.boundsMax.z : mesh.boundsMin.z};
            glm::vec3 world = glm::vec3(instance.transform * glm::vec4(corner, 1.0f));
            model.boundsMin = glm::min(model.boundsMin, world);
            model.boundsMax = glm::max(model.boundsMax, world);
        }
    }
    return model;
}
