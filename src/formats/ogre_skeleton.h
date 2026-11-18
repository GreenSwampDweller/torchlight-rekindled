// OGRE's binary skeleton (`.skeleton`, either byte order): its bones, hierarchy, and animations with
// their bone tracks and keyframes. Byte span in, struct out.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tl::formats {

struct OgreSkeletonBone {
    std::string name;
    uint16_t handle = 0;
    int32_t parent = -1;
    float position[3] = {0.0f, 0.0f, 0.0f};
    /// Quaternion in (x, y, z, w) order, as stored in OGRE's binary stream.
    float orientation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float scale[3] = {1.0f, 1.0f, 1.0f};
};

struct OgreSkeletonKeyframe {
    float time = 0.0f;
    /// Quaternion in (x, y, z, w) order.
    float orientation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float translate[3] = {0.0f, 0.0f, 0.0f};
    float scale[3] = {1.0f, 1.0f, 1.0f};
};

struct OgreSkeletonTrack {
    uint16_t bone = 0;
    std::vector<OgreSkeletonKeyframe> keyframes;
};

struct OgreSkeletonAnimation {
    std::string name;
    float length = 0.0f;
    std::vector<OgreSkeletonTrack> tracks;
};

struct OgreSkeleton {
    bool ok = false;
    std::string error;
    std::string version;  ///< `[Serializer_v1.10]`
    bool big_endian = false;
    std::vector<OgreSkeletonBone> bones;
    std::vector<OgreSkeletonAnimation> animations;
};

OgreSkeleton parse_ogre_skeleton(const uint8_t* data, size_t size);

}  // namespace tl::formats
