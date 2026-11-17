// The player's (and the pet's) item storage: panes, numbered slots, stacking,
// equipping and the default starting gear (`CInventory`).
//
// Slot numbers are the original's, since a save writes them: the 19 fixed equip
// locations first, then each pane's slots in the order the panes were added.
#pragma once

#include "core/effect_manager.h"
#include "core/unit_types.h"
#include "core/units.h"

#include <array>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace tl::core {

class CharacterProgression;
class ItemSetBook;
struct ItemSet;

/// `EINVENTORY_PANES` (pane gate `gEQUIP_UNITTYPES_PANE`), in the order
/// `CCharacter::unitInit`/`CPlayer::unitInit` add them (`CInventory::CInventory`
/// adds `Backpack`; the rest are `CInventory::addSection` calls).
enum class InventoryPane {
    Backpack,  ///< pane id 0, unit type Takeable (31) - the catch-all
    Spell,     ///< pane id 1, unit type Spell (129)
    Fish,      ///< pane id 2, unit type Fish (124)
    Weapon,    ///< pane id 3, unit type Weapon (8) - merchants and stashes only
    Armor,     ///< pane id 4, unit type Armor (13) - merchants and stashes only
};

/// `EEQUIP_LOCATIONS` 0-18, outside every pane (`canEquipIntoSpecificLocation`
/// bounds them `< 0x13`). 0-11 are gated by unit type
/// (`gEQUIP_UNITTYPES_ALLOWED`/`_DISALLOWED`); 12-13 are the second
/// weapon set (`swapWeaponSet`); 15-18 the combine's slots
/// (`CCombineMenu::createMenus`, `CCombineMenu::performInteraction`).
///
namespace equip_location {
inline constexpr int kRightHand = 0;  ///< Weapon, not Bow
inline constexpr int kLeftHand = 1;   ///< Weapon or Shield
inline constexpr int kGloves = 2;
inline constexpr int kHelmet = 3;
inline constexpr int kChest = 4;
inline constexpr int kShoulder = 5;
inline constexpr int kBoots = 6;
inline constexpr int kBelt = 7;
inline constexpr int kRing1 = 8;
inline constexpr int kRing2 = 9;
inline constexpr int kNecklace = 10;
inline constexpr int kReserved11 = 11;  ///< INVENTED: never auto-selected, unnamed
inline constexpr int kRightHand2 = 12;  ///< second weapon set (`swapWeaponSet`)
inline constexpr int kLeftHand2 = 13;
inline constexpr int kReserved14 = 14;  ///< INVENTED: not read yet
inline constexpr int kCombine1 = 15;    ///< where a combine's result lands first
inline constexpr int kCombine2 = 16;
inline constexpr int kCombine3 = 17;
inline constexpr int kCombine4 = 18;
}  // namespace equip_location

/// One slot: empty when `count == 0`; its vector index is the original slot number.
/// `item` is `CInventory::getEquipmentInSlot`, `count`
/// `CInventory::getStackSizeOfEquipment`, `item.max_stack`
/// `getMaxStackSizeOfEquipment`.
struct InventorySlot {
    ItemStats item;
    int count = 0;
    /// `CEquipmentRef +0x21`: the item counts in the set pass. Set at every ref's
    /// construction (`pickupEquipment`, `equipEquipmentIntoFirstFreeLocation`);
    /// only `CInventory::verifyEquipment`
    /// clears it, around one `canEquip` test.
    bool set_bonus = true;
    /// PORT: which item this is, kept as it moves between slots (`verifyEquipment`
    /// compares the worn items' GUIDs, `+0x18`).
    uint64_t instance = 0;
};

/// What the inventory's effect half reads, not owned; any may be null. No `types`/`graphs`:
/// every effect is 0; no `sets`: no set pass (`CSets::getSingleton`); no `affixes`: a set
/// bonus adds nothing (`CEffectManager::addAffix`).
struct InventoryEffectContext {
    const EffectTypeTable* types = nullptr;
    const StatBook* graphs = nullptr;
    const ItemSetBook* sets = nullptr;
    const AffixBook* affixes = nullptr;
};

/// What an item is told as it comes into an inventory and leaves one:
/// `CEquipment::addedToInventory` (`pickupEquipment`)
/// and `removedFromInventory` (`removeEquipment`). `added`
/// may change the item: a new slot keeps the change, a merge loses it with the copy.
struct InventoryItemEvents {
    std::function<void(ItemStats& item)> added;
    std::function<void(const ItemStats& item)> removed;
};

