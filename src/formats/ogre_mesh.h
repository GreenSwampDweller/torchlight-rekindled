// OGRE's binary mesh (`.mesh`, MeshSerializer v1.40 as every Torchlight mesh is, either byte order): the
// submeshes' raw material names, their indices, the vertex positions, normals, first UV set, bone weights,
// and the skeleton it links. Byte span in, struct out.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tl::formats {

/// One vertex's binding to a skeleton bone (`M_MESH_BONE_ASSIGNMENT` / `M_SUBMESH_BONE_ASSIGNMENT`).
struct OgreMeshBoneAssignment {
    uint32_t vertex = 0;
    uint16_t bone = 0;
    float weight = 0.0f;
};

/// One vertex set: `positions` and `normals` hold x, y, z per vertex, in the file's float values;
/// `texcoords` u, v per vertex of the first two-float texture coordinate set, empty
/// when the set has none.
struct OgreMeshVertices {
    bool present = false;
    uint32_t count = 0;
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<float> texcoords;
};

struct OgreMeshSubmesh {
    /// The material name as the file spells it (what `MeshSerializerListener::processMaterialName` sees).
    std::string material;
    bool uses_shared_vertices = true;
    bool indices_32bit = false;
    std::vector<uint32_t> indices;
    /// Its own vertices when it does not use the shared ones.
    OgreMeshVertices vertices;
    std::vector<OgreMeshBoneAssignment> bone_assignments;
};

struct OgreMesh {
    bool ok = false;
    std::string error;
    std::string version;  ///< `[MeshSerializer_v1.40]`
    bool big_endian = false;
    OgreMeshVertices shared;
    std::vector<OgreMeshSubmesh> submeshes;
    /// The skeleton it links (`M_MESH_SKELETON_LINK`, right after the submeshes); empty for none.
    std::string skeleton;
    /// Bone assignments for the `shared` vertex set (`M_MESH_BONE_ASSIGNMENT`).
    std::vector<OgreMeshBoneAssignment> bone_assignments;
};

OgreMesh parse_ogre_mesh(const uint8_t* data, size_t size);

}  // namespace tl::formats
