#include "core/inventory.h"

#include "core/character.h"
#include "core/item_effects.h"
#include "core/item_roll.h"
#include "core/item_sets.h"
#include "core/item_use.h"
#include "core/random.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace tl::core {

namespace {

// PORT: ASCII lower-casing, the case-blind compare of item names.
std::string lowered(const std::string& str) {
    std::string out = str;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    return out;
}

// PORT: a case-blind compare of item names (`STRINGS::StringUpper` on both).
bool names_match(const std::string& a, const std::string& b) { return lowered(a) == lowered(b); }

uint32_t pane_gate_type(InventoryPane pane) {
    switch (pane) {
        case InventoryPane::Backpack: return unit_type::kTakeable;
        case InventoryPane::Spell: return unit_type::kSpell;
        case InventoryPane::Fish: return unit_type::kFish;
        case InventoryPane::Weapon: return unit_type::kWeapon;
        case InventoryPane::Armor: return unit_type::kArmor;
    }
    return unit_type::kTakeable;
}

/// `gEQUIP_UNITTYPES_ALLOWED`/`gEQUIP_UNITTYPES_DISALLOWED`,
/// one pair each per `EEQUIP_LOCATIONS` 0-11. Slot 11 allows nothing, in the original too.
struct LocationGate {
    uint32_t allowed1;
    uint32_t allowed2;
    uint32_t disallowed1;
    uint32_t disallowed2;
};

constexpr uint32_t kNone = unit_type::kNone;

const std::array<LocationGate, 12>& location_gates() {
    static const std::array<LocationGate, 12> table = {{
        {unit_type::kWeapon, kNone, unit_type::kBow, kNone},     // 0 right hand
        {unit_type::kWeapon, unit_type::kShield, kNone, kNone},  // 1 left hand
        {unit_type::kGloves, kNone, kNone, kNone},               // 2
        {unit_type::kHelmet, kNone, kNone, kNone},               // 3
        {unit_type::kChestArmor, kNone, kNone, kNone},           // 4
        {unit_type::kShoulderArmor, kNone, kNone, kNone},        // 5
        {unit_type::kBoots, kNone, kNone, kNone},                // 6
        {unit_type::kBelt, kNone, kNone, kNone},                 // 7
        {unit_type::kRing, kNone, kNone, kNone},                 // 8
        {unit_type::kRing, kNone, kNone, kNone},                 // 9
        {unit_type::kNecklace, kNone, kNone, kNone},             // 10
        {kNone, kNone, kNone, kNone},                            // 11 - INVENTED: never matches
    }};
    return table;
}

/// two items are of one type when their `+0x168` GUIDs agree.
/// PORT: a copy made by hand with no GUID (`unit_guid` -1 on both) is told by
/// its `NAME`, as the rest of the port tells items apart.
bool same_item_type(const ItemStats& a, const ItemStats& b) {
    if (a.unit_guid != -1 || b.unit_guid != -1) return a.unit_guid == b.unit_guid;
    return names_match(a.name, b.name);
}

/// The 12-location gate loop every `CInventory::canEquip`/`pickupEquipment`/
/// `equipEquipmentIntoFirstFreeLocation`/`getComparisonItems` overload repeats:
/// not disallowed, and allowed by at least one of the pair.
bool location_allows(const ItemStats& item, const UnitTypeHierarchy& unit_types, int location) {
    if (location < 0 || location >= 12) return false;
    const LocationGate& g = location_gates()[static_cast<size_t>(location)];
    if (unit_types.isa(item.unittype, g.disallowed1)) return false;
    if (unit_types.isa(item.unittype, g.disallowed2)) return false;
    return unit_types.isa(item.unittype, g.allowed1) || unit_types.isa(item.unittype, g.allowed2);
}

/// The manager context the effect half hands every item's manager and its own:
/// no owner, for an item is not a character and the inventory's own manager is
/// built with none (`CEffectManager::CEffectManager(nullptr)` in
/// `CInventory::CInventory`).
EffectManager::Context manager_context(const InventoryEffectContext& context) {
    EffectManager::Context out;
    out.types = context.types;
    out.graphs = context.graphs;
    out.owner.present = false;
    return out;
}

/// The effect types `calculateEffectValues` caches (`0x91`, exclusive) and the
/// damage types (8, `EDAMAGE_TYPES` and the unfiltered 7).
constexpr int kCachedEffectTypes = 0x91;
constexpr int kCachedDamageTypes = 8;

}  // namespace

Inventory::Inventory(size_t backpack_capacity, bool bare) : capacity_(backpack_capacity) {
    // `CInventory::CInventory`: total
    // capacity starts at `backpack_capacity + 0x13` (19), and pane 0 (Backpack)
    // is added at once, occupying the slots from 19 onward.
    (void)bare;
    slots_.resize(kFixedSlotCount + backpack_capacity);
    panes_.emplace_back(InventoryPane::Backpack, kFixedSlotCount);
    // The constructor's last call; it does
    // not `updateBonuses`.
    calculate_effect_values();
}

// PORT: the port's inventory laid out, a pane after the last.
void Inventory::add_pane(InventoryPane pane, size_t size) {
    panes_.emplace_back(pane, slots_.size());
    slots_.resize(slots_.size() + size);
}

// PORT: the occupied-slot count, for the HUD's `N/capacity`.
size_t Inventory::size() const {
    size_t n = 0;
    for (const InventorySlot& s : slots_) {
        if (s.count > 0) ++n;
    }
    return n;
}

// `CCharacter::giveGold`: floored at 0, a
// positive gift that would overflow stops at INT_MAX.
void Inventory::give_gold(int amount) {
    if (amount >= 1) {
        const int64_t sum = static_cast<int64_t>(gold_) + amount;
        if (sum > INT32_MAX) {
            gold_ = INT32_MAX;
            return;
        }
        gold_ = static_cast<int>(sum);
        return;
    }
    gold_ = std::max(0, gold_ + amount);
}

namespace {

/// `CInventory::getPaneIndex(EINVENTORY_PANES)`: the pane's position,
/// searched from the last added, and **0 when the inventory has no such pane**,
/// so a query about a missing pane answers for the first one, the backpack.
size_t pane_index(const std::vector<std::pair<InventoryPane, size_t>>& panes, InventoryPane pane) {
    for (size_t i = panes.size(); i-- > 0;) {
        if (panes[i].first == pane) return i;
    }
    return 0;
}

}  // namespace

