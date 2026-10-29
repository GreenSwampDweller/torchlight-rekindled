// OGRE's binary skeleton (`.skeleton`, either byte order): its animations' names and lengths, in the file's order.
// What a model's animation list reads of a skeleton; nothing of bones or tracks. Byte span in, struct out.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tl::formats {

struct OgreSkeletonAnimation {
    std::string name;
    float length = 0.0f;
};

struct OgreSkeleton {
    bool ok = false;
    std::string error;
    std::string version;  ///< `[Serializer_v1.10]`
    bool big_endian = false;
    std::vector<OgreSkeletonAnimation> animations;
};

OgreSkeleton parse_ogre_skeleton(const uint8_t* data, size_t size);

}  // namespace tl::formats