/// `CResourceManager::createEquipment(name, from_save false, random_enchant)`:
/// a fresh copy put through `unitInit`'s rolls
/// (`init_item_copy`). Without one a copy is the data as it is.
using CreateEquipment =
    std::function<ItemStats(const ItemStats& data, bool random_enchant_allowed)>;

/// `createDefaultEquipment`'s reading of a unit's `[EQUIPMENT]` (`GetDataGroupByName`, the
/// first child group of that name): the nine item keys, then the nine spawn-class keys, in
/// the console's order, each one authored with a non-empty value.
std::vector<DefaultEquipment> read_default_equipment(const tl::formats::AdmSection& unit);

/// `CSpawnClass::rollSpawnClass` as `loadAndEquipSpawnClass`
/// asks for it - level -1, seed -1, flags 0x10 on
/// `g_Rand` - for the named spawn class: the units rolled, and their enchant flags.
using RollSpawnClass = std::function<void(
    const std::string& spawn_class, std::vector<std::string>& units, std::vector<bool>& enchant)>;
/// `CEquipment::enchant(true)`: `enchant_item(item, true, ...)`,
/// supplied by whoever holds the roll context.
using EnchantEquipment = std::function<void(ItemStats& item)>;

/// What `CPlayer::firstTimeSetup` gives beyond the eight defaults: each
/// is a setting (`CSettings +0x6c`) equal to 1.
struct StartingGifts {
    bool community_goal = false;  ///< `COMMUNITYGOAL` (KSETTINGS_COMMUNITYGOAL): `Bee Swarm`
    bool launch_promo = false;    ///< `LAUNCHPROMO` (KSETTINGS_LAUNCHPROMO): `TrollFish_Giant`
    bool invited = false;         ///< `INVITED` (KSETTINGS_INVITED): `RespecPotion`
};

/// `CInventory`. Items are values, not live objects with a GUID, and methods that test an
/// item's unit type take the `UnitTypeHierarchy` as a parameter.
///
/// The effect half is here too: the own manager (`+0x18`, set bonuses only), the
/// per-damage-type cache (`+0x90..`) and the armour sum (`+0x10`). Every change to what is
/// worn refreshes them as the original does; a copy changed in place through
/// `item_in_slot` needs `calculate_effect_values()` by hand.
///
/// Not transcribed: the GUID-keyed queries, `executeProcs`
/// and `transferEffects`.
class Inventory {
public:
    /// `EEQUIP_LOCATIONS` 0-18, ahead of every pane: `CInventory::CInventory`
    /// adds 0x13 to the capacity.
    static constexpr size_t kFixedSlotCount = 19;

    /// `CCharacter::unitInit`: every character but the
    /// shared stash gets `CInventory(this, 0x32)`, one 50-slot backpack and no other pane
    /// (`CPlayer::unitInit` adds none).
    static constexpr size_t kDefaultBackpackCapacity = 0x32;

    /// `CInventory::CInventory(owner, capacity)`: the 19
    /// fixed locations plus a Backpack of `backpack_capacity` slots, which is also the most
    /// items it holds (`+0x1c`). `bare` is unused: a pane is added only by `add_pane`.
    explicit Inventory(size_t backpack_capacity = kDefaultBackpackCapacity, bool bare = true);

    /// `CInventory::addSection`: appends a pane after the current
    /// last slot and grows total capacity by `size`.
    void add_pane(InventoryPane pane, size_t size);

    // PORT: an accessor of the port's inventory (`CInventory`'s slots and fields it keeps).
    const std::vector<InventorySlot>& slots() const { return slots_; }
    /// Occupied-slot count, for the HUD's "N/capacity" label.
    size_t size() const;
    // PORT: an accessor of the port's inventory (`CInventory`'s slots and fields it keeps).
    size_t capacity() const { return slots_.size(); }
    // PORT: an accessor of the port's inventory (`CInventory`'s slots and fields it keeps).
    bool empty() const { return size() == 0; }