// `CInventory::getPaneIndex`: where a pane's slots start.
int Inventory::pane_start(InventoryPane pane) const {
    if (panes_.empty()) return static_cast<int>(kFixedSlotCount);
    return static_cast<int>(panes_[pane_index(panes_, pane)].second);
}

int Inventory::pane_size(InventoryPane pane) const {
    // `CInventory::getPaneSize`.
    if (panes_.empty()) return 0;
    const size_t i = pane_index(panes_, pane);
    const size_t end = (i + 1 < panes_.size()) ? panes_[i + 1].second : slots_.size();
    return static_cast<int>(end - panes_[i].second);
}

// `CInventory::slotIsInPane`.
bool Inventory::slot_is_in_pane(size_t slot, InventoryPane pane) const {
    const int start = pane_start(pane);
    return slot >= static_cast<size_t>(start) &&
           slot < static_cast<size_t>(start) + static_cast<size_t>(pane_size(pane));
}

// `CInventory::itemsInPane`.
int Inventory::items_in_pane(InventoryPane pane) const {
    const int start = pane_start(pane);
    const int end = start + pane_size(pane);
    int n = 0;
    for (int i = start; i < end; ++i) {
        if (occupied(static_cast<size_t>(i))) ++n;
    }
    return n;
}

InventoryPane Inventory::required_pane(const ItemStats& item,
                                       const UnitTypeHierarchy& unit_types) const {
    // `getRequiredPane` walks the pane list in reverse - the most
    // recently added pane wins over the catch-all Backpack pane 0.
    for (auto it = panes_.rbegin(); it != panes_.rend(); ++it) {
        if (unit_types.isa(item.unittype, pane_gate_type(it->first))) return it->first;
    }
    return panes_.empty() ? InventoryPane::Backpack : panes_.front().first;
}

int Inventory::find_free_slot(const ItemStats& item, const UnitTypeHierarchy& unit_types,
                              int count) const {
    // `CInventory::findFreeSlot`.
    if (size() >= capacity_) return -1;
    const InventoryPane pane = required_pane(item, unit_types);
    const int start = pane_start(pane);
    const int end = start + pane_size(pane);
    // A stack it joins: stacking on (`+0xc`), the item stacking (`+0x1e8` above
    // 1), the same item type (`+0x168`), `count` more fitting under the stack's
    // own maximum.
    const auto joins = [&](const InventorySlot& s) {
        return stacking_ && item.max_stack > 1 && same_item_type(s.item, item) &&
               s.count + count <= s.item.max_stack;
    };
    if (stacking_ && item.max_stack > 0) {
        // the first such stack, then any later one
        // holding more than `count` (`cmpw; ble` against the item's own +0x1e4).
        int best = -1;
        for (int i = start; i < end; ++i) {
            const InventorySlot& s = slots_[static_cast<size_t>(i)];
            if (s.count <= 0 || !joins(s)) continue;
            if (best >= 0 && !(count < s.count)) continue;
            best = i;
        }
        if (best >= 0) return best;
    }
    // the first slot empty or such a stack.
    for (int i = start; i < end; ++i) {
        const InventorySlot& s = slots_[static_cast<size_t>(i)];
        if (s.count <= 0 || joins(s)) return i;
    }
    return -1;
}

bool Inventory::can_pickup(const ItemStats& item, const UnitTypeHierarchy& unit_types,
                           bool ignore_capacity) const {
    // `canPickup`: the required-owner-guid
    // gate (`+0x18`) and the `ISA(item, 2)` base-type test are not modelled.
    if (item.name.empty()) return false;
    if (ignore_capacity) return true;
    // the items held (`+0x24`, worn ones too) under the
    // capacity (`+0x1c`).
    return size() < capacity_;
}

bool Inventory::item_can_equip(const ItemStats& item, const UnitTypeHierarchy& unit_types,
                               const CharacterProgression* progression,
                               const EffectManager* own_effects) const {
    // `CEquipment::canEquip`. A character
    // with the player as its master wears trinkets only (ISA 0x27).
    if (players_pet_ && !unit_types.isa(item.unittype, unit_type::kTrinket)) return false;
    // A character that answers to requirements (`+0x4a0`) cannot wear an
    // unidentified copy (`+0x348`).
    if (progression != nullptr && !item.identified) return false;
    if (!unit_types.isa(item.unittype, unit_type::kWeapon) &&
        !unit_types.isa(item.unittype, unit_type::kArmor) &&
        !unit_types.isa(item.unittype, unit_type::kSpell) &&
        !unit_types.isa(item.unittype, unit_type::kTrinket)) {
        return false;
    }
    if (progression == nullptr) return true;
    // Level, then strength, dexterity, magic, defense, in that order,
    // short-circuiting, through the requirement getters
    // (`CEquipment::getLevelRequirement` & co.,
    // `core/item_roll.h`). A level requirement of 0 is no requirement. The
    // reductions and the four figures are the character's totals, its own
    // manager and this inventory.
    const CharacterEffects total(own_effects, this);
    const RequirementReductions reductions = requirement_reductions(total);
    const int level = item_level_requirement(item, &reductions, unit_types);
    if (level != 0 && progression->level() < level) return false;
    if (progression->strength(total) < item_strength_requirement(item, &reductions, unit_types)) {
        return false;
    }
    if (progression->dexterity(total) < item_dexterity_requirement(item, &reductions, unit_types)) {
        return false;
    }
    if (progression->magic(total) < item_magic_requirement(item, &reductions, unit_types)) {
        return false;
    }
    return !(progression->defense(total) < item_defense_requirement(item, &reductions, unit_types));
}

