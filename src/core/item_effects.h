// What one item copy is worth in effects: the `CEffectManager` an item keeps, and the two
// `CBaseUnit::getEffectValue` overloads the inventory reads it through. An `ItemStats` carries
// no manager; it is rebuilt from the copy on demand by the units' own `EffectManager`.
#pragma once

#include "core/effect_manager.h"
#include "core/unit_types.h"
#include "core/units.h"

#include <functional>
#include <string>

namespace tl::core {

/// The effect type an item's manager never holds: `CEquipment::isEffectValidForUnit`
/// refuses it, and `CEquipment::applyEffectOnUnit`
/// turns it into identifying the copy.
inline constexpr int kEffectTypeIdentifiesItem = 0x2e;

/// The effect `CEquipment::unitInit` adds for a non-zero
/// `BLOCK_CHANCE` on a fresh copy (`CEffect::CEffect`;
/// `CBaseUnit::addNewEffect`).
Effect item_block_chance_effect(int block_chance);

/// An item copy's `CEffectManager`, rebuilt: the copy's own `[EFFECT]` blocks
/// (`ItemStats::effects`), the `BLOCK_CHANCE` effect, then every affix in list order through
/// `EffectManager::add_affix` (`CBaseUnit::addAffix`;
/// `CAffix::addEffectsToEffectManager`). `context.owner`
/// should say no owner: an affix's owner on an item is the item. `skill_blocks` runs where
/// `CBaseUnit::unitInit` reads the `[SKILL]` blocks.
EffectManager item_effect_manager(const ItemStats& item, const EffectManager::Context& context,
                                  const std::function<void()>& skill_blocks = {});

/// The draws an item's deletion makes: its manager destroyed (`~CEffectManager`),
/// and with it each affix's dead elemental effects (`ItemStats::dead_affix_effects`).
void destroy_item_effects(const ItemStats& item, const EffectManager::Context& context,
                          const EffectRolls& rolls);

/// `CBaseUnit::getEffectValue(EEFFECT_ACTIVATION, EEFFECT_TYPE, float,
/// EDAMAGE_TYPES)`: the item's manager
/// narrowed to one activation's list (`CEffectManager::getEffectValue`);
/// 0 without a manager.
float item_effect_value(const ItemStats& item, const EffectManager::Context& context,
                        EffectActivation activation, int type, int damage_type);

/// `CBaseUnit::getEffectValue(EEFFECT_TYPE, float, const std::wstring&)`:
/// the by-name form
/// (`CEffectManager::getEffectValue`).
float item_effect_value(const ItemStats& item, const EffectManager::Context& context, int type,
                        const std::string& name);

/// `CEquipment::createElementalDamages()`, for a weapon:
/// every passive `DAMAGE` or `DAMAGE BONUS` effect of the copy's manager is moved into the
/// copy's damage bonus of its damage type and taken off whichever list it came from.
void create_elemental_damages(ItemStats& item, const EffectManager::Context& context,
                              const UnitTypeHierarchy& unit_types);

/// `CEquipment::clearDamageBonuses()`: the
/// three lists emptied - the inherent damage with the rest - then
/// `createElementalDamages`.
void clear_damage_bonuses(ItemStats& item, const EffectManager::Context& context,
                          const UnitTypeHierarchy& unit_types);

/// `CEquipment::getDamageBonus(EDAMAGE_TYPES)`:
/// the copy's bonus and inherent damage of
/// the type, then every socketed item's passive `DAMAGE` and `DAMAGE BONUS`
/// effects of it, each added as a float and the sum cut back to an integer.
int weapon_damage_bonus(const ItemStats& item, int type, const EffectManager::Context& context);

/// `CEquipment::addContainerItem(CEquipment*)`: the host
/// takes the item while it has a free socket; a plain `SOCKETABLE` then loses every affix the
/// host cannot take (`removeAffixesThatDontSupportUnitType`).
/// Returns whether it went in. Callers: `CGameUI::menuItemClick`,
/// `CEquipment::applySaveState`.
bool add_container_item(ItemStats& host, ItemStats item, const UnitTypeHierarchy& unit_types);

}  // namespace tl::core