    /// `slot < kFixedSlotCount` - equipped, not stored.
    // PORT: an accessor of the port's inventory (`CInventory`'s slots and fields it keeps).
    static bool is_equip_location(size_t slot) { return slot < kFixedSlotCount; }
    // PORT: an accessor of the port's inventory (`CInventory`'s slots and fields it keeps).
    bool occupied(size_t slot) const { return slot < slots_.size() && slots_[slot].count > 0; }
    /// The item in an occupied slot, to change what belongs to that copy
    /// (its rolled damage); null for an empty one.
    // PORT: an accessor of the port's inventory (`CInventory`'s slots and fields it keeps).
    ItemStats* item_in_slot(size_t slot) { return occupied(slot) ? &slots_[slot].item : nullptr; }

    // PORT: an accessor of the port's inventory (`CInventory`'s slots and fields it keeps).
    int gold() const { return gold_; }
    // PORT: an accessor of the port's inventory (`CInventory`'s slots and fields it keeps).
    void set_gold(int amount) { gold_ = amount; }
    void add_gold(int amount) { gold_ += amount; }
    /// `CCharacter::giveGold`: floored at 0,
    /// a positive gift that would overflow stops at INT_MAX.
    void give_gold(int amount);

    /// `CInventory::getPaneSize`/`getPaneIndex`.
    int pane_size(InventoryPane pane) const;
    int pane_start(InventoryPane pane) const;
    /// `CInventory::slotIsInPane`.
    bool slot_is_in_pane(size_t slot, InventoryPane pane) const;
    /// `CInventory::itemsInPane`.
    int items_in_pane(InventoryPane pane) const;
    /// `CInventory::getRequiredPane`: the last-added pane (searched
    /// in reverse) whose gate the item ISAs, falling back to `Backpack`.
    InventoryPane required_pane(const ItemStats& item, const UnitTypeHierarchy& unit_types) const;

    /// `CInventory::findFreeSlot`: -1 when the
    /// inventory holds its capacity; else, in the item's pane, a stack of the
    /// same item type (`+0x168`) that `count` more fits on - the first, or a
    /// later one holding more than `count` - and failing that the first slot
    /// empty or such a stack.
    int find_free_slot(const ItemStats& item, const UnitTypeHierarchy& unit_types,
                       int count = 1) const;
    /// `CInventory +0x1c`: how many items it holds at most, worn ones and every
    /// pane's together (the constructor's argument).
    size_t item_capacity() const { return capacity_; }

    /// `CInventory::canPickup`: unless
    /// `ignore_capacity`, that it holds fewer items than its capacity. The owner-GUID gate
    /// (`+0x18`) and `ISA(item, 2)` are not modelled: any named `ItemStats` is pickable.
    bool can_pickup(const ItemStats& item, const UnitTypeHierarchy& unit_types,
                    bool ignore_capacity) const;

    /// `CEquipment::canEquip(CCharacter*, bool)` with its
    /// category test on: a weapon, armour, spell or trinket; with `progression`
    /// (`CCharacter +0x4a0`, a character that answers to requirements) also identified, and
    /// the level, strength, dexterity, magic and defense requirements met in that order,
    /// against the character's totals (`CCharacter::getEffectValue`: `own_effects` plus this
    /// inventory). The player's pet (`players_pet()`) takes trinkets (ISA 0x27) only.
    /// INVENTED: merchant (ISA 0x29) and stash (0x80) wearers' refusals; nothing is offered
    /// to them to wear.
    bool item_can_equip(const ItemStats& item, const UnitTypeHierarchy& unit_types,
                        const CharacterProgression* progression,
                        const EffectManager* own_effects = nullptr) const;

    /// `CInventory::canEquip`: `can_pickup`, the item's
    /// `item_can_equip`, the hand rules (a two-handed item wants both hands empty; a weapon
    /// or shield wants neither holding a two-handed one), and a free location its type
    /// allows. `own_slot`: where the item sits when already worn, else -1.
    /// `ignore_capacity` is handed to `canPickup`.
    bool can_equip(const ItemStats& item, const UnitTypeHierarchy& unit_types,
                   const CharacterProgression* progression,
                   const EffectManager* own_effects = nullptr, int own_slot = -1,
                   bool ignore_capacity = true) const;
    /// `CInventory::getComparisonItems`: the
    /// worn items `item` would take the place of, as locations, -1 for none.
    /// Over the 12 locations in order: for a weapon, any worn weapon; otherwise
    /// whatever sits in a location the item's type allows. The first found is
    /// `first`, the next `second`, the rest are left.
    void comparison_items(const ItemStats& item, const UnitTypeHierarchy& unit_types, int own_slot,
                          int& first, int& second) const;
    /// `CInventory::canEquipIntoSpecificLocation`:
    /// `can_pickup` plus the location's own gate; no stat check, as the original.
    bool can_equip_into_location(const ItemStats& item, const UnitTypeHierarchy& unit_types,
                                 int location) const;