bool Inventory::can_equip(const ItemStats& item, const UnitTypeHierarchy& unit_types,
                          const CharacterProgression* progression, const EffectManager* own_effects,
                          int own_slot, bool ignore_capacity) const {
    // `CInventory::canEquip(CEquipment*, bool)`:
    // its flag is `canPickup`'s (r5 handed on).
    if (!can_pickup(item, unit_types, ignore_capacity)) return false;
    if (!item_can_equip(item, unit_types, progression, own_effects)) return false;
    // A hand holding something other than the item itself.
    const auto other_in = [&](int location) {
        return occupied(static_cast<size_t>(location)) && location != own_slot;
    };
    const auto two_handed_in = [&](int location) {
        return other_in(location) &&
               unit_types.isa(slots_[static_cast<size_t>(location)].item.unittype,
                              unit_type::kTwoHanded);
    };
    if (unit_types.isa(item.unittype, unit_type::kTwoHanded)) {
        if (other_in(equip_location::kRightHand)) return false;
        if (other_in(equip_location::kLeftHand)) return false;
    }
    if (unit_types.isa(item.unittype, unit_type::kWeapon) ||
        unit_types.isa(item.unittype, unit_type::kShield)) {
        if (two_handed_in(equip_location::kLeftHand)) return false;
        if (two_handed_in(equip_location::kRightHand)) return false;
    }
    for (int loc = 0; loc < 12; ++loc) {
        if (!location_allows(item, unit_types, loc)) continue;
        if (!occupied(static_cast<size_t>(loc)) || loc == own_slot) return true;
    }
    return false;
}

void Inventory::comparison_items(const ItemStats& item, const UnitTypeHierarchy& unit_types,
                                 int own_slot, int& first, int& second) const {
    // `CInventory::getComparisonItems`.
    const auto take = [&](int loc) {
        if (first < 0) {
            first = loc;
        } else if (second < 0) {
            second = loc;
        }
    };
    const bool weapon = unit_types.isa(item.unittype, unit_type::kWeapon);
    for (int loc = 0; loc < 12; ++loc) {
        const bool worn = occupied(static_cast<size_t>(loc));
        if (weapon && worn &&
            unit_types.isa(slots_[static_cast<size_t>(loc)].item.unittype, unit_type::kWeapon) &&
            loc != own_slot) {
            take(loc);
            continue;
        }
        if (location_allows(item, unit_types, loc) && worn && loc != own_slot) take(loc);
    }
}

bool Inventory::can_equip_into_location(const ItemStats& item, const UnitTypeHierarchy& unit_types,
                                        int location) const {
    // `canEquipIntoSpecificLocation`:
    // `canPickup` plus the location's own gate; no stat check, like the
    // original's own path here.
    if (location < 0 || location >= static_cast<int>(kFixedSlotCount)) return false;
    if (!can_pickup(item, unit_types, true)) return false;
    if (location >= 12) return true;  // slots 12-18: no gate table entry
    return location_allows(item, unit_types, location);
}

bool Inventory::remove_quest_items(int64_t quest_guid, const std::string& unit, bool whole_stack,
                                   uint32_t isa, const UnitTypeHierarchy& unit_types) {
    bool removed = false;
    bool emptied = false;
    for (InventorySlot& slot : slots_) {
        if (slot.count <= 0 || slot.item.quest_guid != quest_guid) continue;
        if (!unit.empty() && !names_match(slot.item.name, unit)) continue;
        if (!unit_types.isa(slot.item.unittype, isa)) continue;
        if (whole_stack) {
            slot = InventorySlot{};
            emptied = true;
        } else if (--slot.count == 0) {
            slot = InventorySlot{};
            emptied = true;
        }
        removed = true;
    }
    // An item that goes leaves through `removeEquipment`,
    // which refreshes the effect half. Not
    // raised: its `removedFromInventory` - the quest that is taking its items
    // back is no longer one this port's log hands events to.
    if (emptied) refresh_effects();
    return removed;
}

// `CInventory::pickupEquipment(CEquipment*, bool)`.
int Inventory::pickup(const ItemStats& item, int count, const UnitTypeHierarchy& unit_types,
                      bool equip_when_full) {
    if (count <= 0) return -1;
    // `pickupEquipment(CEquipment*, bool)`.
    if (size() < capacity_) {
        const int slot = find_free_slot(item, unit_types, count);
        // The hand rules switch the fallback off: a two-handed item with either
        // hand in use, a one-handed one beside a two-handed.
        const auto two_handed_in = [&](size_t location) {
            return occupied(location) &&
                   unit_types.isa(slots_[location].item.unittype, unit_type::kTwoHanded);
        };
        if (unit_types.isa(item.unittype, unit_type::kTwoHanded) &&
            (occupied(equip_location::kRightHand) || occupied(equip_location::kLeftHand))) {
            equip_when_full = false;
        }
        if (unit_types.isa(item.unittype, unit_type::kOneHanded) &&
            (two_handed_in(equip_location::kRightHand) ||
             two_handed_in(equip_location::kLeftHand))) {
            equip_when_full = false;
        }
        if (slot >= 0) return pickup_into_slot(item, count, slot) ? slot : -1;
        // The pane is full: the first equip location whose gate takes the item
        // and that is empty, if `canEquipIntoSpecificLocation(item, loc, true)`
        // (`canPickup` ignoring the capacity and the item's
        // `canEquip(owner, true)`).
        if (!equip_when_full) return -1;
        for (int loc = 0; loc < 12; ++loc) {
            if (!location_allows(item, unit_types, loc)) continue;
            if (occupied(static_cast<size_t>(loc))) continue;
            if (!can_pickup(item, unit_types, true) ||
                !item_can_equip(item, unit_types, owner_progression_, owner_effects_)) {
                continue;
            }
            return pickup_into_slot(item, count, loc) ? loc : -1;
        }
        return -1;
    }
    // at capacity, only a stack in the item's pane it joins
    // (the fullest that takes it).
    if (!stacking_ || item.max_stack <= 0) return -1;
    const InventoryPane pane = required_pane(item, unit_types);
    const int start = pane_start(pane);
    const int end = start + pane_size(pane);
    int best = -1;
    for (int i = start; i < end; ++i) {
        const InventorySlot& s = slots_[static_cast<size_t>(i)];
        if (s.count <= 0 || !stacking_ || item.max_stack <= 1) continue;
        if (!same_item_type(s.item, item) || s.count + count > s.item.max_stack) continue;
        if (best >= 0 && !(count < s.count)) continue;
        best = i;
    }
    if (best < 0) return -1;
    return pickup_into_slot(item, count, best) ? best : -1;
}

