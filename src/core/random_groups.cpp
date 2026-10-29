#include "core/random_groups.h"

#include "core/randomizer.h"

#include <algorithm>
#include <map>

namespace tl::core {

// `CRandomGroup::chooseChildrenByRandomChoice`.
// Weight (1): positive weights into a randomizer, the rest kept; `NUMBER` draws without
// replacement, at least one even with nothing to draw from, which keeps child 0 again.
// Random chance (2): weight 1..99 kept when `randomBetween(1, 100)` <= weight,
// others without a roll; stops once `NUMBER` are kept.
std::vector<size_t> choose_children_by_random_choice(const std::vector<LayoutGroup>& groups,
                                                     size_t group,
                                                     const std::vector<size_t>& children,
                                                     RunicRandom& rng) {
    std::vector<size_t> chosen;
    const LayoutGroup& self = groups[group];
    if (children.empty() || !self.can_be_created) return chosen;

    const uint32_t type = self.random_type;
    if (type == 0) return children;

    Randomizer weights(RandomizerType::kNormal);
    uint32_t remaining = self.number_of_picks;
    for (size_t i = 0; i < children.size(); ++i) {
        const int32_t weight = static_cast<int32_t>(groups[children[i]].random_weight);
        if (type == 1) {
            if (0 < weight) {
                weights.add_choice(static_cast<int32_t>(i), weight);
            } else {
                chosen.push_back(children[i]);
            }
            continue;
        }
        if (static_cast<uint32_t>(weight - 1) < 99u) {
            const float roll = rng.between(1.0f, 100.0f);
            if (static_cast<float>(weight) < roll) continue;
            chosen.push_back(children[i]);
            if (--remaining == 0) return chosen;
        } else {
            chosen.push_back(children[i]);
        }
    }

    if (type == 1 && remaining != 0) {
        for (uint32_t pick = 0;;) {
            const uint32_t index = static_cast<uint32_t>(weights.get_random(rng));
            chosen.push_back(tarray_at(children, index));
            weights.remove_choice(index);
            if (weights.choice_count() == 0) break;
            if (++pick >= remaining) break;
        }
    }
    return chosen;
}

namespace {

// `rollGroup(CRandomGroup*, TArrayList<CEditorBaseObject*>&, TArrayList<CRandomGroup*>&)`:
// the group's direct children among the
// loaded groups, in list order; every one not chosen is removed.
void roll_group(const std::vector<LayoutGroup>& groups, size_t group, std::vector<size_t>& removed,
                RunicRandom& rng) {
    std::vector<size_t> children;
    for (size_t i = 0; i < groups.size(); ++i) {
        if (groups[i].parent_id == groups[group].id) children.push_back(i);
    }
    const std::vector<size_t> chosen =
        choose_children_by_random_choice(groups, group, children, rng);
    for (size_t child : children) {
        if (std::find(chosen.begin(), chosen.end(), child) == chosen.end())
            removed.push_back(child);
    }
}

}  // namespace

// `rollRandomGroups`: groups in load order; one removed
// by an ancestor's roll is skipped, one under a removed ancestor still rolls (and draws).
// Pruning then removes every descendant of a removed group (`CEditorBaseObject::
// isChildOfObject`).
std::set<int64_t> roll_random_groups(const std::vector<LayoutGroup>& groups, RunicRandom& rng,
                                     std::vector<int64_t>* created) {
    std::vector<size_t> removed;
    for (size_t g = 0; g < groups.size(); ++g) {
        if (std::find(removed.begin(), removed.end(), g) != removed.end()) continue;
        if (!groups[g].can_be_created) {
            removed.push_back(g);
        } else {
            roll_group(groups, g, removed, rng);
        }
    }

    std::map<int64_t, int64_t> parent_of;
    for (const LayoutGroup& g : groups) parent_of.emplace(g.id, g.parent_id);
    if (created != nullptr) {
        // The list it hands back: every group in load order, then each removed one and every group
        // under it (`CEditorBaseObject::isChildOfObject`) taken out as found - `TArrayList`'s removal
        // puts the last entry in its place - the scan starting again at the second entry and the
        // removed list at its first after each. Groups are made in this order.
        const auto under = [&](size_t g, size_t ancestor) {
            int64_t at = groups[g].parent_id;
            for (size_t guard = 0; at != -1 && guard < groups.size(); ++guard) {
                if (at == groups[ancestor].id) return true;
                const auto it = parent_of.find(at);
                if (it == parent_of.end()) break;
                at = it->second;
            }
            return false;
        };
        std::vector<size_t> list(groups.size());
        for (size_t g = 0; g < groups.size(); ++g) list[g] = g;
        std::vector<size_t> pruned = removed;
        for (size_t r = 0; r < pruned.size(); ++r) {
            for (size_t i = 0; i < list.size(); ++i) {
                if (list[i] != pruned[r] && !under(list[i], pruned[r])) continue;
                if (std::find(pruned.begin(), pruned.end(), list[i]) == pruned.end())
                    pruned.push_back(list[i]);
                list[i] = list.back();
                list.pop_back();
                i = 0;
                r = 0;
            }
        }
        created->clear();
        for (size_t g : list) created->push_back(groups[g].id);
    }
    std::set<int64_t> out;
    for (size_t r : removed) out.insert(groups[r].id);
    for (const LayoutGroup& g : groups) {
        int64_t at = g.parent_id;
        for (size_t guard = 0; at != -1 && guard < groups.size(); ++guard) {
            if (out.count(at) != 0) {
                out.insert(g.id);
                break;
            }
            const auto it = parent_of.find(at);
            if (it == parent_of.end()) break;
            at = it->second;
        }
    }
    return out;
}

}  // namespace tl::core
