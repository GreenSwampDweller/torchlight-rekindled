// An item copy's effects. See `item_effects.h`.
#include "core/item_effects.h"

#include "core/affix_book.h"
#include "core/combat.h"
#include "core/item_roll.h"

#include <algorithm>

#include <utility>

namespace tl::core {

Effect item_block_chance_effect(int block_chance) {
    // `CEquipment::unitInit`.
    const float value = static_cast<float>(block_chance);
    return value_effect(item_effect::kBlockChance, kDurationAlways, value,
                        EffectActivation::Passive,
                        /*flag30=*/0.0f < value, /*f38=*/1.0f);
}

namespace {

/// The copy's manager as `unitInit` fills it, its lists in the order the
/// effects were added - and, when `ordered`, the `PASSIVE` list put back in the
/// order the console's removals left it (`ItemStats::passive_order`).
EffectManager build_item_manager(const ItemStats& item, const EffectManager::Context& context,
                                 const std::function<void()>& skill_blocks, bool ordered) {
    EffectManager manager;
    manager.set_context(context);
    const auto add_affix = [&](const ItemStats::RolledAffix& rolled) {
        // `CAffix::addEffectsToEffectManager` on an item:
        // `isEffectValidForUnit` and `applyEffectOnUnit`
        // keep type 0x2e off the list.
        Affix affix = rolled.affix;
        std::vector<Effect> kept;
        for (Effect& effect : affix.effects) {
            if (effect.type != kEffectTypeIdentifiesItem) kept.push_back(std::move(effect));
        }
        affix.effects = std::move(kept);
        // `CBaseUnit::addAffix` ->
        // `CEffectManager::addAffix` at the
        // rolled level, no duration of its own.
        manager.add_affix(affix, rolled.level, -1.0f);
    };
    const size_t split = std::min(item.affixes_before_combat, item.affixes.size());
    // `CBaseUnit::unitInit`: `reapplyEffects`
    // only when not from a save, the
    // `[SKILL]` blocks, then `reapplyAffixes` - not from a
    // save, or a `SOCKETABLE`.
    if (!item.from_save) {
        for (const Effect& effect : item.effects) manager.add_effect(effect);
    }
    if (skill_blocks) skill_blocks();
    for (size_t i = 0; i < split; ++i) add_affix(item.affixes[i]);
    // Then `CEquipment::unitInit`: the `BLOCK_CHANCE`
    // effect, `calculateCombatStats`, and `enchant`'s
    // affixes - the rest of `affixes`.
    if (!item.from_save && item.block_chance != 0) {
        manager.add_effect(item_block_chance_effect(item.block_chance));
    }
    for (const Effect& effect : item.combat_effects) manager.add_effect(effect);
    // A loaded copy: `CEquipment::applySaveState` puts each
    // saved effect back and lays the saved value over the computed one.
    // INVENTED: the saved effects go before the saved affixes; only the float
    // sum order depends on it.
    for (const ItemStats::SavedEffect& saved : item.saved_effects) {
        LiveEffect& live = manager.add_effect(saved.effect);
        live.value = saved.value;
        live.elapsed = saved.elapsed;
    }
    for (size_t i = split; i < item.affixes.size(); ++i) add_affix(item.affixes[i]);
    if (ordered && !item.passive_order.empty()) {
        manager.reorder(EffectActivation::Passive, item.passive_order);
    }
    manager.calculate_effect_values();
    return manager;
}

}  // namespace

EffectManager item_effect_manager(const ItemStats& item, const EffectManager::Context& context,
                                  const std::function<void()>& skill_blocks) {
    return build_item_manager(item, context, skill_blocks, /*ordered=*/true);
}

void destroy_item_effects(const ItemStats& item, const EffectManager::Context& context,
                          const EffectRolls& rolls) {
    // `CAffix::clearEffectsFromOwner` rolls every effect of the affix's own list
    // (+0xb0), the dead elemental ones too, which are no longer on the manager. Only the number of
    // draws is kept: the values die with the item.
    item_effect_manager(item, context).destroy(rolls);
    for (Effect effect : item.dead_affix_effects) rolls.roll(effect);
}

float item_effect_value(const ItemStats& item, const EffectManager::Context& context,
                        EffectActivation activation, int type, int damage_type) {
    // `CBaseUnit::getEffectValue(EEFFECT_ACTIVATION, EEFFECT_TYPE, float,
    // EDAMAGE_TYPES)`; the float is not read.
    return item_effect_manager(item, context).effect_value(activation, type, damage_type);
}

float item_effect_value(const ItemStats& item, const EffectManager::Context& context, int type,
                        const std::string& name) {
    // `CBaseUnit::getEffectValue(EEFFECT_TYPE, float, const std::wstring&)`.
    //
    return item_effect_manager(item, context).effect_value(type, name);
}

namespace {

/// What `createElementalDamages` and `getDamageBonus` look for on a manager's
/// first list: a passive `DAMAGE` (0x34) or `DAMAGE BONUS` (0xa).
bool elemental_damage_effect(const Effect& effect) {
    return effect.activation == EffectActivation::Passive &&
           (effect.type == 0x34 || effect.type == 0xa);
}

}  // namespace

// `CEquipment::createElementalDamages()`.
void create_elemental_damages(ItemStats& item, const EffectManager::Context& context,
                              const UnitTypeHierarchy& unit_types) {
    if (!item_isa(item, unit_type::kWeapon, unit_types)) return;
    const EffectManager manager = item_effect_manager(item, context);
    bool folded = false;
    for (const LiveEffect& live : manager.effects(EffectActivation::Passive)) {
        if (!elemental_damage_effect(live.effect)) continue;
        add_damage_bonus(item, static_cast<int>(live.effect.damage_type),
                         static_cast<int>(live.value));
        folded = true;
    }
    if (!folded) return;
    // `deleteDeadEffects` moves the last entry into each
    // removed one's place; the order it leaves is kept in `passive_order`.
    {
        const EffectManager unordered = build_item_manager(item, context, {}, /*ordered=*/false);
        const std::vector<LiveEffect>& natural = unordered.effects(EffectActivation::Passive);
        const size_t n = natural.size();
        std::vector<uint32_t> current(n);
        for (size_t i = 0; i < n; ++i) current[i] = static_cast<uint32_t>(i);
        if (item.passive_order.size() <= n) {
            for (size_t i = 0; i < item.passive_order.size(); ++i)
                current[i] = item.passive_order[i];
        }
        std::vector<bool> killed(n);
        for (size_t k = 0; k < n; ++k) killed[k] = elemental_damage_effect(natural[k].effect);
        size_t i = 0;
        while (i < current.size()) {
            if (!killed[current[i]]) {
                ++i;
                continue;
            }
            current[i] = current.back();
            current.pop_back();
        }
        std::vector<uint32_t> after(n);
        uint32_t kept = 0;
        for (size_t k = 0; k < n; ++k) {
            after[k] = kept;
            if (!killed[k]) ++kept;
        }
        std::vector<uint32_t> order;
        bool identity = true;
        for (size_t j = 0; j < current.size(); ++j) {
            order.push_back(after[current[j]]);
            if (order.back() != j) identity = false;
        }
        item.passive_order = identity ? std::vector<uint32_t>{} : order;
    }
    // The effects are dead and deleted: off each list `item_effect_manager`
    // puts on the manager.
    const auto drop = [](std::vector<Effect>& list) {
        list.erase(std::remove_if(list.begin(), list.end(), elemental_damage_effect), list.end());
    };
    if (!item.from_save) drop(item.effects);
    drop(item.combat_effects);
    item.saved_effects.erase(std::remove_if(item.saved_effects.begin(), item.saved_effects.end(),
                                            [](const ItemStats::SavedEffect& saved) {
                                                return elemental_damage_effect(saved.effect);
                                            }),
                             item.saved_effects.end());
    for (ItemStats::RolledAffix& rolled : item.affixes) {
        // The affix keeps them, dead (see `ItemStats::dead_affix_effects`).
        for (const Effect& effect : rolled.affix.effects)
            if (elemental_damage_effect(effect)) item.dead_affix_effects.push_back(effect);
        drop(rolled.affix.effects);
    }
}

// `CEquipment::clearDamageBonuses()`.
void clear_damage_bonuses(ItemStats& item, const EffectManager::Context& context,
                          const UnitTypeHierarchy& unit_types) {
    item.damage_types.clear();
    item.damage_bonus = {};
    item.inherent_damage = {};
    create_elemental_damages(item, context, unit_types);
}

// `CEquipment::getDamageBonus(EDAMAGE_TYPES)`.
//
int weapon_damage_bonus(const ItemStats& item, int type, const EffectManager::Context& context) {
    int total = 0;
    for (const int held : item.damage_types) {
        if (held != type) continue;
        total += item.damage_bonus[static_cast<size_t>(type)];
        total += item.inherent_damage[static_cast<size_t>(type)];
    }
    for (const ItemStats& socketed : item.socketed) {
        const EffectManager manager = item_effect_manager(socketed, context);
        for (const LiveEffect& live : manager.effects(EffectActivation::Passive)) {
            if (!elemental_damage_effect(live.effect)) continue;
            if (static_cast<int>(live.effect.damage_type) != type) continue;
            total = static_cast<int>(static_cast<float>(total) + live.value);
        }
    }
    return total;
}

bool add_container_item(ItemStats& host, ItemStats item, const UnitTypeHierarchy& unit_types) {
    // `CEquipment::addContainerItem(CEquipment*)`.
    //
    if (!(static_cast<uint32_t>(host.socketed.size()) < static_cast<uint32_t>(host.sockets))) {
        return false;
    }
    if (item_isa(item, unit_type::kSocketable, unit_types) &&
        !item_isa(item, unit_type::kRandomMagicSocketable, unit_types)) {
        remove_affixes_that_dont_support_unit_type(item, item_unit_type(host, unit_types),
                                                   unit_types);
    }
    host.socketed.push_back(std::move(item));
    return true;
}

}  // namespace tl::core
