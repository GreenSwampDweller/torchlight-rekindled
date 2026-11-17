// The console's hotbar in play: which of the ten function slots a pad press
// fires, and what the HUD draws in each slot (`CPlayer +0x8b0`/`+0x900`, Xbox
// `+0x840`/`+0x890`):
//
// - the press, from the Xbox in-game dispatch: the first slot, in
//   order, whose skill is known and whose button is pressed or held hands its
//   skill to the skill-use call (ending in `CCharacter::castSkill`);
//   an item slot is used on the press alone;
// - the picture, from `CGameUI::updateSlots`:
//   a skill slot's icon and whole seconds of cooldown, greyed while cooling;
//   an item slot's icon and count.
//
// No OGRE and no input device: the renderer hands in what is pressed.
#pragma once

#include "core/inventory.h"
#include "core/skills.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string>

namespace tl::core {

/// The six buttons the hotbar answers to on the console.
enum class HotbarButton { None, LB, RB, Y, B, LT, RT };

/// Which button fires slot `slot` in skill set `skill_set`.
///
/// The dispatch indexes a ten-entry table through the pad's
/// binding set (swaps it with the skill set); the table is filled at
/// start-up by code not yet found. This is the inverse of the map handler that
/// writes the slots (`CSkillMenuXenon`, `skill_button_slot`) over the
/// current set, with LB and RB on slots 0 and 1 in both. A slot of the other
/// set has no button.
HotbarButton hotbar_slot_button(int slot, int skill_set);

/// What the dispatch picked this frame.
struct HotbarPress {
    int slot = -1;       ///< -1: nothing
    bool skill = false;  ///< the skill half (`+0x840`); else the item half (`+0x890`)
    /// The skill-use call's third argument: the button was already down last
    /// frame (`+0x249` clear), so this is a hold rather than a fresh press.
    bool continuing = false;
};

/// The slot loop of the Xbox in-game dispatch:
/// slots 0 to 9 in order; a slot whose skill GUID is set and known fires while
/// its button is pressed (`+0x249`) or held (`+0x449`); otherwise a slot whose
/// item GUID is set fires on the press alone. The first that fires ends the
/// loop.
HotbarPress hotbar_press(const std::array<int64_t, kSkillFunctionSlots>& skill_slots,
                         const std::array<int64_t, kSkillFunctionSlots>& item_slots,
                         const std::function<bool(int64_t)>& skill_known, int skill_set,
                         const std::function<bool(HotbarButton)>& pressed,
                         const std::function<bool(HotbarButton)>& held);

/// The release test of the same dispatch (its first walk of the
/// ten slots, run when neither face button was pressed this frame and the
/// player is performing a skill): true when the button that keeps the active
/// skill going is no longer down, and the dispatch calls
/// `CPlayer::attemptToStopPlayerSkill`.
///
/// The first slot in order whose skill GUID is set and whose button is
/// pressed or held decides: it lets go unless that slot's skill is the active
/// one (`getSkillByGuid(slot) != +0x2c4`). With no such slot it lets go. The
/// PC's mouse path (`CGameClient::processIngameInput`)
/// is the same test over its ten keys.
bool hotbar_lets_go(const std::array<int64_t, kSkillFunctionSlots>& skill_slots,
                    int64_t active_skill, int skill_set,
                    const std::function<bool(HotbarButton)>& pressed,
                    const std::function<bool(HotbarButton)>& held);

/// One slot as `CGameUI::updateSlots` dresses
/// it.
struct HotbarSlotView {
    std::string icon;     ///< empty: no picture
    std::string text;     ///< the `SkillText` caption
    bool dimmed = false;  ///< `ImageColours` 0.6 grey rather than white
};

/// A skill slot: its icon, and `ceilf(getSkillCoolingTime)` as an integer.
/// Below 1 the icon is white and the caption empty; otherwise the icon is
/// greyed and the caption is the number of seconds.
HotbarSlotView skill_slot_view(const std::string& icon, float cooling_time);

/// An item slot: its `ICON` and `getEquipmentCountOfGuid`. None held greys the
/// icon and blanks the caption; otherwise the caption is the count. An item the
/// resource manager does not know (`icon` empty) is an empty slot.
HotbarSlotView item_slot_view(const std::string& icon, int count);

/// PORT: whether `item` is a potion of `bound`'s kind: the same `UNITTYPE` and the same first
/// effect (`MANA RECHARGE`, `HP RECHARGE PLAYER`), which is all that tells the sizes of one
/// potion from another potion in the data.
bool same_potion_kind(const ItemStats& bound, const ItemStats& item);

/// PORT: what a bumper drinks. The console binds each bumper to the smallest potion's GUID and
/// has nothing to drink once those are gone, whatever larger ones are carried. Here the slot
/// of the bound unit while one is carried, else of the carried potion of the same kind with
/// the lowest `LEVEL` (the next size up); -1 for none.
int potion_slot_for(const Inventory& inventory, const ItemStats& bound);

/// PORT: every potion of `bound`'s kind carried, all sizes: the bumper's count.
int potion_count_for(const Inventory& inventory, const ItemStats& bound);

/// `CInventory::getEquipmentCountOfGuid`: every item of unit `guid`
/// in the bag, an unstackable one (`+0x23c` below 1) counting 1 and any other
/// its stack (`+0x238`).
int equipment_count_of_guid(const Inventory& inventory, int64_t guid);

}  // namespace tl::core