bool Inventory::pickup_into_slot(const ItemStats& item, int count, int slot) {
    // `pickupEquipment(CEquipment*, int, bool)`:
    // merge onto a matching stack already
    // there, else occupy the (assumed empty) slot.
    if (slot < 0 || static_cast<size_t>(slot) >= slots_.size() || count <= 0) return false;
    InventorySlot& s = slots_[static_cast<size_t>(slot)];
    if (stacking_ && s.count > 0 && names_match(s.item.name, item.name) && item.max_stack > 1) {
        // A merge refreshes nothing: the original returns before
        // `updateBonuses` on this path.
        s.count = std::min(item.max_stack, s.count + count);
        ++pickups_;
        // `addedToInventory` on the item picked up, which the merge
        // then deletes.
        if (item_events_.events != nullptr && item_events_.events->added) {
            ItemStats merged = item;
            item_events_.events->added(merged);
        }
        return true;
    }
    // Stacking off (`+0x14`), or an item that does not stack: an occupied
    // slot refuses it.
    if (s.count > 0) return false;
    s.item = item;
    s.count = (item.max_stack > 1) ? std::min(item.max_stack, count) : count;
    s.set_bonus = true;
    s.instance = ++last_instance_;
    ++pickups_;
    // `addedToInventory`, before the listeners and the bonuses.
    if (item_events_.events != nullptr && item_events_.events->added) {
        item_events_.events->added(s.item);
    }
    // A new `CEquipmentRef`: into a
    // worn location, `updateBonuses`, `calculateEffectValues` and
    // `verifyEquipment`; into the bag only when the worn items are not the ones
    // `verifyEquipment` last saw (`+0xe0`).
    if (static_cast<size_t>(slot) < 12 || !worn_as_verified()) refresh_effects();
    return true;
}

// PORT: and `verifyEquipment`'s own test: the worn
// items are the ones last verified (`+0xe0`).
bool Inventory::worn_as_verified() const {
    for (size_t location = 0; location < 12; ++location) {
        const uint64_t worn = occupied(location) ? slots_[location].instance : uint64_t{0};
        if (verified_[location] != worn) return false;
    }
    return true;
}

namespace {

/// The tests `equipEquipmentIntoFirstFreeLocation`
/// makes after `canEquip`, on plain
/// occupancy: a one-handed item beside no two-handed one, a two-handed item
/// with both hands empty.
bool hands_take(const std::vector<InventorySlot>& slots, const ItemStats& item,
                const UnitTypeHierarchy& unit_types) {
    const InventorySlot& right = slots[equip_location::kRightHand];
    const InventorySlot& left = slots[equip_location::kLeftHand];
    if (unit_types.isa(item.unittype, unit_type::kOneHanded)) {
        if (right.count > 0 && unit_types.isa(right.item.unittype, unit_type::kTwoHanded))
            return false;
        if (left.count > 0 && unit_types.isa(left.item.unittype, unit_type::kTwoHanded))
            return false;
    }
    if (unit_types.isa(item.unittype, unit_type::kTwoHanded)) {
        if (right.count > 0) return false;
        if (left.count > 0) return false;
    }
    return true;
}

}  // namespace

int Inventory::first_free_location(const ItemStats& item, const UnitTypeHierarchy& unit_types,
                                   const CharacterProgression* progression,
                                   const EffectManager* own_effects) const {
    // `equipEquipmentIntoFirstFreeLocation`,
    // up to the placing.
    if (!can_equip(item, unit_types, progression, own_effects)) return -1;
    if (!hands_take(slots_, item, unit_types)) return -1;
    for (int loc = 0; loc < 12; ++loc) {
        if (occupied(static_cast<size_t>(loc))) continue;
        if (location_allows(item, unit_types, loc)) return loc;
    }
    return -1;
}

bool Inventory::equip_into_first_free_location(size_t slot, const UnitTypeHierarchy& unit_types,
                                               const CharacterProgression* progression,
                                               const EffectManager* own_effects) {
    if (!occupied(slot)) return false;
    // nothing while the inventory holds its capacity
    // (`+0x24 < +0x1c`).
    if (size() >= capacity_) return false;
    const ItemStats item = slots_[slot].item;
    const int loc = first_free_location(item, unit_types, progression, own_effects);
    if (loc < 0) return false;
    // The item's own ref moves (its slot +0xc changed), the same item.
    InventorySlot moved = std::move(slots_[slot]);
    slots_[slot] = InventorySlot{};
    moved.set_bonus = true;
    slots_[static_cast<size_t>(loc)] = std::move(moved);
    // `equipEquipmentIntoFirstFreeLocation`,
    // after the equip callback.
    refresh_effects();
    return true;
}

Inventory::WearOutcome Inventory::wear_item(size_t slot, const UnitTypeHierarchy& unit_types,
                                            const CharacterProgression* progression,
                                            const EffectManager* own_effects) {
    // the branch for an identified item that is not worn.
    //
    WearOutcome out;
    if (!occupied(slot)) return out;
    const InventorySlot held = slots_[slot];
    const ItemStats& item = held.item;
    if (!unit_types.isa(item.unittype, unit_type::kWeapon) &&
        !unit_types.isa(item.unittype, unit_type::kArmor) &&
        !unit_types.isa(item.unittype, unit_type::kTrinket) &&
        !unit_types.isa(item.unittype, unit_type::kShield)) {
        return out;
    }
    // The item's own `canEquip(character, true)` (vtable `+0x15c`).
    if (!item_can_equip(item, unit_types, progression, own_effects)) return out;

    // `removeEquipment`, then `equipEquipmentIntoFirstFreeLocation`
    // for an item the inventory no longer holds.
    slots_[slot] = InventorySlot{};
    refresh_effects();
    const auto put_on = [&]() {
        const int loc = first_free_location(item, unit_types, progression, own_effects);
        if (loc < 0) return false;
        slots_[static_cast<size_t>(loc)] = held;
        refresh_effects();
        out.equipped = true;
        out.location = loc;
        return true;
    };
    if (put_on()) {
        out.sounded.push_back(item);
        return out;
    }

    // `getComparisonItems`: what is in the way.
    int first = -1;
    int second = -1;
    comparison_items(item, unit_types, -1, first, second);
    const bool two_handed = unit_types.isa(item.unittype, unit_type::kTwoHanded);
    const auto take_out = [&](int location) {
        const InventorySlot taken = slots_[static_cast<size_t>(location)];
        slots_[static_cast<size_t>(location)] = InventorySlot{};
        refresh_effects();
        return taken;
    };
    // Back to the bag (`pickupEquipment(item, true)`), or to the
    // floor at the character.
    const auto put_back = [&](const InventorySlot& taken) {
        if (pickup(taken.item, taken.count, unit_types, true) < 0) {
            out.dropped.push_back(taken);
        } else {
            out.sounded.push_back(taken.item);
        }
    };
    InventorySlot second_taken;
    if (two_handed && second >= 0) second_taken = take_out(second);
    if (first >= 0) {
        const InventorySlot first_taken = take_out(first);
        if (put_on()) out.sounded.push_back(item);
        put_back(first_taken);
    }
    if (two_handed && second >= 0) put_back(second_taken);
    if (!out.equipped) {
        // `pickupEquipment(item, false)`. DIVERGENCE: the original ignores a
        // refusal here and the item is gone; it goes to the floor instead.
        if (pickup(held.item, held.count, unit_types, false) < 0) out.dropped.push_back(held);
        return out;
    }
    out.sounded.push_back(item);
    return out;
}