    /// `CInventory::pickupEquipment(CEquipment*, bool)`:
    /// finds a slot (`find_free_slot`, then
    /// every equip location in order) and places or merges the item there.
    /// Returns the slot the item landed in, or -1. With the pane full,
    /// `equip_when_full` puts the item on instead, unless the hand rules forbid it.
    int pickup(const ItemStats& item, int count, const UnitTypeHierarchy& unit_types,
               bool equip_when_full = true);
    /// `CInventory::pickupEquipment(CEquipment*, int, bool)`:
    /// places (or merges) into an exact slot
    /// - what a loaded save replays.
    bool pickup_into_slot(const ItemStats& item, int count, int slot);
    /// How many items `pickup_into_slot` has taken, each one round of the listeners'
    /// `equipmentPickedUp` (`CCharacter::equipmentPickedUp`).
    ///
    uint32_t pickups() const { return pickups_; }

    /// `CInventory::equipEquipmentIntoFirstFreeLocation`:
    /// moves the item already sitting at
    /// `slot` into the first free equip location its gate allows, after
    /// `can_equip` and the function's own hand rules (a one-handed item beside
    /// no two-handed one, a two-handed item with both hands empty).
    bool equip_into_first_free_location(size_t slot, const UnitTypeHierarchy& unit_types,
                                        const CharacterProgression* progression,
                                        const EffectManager* own_effects = nullptr);

    /// What the console's A did to an item (`wear_item`, `take_off_item`).
    struct WearOutcome {
        /// The item went on, at `location`.
        bool equipped = false;
        int location = -1;
        /// The item came off into the bag.
        bool taken_off = false;
        /// Sample 0x11 of each of these items' banks, in the order played (the
        /// item twice after a swap, as the original does).
        std::vector<ItemStats> sounded;
        /// What the bag had no room for: onto the level at the character
        /// (`CLevel::addItem`, then the item's drop).
        std::vector<InventorySlot> dropped;
    };
    /// The console's A on a bag item that is not worn.
    /// Nothing unless a weapon, armour, trinket or shield that `item_can_equip` accepts. The
    /// item leaves the bag for its first free location; with none, the worn item it compares
    /// with (`comparison_items`' first, and the second too under a two-handed one) comes off
    /// to the bag or, with no room there, the floor. An item that still could not go on
    /// returns to the bag.
    WearOutcome wear_item(size_t slot, const UnitTypeHierarchy& unit_types,
                          const CharacterProgression* progression,
                          const EffectManager* own_effects = nullptr);
    /// The same A on a worn item (locations 0-11): off, and
    /// into the bag without the equip fallback; to the floor when the bag is
    /// full.
    WearOutcome take_off_item(size_t location, const UnitTypeHierarchy& unit_types);
    /// `CInventory::equipEquipmentIntoSpecificLocation(CEquipment*, int)`:
    /// a new item put straight on, as a unit's own
    /// equipment is (`createEquipmentAndEquip`). No location gate: below 19, nothing there
    /// yet, `canPickup` and the item's `canEquip(owner, true)`; a one-handed item in a hand
    /// takes a two-handed one out of the other into the bag. Then `updateBonuses`,
    /// `calculateEffectValues` and `verify_equipment`.
    bool equip_into_location(const ItemStats& item, int location,
                             const UnitTypeHierarchy& unit_types,
                             const CharacterProgression* progression = nullptr,
                             const EffectManager* own_effects = nullptr);
    /// The same for an item the inventory already holds at `slot`: its ref moves to the
    /// location (`+0xc` rewritten) instead of a new one being made.
    bool equip_held_into_location(size_t slot, int location, const UnitTypeHierarchy& unit_types,
                                  const CharacterProgression* progression = nullptr,
                                  const EffectManager* own_effects = nullptr);
    /// `CInventory::verifyEquipment()`: when
    /// the worn items are not the ones it saw last, each worn one in turn with
    /// its set bonus off is tested with `can_equip` (the values worked out
    /// before and after), and taken off when it fails, until a round takes
    /// nothing off (20 at most); then `updateBonuses`, and the worn items kept.
    void verify_equipment(const UnitTypeHierarchy& unit_types,
                          const CharacterProgression* progression,
                          const EffectManager* own_effects);
    /// `CInventory +0x14`: the owning character. With one set, every change ends as the
    /// console's do: `updateBonuses`, `calculateEffectValues`, then `verifyEquipment`
    /// (`removeEquipment`, `pickupEquipment`), so a take-off
    /// inside `verify_equipment` verifies again, nested. Without one nothing is verified.
    /// `progression` and `own_effects` may be null.
    void set_owner(const UnitTypeHierarchy* unit_types, const CharacterProgression* progression,
                   const EffectManager* own_effects);
    /// `CInventory::swapWeaponSet`: slots 0<->12 and 1<->13.
    void swap_weapon_set();
    /// `CInventory::unequipEquipment`: the item at an
    /// equip location back into the panes (`pickup`, `force=true`).
    /// INVENTED: with no room it is lost; the original drops it into the level.
    bool unequip(int location, const UnitTypeHierarchy& unit_types);

