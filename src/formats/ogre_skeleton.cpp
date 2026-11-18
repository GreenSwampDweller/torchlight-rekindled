// OGRE's binary skeleton: bones, hierarchy, animations, tracks, and keyframes (formats/ogre_skeleton.h).
// The chunk ids are OGRE's (`OgreSkeletonFileFormat.h`) and so is the walk, `SkeletonSerializer::importSkeleton`:
// chunks in sequence to the end of the file, each past its length as OGRE's own reader takes it - a bone's length
// without its name (a scale is read only when the chunk is long enough for one), every other chunk's whole.
#include "formats/ogre_skeleton.h"

#include <cstring>

namespace tl::formats {

namespace {

constexpr uint16_t kHeader = 0x1000;
constexpr uint16_t kBone = 0x2000;
constexpr uint16_t kBoneParent = 0x3000;
constexpr uint16_t kAnimation = 0x4000;
constexpr uint16_t kAnimationBaseInfo = 0x4010;
constexpr uint16_t kAnimationTrack = 0x4100;
constexpr uint16_t kAnimationTrackKeyframe = 0x4110;
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

float f32_at(const uint8_t* b, bool swap) {
    const uint32_t bits = u32_at(b, swap);
    float f = 0.0f;
    std::memcpy(&f, &bits, 4);
    return f;
}

/// A string to its '\n' (`Serializer::readString`), from `at`; `at` left past it. False when it runs off `end`.
bool line(const uint8_t* data, size_t end, size_t& at, std::string& out) {
    out.clear();
    while (at < end && data[at] != '\n') out.push_back(static_cast<char>(data[at++]));
    if (at >= end) return false;
    ++at;
    return true;
}

bool read_keyframe(const uint8_t* data, size_t end, bool swap, size_t& p,
                   OgreSkeletonTrack& track) {
    const uint32_t kf_len = u32_at(data + p + 2, swap);
    if (kf_len < kChunkHeader + 32 || kf_len > end - p) return false;
    const uint8_t* kb = data + p + kChunkHeader;
    OgreSkeletonKeyframe kf;
    kf.time = f32_at(kb, swap);
    for (int i = 0; i < 4; ++i) kf.orientation[i] = f32_at(kb + 4 + i * 4, swap);
    for (int i = 0; i < 3; ++i) kf.translate[i] = f32_at(kb + 20 + i * 4, swap);
    if (kf_len >= kChunkHeader + 44) {
        for (int i = 0; i < 3; ++i) kf.scale[i] = f32_at(kb + 32 + i * 4, swap);
    }
    track.keyframes.push_back(kf);
    p += kf_len;
    return true;
}

bool read_track(const uint8_t* data, size_t end, bool swap, size_t& p,
                OgreSkeletonAnimation& animation) {
    if (end - p < kChunkHeader + 2) return false;
    OgreSkeletonTrack track;
    track.bone = u16_at(data + p + kChunkHeader, swap);
    p += kChunkHeader + 2;
    while (end - p >= kChunkHeader && u16_at(data + p, swap) == kAnimationTrackKeyframe) {
        if (!read_keyframe(data, end, swap, p, track)) return false;
    }
    animation.tracks.push_back(std::move(track));
    return true;
}

bool read_animation_tracks(const uint8_t* data, size_t end, bool swap, size_t& p,
                           OgreSkeletonAnimation& animation) {
    while (p < end) {
        if (end - p < kChunkHeader) return false;
        const uint16_t sub_id = u16_at(data + p, swap);
        if (sub_id == kAnimationBaseInfo) {
            p += kChunkHeader;
            std::string base_name;
            if (!line(data, end, p, base_name) || end - p < 4) return false;
            p += 4;
        } else if (sub_id == kAnimationTrack) {
            if (!read_track(data, end, swap, p, animation)) return false;
        } else {
            return false;
        }
    }
    return true;
}

bool read_bone(const uint8_t* data, size_t size, bool swap, size_t at, uint32_t length, size_t& end,
               OgreSkeleton& skeleton) {
    // `calcBoneSize` leaves the bone's name out of its length: `readBone` reads the name, then the rest.
    size_t p = at + kChunkHeader;
    OgreSkeletonBone bone;
    const size_t body = length - kChunkHeader;
    if (!line(data, size, p, bone.name) || body < 30 || body > size - p) {
        skeleton.error = "a bone runs past the file";
        return false;
    }
    bone.handle = u16_at(data + p, swap);
    for (int i = 0; i < 3; ++i) bone.position[i] = f32_at(data + p + 2 + i * 4, swap);
    for (int i = 0; i < 4; ++i) bone.orientation[i] = f32_at(data + p + 14 + i * 4, swap);
    if (body >= 42) {
        for (int i = 0; i < 3; ++i) bone.scale[i] = f32_at(data + p + 30 + i * 4, swap);
    }
    skeleton.bones.push_back(std::move(bone));
    end = p + body;
    return true;
}

bool read_bone_parent(const uint8_t* data, bool swap, size_t at, uint32_t length,
                      OgreSkeleton& skeleton) {
    if (length < kChunkHeader + 4) {
        skeleton.error = "a bone parent ends early";
        return false;
    }
    const uint16_t child = u16_at(data + at + kChunkHeader, swap);
    const uint16_t parent = u16_at(data + at + kChunkHeader + 2, swap);
    for (OgreSkeletonBone& bone : skeleton.bones) {
        if (bone.handle == child) bone.parent = static_cast<int32_t>(parent);
    }
    return true;
}

bool read_animation(const uint8_t* data, bool swap, size_t at, size_t end, OgreSkeleton& skeleton) {
    size_t p = at + kChunkHeader;
    OgreSkeletonAnimation animation;
    if (!line(data, end, p, animation.name) || p + 4 > end) {
        skeleton.error = "an animation ends early";
        return false;
    }
    animation.length = f32_at(data + p, swap);
    p += 4;
    if (!read_animation_tracks(data, end, swap, p, animation)) {
        skeleton.error = "an animation track ends early";
        return false;
    }
    skeleton.animations.push_back(std::move(animation));
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
        if (id == kBone && !read_bone(data, size, swap, at, length, end, skeleton)) return skeleton;
        if (id == kBoneParent && !read_bone_parent(data, swap, at, length, skeleton))
            return skeleton;
        if (id == kAnimation && !read_animation(data, swap, at, end, skeleton)) return skeleton;
        at = end;
    }
    skeleton.ok = true;
    return skeleton;
}

}  // namespace tl::formats
