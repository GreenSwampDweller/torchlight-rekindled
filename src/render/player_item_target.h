// The player's target item, as the pad's A or the pointer's click sets it.
#pragma once

#include "core/world.h"

#include <OgreVector.h>

#include <cstdint>
#include <string>

namespace Ogre {
class SceneNode;
}

namespace tl::render {

/// The target item (`CCharacter::setTargetItem`, `+0x350`, AI state 7): what `getItemAI`
/// walks to and uses (`step_item_ai`).
struct PlayerItemTarget {
    /// `Mimic`: a sleeping mimic as the target (`+0x340`) for `attackAI`.
    enum class Kind { None, Dropped, Trigger, Portal, Mimic };
    Kind kind = Kind::None;
    Ogre::SceneNode* dropped = nullptr;         ///< the floor item's node
    int machine = tl::core::World::kNoMachine;  ///< a `Unit Trigger`'s (or mimic's) machine
    int64_t node = 0;                           ///< and node
    std::string portal;                         ///< an open portal's unit
    Ogre::Vector3 portal_at = Ogre::Vector3::ZERO;
};

}  // namespace tl::render
