// The mouse layer: the skill foldout over the PC's HUD.
#include "render/mouse_game.h"

#include <algorithm>
#include <cmath>

namespace tl::render {

// CGameUI::handle_SkillClick, the plain left click on a slot: the
// foldout opens from that window, or closes when it was opened from it. `target` is a
// numbered slot from 0, or `kFoldoutRight`.
// CSkillFoldout::showFoldout: the skills with a level that are enabled,
// not hidden and not passive, a row for each fifth of `LEVEL_REQUIRED` below the tenth
// that has any, ten to a row.
// A numbered slot's also lists the bag's consumables that are not spells, one of each
// kind, in the rows after the skills.
// INVENTED: the PC has a foldout for the left button too; it is not here.
void Renderer::Impl::MouseGame::toggle_foldout(int target, const std::string& window) {
    if (foldout.shown() && foldout_for == target) return foldout.hide();
    foldout_for = target;
    foldout_window = window;
    foldout_skills.clear();
    foldout_items.clear();
    std::vector<PcFoldout::Entry> entries;
    int row = 0;
    for (int tier = 0; tier < 10; ++tier) {
        int column = 0;
        for (const tl::core::KnownSkill& known : g.player_skills.skills()) {
            if (known.def == nullptr || column >= 10) continue;
            const int its = std::min(
                10, static_cast<int>(
                        std::floor(static_cast<float>(known.entry.level_required) / 5.0f)));
            const bool castable =
                known.effective != 0 && known.enabled && !known.def->hidden && !known.passive();
            if (its != tier || !castable) continue;
            entries.push_back(PcFoldout::Entry{known.def->icon, column++, row});
            foldout_skills.push_back(known.guid());
            foldout_items.push_back(false);
        }
        if (column != 0) ++row;
    }
    if (target != kFoldoutRight) list_foldout_items(entries, row);
    PcFoldout::Place place;
    float wide = 0.0f, tall = 0.0f;
    pc_hud.screen().window_box(window, place.x, place.y, wide, tall);
    place.width = g.config.width;
    place.height = g.config.height;
    foldout.show(entries, place, pc_hud.screen());
    TL_LOG_INFO("mouse: the skill foldout opens from %s with %zu skill(s)", window.c_str(),
                entries.size());
}

/// The bag's consumables that are not spells, one cell for each kind (its unit's GUID),
/// ten to a row from `row` on. An item's icon is drawn 48 tall in its own proportion.
void Renderer::Impl::MouseGame::list_foldout_items(std::vector<PcFoldout::Entry>& entries,
                                                   int row) {
    // INVENTED: two wide for three tall, the proportion of the layouts' item slots (32 by
    // 48); the PC measures each picture.
    constexpr float kItemAspect = 32.0f / 48.0f;
    g.load_unit_types();
    int column = 0;
    for (const tl::core::InventorySlot& held : g.player_inventory.slots()) {
        if (held.count <= 0 || row >= 10) continue;
        const uint32_t type = g.unit_types.id_of(held.item.unittype);
        if (!g.unit_types.isa(type, tl::core::unit_type::kConsumable) ||
            g.unit_types.isa(type, tl::core::unit_type::kSpell)) {
            continue;
        }
        bool listed = false;
        for (size_t cell = 0; cell < foldout_skills.size(); ++cell) {
            listed = listed || (foldout_items[cell] && foldout_skills[cell] == held.item.unit_guid);
        }
        if (listed) continue;
        entries.push_back(PcFoldout::Entry{held.item.icon, column, row, kItemAspect});
        foldout_skills.push_back(held.item.unit_guid);
        foldout_items.push_back(true);
        if (++column == 10) {
            column = 0;
            ++row;
        }
    }
}

/// A press while the foldout is up. On one of its skills: that skill onto the slot or the
/// button it was opened from, and the foldout down. Anywhere else outside it: down. True
/// when the press was the foldout's.
bool Renderer::Impl::MouseGame::click_foldout(const tl::core::MouseButtons& buttons) {
    if (!foldout.shown() || !(buttons.left_pressed || buttons.right_pressed)) return false;
    const int index = foldout.point(input.pointer_x, input.pointer_y);
    if (index < 0) {
        if (foldout.holds(input.pointer_x, input.pointer_y)) return true;
        // The window it was opened from closes it itself.
        if (over_hud != foldout_window) foldout.hide();
        return false;
    }
    const std::int64_t guid = foldout_skills[static_cast<size_t>(index)];
    const bool item = foldout_items[static_cast<size_t>(index)];
    if (foldout_for == kFoldoutRight) {
        active_skill = guid;
    } else {
        const auto at = static_cast<size_t>(foldout_for);
        std::vector<int64_t> skills(g.player_skills.slots().begin(), g.player_skills.slots().end());
        std::vector<int64_t> held(g.player_skills.second_slots().begin(),
                                  g.player_skills.second_slots().end());
        // A slot holds a skill or an item, never both.
        skills[at] = item ? -1 : guid;
        held[at] = item ? guid : -1;
        g.player_skills.set_slots(skills, held);
        g.refresh_game_hud();
    }
    g.play_ui(tl::core::InterfaceSound::SkillAssign);
    const tl::core::KnownSkill* chosen = item ? nullptr : skill(guid);
    TL_LOG_INFO("mouse: %s '%s' onto %s", item ? "an item" : "the skill",
                chosen != nullptr ? chosen->entry.name.c_str() : "", foldout_window.c_str());
    foldout.hide();
    return true;
}

}  // namespace tl::render