Inventory::WearOutcome Inventory::take_off_item(size_t location,
                                                const UnitTypeHierarchy& unit_types) {
    // the branch for a worn item: `removeEquipment`,
    // `pickupEquipment(item, false)`, and the floor
    // at the character when the bag has no room.
    WearOutcome out;
    if (location >= 12 || !occupied(location)) return out;
    const InventorySlot taken = slots_[location];
    slots_[location] = InventorySlot{};
    refresh_effects();
    out.taken_off = true;
    if (pickup(taken.item, taken.count, unit_types, false) < 0) {
        out.dropped.push_back(taken);
    } else {
        out.sounded.push_back(taken.item);
    }
    return out;
}

bool Inventory::equip_into_location(const ItemStats& item, int location,
                                    const UnitTypeHierarchy& unit_types,
                                    const CharacterProgression* progression,
                                    const EffectManager* own_effects) {
    return place_into_location(item, location, unit_types, progression, own_effects, -1);
}

bool Inventory::equip_held_into_location(size_t slot, int location,
                                         const UnitTypeHierarchy& unit_types,
                                         const CharacterProgression* progression,
                                         const EffectManager* own_effects) {
    if (!occupied(slot)) return false;
    const ItemStats item = slots_[slot].item;
    return place_into_location(item, location, unit_types, progression, own_effects,
                               static_cast<int>(slot));
}

bool Inventory::place_into_location(const ItemStats& item, int location,
                                    const UnitTypeHierarchy& unit_types,
                                    const CharacterProgression* progression,
                                    const EffectManager* own_effects, int held_slot) {
    // `CInventory::equipEquipmentIntoSpecificLocation(CEquipment*, int)`.
    //
    if (location < 0) return false;
    // `canEquipIntoSpecificLocation(item, location, false)`:
    // below 19, nothing at the location, `canPickup`, the item's
    // `canEquip(owner, true)` (vtable +0x15c).
    const bool can = location < static_cast<int>(kFixedSlotCount) &&
                     !occupied(static_cast<size_t>(location)) &&
                     can_pickup(item, unit_types, false) &&
                     item_can_equip(item, unit_types, progression, own_effects);
    if (!can) return false;
    // a one-handed item (ISA 9) in a hand takes a
    // two-handed one (ISA 0xa) out of the other: `removeEquipment`, then
    // `pickupEquipment(item, 1)`, else onto the level at the owner's feet.
    if (unit_types.isa(item.unittype, unit_type::kOneHanded) &&
        (location == equip_location::kRightHand || location == equip_location::kLeftHand)) {
        const size_t other = location == equip_location::kRightHand ? equip_location::kLeftHand
                                                                    : equip_location::kRightHand;
        if (occupied(other) && unit_types.isa(slots_[other].item.unittype, unit_type::kTwoHanded)) {
            InventorySlot displaced = slots_[other];
            slots_[other] = InventorySlot{};
            // `removeEquipment` refreshes as it goes.
            refresh_effects();
            const int free_slot = find_free_slot(displaced.item, unit_types);
            if (free_slot >= 0) pickup_into_slot(displaced.item, displaced.count, free_slot);
            // INVENTED: no level to put it on - lost.
        }
    }
    if (held_slot >= 0) {
        // the item's own ref, found in the list, its location
        // (+0xc) rewritten.
        InventorySlot moved = std::move(slots_[static_cast<size_t>(held_slot)]);
        slots_[static_cast<size_t>(held_slot)] = InventorySlot{};
        slots_[static_cast<size_t>(location)] = std::move(moved);
    } else {
        // A new `CEquipmentRef` (its `+0x15` set).
        InventorySlot& placed = slots_[static_cast<size_t>(location)];
        placed = InventorySlot{};
        placed.item = item;
        placed.count = 1;
        placed.instance = ++last_instance_;
    }
    //
    update_bonuses();
    calculate_effect_values();
    if (owner_types_ != nullptr) verify_equipment(unit_types, progression, own_effects);
    return true;
}

void Inventory::verify_equipment(const UnitTypeHierarchy& unit_types,
                                 const CharacterProgression* progression,
                                 const EffectManager* own_effects) {
    // `CInventory::verifyEquipment()`. Only
    // for an inventory with an owner (`+0x14`, `set_owner`; the callers test
    // it); `+0xd` is never set.
    const auto worn = [this](size_t location) {
        return occupied(location) ? slots_[location].instance : uint64_t{0};
    };
    if (worn_as_verified()) return;
    for (int round = 0; round <= 0x13; ++round) {
        bool kept = true;
        for (size_t location = 0; location < 12; ++location) {
            if (!occupied(location)) continue;
            slots_[location].set_bonus = false;
            calculate_effect_values();
            const bool can = can_equip(slots_[location].item, unit_types, progression, own_effects,
                                       static_cast<int>(location), /*ignore_capacity=*/false);
            slots_[location].set_bonus = true;
            if (!can) {
                kept = false;
                unequip(static_cast<int>(location), unit_types);
            }
            calculate_effect_values();
        }
        if (kept) break;
    }
    update_bonuses();
    for (size_t location = 0; location < 12; ++location) verified_[location] = worn(location);
}

