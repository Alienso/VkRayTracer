//
// Loads glTF 2.0 models (.gltf / .glb) into plain CPU-side meshes, using cgltf.
//

#ifndef VULKANDEMO_MODELLOADER_H
#define VULKANDEMO_MODELLOADER_H

#include <glm/glm.hpp>
#include <string>
#include <vector>

// Must match Vertex in shaders/raytrace.comp (scalar layout)
struct SceneVertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 uv;
};

// One glTF "primitive": a list of triangles that share one material.
// Positions and normals are in the mesh's own (object) space; instances place it in the world.
struct Mesh {
    std::string name;
    std::vector<SceneVertex> vertices;
    std::vector<uint32_t> indices;
    glm::vec4 baseColor = {0.8f, 0.8f, 0.8f, 1.0f};  // default material: grey matte
    bool hasNormals = false;  // false = the shader uses flat normals from the triangle edges
    glm::vec3 boundsMin = glm::vec3(0.0f);
    glm::vec3 boundsMax = glm::vec3(0.0f);
};

// A placed copy of a mesh. glTF nodes can reuse the same mesh several times.
struct MeshInstance {
    uint32_t meshIndex;
    glm::mat4 transform;
};

struct Model {
    std::string name;
    std::vector<Mesh> meshes;
    std::vector<MeshInstance> instances;
    glm::vec3 boundsMin = glm::vec3(0.0f);  // world space, over all instances
    glm::vec3 boundsMax = glm::vec3(0.0f);
};

// Throws std::runtime_error if the file can't be loaded.
Model loadGltfModel(const std::string& path);

#endif //VULKANDEMO_MODELLOADER_H
