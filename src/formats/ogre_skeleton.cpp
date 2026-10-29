// OGRE's binary skeleton, the animations' names and lengths (formats/ogre_skeleton.h). The chunk ids are OGRE's
// (`OgreSkeletonFileFormat.h`) and so is the walk, `SkeletonSerializer::importSkeleton`: chunks in sequence to the
// end of the file, each past its length as OGRE's own reader takes it - a bone's length without its name (a scale is
// read only when the chunk is long enough for one), every other chunk's whole.
#include "formats/ogre_skeleton.h"

#include <cstring>

namespace tl::formats {

namespace {

constexpr uint16_t kHeader = 0x1000;
constexpr uint16_t kBone = 0x2000;
constexpr uint16_t kAnimation = 0x4000;
constexpr size_t kChunkHeader = 6;  // uint16 id, uint32 length

uint16_t u16_at(const uint8_t* b, bool swap) {
    return swap ? static_cast<uint16_t>((b[0] << 8) | b[1])
                : static_cast<uint16_t>(b[0] | (b[1] << 8));
}

uint32_t u32_at(const uint8_t* b, bool swap) {
    if (swap)
        return (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) |
               (static_cast<uint32_t>(b[2]) << 8) | b[3];
    return b[0] | (static_cast<uint32_t>(b[1]) << 8) | (static_cast<uint32_t>(b[2]) << 16) |
           (static_cast<uint32_t>(b[3]) << 24);
}

/// A string to its '\n' (`Serializer::readString`), from `at`; `at` left past it. False when it runs off `end`.
bool line(const uint8_t* data, size_t end, size_t& at, std::string& out) {
    out.clear();
    while (at < end && data[at] != '\n') out.push_back(static_cast<char>(data[at++]));
    if (at >= end) return false;
    ++at;
    return true;
}

}  // namespace

OgreSkeleton parse_ogre_skeleton(const uint8_t* data, size_t size) {
    OgreSkeleton skeleton;
    if (size < 2) {
        skeleton.error = "too short";
        return skeleton;
    }
    // `Serializer::determineEndianness`: the header id as written tells the byte order.
    const uint16_t first = static_cast<uint16_t>(data[0] | (data[1] << 8));
    bool swap = false;
    if (first == 0x0010) {
        swap = true;
        skeleton.big_endian = true;
    } else if (first != kHeader) {
        skeleton.error = "not an OGRE skeleton";
        return skeleton;
    }
    size_t at = 2;
    if (!line(data, size, at, skeleton.version)) {
        skeleton.error = "the header ends early";
        return skeleton;
    }
    while (at + kChunkHeader <= size) {
        const uint16_t id = u16_at(data + at, swap);
        const uint32_t length = u32_at(data + at + 2, swap);
        if (length < kChunkHeader || length > size - at) {
            skeleton.error = "a chunk runs past the file";
            return skeleton;
        }
        size_t end = at + length;
        if (id == kBone) {
            // `calcBoneSize` leaves the bone's name out of its length: `readBone` reads the name, then the rest.
            size_t p = at + kChunkHeader;
            std::string bone;
            if (!line(data, size, p, bone) || length - kChunkHeader > size - p) {
                skeleton.error = "a bone runs past the file";
                return skeleton;
            }
            end = p + (length - kChunkHeader);
        } else if (id == kAnimation) {
            // `readAnimation`: the name, then the length, then its tracks.
            size_t p = at + kChunkHeader;
            OgreSkeletonAnimation animation;
            if (!line(data, end, p, animation.name) || p + 4 > end) {
                skeleton.error = "an animation ends early";
                return skeleton;
            }
            const uint32_t bits = u32_at(data + p, swap);
            std::memcpy(&animation.length, &bits, 4);
            skeleton.animations.push_back(std::move(animation));
        }
        at = end;
    }
    skeleton.ok = true;
    return skeleton;
}

}  // namespace tl::formats
