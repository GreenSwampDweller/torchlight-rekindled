#include "formats/ogre_skeleton.h"
#include "formats/ogre_mesh.h"
#include "test_support.h"

#include <cstring>
#include <string>
#include <vector>

using tl::formats::OgreMesh;
using tl::formats::OgreSkeleton;
using tl::formats::parse_ogre_mesh;
using tl::formats::parse_ogre_skeleton;

namespace {

// A made-up skeleton or mesh in OGRE's chunk layout, little- or big-endian.
struct Writer {
    bool big = false;
    std::vector<uint8_t> bytes;
    void u8(uint8_t v) { bytes.push_back(v); }
    void u16(uint16_t v) {
        if (big) {
            bytes.push_back(static_cast<uint8_t>(v >> 8));
            bytes.push_back(static_cast<uint8_t>(v));
        } else {
            bytes.push_back(static_cast<uint8_t>(v));
            bytes.push_back(static_cast<uint8_t>(v >> 8));
        }
    }
    void u32(uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            const int shift = big ? 24 - 8 * i : 8 * i;
            bytes.push_back(static_cast<uint8_t>(v >> shift));
        }
    }
    void f32(float f) {
        uint32_t u = 0;
        std::memcpy(&u, &f, 4);
        u32(u);
    }
    void line(const std::string& s) {
        bytes.insert(bytes.end(), s.begin(), s.end());
        bytes.push_back('\n');
    }
};

std::vector<uint8_t> made_up(bool big) {
    Writer w;
    w.big = big;
    w.u16(0x1000);
    w.line("[Serializer_v1.10]");
    // Bone 0 ("root", no scale) and Bone 1 ("arm", with scale): `calcBoneSize` leaves the name out.
    w.u16(0x2000);
    w.u32(6 + 2 + 7 * 4);
    w.line("root");
    w.u16(0);
    w.f32(1.0f);
    w.f32(2.0f);
    w.f32(3.0f);
    w.f32(0.0f);
    w.f32(0.0f);
    w.f32(0.0f);
    w.f32(1.0f);
    w.u16(0x2000);
    w.u32(6 + 2 + 10 * 4);
    w.line("arm");
    w.u16(1);
    w.f32(4.0f);
    w.f32(5.0f);
    w.f32(6.0f);
    w.f32(0.0f);
    w.f32(0.707f);
    w.f32(0.0f);
    w.f32(0.707f);
    w.f32(2.0f);
    w.f32(2.0f);
    w.f32(2.0f);
    // Bone 1's parent is Bone 0.
    w.u16(0x3000);
    w.u32(6 + 4);
    w.u16(1);
    w.u16(0);
    // Two animations: the first with one track and two keyframes (without and with scale).
    const std::string names[] = {"Die", "Death_Land"};
    const float lengths[] = {1.5f, 0.75f};
    for (int a = 0; a < 2; ++a) {
        const bool track = a == 0;
        const uint32_t track_bytes = track ? (6 + 2 + 38 + 50) : 0;
        w.u16(0x4000);
        w.u32(static_cast<uint32_t>(6 + names[a].size() + 1 + 4 + track_bytes));
        w.line(names[a]);
        w.f32(lengths[a]);
        if (track) {
            w.u16(0x4100);
            w.u32(track_bytes);
            w.u16(1);
            // Keyframe 0 (38 bytes, no scale)
            w.u16(0x4110);
            w.u32(38);
            w.f32(0.0f);
            w.f32(0.0f);
            w.f32(0.0f);
            w.f32(0.0f);
            w.f32(1.0f);
            w.f32(0.5f);
            w.f32(1.5f);
            w.f32(2.5f);
            // Keyframe 1 (50 bytes, with scale)
            w.u16(0x4110);
            w.u32(50);
            w.f32(1.5f);
            w.f32(0.0f);
            w.f32(1.0f);
            w.f32(0.0f);
            w.f32(0.0f);
            w.f32(-1.0f);
            w.f32(0.0f);
            w.f32(1.0f);
            w.f32(1.25f);
            w.f32(1.5f);
            w.f32(1.75f);
        }
    }
    return w.bytes;
}

