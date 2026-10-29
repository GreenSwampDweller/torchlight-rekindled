// Random groups inside a layout: which of a `Group`'s child groups a loaded layout keeps.
// `CEditorScene::loadCompressedLayout` rolls every
// `Group` first, with `rollRandomGroups`; a removed
// group's subtree is never loaded. The generator is `g_Rand` as the load finds it (a room
// piece reseeds it in `CLayout::loadLayoutFile`).
#pragma once

#include "core/random.h"

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace tl::core {

/// `CRandomGroup` (size 0x198) as rolling reads it, with the defaults
/// `CRandomGroup::CRandomGroup` gives an unauthored
/// group (`DIFFICULTY MODE` -1 is no gate).
struct LayoutGroup {
    int64_t id = 0;
    int64_t parent_id = -1;
    /// `CRandomGroup::getGroupCanBeCreated`:
    /// the class, quest, dungeon and difficulty gates, evaluated by the caller.
    bool can_be_created = true;
    uint32_t random_type = 0;      ///< +0x11c `CHOICE`: 0 all, 1 weight, 2 random chance
    uint32_t number_of_picks = 1;  ///< +0x124 `NUMBER`
    /// +0x120 `RANDOMIZATION`, its weight or chance inside its parent
    uint32_t random_weight = 1;
};

/// `CRandomGroup::chooseChildrenByRandomChoice`.
/// `children` index `groups`; the result holds the kept ones, possibly with a repeat.
std::vector<size_t> choose_children_by_random_choice(const std::vector<LayoutGroup>& groups,
                                                     size_t group,
                                                     const std::vector<size_t>& children,
                                                     RunicRandom& rng);

/// `rollRandomGroups` with `rollGroup`:
/// the ids of every group the layout does not create, descendants
/// included. `groups` is in load (file) order. `created`, when given, takes the ids of the groups
/// made, in the order the load makes their objects.
std::set<int64_t> roll_random_groups(const std::vector<LayoutGroup>& groups, RunicRandom& rng,
                                     std::vector<int64_t>* created = nullptr);

}  // namespace tl::core