void Inventory::swap_weapon_set() {
    // `swapWeaponSet`: 0<->12, 1<->13.
    std::swap(slots_[equip_location::kRightHand], slots_[equip_location::kRightHand2]);
    std::swap(slots_[equip_location::kLeftHand], slots_[equip_location::kLeftHand2]);
    refresh_effects();
}

bool Inventory::unequip(int location, const UnitTypeHierarchy& unit_types) {
    if (location < 0 || static_cast<size_t>(location) >= kFixedSlotCount) return false;
    if (!occupied(static_cast<size_t>(location))) return false;
    const ItemStats item = slots_[static_cast<size_t>(location)].item;
    const int count = slots_[static_cast<size_t>(location)].count;
    const uint64_t instance = slots_[static_cast<size_t>(location)].instance;
    slots_[static_cast<size_t>(location)] = InventorySlot{};
    // `unequipEquipment` is
    // `removeEquipment`, which refreshes, then `pickupEquipment(item, true)`,
    // which places it as any pickup does - with the room the
    // removal made.
    refresh_effects();
    const uint64_t before = last_instance_;
    const int placed = pickup(item, count, unit_types, /*equip_when_full=*/true);
    if (placed >= 0) {
        // The same item, back in the bag (not a stack it joined).
        if (slots_[static_cast<size_t>(placed)].instance > before) {
            slots_[static_cast<size_t>(placed)].instance = instance;
        }
        return true;
    }
    // INVENTED: no room and no level to drop it into - lost.
    return false;
}

// `CInventory::useEquipment`: the stack less one,
// the slot cleared at zero.
bool Inventory::use_item(size_t slot, int count) {
    if (!occupied(slot) || count <= 0) return false;
    return remove_item(slot, count);
}

bool Inventory::remove_item(size_t slot, int count) {
    if (slot >= slots_.size() || count <= 0 || slots_[slot].count <= 0) return false;
    if (slots_[slot].count <= count) {
        const ItemStats gone = slots_[slot].item;
        slots_[slot] = InventorySlot{};
        // The item's ref goes, then `removedFromInventory`:
        // `removeEquipment` ends with the
        // pair.
        if (item_events_.events != nullptr && item_events_.events->removed) {
            item_events_.events->removed(gone);
        }
        refresh_effects();
    } else {
        // Only the stack shrinks (`incrementStackBy`, vtable `+0x338`), which
        // refreshes nothing.
        slots_[slot].count -= count;
    }
    return true;
}

// PORT: how many items of this name the inventory holds.
int Inventory::count_by_name(const std::string& name) const {
    int total = 0;
    for (const InventorySlot& slot : slots_) {
        if (slot.count > 0 && names_match(slot.item.name, name)) total += slot.count;
    }
    return total;
}

// PORT: the first slot holding an item of this name.
int Inventory::find_by_name(const std::string& name) const {
    for (size_t i = 0; i < slots_.size(); ++i) {
        if (slots_[i].count > 0 && names_match(slots_[i].item.name, name))
            return static_cast<int>(i);
    }
    return -1;
}

// INVENTED: the Xbox release's `Improbably Large Fish`, its record transcribed
// from `FISH_TROLLFISH_PERM.DAT`; that a new character is given one is from play.
const ItemStats& xbox_starting_fish() {
    static const ItemStats fish = [] {
        ItemStats item;
        item.name = "TrollFish_Giant";
        item.display_name = "Improbably Large Fish";
        item.base_name = item.display_name;
        item.unittype = "FISH";
        item.description = "Feed fish to pet\nWarning: Permanent Effect!";
        item.mesh = "media/models/items/fish/fish";
        item.icon = "fish_1";
        item.value = 55;
        item.rarity = 0;
        item.level = 1;
        item.max_stack = 5;
        item.unit_guid = 1281705585681568277;
        item.uses = 1;
        item.use_sound = "PotionUse";
        // Its one `[EFFECT]`: `NAME:Pet Troll`, `ACTIVATION:DYNAMIC`,
        // `DURATION:INSTANT`, `TYPE:TRANSFORM PERMANENT` (0x49),
        // `PARTICLE_FX:GENERICPOTION`, `MIN` and `MAX` 0.
        Effect transform;
        transform.type = kEffectTypeTransformPermanent;
        transform.activation = EffectActivation::Dynamic;
        transform.duration = kDurationInstant;
        transform.authored_duration = true;
        transform.name = "Pet Troll";
        transform.particle_fx = "GENERICPOTION";
        item.effects.push_back(transform);
        return item;
    }();
    return fish;
}

// PORT: an item put in its first equip location, or through `pickup`.
bool Inventory::add_item(const ItemStats& item, int count, const UnitTypeHierarchy& unit_types,
                         bool equipped) {
    if (count <= 0) return false;
    if (equipped) {
        for (int loc = 0; loc < 12; ++loc) {
            if (occupied(static_cast<size_t>(loc))) continue;
            if (!location_allows(item, unit_types, loc)) continue;
            return pickup_into_slot(item, count, loc);
        }
        // Nowhere to equip it - fall through to the ordinary panes.
    }
    return pickup(item, count, unit_types) >= 0;
}

std::vector<DefaultEquipment> read_default_equipment(const tl::formats::AdmSection& unit) {
    // `CCharacter::createDefaultEquipment`: the unit's
    // `EQUIPMENT` group (`CDataGroup::GetDataGroupByName`), then each key's string,
    // an empty one skipped.
    std::vector<DefaultEquipment> out;
    const tl::formats::AdmSection* group = nullptr;
    for (const tl::formats::AdmSection& child : unit.children) {
        if (child.name == "EQUIPMENT") {
            group = &child;
            break;
        }
    }
    if (group == nullptr) return out;
    static const struct {
        const char* key;
        int location;
    } kItems[] = {{"RIGHTHAND", 0}, {"LEFTHAND", 1},  {"CHEST", 4},
                  {"SHIELD", 0xb},  {"SHOULDERS", 5}, {"BOOTS", 6},
                  {"GLOVES", 2},    {"HELMET", 3},    {"WEAPON", 0xc}},
      kSpawns[] = {{"SPAWNRIGHTHAND", 0}, {"SPAWNLEFTHAND", 1},  {"SPAWNCHEST", 4},
                   {"SPAWNSHIELD", 0xb},  {"SPAWNSHOULDERS", 5}, {"SPAWNBOOTS", 6},
                   {"SPAWNGLOVES", 2},    {"SPAWNHELMET", 3},    {"SPAWNCLASS", 0xc}};
    for (const auto& k : kItems) {
        std::string name = group->string_or(k.key, "");
        if (!name.empty()) out.push_back({std::move(name), k.location, false});
    }
    for (const auto& k : kSpawns) {
        std::string name = group->string_or(k.key, "");
        if (!name.empty()) out.push_back({std::move(name), k.location, true});
    }
    return out;
}

