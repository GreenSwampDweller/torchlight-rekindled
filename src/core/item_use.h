// Using an item on a character - a potion drunk, a scroll read - and the two per-frame updates
// that turn what it left behind into health and mana. `CGameUI::useItem`;
// the hotbar's `CGameUI::activateItemSlot`. The caller carries out
// the sounds and the bag change it is told of.
#pragma once

#include "core/character_effects.h"
#include "core/effect_manager.h"
#include "core/inventory.h"
#include "core/skills.h"
#include "core/stats.h"
#include "core/unit_types.h"
#include "core/units.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace tl::core {

class CharacterProgression;

/// The effect types this block tests by number, rows of `media/EffectsList.dat`.
inline constexpr int kEffectTypeManaRecharge = 6;           ///< `MANA RECHARGE`
inline constexpr int kEffectTypeHpRecharge = 7;             ///< `HP RECHARGE`
inline constexpr int kEffectTypeOpenTownPortal = 0x2d;      ///< `OPEN TOWN PORTAL`
inline constexpr int kEffectTypeIdentify = 0x2e;            ///< `IDENTIFY`
inline constexpr int kEffectTypeDamage = 0x34;              ///< `DAMAGE`
inline constexpr int kEffectTypeTransform = 0x39;           ///< `TRANSFORM`
inline constexpr int kEffectTypeStun = 0x40;                ///< `STUN`
inline constexpr int kEffectTypeLearnSkill = 0x41;          ///< `LEARN SKILL`
inline constexpr int kEffectTypeTurnAlignment = 0x50;       ///< `TURN ALIGNMENT`
inline constexpr int kEffectTypeTransformPermanent = 0x49;  ///< `TRANSFORM PERMANENT`
inline constexpr int kEffectTypePotionEfficiency = 0x5a;    ///< `POTION EFFICIENCY`
inline constexpr int kEffectTypeKill = 0x6e;                ///< `KILL`
inline constexpr int kEffectTypePermanentStrength = 0x72;   ///< `PERMANENT STRENGTH`
inline constexpr int kEffectTypePermanentDexterity = 0x73;  ///< `PERMANENT DEXTERITY`
inline constexpr int kEffectTypePermanentMagic = 0x74;      ///< `PERMANENT MAGIC`
inline constexpr int kEffectTypePermanentHealth = 0x75;     ///< `PERMANENT HEALTH`
inline constexpr int kEffectTypePermanentDefense = 0x76;    ///< `PERMANENT DEFENSE`
inline constexpr int kEffectTypePermanentMana = 0x77;       ///< `PERMANENT MANA`
inline constexpr int kEffectTypeAwardStatpoint = 0x78;      ///< `AWARD STATPOINT`
inline constexpr int kEffectTypeManaRechargePlayer = 0x7b;  ///< `MANA RECHARGE PLAYER`
inline constexpr int kEffectTypeWarp = 0x71;                ///< `WARP`
inline constexpr int kEffectTypeOpenPortal = 0x8d;          ///< `OPEN PORTAL`
inline constexpr int kEffectTypeRespec = 0x8f;              ///< `RESPEC`
inline constexpr int kEffectTypeHpRechargePlayer = 0x7c;    ///< `HP RECHARGE PLAYER`
inline constexpr int kEffectTypeStopSkill = 0x7f;           ///< `STOP SKILL`
inline constexpr int kEffectTypeSwapPosWithPet = 0x8e;      ///< `SWAP POS WITH PET`

/// `CEquipment +0x248`'s "used up for good" marker: `useOnTarget` never counts
/// it down and `performItemUse` never removes the item for it.
inline constexpr int kUnlimitedUses = -9999;

