//
// glTF 2.0 loading with cgltf.
//
// A glTF file is a tree of nodes. Each node has a transform and may point at a mesh; a mesh is a list of
// primitives (triangles + one material). We turn every primitive into one Mesh and every node that uses
// a mesh into instances, with the node's transform multiplied down from its parents.
//

#define CGLTF_IMPLEMENTATION
#include "../include/cgltf/cgltf.h"

#include "ModelLoader.h"
#include <glm/gtc/type_ptr.hpp>
#include <stdexcept>
#include <iostream>
#include <algorithm>
#include <limits>

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

static Mesh loadPrimitive(const cgltf_mesh& gltfMesh, const cgltf_primitive& primitive, size_t primitiveIndex) {
    Mesh mesh;
    mesh.name = gltfMesh.name ? gltfMesh.name : "mesh";
    if (gltfMesh.primitives_count > 1) {
        mesh.name += " #" + std::to_string(primitiveIndex);
    }

    const cgltf_accessor* positions = findAttribute(primitive, cgltf_attribute_type_position);
    const cgltf_accessor* normals = findAttribute(primitive, cgltf_attribute_type_normal);
    const cgltf_accessor* uvs = findAttribute(primitive, cgltf_attribute_type_texcoord);

    std::vector<float> positionData = readFloats(positions);
    std::vector<float> normalData = normals ? readFloats(normals) : std::vector<float>();
    std::vector<float> uvData = uvs ? readFloats(uvs) : std::vector<float>();
    mesh.hasNormals = normals != nullptr;

    mesh.vertices.resize(positions->count);
    for (size_t i = 0; i < positions->count; i++) {
        SceneVertex& vertex = mesh.vertices[i];
        vertex.position = glm::make_vec3(&positionData[i * 3]);
        vertex.normal = mesh.hasNormals ? glm::make_vec3(&normalData[i * 3]) : glm::vec3(0.0f);
        vertex.uv = uvs ? glm::make_vec2(&uvData[i * 2]) : glm::vec2(0.0f);
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

    // Only the constant color for now; textures come with Phase 5. glTF stores it already linear.
    if (primitive.material && primitive.material->has_pbr_metallic_roughness) {
        mesh.baseColor = glm::make_vec4(primitive.material->pbr_metallic_roughness.base_color_factor);
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
    model.name = path.substr(path.find_last_of("/\\") + 1);

    // 1: meshes. One glTF mesh can turn into several of ours (one per primitive).
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
            meshesOfGltfMesh[m].push_back(static_cast<uint32_t>(model.meshes.size()));
            model.meshes.push_back(std::move(mesh));
        }
    }

    // 2: instances, by walking the node tree of the default scene (or all root nodes if there is none)
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

    // 3: world space bounds: transform the 8 corners of every instance's box
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