void Inventory::create_equipment_and_equip(const std::string& name, int location, bool enchant_it,
                                           const ItemBook& item_book,
                                           const UnitTypeHierarchy& unit_types,
                                           const CreateEquipment& create,
                                           const EnchantEquipment& enchant) {
    // `createEquipmentAndEquip`.
    const ItemStats* data = item_book.find(name);
    if (data == nullptr) return;
    ItemStats item = create ? create(*data, false) : *data;
    // `pickupEquipment(item, true)`, its outcome not looked at.
    const int slot = pickup(item, 1, unit_types, true);
    // `CEquipment::enchant(true)` on the item where it now is.
    if (enchant_it && enchant) {
        if (slot >= 0) {
            enchant(slots_[static_cast<size_t>(slot)].item);
            item = slots_[static_cast<size_t>(slot)].item;
        } else {
            enchant(item);
        }
    }
    if (location == equip_location::kRightHand2) {
        // `equipEquipmentIntoFirstFreeLocation`.
        if (slot >= 0) {
            equip_into_first_free_location(static_cast<size_t>(slot), unit_types,
                                           owner_progression_, owner_effects_);
        } else if (size() < capacity_) {
            const int loc =
                first_free_location(item, unit_types, owner_progression_, owner_effects_);
            if (loc >= 0) {
                InventorySlot& placed = slots_[static_cast<size_t>(loc)];
                placed = InventorySlot{};
                placed.item = item;
                placed.count = 1;
                placed.instance = ++last_instance_;
                refresh_effects();
            }
        }
    } else if (slot >= 0) {
        equip_held_into_location(static_cast<size_t>(slot), location, unit_types,
                                 owner_progression_, owner_effects_);
    } else {
        equip_into_location(item, location, unit_types, owner_progression_, owner_effects_);
    }
    // Then the item's model (vtable +0xec, its entity +0x5c) is handed 0x46 through the
    // entity's vtable +0x7c: drawing, the port's own.
}

void Inventory::create_default_equipment(const std::vector<DefaultEquipment>& equipment,
                                         const ItemBook& item_book,
                                         const UnitTypeHierarchy& unit_types,
                                         const CreateEquipment& create, const RollSpawnClass& roll,
                                         const EnchantEquipment& enchant) {
    // `CCharacter::createDefaultEquipment`.
    for (const DefaultEquipment& entry : equipment) {
        if (!entry.spawn_class) {
            create_equipment_and_equip(entry.name, entry.location, false, item_book, unit_types,
                                       create, enchant);
            continue;
        }
        // `loadAndEquipSpawnClass`.
        if (!roll) continue;
        std::vector<std::string> units;
        std::vector<bool> flags;
        roll(entry.name, units, flags);
        if (units.empty()) continue;
        // Every unit takes the first one's flag (`lbz 0` of the list).
        const bool first_flag = !flags.empty() && flags[0];
        if (entry.location != equip_location::kRightHand2) {
            create_equipment_and_equip(units[0], entry.location, first_flag, item_book, unit_types,
                                       create, enchant);
            continue;
        }
        for (const std::string& unit : units) {
            create_equipment_and_equip(unit, equip_location::kRightHand2, first_flag, item_book,
                                       unit_types, create, enchant);
        }
    }
}

void Inventory::first_time_setup(const ItemBook& item_book, const UnitTypeHierarchy& unit_types,
                                 const StartingGifts& gifts, const CreateEquipment& create) {
    // `CPlayer::firstTimeSetup`: each one
    // `createEquipment(name, true)` and, when made, `pickupEquipment(item, true)`.
    const auto give = [&](const char* name) {
        const ItemStats* data = item_book.find(name);
        if (data == nullptr) return;
        pickup(create ? create(*data, true) : *data, 1, unit_types, true);
    };
    for (const char* name :
         {"Health Potion", "Mana Potion", "Town Portal Scroll", "Identify Scroll"}) {
        give(name);
        give(name);
    }
    if (gifts.community_goal) give("Bee Swarm");
    if (gifts.launch_promo) give("TrollFish_Giant");
    if (gifts.invited) give("RespecPotion");
}

// PORT: the inventory emptied.
void Inventory::clear() {
    for (InventorySlot& s : slots_) s = InventorySlot{};
    gold_ = 0;
    refresh_effects();
}

// --------------------------------------------------------------------------
// The effect half

// `CInventory::updateBonuses()`, then the
// effect values worked out again.
void Inventory::refresh_effects() {
    update_bonuses();
    calculate_effect_values();
    if (owner_types_ != nullptr)
        verify_equipment(*owner_types_, owner_progression_, owner_effects_);
}

// PORT: the owner's handles, for `refresh_effects`.
void Inventory::set_owner(const UnitTypeHierarchy* unit_types,
                          const CharacterProgression* progression,
                          const EffectManager* own_effects) {
    owner_types_ = unit_types;
    owner_progression_ = progression;
    owner_effects_ = own_effects;
}

// PORT: the effect half handed its tables, and the values worked out again.
void Inventory::set_effect_context(const InventoryEffectContext& context) {
    effect_context_ = context;
    calculate_effect_values();
}

int Inventory::set_count(const ItemSet& set) const {
    // `CInventory::getSetCount(CSet*)`: the
    // first ref at each of locations 0-11, an exact comparison of `SET` with
    // `NAME` (lengths, then `wmemcmp`).
    int count = 0;
    for (size_t location = 0; location < 12; ++location) {
        if (!occupied(location)) continue;
        if (slots_[location].item.set == set.name) ++count;
    }
    return count;
}

