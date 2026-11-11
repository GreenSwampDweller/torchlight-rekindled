// OGRE's binary mesh (`.mesh`, MeshSerializer v1.40 as every Torchlight mesh is, either byte order): the
// submeshes' raw material names, their indices, the vertex positions and the skeleton it links. What
// collision and a model's animation list read of a mesh; nothing of skinning, levels of detail or
// animation. Byte span in, struct out.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tl::formats {

/// One vertex set: `positions` holds x, y, z per vertex, in the file's float values;
/// `texcoords` u, v per vertex of the first two-float texture coordinate set, empty
/// when the set has none.
struct OgreMeshVertices {
    bool present = false;
    uint32_t count = 0;
    std::vector<float> positions;
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
};

OgreMesh parse_ogre_mesh(const uint8_t* data, size_t size);

}  // namespace tl::formats
