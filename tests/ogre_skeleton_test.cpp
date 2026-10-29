#include "formats/ogre_skeleton.h"
#include "test_support.h"

#include <cstring>
#include <string>
#include <vector>

using tl::formats::OgreSkeleton;
using tl::formats::parse_ogre_skeleton;

namespace {

// A made-up skeleton in OGRE's chunk layout, little- or big-endian.
struct Writer {
    bool big = false;
    std::vector<uint8_t> bytes;
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
    // A bone: its length, as `calcBoneSize` writes it, leaves the name out.
    w.u16(0x2000);
    w.u32(6 + 2 + 7 * 4);
    w.line("root");
    w.u16(0);
    for (int i = 0; i < 7; ++i) w.f32(0.0f);
    // A bone parent.
    w.u16(0x3000);
    w.u32(6 + 4);
    w.u16(1);
    w.u16(0);
    // Two animations, the first with one empty track.
    const std::string names[] = {"Die", "Death_Land"};
    const float lengths[] = {1.5f, 0.75f};
    for (int a = 0; a < 2; ++a) {
        const bool track = a == 0;
        w.u16(0x4000);
        w.u32(static_cast<uint32_t>(6 + names[a].size() + 1 + 4 + (track ? 6 + 2 : 0)));
        w.line(names[a]);
        w.f32(lengths[a]);
        if (track) {
            w.u16(0x4100);
            w.u32(6 + 2);
            w.u16(0);
        }
    }
    return w.bytes;
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

TL_TEST(ogre_skeleton_refuses_what_is_not_one) {
    const uint8_t not_one[] = {0x34, 0x12, 'x', '\n'};
    TL_CHECK(!parse_ogre_skeleton(not_one, sizeof not_one).ok);
    std::vector<uint8_t> cut = made_up(false);
    cut.resize(cut.size() - 3);
    TL_CHECK(!parse_ogre_skeleton(cut.data(), cut.size()).ok);
}