/// The character an item is used on, as the use path reads it.
struct ItemUseTarget {
    /// `CCharacter::HP()` (inlined on the console, its `floor`)
    /// and `maxHP()`, and the mana pair.
    /// The original compares them as integers.
    float hp = 0.0f;
    float max_hp = 0.0f;
    float mana = 0.0f;
    float max_mana = 0.0f;
    /// `DAMAGEABLE`.
    bool damageable = true;
    /// The character's own unit type, for `ISA(this, PLAYER)`.
    std::string unittype = "PLAYER";
    /// The character's level, against the item's level requirement.
    int level = 1;
    /// The unit's effect manager; null is a unit that has never held an effect.
    EffectManager* effects = nullptr;
    /// The inventory half of the drinker's totals (`POTION EFFICIENCY`); null for none.
    const Inventory* inventory = nullptr;
    /// The character's level has a template that allows portals (`town_portal_allowed`).
    bool level_allows_portals = false;
    /// The same level for `OPEN PORTAL`: a template that allows portals or is
    /// a town's (`map_portal_allowed`).
    bool level_allows_map_portals = false;
    /// `CBaseUnit::hasUnitTheme(L"STUNNED")`, which refuses
    /// a `STUN`.
    bool stunned = false;
    /// The character's state is 5 or 6: dying or dead.
    bool dying_or_dead = false;
    /// Its alignment; `TURN ALIGNMENT` takes only a 2.
    int alignment = 0;
    /// Whether it has a master, and whether that master `isPlayer()`.
    bool has_master = false;
    bool master_is_player = false;
    /// The skills and spell slots `LEARN SKILL` tests and fills; null refuses it.
    SkillBook* skills = nullptr;
    /// `canLearnSpell`'s pet test: a master or `ISA PET`.
    bool pet = false;
    /// A skill file by `NAME`, for `learnSpell`'s `addSkillByName` and for the
    /// item's own skills `canUseOnTarget` tests.
    std::function<const SkillDef*(const std::string&)> skill_def;
    /// `CCharacter::performingSkillLoose`: a character still
    /// held by a skill takes no item that carries skills.
    bool skill_holds = false;
    /// The effect-type table the item's skills' blocks are read with; null
    /// leaves an item's skills untested.
    const EffectTypeTable* effect_types = nullptr;
    /// The character's attributes, points and permanent bonuses, which the
    /// `PERMANENT ...` and `AWARD STATPOINT` cases add to; null takes none.
    CharacterProgression* character = nullptr;
};

/// `CItem::isUseable`: `ISA(CONSUMABLE)` or
/// `ISA(INTERACTABLE)`.
bool item_is_useable(const ItemStats& item, const UnitTypeHierarchy& unit_types);

/// `CCharacter::isEffectValidForUnit`:
/// whether `effect`, carried by `item` (null for none), may go on `target`.
/// `value` is the effect's `CEffect::value(0)` as the item holds it; only its
/// sign is read.
bool effect_valid_for_unit(const ItemUseTarget& target, const ItemStats* item, const Effect& effect,
                           float value, const UnitTypeHierarchy& unit_types);

/// `CEquipment::canUseOnTarget`, the item
/// used by and on the same character: uses left, the level requirement, and at
/// least one of its effects valid for the target.
bool can_use_on_target(const ItemStats& item, const ItemUseTarget& target,
                       const UnitTypeHierarchy& unit_types, const StatBook& graphs);

/// which the inventory's A legend asks of a selected item:
/// an `ISA SPELL` item with a use left and a level requirement
/// the character meets, whose first `LEARN SKILL` effect `canLearnSpell`
/// passes. The legend then reads "Learn".
bool spell_learnable(const ItemStats& item, int uses, const ItemUseTarget& target,
                     const UnitTypeHierarchy& unit_types, const StatBook& graphs);

