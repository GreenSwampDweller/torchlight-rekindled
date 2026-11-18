// PORT: what the pose pass keeps between its calls (renderer_poses.cpp).
#pragma once

#include <memory>
#include <vector>

namespace Ogre {
class Entity;
}

namespace tl::render {

struct PosePass {
    std::vector<Ogre::Entity*> jobs;
    /// Since the last timing window: the entities posed, and the game thread's microseconds
    /// handing them out and joining the workers.
    unsigned long long posed = 0;
    unsigned long long prep_us = 0;
    unsigned long long join_us = 0;
    /// The listener that joins the workers before the scene's graph is brought up to date.
    std::shared_ptr<void> joiner;

    void reset() { posed = prep_us = join_us = 0; }
};

}  // namespace tl::render