    /// `CInventory::useEquipment`: the stack less
    /// one, cleared at zero. No `canUseEquipment` gate: the caller knows what it uses.
    bool use_item(size_t slot, int count = 1);

    /// `CInventory::removeEquipment`, by slot rather
    /// than by item pointer. The slot empties at zero but **does not shift**.
    bool remove_item(size_t slot, int count = 1);

    /// `CPlayer::removeQuestItemFromInventoryOrPetsInventoryByGuid`,
    /// one bag's half: every item of quest
    /// `quest_guid` (`CInventory::getEquipmentsOfQuestID`),
    /// narrowed to the unit `unit` (its `NAME`) when not empty, that `ISA(isa)`.
    /// `whole_stack` takes the stack; otherwise one goes. True when anything matched.
    bool remove_quest_items(int64_t quest_guid, const std::string& unit, bool whole_stack,
                            uint32_t isa, const UnitTypeHierarchy& unit_types);

    /// Total count of items matching `name` (case-insensitive), occupied slots
    /// only.
    int count_by_name(const std::string& name) const;

    /// First occupied slot whose item matches `name` (case-insensitive), or -1.
    int find_by_name(const std::string& name) const;

    /// `CInventory::isEquipmentEquipped` / `CInventory::getEquipmentEquippedAt`
    /// for the slot-indexed model: is `location` (0-18)
    /// occupied.
    bool equip_location_occupied(int location) const {
        return occupied(static_cast<size_t>(location));
    }

    /// `CCharacter::createDefaultEquipment()`: each entry
    /// of `read_default_equipment` in order. An item key is `createEquipmentAndEquip`
    /// without the enchant; a spawn-class key is
    /// `loadAndEquipSpawnClass`: `roll`, then the first unit rolled on the
    /// entry's location, or for 12 every unit rolled - each with the *first* unit's enchant
    /// flag, as the console reads it. A name the item book lacks makes nothing.
    /// `create` and `roll` default to the data as it is and no roll; `enchant` to nothing.
    void create_default_equipment(const std::vector<DefaultEquipment>& equipment,
                                  const ItemBook& item_book, const UnitTypeHierarchy& unit_types,
                                  const CreateEquipment& create = nullptr,
                                  const RollSpawnClass& roll = nullptr,
                                  const EnchantEquipment& enchant = nullptr);
    /// `createEquipmentAndEquip(character, group, name, location, enchant)`:
    /// `createEquipment(name, false)`, `pickupEquipment(item, true)`, the
    /// forced enchant when asked, then `equipEquipmentIntoFirstFreeLocation` for 12 and
    /// `equipEquipmentIntoSpecificLocation` otherwise - which moves the item picked up, or
    /// leaves it in the bag when the location refuses it.
    void create_equipment_and_equip(const std::string& name, int location, bool enchant_it,
                                    const ItemBook& item_book, const UnitTypeHierarchy& unit_types,
                                    const CreateEquipment& create = nullptr,
                                    const EnchantEquipment& enchant = nullptr);
    /// `CPlayer::firstTimeSetup()`: two each of `Health
    /// Potion`, `Mana Potion`, `Town Portal Scroll` and `Identify Scroll`, then the gifts
    /// (`StartingGifts`), each `createEquipment(name, true)` and `pickupEquipment(item,
    /// true)` one at a time; a name the item book lacks is skipped. Its last call, the
    /// welcome tip (`queueTip(9)`), is the caller's.
    void first_time_setup(const ItemBook& item_book, const UnitTypeHierarchy& unit_types,
                          const StartingGifts& gifts, const CreateEquipment& create = nullptr);

