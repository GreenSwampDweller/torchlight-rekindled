// The console's hotbar in play.
#include "core/hotbar.h"

#include "core/item_use.h"

#include <cmath>

namespace tl::core {

// PORT: the HUD's button for a hotbar slot, the inverse of `skill_button_slot` over
// the current set (`hud_xenon.layout`).
HotbarButton hotbar_slot_button(int slot, int skill_set) {
    if (slot == 0) return HotbarButton::LB;
    if (slot == 1) return HotbarButton::RB;
    for (SkillButton button : {SkillButton::Y, SkillButton::B, SkillButton::LT, SkillButton::RT}) {
        if (skill_button_slot(button, skill_set) != slot) continue;
        switch (button) {
            case SkillButton::Y: return HotbarButton::Y;
            case SkillButton::B: return HotbarButton::B;
            case SkillButton::LT: return HotbarButton::LT;
            case SkillButton::RT: return HotbarButton::RT;
        }
    }
    return HotbarButton::None;
}

HotbarPress hotbar_press(const std::array<int64_t, kSkillFunctionSlots>& skill_slots,
                         const std::array<int64_t, kSkillFunctionSlots>& item_slots,
                         const std::function<bool(int64_t)>& skill_known, int skill_set,
                         const std::function<bool(HotbarButton)>& pressed,
                         const std::function<bool(HotbarButton)>& held) {
    // The Xbox in-game dispatch, its second walk of the ten slots.
    HotbarPress press;
    for (int slot = 0; slot < kSkillFunctionSlots; ++slot) {
        const HotbarButton button = hotbar_slot_button(slot, skill_set);
        const bool down = button != HotbarButton::None && pressed && pressed(button);
        const bool still = button != HotbarButton::None && held && held(button);
        const int64_t skill = skill_slots[static_cast<size_t>(slot)];
        if (skill != -1 && skill_known && skill_known(skill)) {
            // `+0x449` or `+0x249`, then the skill-use call with the third
            // argument `+0x249 == 0`.
            if (down || still) {
                press.slot = slot;
                press.skill = true;
                press.continuing = !down;
                return press;
            }
            // A release on a skill slot drops the pending targets (`+0x649`);
            // nothing in this port holds them.
            continue;
        }
        if (item_slots[static_cast<size_t>(slot)] != -1 && down) {
            press.slot = slot;
            press.skill = false;
            return press;
        }
    }
    return press;
}

bool hotbar_lets_go(const std::array<int64_t, kSkillFunctionSlots>& skill_slots,
                    int64_t active_skill, int skill_set,
                    const std::function<bool(HotbarButton)>& pressed,
                    const std::function<bool(HotbarButton)>& held) {
    // The Xbox in-game dispatch, its first walk of the ten slots
    // (`CPlayer +0x840`), then when the flag survives.
    for (int slot = 0; slot < kSkillFunctionSlots; ++slot) {
        const int64_t skill = skill_slots[static_cast<size_t>(slot)];
        if (skill == -1) continue;
        const HotbarButton button = hotbar_slot_button(slot, skill_set);
        const bool down = button != HotbarButton::None &&
                          ((held && held(button)) || (pressed && pressed(button)));
        // `+0x449` or `+0x249`: this slot decides, and the walk ends.
        if (down) return skill != active_skill;
    }
    return true;
}

HotbarSlotView skill_slot_view(const std::string& icon, float cooling_time) {
    // `CGameUI::updateSlots`, the skill
    // branch: `(int)ceilf(...)`.
    HotbarSlotView view;
    view.icon = icon;
    const int seconds = static_cast<int>(std::ceil(cooling_time));
    if (seconds < 1) return view;
    view.dimmed = true;
    view.text = std::to_string(seconds);
    return view;
}

HotbarSlotView item_slot_view(const std::string& icon, int count) {
    // `CGameUI::updateSlots`, the item
    // branch.
    HotbarSlotView view;
    if (icon.empty()) return view;
    view.icon = icon;
    if (count == 0) {
        view.dimmed = true;
        return view;
    }
    view.text = std::to_string(count);
    return view;
}

bool same_potion_kind(const ItemStats& bound, const ItemStats& item) {
    if (item.unit_guid == bound.unit_guid) return true;
    if (bound.effects.empty() || item.effects.empty()) return false;
    return item.unittype == bound.unittype && item.effects[0].type == bound.effects[0].type;
}

int potion_slot_for(const Inventory& inventory, const ItemStats& bound) {
    const int own = equipment_of_guid(inventory, bound.unit_guid);
    if (own >= 0) return own;
    int best = -1;
    const std::vector<InventorySlot>& slots = inventory.slots();
    for (size_t i = 0; i < slots.size(); ++i) {
        const InventorySlot& slot = slots[i];
        if (slot.count <= 0 || !same_potion_kind(bound, slot.item)) continue;
        if (best < 0 || slot.item.level < slots[static_cast<size_t>(best)].item.level) {
            best = static_cast<int>(i);
        }
    }
    // The smallest stack of that unit, as the original takes one.
    return best < 0 ? -1
                    : equipment_of_guid(inventory, slots[static_cast<size_t>(best)].item.unit_guid);
}

int potion_count_for(const Inventory& inventory, const ItemStats& bound) {
    int count = 0;
    for (const InventorySlot& slot : inventory.slots()) {
        if (slot.count <= 0 || !same_potion_kind(bound, slot.item)) continue;
        count += slot.item.max_stack < 1 ? 1 : slot.count;
    }
    return count;
}

int equipment_count_of_guid(const Inventory& inventory, int64_t guid) {
    // `CInventory::getEquipmentCountOfGuid`.
    // The original walks its item list and the port its slots; a sum does not
    // depend on the order.
    int count = 0;
    for (const InventorySlot& slot : inventory.slots()) {
        if (slot.count <= 0 || slot.item.unit_guid != guid) continue;
        count += slot.item.max_stack < 1 ? 1 : slot.count;
    }
    return count;
}

}  // namespace tl::core