std::vector<uint8_t> made_up_mesh(bool big) {
    Writer w;
    w.big = big;
    w.u16(0x1000);
    w.line("[MeshSerializer_v1.40]");
    w.u16(0x3000);  // M_MESH
    w.u32(6);
    w.u8(1);  // skeletallyAnimated
    // Shared geometry (1 vertex: pos + normal + uv = 32 bytes)
    w.u16(0x5000);  // M_GEOMETRY
    w.u32(6);
    w.u32(1);
    w.u16(0x5100);  // M_GEOMETRY_VERTEX_DECLARATION
    w.u32(6);
    auto elem = [&](uint16_t type, uint16_t semantic, uint16_t offset) {
        w.u16(0x5110);
        w.u32(16);
        w.u16(0);
        w.u16(type);
        w.u16(semantic);
        w.u16(offset);
        w.u16(0);
    };
    elem(2, 1, 0);   // float3 position
    elem(2, 4, 12);  // float3 normal
    elem(1, 7, 24);  // float2 texcoord
    w.u16(0x5200);   // M_GEOMETRY_VERTEX_BUFFER
    w.u32(6);
    w.u16(0);
    w.u16(32);
    w.u16(0x5210);  // M_GEOMETRY_VERTEX_BUFFER_DATA
    w.u32(6 + 32);
    w.f32(10.0f);
    w.f32(20.0f);
    w.f32(30.0f);
    w.f32(0.0f);
    w.f32(1.0f);
    w.f32(0.0f);
    w.f32(0.25f);
    w.f32(0.75f);
    // One submesh using shared vertices, with a submesh bone assignment
    w.u16(0x4000);  // M_SUBMESH
    w.u32(6);
    w.line("ArmorMat");
    w.u8(1);  // usesSharedVertices
    w.u32(3);
    w.u8(0);  // 16-bit indices
    w.u16(0);
    w.u16(0);
    w.u16(0);
    w.u16(0x4100);  // M_SUBMESH_BONE_ASSIGNMENT
    w.u32(16);
    w.u32(0);
    w.u16(2);
    w.f32(0.25f);
    // Skeleton link + shared bone assignment
    w.u16(0x6000);  // M_MESH_SKELETON_LINK
    w.u32(6);
    w.line("Destroyer.skeleton");
    w.u16(0x7000);  // M_MESH_BONE_ASSIGNMENT
    w.u32(16);
    w.u32(0);
    w.u16(1);
    w.f32(0.75f);
    return w.bytes;
}

bool skeleton_bones_and_tracks_match(const OgreSkeleton& s) {
    if (s.bones.size() != 2 || s.animations.size() != 2) return false;
    if (s.bones[0].name != "root" || s.bones[0].parent != -1 || s.bones[0].position[1] != 2.0f)
        return false;
    if (s.bones[1].name != "arm" || s.bones[1].parent != 0 || s.bones[1].scale[0] != 2.0f)
        return false;
    if (s.animations[0].tracks.size() != 1) return false;
    const auto& tr = s.animations[0].tracks[0];
    if (tr.bone != 1 || tr.keyframes.size() != 2) return false;
    return tr.keyframes[0].translate[1] == 1.5f && tr.keyframes[0].scale[0] == 1.0f &&
           tr.keyframes[1].time == 1.5f && tr.keyframes[1].orientation[1] == 1.0f &&
           tr.keyframes[1].scale[2] == 1.75f;
}

bool mesh_normals_and_weights_match(const OgreMesh& m, bool big) {
    if (!m.ok || m.big_endian != big || m.skeleton != "Destroyer.skeleton") return false;
    if (m.shared.normals.size() != 3 || m.shared.normals[1] != 1.0f) return false;
    if (m.bone_assignments.size() != 1 || m.bone_assignments[0].bone != 1 ||
        m.bone_assignments[0].weight != 0.75f)
        return false;
    if (m.submeshes.size() != 1 || m.submeshes[0].bone_assignments.size() != 1) return false;
    return m.submeshes[0].bone_assignments[0].bone == 2 &&
           m.submeshes[0].bone_assignments[0].weight == 0.25f;
}

}  // namespace

TL_TEST(ogre_skeleton_lists_its_animations_in_either_byte_order) {
    for (const bool big : {false, true}) {
        const std::vector<uint8_t> bytes = made_up(big);
        const OgreSkeleton s = parse_ogre_skeleton(bytes.data(), bytes.size());
        TL_CHECK(s.ok);
        TL_CHECK(s.big_endian == big);
        TL_CHECK_EQ(s.version, std::string("[Serializer_v1.10]"));
        TL_CHECK_EQ(s.animations.size(), size_t{2});
        if (s.animations.size() != 2) continue;
        TL_CHECK_EQ(s.animations[0].name, std::string("Die"));
        TL_CHECK(s.animations[0].length == 1.5f);
        TL_CHECK_EQ(s.animations[1].name, std::string("Death_Land"));
        TL_CHECK(s.animations[1].length == 0.75f);
    }
}

TL_TEST(ogre_skeleton_reads_bones_and_tracks_in_either_byte_order) {
    const std::vector<uint8_t> le = made_up(false);
    const std::vector<uint8_t> be = made_up(true);
    TL_CHECK(skeleton_bones_and_tracks_match(parse_ogre_skeleton(le.data(), le.size())));
    TL_CHECK(skeleton_bones_and_tracks_match(parse_ogre_skeleton(be.data(), be.size())));
}

TL_TEST(ogre_skeleton_refuses_what_is_not_one) {
    const uint8_t not_one[] = {0x34, 0x12, 'x', '\n'};
    TL_CHECK(!parse_ogre_skeleton(not_one, sizeof not_one).ok);
    std::vector<uint8_t> cut = made_up(false);
    cut.resize(cut.size() - 3);
    TL_CHECK(!parse_ogre_skeleton(cut.data(), cut.size()).ok);
}

TL_TEST(ogre_mesh_reads_normals_and_bone_assignments_in_either_byte_order) {
    const std::vector<uint8_t> le = made_up_mesh(false);
    const std::vector<uint8_t> be = made_up_mesh(true);
    TL_CHECK(mesh_normals_and_weights_match(parse_ogre_mesh(le.data(), le.size()), false));
    TL_CHECK(mesh_normals_and_weights_match(parse_ogre_mesh(be.data(), be.size()), true));
}