    /// Convenience over `pickup`/`pickup_into_slot`: `equipped` places the item in its
    /// first free equip location that takes it, otherwise it goes through `pickup`.
    bool add_item(const ItemStats& item, int count, const UnitTypeHierarchy& unit_types,
                  bool equipped = false);

    void clear();

    // ---- the effect half ---------------------------------------------------

    /// Hands the effect half what it reads (see `InventoryEffectContext`), then
    /// recalculates.
    void set_effect_context(const InventoryEffectContext& context);
    const InventoryEffectContext& effect_context() const { return effect_context_; }

    /// `CInventory::getSetCount(CSet*)`: over
    /// equip locations 0-11 (not the second weapon set), how many equipped
    /// items' `SET` equals the set's `NAME` exactly.
    int set_count(const ItemSet& set) const;

    /// `CInventory::calculateEffectValues()`: the own
    /// manager's affixes cleared (`clearOutAffixEffects`,
    /// `clearEffects(false)`); for locations 0-11 in order, each
    /// known set once (`set_bonus` up) adds by name every bonus with `COUNT <= set_count`,
    /// duration -1.0; then the own manager's cache; then, for damage types 0-7 and effect
    /// types 0-0x90, the per-type maps, each emptied first, **a value stored only when not
    /// 0**. The worn items' managers are rebuilt from their copies first.
    void calculate_effect_values();

    /// `CInventory::calculateEffectValue(EEFFECT_TYPE, EDAMAGE_TYPES)`:
    /// for locations 0-11, the item's value
    /// on the passive list (activation 0), `ceilf`'d and added, then each
    /// socketed item's, the same way; **last**, the own manager's (activation
    /// 0), `ceilf`'d. All `float`.
    float calculate_effect_value(int type, int damage_type) const;

    /// `CInventory::getEffectValue(EEFFECT_TYPE, EDAMAGE_TYPES)`:
    /// the cached total, 0 when the map has
    /// none.
    float effect_value(int type, int damage_type) const;
    /// The cached totals of one damage type (`+0x90 + damage_type * 0x30`),
    /// by effect type; empty outside 0-7. For a dump or a test.
    const std::map<int, float>& effect_cache(int damage_type) const;
    /// `CInventory::getEffectValue(EEFFECT_TYPE, float, const std::wstring&)`:
    /// `calculate_effect_value`'s sum
    /// through the by-name query (`CBaseUnit::getEffectValue`),
    /// not cached.
    float effect_value(int type, const std::string& name) const;
    /// `CInventory::getEffectValueFromEquipmentSlot`:
    /// one location's item and its socketed
    /// items, each `ceilf`'d, the total `ceilf`'d again; 0 for an empty
    /// location and for location 12.
    float effect_value_from_equipment_slot(int type, int location, int damage_type) const;
    /// `CInventory::getEffectValueMinusEquipmentSlot`:
    ///
    /// `t = ceilf(effect_value(type, dt))`, 0 when `t == 0`, otherwise `t` less
    /// `effect_value_from_equipment_slot`.
    float effect_value_minus_equipment_slot(int type, int location, int damage_type) const;

    /// `CInventory::updateBonuses()`:
    /// `+0x10`, the equipped items' armour points (`ItemStats::graph_ac`,
    /// `CEquipment +0x338`) summed over locations 0-11.
    void update_bonuses();
    // PORT: an accessor of the port's inventory (`CInventory`'s slots and fields it keeps).
    int armor_bonus() const { return armor_bonus_; }

    /// `+0x18`: the inventory's own manager, which only ever holds set bonuses.
    // PORT: an accessor of the port's inventory (`CInventory`'s slots and fields it keeps).
    const EffectManager& effects() const { return effects_; }
    /// The own manager (X360 `+0x17c`) of the item at an equip location 0-11, as
    /// `CInventory::getEquipmentAtLocation` hands it to `transferEffects`; null for an
    /// empty location.
    const EffectManager* equipped_item_effects(size_t location) const {
        if (location >= equipped_effects_.size() || !equipped_effects_[location].present) {
            return nullptr;
        }
        return &equipped_effects_[location].item;
    }