void Inventory::calculate_effect_values() {
    // The equipped copies' own managers, which the original keeps on each
    // item (`+0x1b8`) and this port rebuilds from the copy here.
    const EffectManager::Context context = manager_context(effect_context_);
    for (size_t location = 0; location < 12; ++location) {
        EquippedEffects& equipped = equipped_effects_[location];
        equipped = EquippedEffects{};
        if (location >= slots_.size() || !occupied(location)) continue;
        equipped.present = true;
        equipped.item = item_effect_manager(slots_[location].item, context);
        for (const ItemStats& socketed : slots_[location].item.socketed) {
            equipped.socketed.push_back(item_effect_manager(socketed, context));
        }
    }

    // `CInventory::calculateEffectValues()`.
    effects_.set_context(context);
    effects_.clear_out_affix_effects();
    effects_.clear_effects(false);
    std::vector<const ItemSet*> applied;  // each set once, however many pieces
    for (size_t location = 0; location < 12; ++location) {
        if (!occupied(location)) continue;
        const InventorySlot& slot = slots_[location];
        if (!slot.set_bonus) continue;
        if (slot.item.set.empty()) continue;
        if (effect_context_.sets == nullptr) continue;
        const ItemSet* set = effect_context_.sets->get_set(slot.item.set);
        if (set == nullptr) continue;
        if (std::find(applied.begin(), applied.end(), set) != applied.end()) continue;
        applied.push_back(set);
        const int worn = set_count(*set);
        for (const ItemSetBonus& bonus : set->bonuses) {
            if (bonus.count <= worn) {
                // The inventory's manager has no unit (`CInventory::CInventory`):
                // the affix's rolls are an item's, on
                // `g_RandVolatile`.
                EffectRolls rolls;
                rolls.graphs = effect_context_.graphs;
                rolls.rng = &volatile_random();
                effects_.add_affix_by_name(bonus.affix_name, bonus.level, effect_context_.affixes,
                                           -1.0f, rolls);
            }
        }
    }
    effects_.calculate_effect_values();

    // The cache: eight maps, one per damage type, each emptied, then a total
    // stored for every effect type whose total is not 0 (a NaN is stored).
    for (int damage_type = 0; damage_type < kCachedDamageTypes; ++damage_type) {
        std::map<int, float>& cache = effect_cache_[static_cast<size_t>(damage_type)];
        cache.clear();
        for (int type = 0; type < kCachedEffectTypes; ++type) {
            const float value = calculate_effect_value(type, damage_type);
            if (value != 0.0f) cache[type] = value;
        }
    }
}

float Inventory::calculate_effect_value(int type, int damage_type) const {
    // `CInventory::calculateEffectValue(EEFFECT_TYPE, EDAMAGE_TYPES)`:
    // each term `ceilf`'d before it is added
    // (`addss` after each `ceilf`).
    float total = 0.0f;
    for (const EquippedEffects& equipped : equipped_effects_) {
        if (!equipped.present) continue;
        total =
            std::ceil(equipped.item.effect_value(EffectActivation::Passive, type, damage_type)) +
            total;
        for (const EffectManager& socketed : equipped.socketed) {
            total = std::ceil(socketed.effect_value(EffectActivation::Passive, type, damage_type)) +
                    total;
        }
    }
    return std::ceil(effects_.effect_value(EffectActivation::Passive, type, damage_type)) + total;
}

float Inventory::effect_value(int type, int damage_type) const {
    // `CInventory::getEffectValue(EEFFECT_TYPE, EDAMAGE_TYPES)`.
    //
    if (damage_type < 0 || damage_type >= kCachedDamageTypes) return 0.0f;
    const std::map<int, float>& cache = effect_cache_[static_cast<size_t>(damage_type)];
    const auto it = cache.find(type);
    return it != cache.end() ? it->second : 0.0f;
}

// PORT: the cached totals of one damage type (`+0x90 + damage_type * 0x30`), for a dump or a test.
const std::map<int, float>& Inventory::effect_cache(int damage_type) const {
    static const std::map<int, float> kNone;
    if (damage_type < 0 || damage_type >= kCachedDamageTypes) return kNone;
    return effect_cache_[static_cast<size_t>(damage_type)];
}

float Inventory::effect_value(int type, const std::string& name) const {
    // `CInventory::getEffectValue(EEFFECT_TYPE, float, const std::wstring&)`:
    // the same walk through the by-name
    // virtual (`+0x258`), and the own manager's by-name total last.
    float total = 0.0f;
    for (const EquippedEffects& equipped : equipped_effects_) {
        if (!equipped.present) continue;
        total = std::ceil(equipped.item.effect_value(type, name)) + total;
        for (const EffectManager& socketed : equipped.socketed) {
            total = std::ceil(socketed.effect_value(type, name)) + total;
        }
    }
    return std::ceil(effects_.effect_value(type, name)) + total;
}

float Inventory::effect_value_from_equipment_slot(int type, int location, int damage_type) const {
    // `CInventory::getEffectValueFromEquipmentSlot`.
    // Only location 12 (`0xc`) is refused
    // outright; any other finds whatever sits there.
    if (location == equip_location::kRightHand2) return 0.0f;
    if (location < 0 || !occupied(static_cast<size_t>(location))) return 0.0f;
    const ItemStats& item = slots_[static_cast<size_t>(location)].item;
    const EffectManager::Context context = manager_context(effect_context_);
    float total = std::ceil(
        tl::core::item_effect_value(item, context, EffectActivation::Passive, type, damage_type));
    for (const ItemStats& socketed : item.socketed) {
        total = total + std::ceil(tl::core::item_effect_value(
                            socketed, context, EffectActivation::Passive, type, damage_type));
    }
    return std::ceil(total);
}

float Inventory::effect_value_minus_equipment_slot(int type, int location, int damage_type) const {
    // `CInventory::getEffectValueMinusEquipmentSlot`.
    // A NaN total is not equal to 0 and
    // takes the subtraction (`jp` at the test).
    const float total = std::ceil(effect_value(type, damage_type));
    if (total != 0.0f) {
        return total - effect_value_from_equipment_slot(type, location, damage_type);
    }
    return 0.0f;
}

void Inventory::update_bonuses() {
    // `CInventory::updateBonuses()`: `+0x10`
    // from 0, then each of locations 0-11's item's `+0x338`.
    armor_bonus_ = 0;
    for (size_t location = 0; location < 12; ++location) {
        if (occupied(location)) armor_bonus_ += slots_[location].item.graph_ac;
    }
}

}  // namespace tl::core