/// What `useOnTarget` did, for the caller to carry out.
struct ItemUseResult {
    int applied = 0;  ///< effects that went on
    /// an effect the target's `applyEffectOnUnit` took, or a skill on the
    /// item. Only then is the `USE_SOUND` played and a use spent.
    bool used = false;
    /// The item's first `[SKILL]`, for the user's `CCharacter::performUnknownSkill(CSkill*)`
    /// - the caller's. Empty for none.
    std::string skill;
    uint32_t skill_level = 0;
    bool potion_counted = false;  ///< journal statistic 0xc, `POTIONS USED`
    bool play_use_sound = false;  ///< the item bank's slot 0x14, `USE_SOUND`
    /// `performItemUse`'s removal test: the stack was at most one and no use
    /// is left, so the item leaves the bag rather than losing one from it.
    bool remove_item = false;
    /// `applyInstantEffect` reached `OPEN TOWN PORTAL`: the
    /// character's `openPortal` on its level, then one `PORTALS USED` count.
    bool open_town_portal = false;
    /// `applyInstantEffect` reached `OPEN PORTAL` (0x8d): the character's
    /// `openMapPortal(NAME, level)` for each such effect, in the item's order.
    std::vector<std::string> open_map_portals;
    /// `applyInstantEffect`'s `LEARN SKILL` learned this spell: the
    /// player's bank queues global slot 0x41 (`VOICESPELLLEARNED_SOUND`) and
    /// `CGameUI::queueTip(8)` follows. Empty when nothing was learned.
    std::string learned_spell;
    /// `applyInstantEffect`'s `RESPEC` (0x8f) reached a player: `CPlayer::resetSkills`
    /// - the caller's (`SkillBook::reset_skills`).
    bool respec = false;
    /// A `PERMANENT HEALTH` or `PERMANENT MANA` went on: `maxHP()` or
    /// `maxMana()` reads differently now.
    bool maxima_changed = false;
    /// `applyInstantEffect`'s `TRANSFORM` and `TRANSFORM PERMANENT`:
    /// `transformCharacter(NAME, value)` on the target, -999 for the
    /// permanent one - the caller's, in the order the effects came. A fish fed
    /// to the pet is one of these.
    struct Transform {
        std::string unit;
        float duration = 0.0f;
    };
    std::vector<Transform> transforms;
};

/// `CEquipment::useOnTarget` with the
/// target's `applyEffectOnUnit` for each
/// effect, and `performItemUse`'s bookkeeping
/// after it. `stack` is the copy's count; `uses` its uses left, counted down here.
ItemUseResult use_on_target(const ItemStats& item, int stack, int& uses, ItemUseTarget& target,
                            const UnitTypeHierarchy& unit_types, const StatBook& graphs);

/// `CEquipment::isEffectValidForUnit`, an
/// item as the target: any effect but `IDENTIFY`, which only an unidentified
/// copy takes.
bool effect_valid_for_item(const ItemStats& target, const Effect& effect);

/// An item used on another item: `useOnTarget` with the
/// target's `applyEffectOnUnit`. Only `IDENTIFY` counts as a
/// use; a use plays `USE_SOUND` and spends a use off the last of a stack, else one off the
/// stack. `remove_item` is the caller's: the copy is spent.
ItemUseResult use_on_item(const ItemStats& item, int stack, int& uses, ItemStats& target);

/// `CInventory::getEquipmentOfGuid`: the slot of the copy of unit
/// `guid` with the smallest stack, the first one met on a tie, or -1.
int equipment_of_guid(const Inventory& inventory, int64_t guid);

/// `GLOBALS.DAT`'s regeneration rates, percent of the maximum per second (`reload`).
///
struct RechargeRates {
    float hp = 0.0f;      ///< `HP_RECHARGE_RATE`
    float pet_hp = 0.0f;  ///< `PET_HP_RECHARGE_RATE`
    float mana = 0.0f;    ///< `MANA_RECHARGE_RATE`
};

/// `CCharacter::getEffectValuesOfDamage`: `HP RECHARGE` plus
/// `HP RECHARGE PLAYER`, less `DAMAGE` made negative.
float effect_values_of_damage(const CharacterEffects& effects);

/// `CCharacter::updateHP`, the change to
/// health over `dt`: the base rate (`PET_HP_RECHARGE_RATE` for an invincible
/// unit with a master, the player's pet), then the effects.
float hp_change(float max_hp, const RechargeRates& rates, const CharacterEffects& effects, float dt,
                bool invincible_pet = false);

/// `CCharacter::updateMana`, the change to
/// mana over `dt`.
float mana_change(float max_mana, const RechargeRates& rates, const CharacterEffects& effects,
                  float dt);

}  // namespace tl::core