    /// `+0x14`: whether a pickup may merge onto a stack. Cleared for a merchant by
    /// `CCharacter::generateMerchantInventory` and
    /// `CCharacter::applySaveState`.
    bool stacking() const { return stacking_; }
    // PORT: an accessor of the port's inventory (`CInventory`'s slots and fields it keeps).
    void set_stacking(bool stacking) { stacking_ = stacking; }

    /// Whether the inventory's character has the player as its master
    /// (`CCharacter +0x640`, ISA 0x1c): the pet's bag. `canEquip` reads it.
    // PORT: an accessor of the port's inventory (`CInventory`'s slots and fields it keeps).
    bool players_pet() const { return players_pet_; }
    // PORT: an accessor of the port's inventory (`CInventory`'s slots and fields it keeps).
    void set_players_pet(bool pet) { players_pet_ = pet; }

    /// Who hears this inventory's items come and go, null for nobody. It is not copied:
    /// a copy has none, and an inventory assigned over keeps its own.
    void set_item_events(const InventoryItemEvents* events) { item_events_.events = events; }

private:
    bool players_pet_ = false;
    /// `equipEquipmentIntoFirstFreeLocation`
    /// up to the placing: the location an item would go into, -1 for none.
    int first_free_location(const ItemStats& item, const UnitTypeHierarchy& unit_types,
                            const CharacterProgression* progression,
                            const EffectManager* own_effects) const;
    /// `updateBonuses` then `calculateEffectValues`, the pair every changing
    /// method in the original ends with (before `verifyEquipment`).
    void refresh_effects();
    bool worn_as_verified() const;
    /// `equip_into_location` and `equip_held_into_location`: `held_slot` is the slot of the
    /// item's ref, -1 for an item the inventory does not hold.
    bool place_into_location(const ItemStats& item, int location,
                             const UnitTypeHierarchy& unit_types,
                             const CharacterProgression* progression,
                             const EffectManager* own_effects, int held_slot);

    /// An equipped item's manager and its socketed items', rebuilt by
    /// `calculate_effect_values`.
    struct EquippedEffects {
        bool present = false;
        EffectManager item;
        std::vector<EffectManager> socketed;
    };

    std::vector<InventorySlot> slots_;
    /// Pane id and start slot, in declaration order.
    std::vector<std::pair<InventoryPane, size_t>> panes_;
    int gold_ = 0;

    InventoryEffectContext effect_context_;
    int armor_bonus_ = 0;   ///< `+0x10`
    size_t capacity_ = 0;   ///< `+0x1c`
    bool stacking_ = true;  ///< `+0x14`
    uint32_t pickups_ = 0;
    /// PORT: the last `InventorySlot::instance` given.
    uint64_t last_instance_ = 0;
    /// `+0x14` (`set_owner`).
    const UnitTypeHierarchy* owner_types_ = nullptr;
    const CharacterProgression* owner_progression_ = nullptr;
    const EffectManager* owner_effects_ = nullptr;
    /// X360 `+0xe0`: the worn items `verify_equipment` last saw, by instance (0
    /// for none, the console's -1).
    std::array<uint64_t, 12> verified_{};
    /// The hooks belong to the inventory they were set on: a copy starts
    /// without them, and assigning one leaves the target's own in place.
    struct ItemEventsSlot {
        const InventoryItemEvents* events = nullptr;
        ItemEventsSlot() = default;
        ItemEventsSlot(const ItemEventsSlot&) {}
        // Assigns nothing, so assigning to itself is as safe as anything else.
        // NOLINTNEXTLINE(bugprone-unhandled-self-assignment)
        ItemEventsSlot& operator=(const ItemEventsSlot&) {  // cppcheck-suppress operatorEqVarError
            return *this;
        }
    };
    ItemEventsSlot item_events_;
    EffectManager effects_;                             ///< `+0x18`
    std::array<std::map<int, float>, 8> effect_cache_;  ///< `+0x90 + damage_type * 0x30`
    std::array<EquippedEffects, 12> equipped_effects_;
};

/// The Xbox release's `TrollFish_Giant` (`FISH_TROLLFISH_PERM.DAT`), which neither PC
/// archive carries; everything it points at is in the PC archive. A mounted Xbox archive's
/// own file is read instead. `CPlayer::firstTimeSetup` gives it when
/// `LAUNCHPROMO` is 1 (`StartingGifts::launch_promo`; `CContinueGameMenu::reloadFiles`
/// starts it at 0); the port gives it by default (`--no-starting-fish`
/// turns it off).
const ItemStats& xbox_starting_fish();

}  // namespace tl::core
