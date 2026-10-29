#include "core/item_roll.h"
#include "core/level_memory.h"

#include "formats/float_convert.h"

#include "core/item_effects.h"

#include "core/combat.h"
#include "core/item_drop.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

namespace tl::core {

namespace {

/// `CBaseUnit::ISA` over the five kinds `enchant` and `canEnchant` accept:
/// Weapon, Armor, Ring, `RANDOMMAGIC SOCKETABLE` (0xa0) and Necklace, tested in that order.
bool enchantable_kind(const ItemStats& item, const UnitTypeHierarchy& unit_types) {
    return item_isa(item, unit_type::kWeapon, unit_types) ||
           item_isa(item, unit_type::kArmor, unit_types) ||
           item_isa(item, unit_type::kRing, unit_types) ||
           item_isa(item, unit_type::kRandomMagicSocketable, unit_types) ||
           item_isa(item, unit_type::kNecklace, unit_types);
}

/// The four kinds the socket roll, `addSockets` and `addEnchant` accept: the same without
/// `RANDOMMAGIC SOCKETABLE`. PORT: their shared kind test.
bool socketable_kind(const ItemStats& item, const UnitTypeHierarchy& unit_types) {
    return item_isa(item, unit_type::kWeapon, unit_types) ||
           item_isa(item, unit_type::kArmor, unit_types) ||
           item_isa(item, unit_type::kRing, unit_types) ||
           item_isa(item, unit_type::kNecklace, unit_types);
}

/// The requirement getters' shared body: the stored figure less the general reduction and the
/// first category's, never below 0. PORT: a helper of `CEquipment`'s requirement getters.
int reduced_requirement(int stored, const ItemStats& item, const RequirementReductions* character,
                        const UnitTypeHierarchy& unit_types) {
    int reduction = 0;
    if (character != nullptr) reduction = static_cast<int>(character->all);
    const float* category = nullptr;
    if (item_isa(item, 0xa2, unit_types)) {
        category = character != nullptr ? &character->martial : nullptr;
    } else if (item_isa(item, 0xa3, unit_types)) {
        category = character != nullptr ? &character->ranged : nullptr;
    } else if (item_isa(item, 0xa4, unit_types)) {
        category = character != nullptr ? &character->magic : nullptr;
    } else if (item_isa(item, unit_type::kArmor, unit_types)) {
        category = character != nullptr ? &character->armor : nullptr;
    } else if (item_isa(item, unit_type::kSpell, unit_types)) {
        category = character != nullptr ? &character->spell : nullptr;
    } else {
        // None of the five: the general reduction alone, unconverted.
        const int left = stored - reduction;
        return left < 0 ? 0 : left;
    }
    // A category with no character adds 0.0 (the getters' zero arm).
    const float extra = category != nullptr ? *category : 0.0f;
    reduction = static_cast<int>(static_cast<float>(reduction) + extra);
    const int left = stored - reduction;
    return left < 0 ? 0 : left;
}

/// `CEquipment::setRequirements`' shape for one stat: a percentage of the stat's curve at the
/// item level, less the heirloom steps, kept only above 1. PORT: a helper of it.
int stat_requirement(int authored, const char* curve, const ItemStats& item, int heirloom_cap,
                     const StatBook& graphs, const UnitTypeHierarchy& unit_types) {
    if (authored == 0) return authored;
    if (!item_isa(item, unit_type::kWeapon, unit_types) &&
        !item_isa(item, unit_type::kArmor, unit_types)) {
        return authored;
    }
    const float value = graphs.value(curve, static_cast<float>(item.level), 0.0f);
    // `fmuls` curve by the authored figure, `fmuls` by 0.01f, `floorf`.
    const float scaled = std::floor((value * static_cast<float>(authored)) * 0.01f);
    const int figure = static_cast<int>(scaled) - heirloom_cap;
    return 1 < figure ? figure : 0;
}

}  // namespace

// --------------------------------------------------------------------------
// Globals and base data

ItemRollGlobals read_item_roll_globals(const tl::formats::AdmFile& globals) {
    // `CGameGlobals::reload` over
    // `CGameGlobals::CGameGlobals`'s zeros,
    // in `reload`'s order of reads.
    ItemRollGlobals out;
    const tl::formats::AdmSection& root = globals.root;
    out.random_enchant_chance = root.float_or("RANDOM_ENCHANT_CHANCE", out.random_enchant_chance);
    out.min_random_enchant_slots = static_cast<int32_t>(root.integer_or(
        "MIN_RANDOM_ENCHANT_SLOTS", static_cast<uint32_t>(out.min_random_enchant_slots)));
    out.max_random_enchant_slots = static_cast<int32_t>(root.integer_or(
        "MAX_RANDOM_ENCHANT_SLOTS", static_cast<uint32_t>(out.max_random_enchant_slots)));
    out.min_magic_item_slots = static_cast<int32_t>(
        root.integer_or("MIN_MAGIC_ITEM_SLOTS", static_cast<uint32_t>(out.min_magic_item_slots)));
    out.max_magic_item_slots = static_cast<int32_t>(
        root.integer_or("MAX_MAGIC_ITEM_SLOTS", static_cast<uint32_t>(out.max_magic_item_slots)));
    out.min_unique_item_slots = static_cast<int32_t>(
        root.integer_or("MIN_UNIQUE_ITEM_SLOTS", static_cast<uint32_t>(out.min_unique_item_slots)));
    out.max_unique_item_slots = static_cast<int32_t>(
        root.integer_or("MAX_UNIQUE_ITEM_SLOTS", static_cast<uint32_t>(out.max_unique_item_slots)));
    out.enchanter_socket_chance =
        root.float_or("ENCHANTER_SOCKET_CHANCE", out.enchanter_socket_chance);
    out.enchanter_magic_chance =
        root.float_or("ENCHANTER_MAGIC_CHANCE", out.enchanter_magic_chance);
    out.enchanter_price_per_enchant =
        root.float_or("ENCHANTER_PRICE_PER_ENCHANT", out.enchanter_price_per_enchant);
    out.enchanter_disenchant_chance =
        root.float_or("ENCHANTER_DISENCHANT_CHANCE", out.enchanter_disenchant_chance);
    out.enchanter_max_disenchant_chance =
        root.float_or("ENCHANTER_MAX_DISENCHANT_CHANCE", out.enchanter_max_disenchant_chance);
    out.shrine_socket_chance =
        root.float_or("ENCHANTSHRINE_SOCKET_CHANCE", out.shrine_socket_chance);
    out.shrine_magic_chance = root.float_or("ENCHANTSHRINE_MAGIC_CHANCE", out.shrine_magic_chance);
    out.shrine_disenchant_chance =
        root.float_or("ENCHANTSHRINE_DISENCHANT_CHANCE", out.shrine_disenchant_chance);
    out.shrine_max_disenchant_chance =
        root.float_or("ENCHANTSHRINE_MAX_DISENCHANT_CHANCE", out.shrine_max_disenchant_chance);
    out.enchanter_max_enchants = static_cast<int32_t>(root.integer_or(
        "ENCHANTER_MAX_ENCHANTS", static_cast<uint32_t>(out.enchanter_max_enchants)));
    out.enchanter_disenchant_increase =
        root.float_or("ENCHANTER_DISENCHANT_INCREASE", out.enchanter_disenchant_increase);
    out.shrine_max_enchants = static_cast<int32_t>(root.integer_or(
        "ENCHANTSHRINE_MAX_ENCHANTS", static_cast<uint32_t>(out.shrine_max_enchants)));
    out.shrine_disenchant_increase =
        root.float_or("ENCHANTSHRINE_DISENCHANT_INCREASE", out.shrine_disenchant_increase);
    out.retirement_age = static_cast<int32_t>(
        root.integer_or("RETIREMENT_AGE", static_cast<uint32_t>(out.retirement_age)));
    out.random_socket_chance = root.float_or("RANDOM_SOCKET_CHANCE", out.random_socket_chance);
    out.second_socket_chance = root.float_or("SECOND_SOCKET_CHANCE", out.second_socket_chance);
    return out;
}

void read_item_roll_data(ItemStats& item, const tl::formats::AdmSection& section) {
    // `recalculatePrice`.
    item.value = static_cast<int>(section.integer_or("VALUE", 100));
    // `setRequirements`.
    item.authored_requirements.level = static_cast<int>(section.integer_or("LEVEL_REQUIRED", 0));
    item.authored_requirements.strength =
        static_cast<int>(section.integer_or("STRENGTH_REQUIRED", 0));
    item.authored_requirements.dexterity =
        static_cast<int>(section.integer_or("DEXTERITY_REQUIRED", 0));
    item.authored_requirements.magic = static_cast<int>(section.integer_or("MAGIC_REQUIRED", 0));
    item.authored_requirements.defense =
        static_cast<int>(section.integer_or("DEFENSE_REQUIRED", 0));
    // `getMaxSockets`, `unitInit`,
    // `enchant`.
    //
    item.max_sockets = static_cast<int>(section.integer_or("MAX_SOCKETS", 2));
    item.authored_sockets = static_cast<int>(section.integer_or("SOCKETS", 0));
    item.always_identified = section.bool_or("ALWAYS_IDENTIFIED", false);
    // `CEquipment::unitInit`, defaulting to the constructor's 0.
    item.merchant_infinite = section.bool_or("MERCHANTINFINITE", false);
    item.block_chance = static_cast<int>(section.integer_or("BLOCK_CHANCE", 0));
    // `CEquipment::getSet`.
    item.set = section.string_or("SET", "");
    // `CEquipment::unitInit`.
    item.drop_particle = section.string_or("DROPPARTICLE", "");
    // `CBaseUnit::unitInit` into `+0x1a9`,
    // true from the constructor.
    item.shadows = section.bool_or("SHADOWS", true);
    // `CBaseUnit::unitInit` and
    // `CItemGold::unitInit`.
    item.scale = section.float_or("SCALE", 1.0f);
    item.scale_variation = section.float_or("SCALE_VARIATION", 0.0f);
    item.min_value = section.float_or("MINVALUE", 100.0f);
    item.max_value = section.float_or("MAXVALUE", 100.0f);
    // `CBaseUnit::reapplyAffixes`: the
    // `AFFIXES` group's values.
    item.authored_affixes.clear();
    for (const tl::formats::AdmSection& child : section.children) {
        if (child.name != "AFFIXES") continue;
        for (const tl::formats::AdmRecord& record : child.records) {
            item.authored_affixes.push_back(record.text);
        }
        break;
    }
    // `calculateCombatStats`, the armour
    // half.
    item.armor = static_cast<int>(section.integer_or("ARMOR", 0));
    item.armor_min = static_cast<int>(section.integer_or("ARMORMIN", 0));
    item.armor_max = static_cast<int>(section.integer_or("ARMORMAX", 0));
    item.rarity_armor_mod = static_cast<int>(section.integer_or("RARITY_AMR_MOD", 100));
    item.special_armor_mod = static_cast<int>(section.integer_or("SPECIAL_AMR_MOD", 100));
    item.armor_split = {static_cast<int>(section.integer_or("ARMOR_PHYSICAL", -1)),
                        0,
                        static_cast<int>(section.integer_or("ARMOR_FIRE", 0)),
                        static_cast<int>(section.integer_or("ARMOR_ICE", 0)),
                        static_cast<int>(section.integer_or("ARMOR_ELECTRIC", 0)),
                        static_cast<int>(section.integer_or("ARMOR_POISON", 0)),
                        0};
}

// --------------------------------------------------------------------------
// Part A

// PORT: an item's unit type id (`CBaseUnit +0x1ac`) from `UNITTYPE`.
uint32_t item_unit_type(const ItemStats& item, const UnitTypeHierarchy& unit_types) {
    return unit_types.id_of(item.unittype);
}

// `CBaseUnit::ISA(type)` for an item. PORT: through the port's hierarchy.
bool item_isa(const ItemStats& item, uint32_t want, const UnitTypeHierarchy& unit_types) {
    return unit_types.isa(item_unit_type(item, unit_types), want);
}

// PORT: the value rolls of an item's roll context: its curves and `g_RandVolatile`.
EffectRolls item_effect_rolls(const ItemRollContext& context) {
    EffectRolls rolls;
    rolls.graphs = context.graphs;
    rolls.rng = context.volatile_rng;
    return rolls;
}

void destroy_item_rolls(const ItemStats& item, const EffectRolls& rolls) {
    // `~CEffectManager` -> `clearEffects(false)`
    // -> `~CAffix` -> `clearEffectsFromOwner`,
    // an affix at a time.
    if (!rolls.on()) return;
    for (const ItemStats::RolledAffix& rolled : item.affixes) {
        for (Effect effect : rolled.affix.effects) rolls.roll(effect);
    }
    for (Effect effect : item.dead_affix_effects) rolls.roll(effect);
}

void add_affix_to_item(ItemStats& item, Affix affix, uint32_t level, const EffectRolls& rolls) {
    // `CEffectManager::addAffix(CAffix*, unsigned, CBaseUnit*, float)`,
    // with no duration (-1.0). The affix stands in for
    // its template in `setLevel`: it is the template's copy at the same level.
    if (level != 0xffffffffu) affix_set_level(affix, affix, level, rolls);
    // `CAffix::setOwner(item)`, then `addEffectsToEffectManager`,
    // with no character.
    affix_set_owner(affix, static_cast<uint32_t>(item.level), rolls);
    affix_add_effects_rolls(affix, rolls);
    item.affixes.push_back(ItemStats::RolledAffix{std::move(affix), level});
}

void create_affixes_for_unit(ItemStats& item, const AffixBook& book, uint32_t level, uint32_t count,
                             RunicRandom& game_rng, const UnitTypeHierarchy& unit_types,
                             const EffectRolls& rolls) {
    // `CResourceManager::createAffixesForUnit`.
    //
    if (count == 0) return;
    std::vector<Affix> rolled = book.roll_and_create_random_affixes(
        item_unit_type(item, unit_types), level, static_cast<int32_t>(count), game_rng, unit_types,
        rolls);
    for (Affix& affix : rolled) {
        // `CBaseUnit::addAffix(CAffix*, unsigned, CBaseUnit*, float)`:
        // onto the item's effect manager at
        // `level`, with no duration of its own (-1.0).
        add_affix_to_item(item, std::move(affix), level, rolls);
    }
}

uint32_t item_passive_effect_count(const ItemStats& item) {
    // `unitInit`: `BLOCK_CHANCE` other than 0
    // becomes a passive effect of type 0x3e (`CBaseUnit::addNewEffect`).
    //
    uint32_t count = item.block_chance != 0 ? 1 : 0;
    for (const Effect& effect : item.effects) {
        if (effect.activation == EffectActivation::Passive) ++count;
    }
    for (const ItemStats::RolledAffix& rolled : item.affixes) {
        for (const Effect& effect : rolled.affix.effects) {
            if (effect.activation == EffectActivation::Passive) ++count;
        }
    }
    return count;
}

void enchant_item(ItemStats& item, bool force, const ItemRollContext& context) {
    // `CEquipment::enchant(bool)`.
    const UnitTypeHierarchy& types = *context.unit_types;
    const ItemRollGlobals& globals = *context.globals;
    RunicRandom& rolls = *context.volatile_rng;

    if (item_isa(item, unit_type::kRandomMagic, types)) force = true;
    // A unique takes the first pass only while it holds no affixes.
    const bool unique = item_isa(item, unit_type::kUnique, types);
    const bool unique_untouched = unique && item.affixes.empty();

    bool roll_first = true;
    if (!item_isa(item, unit_type::kMagic, types)) {
        if (!item.random_enchant_allowed) {
            roll_first = false;
        } else if (!force) {
            const float roll = rolls.between(0.0f, 1000.0f);
            if (globals.random_enchant_chance * 10.0f <= roll) roll_first = false;
        }
    }
    if (roll_first && (!unique || unique_untouched) && enchantable_kind(item, types)) {
        int32_t slots = 0;
        if (unique) {
            slots =
                rolls.integer_between(globals.min_unique_item_slots, globals.max_unique_item_slots);
        } else if (item_isa(item, unit_type::kMagic, types)) {
            slots =
                rolls.integer_between(globals.min_magic_item_slots, globals.max_magic_item_slots);
        } else {
            slots = rolls.integer_between(globals.min_random_enchant_slots,
                                          globals.max_random_enchant_slots);
        }
        create_affixes_for_unit(item, *context.affixes, static_cast<uint32_t>(item.level),
                                static_cast<uint32_t>(slots), *context.game_rng, types,
                                item_effect_rolls(context));
        item.identified = false;
    }

    // A unique whose effect manager holds no passive effect rolls again, one level up.
    if (unique && item_passive_effect_count(item) == 0) {
        const int32_t slots =
            rolls.integer_between(globals.min_unique_item_slots, globals.max_unique_item_slots);
        create_affixes_for_unit(item, *context.affixes, static_cast<uint32_t>(item.level + 1),
                                static_cast<uint32_t>(slots), *context.game_rng, types,
                                item_effect_rolls(context));
        item.identified = false;
    }

    if (item.sockets == 0 && context.in_level && item.random_enchant_allowed) {
        const float roll = rolls.between(0.0f, 1000.0f);
        if (roll < globals.random_socket_chance * 10.0f &&
            !item_isa(item, unit_type::kUnique, types) && socketable_kind(item, types)) {
            const float second = rolls.between(0.0f, 1000.0f);
            item.sockets = (second < globals.second_socket_chance * 10.0f ? 1 : 0) + 1;
        }
    }

    if (!item.identified && item.always_identified) item.identified = true;
    recalculate_item_price(item, *context.graphs, types, context.gambler_owner);
}

void remove_affixes_that_dont_support_unit_type(ItemStats& item, uint32_t type,
                                                const UnitTypeHierarchy& unit_types) {
    // `CEquipment::removeAffixesThatDontSupportUnitType`:
    // collect in list order, then delete
    // each.
    std::vector<size_t> doomed;
    for (size_t i = 0; i < item.affixes.size(); ++i) {
        if (!affix_can_be_applied_to_unit_type(item.affixes[i].affix, type, unit_types)) {
            doomed.push_back(i);
        }
    }
    // Deletion is by identity; track each doomed entry's current slot through
    // `CEffectManager::deleteAffix`'s
    // swap-removes.
    std::vector<size_t> slot_of(item.affixes.size());
    std::vector<size_t> owner_at(item.affixes.size());
    for (size_t i = 0; i < slot_of.size(); ++i) {
        slot_of[i] = i;
        owner_at[i] = i;
    }
    for (size_t original : doomed) {
        const size_t slot = slot_of[original];
        const size_t last = item.affixes.size() - 1;
        if (slot != last) {
            item.affixes[slot] = std::move(item.affixes[last]);
            const size_t moved = owner_at[last];
            owner_at[slot] = moved;
            slot_of[moved] = slot;
        }
        item.affixes.pop_back();
    }
}

bool item_is_magical(const ItemStats& item, const UnitTypeHierarchy& unit_types) {
    // `CEquipment::isMagical()`.
    if (item_isa(item, unit_type::kMagic, unit_types)) return true;
    if (item_isa(item, unit_type::kSocketable, unit_types)) return false;
    // Any damage type it holds a bonus of (`+0x368`): what
    // `createElementalDamages` folded out of its effects.
    for (const int type : item.damage_types) {
        if (item.damage_bonus[static_cast<size_t>(type)] != 0) return true;
    }
    if (!item.affixes.empty()) return true;
    // The passive list (`+0x28`): a loaded copy's is the saved one.
    if (!item.from_save) {
        for (const Effect& effect : item.effects) {
            if (effect.activation == EffectActivation::Passive &&
                effect.type != item_effect::kBlockChance) {
                return true;
            }
        }
    }
    for (const ItemStats::SavedEffect& saved : item.saved_effects) {
        if (saved.effect.activation == EffectActivation::Passive &&
            saved.effect.type != item_effect::kBlockChance) {
            return true;
        }
    }
    // The affixes' own effects sit in the same list, but any affix has
    // already answered above.
    return false;
}

// --------------------------------------------------------------------------
// Part B

void set_item_requirements(ItemStats& item, const StatBook& graphs,
                           const UnitTypeHierarchy& unit_types) {
    // `CEquipment::setRequirements()`.
    item.min_level_required = item.authored_requirements.level;
    const int level_steps = item.heirloom_level < 6 ? item.heirloom_level : 5;
    if (item.min_level_required == 0 && (item_isa(item, unit_type::kWeapon, unit_types) ||
                                         item_isa(item, unit_type::kArmor, unit_types) ||
                                         item_isa(item, unit_type::kTrinket, unit_types))) {
        const float value = std::floor(
            graphs.value("ITEM_LEVEL_REQUIREMENTS", static_cast<float>(item.level), 0.0f));
        const int figure = static_cast<int>(value) - level_steps;
        item.min_level_required = 1 < figure ? figure : 0;
    }
    const int stat_steps = item.heirloom_level < 11 ? item.heirloom_level : 10;
    item.strength_required =
        stat_requirement(item.authored_requirements.strength, "ITEM_STRENGTH_REQUIREMENTS", item,
                         stat_steps, graphs, unit_types);
    item.dexterity_required =
        stat_requirement(item.authored_requirements.dexterity, "ITEM_DEXTERITY_REQUIREMENTS", item,
                         stat_steps, graphs, unit_types);
    item.magic_required =
        stat_requirement(item.authored_requirements.magic, "ITEM_MAGIC_REQUIREMENTS", item,
                         stat_steps, graphs, unit_types);
    item.defense_required =
        stat_requirement(item.authored_requirements.defense, "ITEM_DEFENSE_REQUIREMENTS", item,
                         stat_steps, graphs, unit_types);
}

int item_level_requirement(const ItemStats& item, const RequirementReductions* character,
                           const UnitTypeHierarchy& unit_types) {
    // `CEquipment::getLevelRequirement(CCharacter*)`: no
    // character returns the stored figure untouched.
    if (character == nullptr) return item.min_level_required;
    return reduced_requirement(item.min_level_required, item, character, unit_types);
}

int item_strength_requirement(const ItemStats& item, const RequirementReductions* character,
                              const UnitTypeHierarchy& unit_types) {
    // `CEquipment::getStrengthRequirement(CCharacter*)`.
    return reduced_requirement(item.strength_required, item, character, unit_types);
}

int item_dexterity_requirement(const ItemStats& item, const RequirementReductions* character,
                               const UnitTypeHierarchy& unit_types) {
    // `CEquipment::getDexterityRequirement(CCharacter*)`.
    //
    return reduced_requirement(item.dexterity_required, item, character, unit_types);
}

int item_magic_requirement(const ItemStats& item, const RequirementReductions* character,
                           const UnitTypeHierarchy& unit_types) {
    // `CEquipment::getMagicRequirement(CCharacter*)`.
    //
    return reduced_requirement(item.magic_required, item, character, unit_types);
}

int item_defense_requirement(const ItemStats& item, const RequirementReductions* character,
                             const UnitTypeHierarchy& unit_types) {
    // `CEquipment::getDefenseRequirement(CCharacter*)`.
    //
    return reduced_requirement(item.defense_required, item, character, unit_types);
}

// --------------------------------------------------------------------------
// Part C

void recalculate_item_price(ItemStats& item, const StatBook& graphs,
                            const UnitTypeHierarchy& unit_types, bool gambler_owner) {
    // `CEquipment::recalculatePrice()`.
    // `isMagical` is only asked when the item is not unique; it draws nothing,
    // so asking it up front changes nothing.
    const bool unique = item_isa(item, unit_type::kUnique, unit_types);
    set_item_prices(item, graphs, unique, !unique && item_is_magical(item, unit_types),
                    gambler_owner);
}

void set_item_prices(ItemStats& item, const StatBook& graphs, bool unique, bool magical,
                     bool gambler_owner) {
    item.buy_price = 1;
    item.sell_price = 1;
    const int value = item.value;
    if (value == 0) return;
    const int level = 0 < item.level ? item.level : 1;
    const char* buy_curve = "PRICE_PLAYERBUY_NORMAL";
    const char* sell_curve = "PRICE_PLAYERSELL_NORMAL";
    if (unique) {
        buy_curve = "PRICE_PLAYERBUY_UNIQUE";
        sell_curve = "PRICE_PLAYERSELL_UNIQUE";
    } else if (magical) {
        // The vtable `+0x2b0` call, read as `isMagical`.
        buy_curve = "PRICE_PLAYERBUY_MAGIC";
        sell_curve = "PRICE_PLAYERSELL_MAGIC";
    }
    const float at = static_cast<float>(level);
    // Each price is `ceilf((value * curve) * 0.01f)` (two
    // `fmuls`, the value first).
    const float worth = static_cast<float>(value);
    const auto price = [worth](float curve) {
        return static_cast<int>(std::ceil((worth * curve) * 0.01f));
    };
    const float buy = graphs.value(buy_curve, at, 0.0f);
    const float sell = graphs.value(sell_curve, at, 0.0f);
    item.buy_price = price(buy);
    item.sell_price = price(sell);
    if (gambler_owner) {
        const float gamble = graphs.value("PRICE_PLAYERGAMBLE_MAGIC", at, 0.0f);
        item.buy_price = price(gamble);
    }
    const float normal_buy = graphs.value("PRICE_PLAYERBUY_NORMAL", at, 0.0f);
    const float normal_sell = graphs.value("PRICE_PLAYERSELL_NORMAL", at, 0.0f);
    item.buy_price_normal = price(normal_buy);
    item.sell_price_normal = price(normal_sell);
}

int item_sell_price_of(const ItemStats& item, int stack, const float* player_price_bonus) {
    // `CEquipment::sellPrice()`.
    const int count = 0 < stack ? stack : 1;
    if (!item.identified) return count * item.sell_price_normal;
    // an identified copy whose data group is the
    // unit list's `RespecPotion` sells for nothing.
    if (upper_ascii(item.name) == "RESPECPOTION") return 0;
    int total = count * item.sell_price;
    if (player_price_bonus != nullptr) {
        // the bonus times 0.01f; `fctiwz`.
        const float share = *player_price_bonus * 0.01f;
        if (share != 0.0f) total += tl::formats::ppc_fctiwz(static_cast<float>(total) * share);
    }
    return total;
}

int item_buy_price_of(const ItemStats& item, int stack, const float* player_price_bonus) {
    // `CEquipment::buyPrice()`.
    const int count = 0 < stack ? stack : 1;
    if (!item.identified) return count * item.buy_price_normal;
    int total = count * item.buy_price;
    if (player_price_bonus != nullptr && 0 < total) {
        // the bonus times 0.01f; `fctiwz`.
        const float share = *player_price_bonus * 0.01f;
        if (share != 0.0f) {
            const int reduced = total - tl::formats::ppc_fctiwz(static_cast<float>(total) * share);
            total = 0 < reduced ? reduced : 1;
        }
    }
    return total;
}

bool item_can_enchant(const ItemStats& item, const UnitTypeHierarchy& unit_types) {
    // `CEquipment::canEnchant()`.
    return enchantable_kind(item, unit_types);
}

int item_enchant_price(const ItemStats& item, const StatBook& graphs,
                       const ItemRollGlobals& globals) {
    // `CEquipment::enchantPrice()`.
    const float price = graphs.value("PRICE_ENCHANT", static_cast<float>(item.level), 0.0f);
    const int figure = static_cast<int>(globals.enchanter_price_per_enchant * price *
                                        static_cast<float>(item.enchant_count)) +
                       static_cast<int>(price);
    return figure < 300 ? 300 : figure;
}

// --------------------------------------------------------------------------
// Part D

int item_max_sockets(const ItemStats& item) {
    // `CEquipment::getMaxSockets()`.
    return item.max_sockets;
}

void add_sockets(ItemStats& item, const ItemRollContext& context) {
    // `CEquipment::addSockets()`.
    const int most = item_max_sockets(item);
    if (!(item.sockets < most) || !context.in_level) return;
    if (!socketable_kind(item, *context.unit_types)) return;
    if (item.sockets < most - 1) {
        const float roll = context.volatile_rng->between(0.0f, 1000.0f);
        item.sockets += roll < context.globals->second_socket_chance * 10.0f ? 2 : 1;
    } else {
        item.sockets += 1;
    }
}

// --------------------------------------------------------------------------
// Part F

void add_enchant(ItemStats& item, int level, int min_slots, int max_slots,
                 const ItemRollContext& context) {
    // `CEquipment::addEnchant(int, int, int)`.
    //
    const UnitTypeHierarchy& types = *context.unit_types;
    if (context.in_level && socketable_kind(item, types)) {
        const int32_t slots = context.volatile_rng->integer_between(min_slots, max_slots);
        create_affixes_for_unit(item, *context.affixes, static_cast<uint32_t>(level),
                                static_cast<uint32_t>(slots), *context.game_rng, types,
                                item_effect_rolls(context));
    }
    // `calculateEffectValues`, then `createElementalDamages`.
    //
    if (context.effects != nullptr) create_elemental_damages(item, *context.effects, types);
    set_item_requirements(item, *context.graphs, types);
    recalculate_item_price(item, *context.graphs, types, context.gambler_owner);
}

void reapply_affixes(ItemStats& item, const AffixBook& affixes, bool reapply,
                     const EffectRolls& rolls) {
    // `CBaseUnit::reapplyAffixes(bool)`. The
    // level is `LEVEL`, defaulting to `+0x100`; `ItemStats::level` is that key.
    const uint32_t level = static_cast<uint32_t>(item.level);
    const auto upper_ascii = [](std::string text) {
        for (char& c : text) {
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        }
        return text;
    };
    for (const std::string& name : item.authored_affixes) {
        if (reapply) {
            const std::string upper = upper_ascii(name);
            bool held = false;
            for (const ItemStats::RolledAffix& rolled : item.affixes) {
                if (upper_ascii(rolled.affix.name) == upper) held = true;
            }
            if (held) continue;
        }
        std::optional<Affix> affix = new_affix_by_name(affixes, name, level, rolls);
        if (affix) add_affix_to_item(item, std::move(*affix), level, rolls);
    }
}

void init_item_copy(ItemStats& item, bool from_save, bool random_enchant_allowed,
                    bool force_enchant, const ItemRollContext& context) {
    // `CResourceManager::createItem` stores the flag, then
    // calls `CEquipment::unitInit`.
    item.random_enchant_allowed = random_enchant_allowed;
    const UnitTypeHierarchy& types = *context.unit_types;
    // `CItem::unitInit` ->
    // `CBaseUnit::unitInit`: first the scale,
    // `SCALE + randomBetween(0, SCALE_VARIATION)`.
    (void)context.volatile_rng->between(0.0f, item.scale_variation);
    // A fresh copy then `reapplyEffects(false)`:
    // `CEffectManager::createEffects` makes each `ItemStats::effects` block with `createEffect`,
    // whose constructor ends in a roll.
    if (!from_save) {
        const EffectRolls rolls = item_effect_rolls(context);
        for (Effect& effect : item.effects) rolls.roll(effect);
    }
    if (!from_save || item_isa(item, unit_type::kSocketable, types)) {
        reapply_affixes(item, *context.affixes, false, item_effect_rolls(context));
    }
    item.sockets = item.authored_sockets < 2 ? item.authored_sockets : 2;
    item.affixes_before_combat = item.affixes.size();
    calculate_combat_stats(item, item_isa(item, unit_type::kWeapon, types),
                           item_isa(item, unit_type::kArmor, types), from_save, *context.graphs,
                           *context.volatile_rng, item.heirloom_level);
    set_item_requirements(item, *context.graphs, types);
    if (!from_save) {
        // A `UNIQUE` starts unidentified; after the roll a
        // `SOCKETABLE` is identified, and anything else unidentified
        // only when it authors `ALWAYS_IDENTIFIED`.
        if (item_isa(item, unit_type::kUnique, types)) item.identified = false;
        enchant_item(item, false, context);
        if (item_isa(item, unit_type::kSocketable, types) ||
            (!item.identified && item.always_identified)) {
            item.identified = true;
        }
    }
    // `calculateEffectValues`, `createElementalDamages`, then the
    // price.
    if (context.effects != nullptr) create_elemental_damages(item, *context.effects, types);
    recalculate_item_price(item, *context.graphs, types, context.gambler_owner);
    // the item's sound bank. Each sample a definition's GUID finds
    // (`CSoundBank::addSample`) leaves `g_Rand` as `setSeed(low word)`.
    // The bank itself is the renderer's.
    if (context.sounds != nullptr && context.game_rng != nullptr) {
        const auto upper_ascii = [](std::string text) {
            for (char& c : text) {
                if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
            }
            return text;
        };
        for (const std::string* sound : {&item.fall_sound, &item.land_sound, &item.take_sound,
                                         &item.attack_sound, &item.strike_sound, &item.use_sound}) {
            const SoundData* named = context.sounds->find(upper_ascii(*sound));
            if (named == nullptr || context.sounds->find_by_guid(named->guid) == nullptr) continue;
            context.game_rng->set_seed(
                static_cast<int32_t>(static_cast<uint32_t>(context.game_rng->state())));
        }
    }
    if (force_enchant) enchant_item(item, true, context);
}

// --------------------------------------------------------------------------
// Part E

GoldPile make_gold_pile(int32_t amount, RunicRandom& volatile_rng, const GoldPileSounds& bank) {
    // `CItemGold::CItemGold(CResourceManager*, int)`.
    //
    GoldPile pile;
    pile.amount = amount;
    // Its sound bank: `GOLDPICKUP` added at two slots, each
    // `CSoundBank::addSample` leaving `g_Rand` as `setSeed(low word)`.
    if (bank.sounds != nullptr && bank.game_rng != nullptr) {
        const SoundData* named = bank.sounds->find("GOLDPICKUP");
        if (named != nullptr && bank.sounds->find_by_guid(named->guid) != nullptr) {
            for (int slot = 0; slot < 2; ++slot) {
                bank.game_rng->set_seed(
                    static_cast<int32_t>(static_cast<uint32_t>(bank.game_rng->state())));
            }
        }
    }
    pile.heading = volatile_rng.between(0.0f, 360.0f);
    pile.scale = volatile_rng.between(0.8f, 1.2f);
    return pile;
}

void init_gold_copy(ItemStats& item, bool from_save, int32_t level_number, int32_t difficulty,
                    const StatBook& graphs, RunicRandom& volatile_rng, const GoldPileSounds& bank) {
    // `CResourceManager::createNewItem`:
    // `CItemGold(rm, 1)`.
    item.gold = make_gold_pile(1, volatile_rng, bank);
    // `CItemGold::unitInit` ->
    // `CItem::unitInit` ->
    // `CBaseUnit::unitInit`: `setScale(SCALE
    // + randomBetween(0, SCALE_VARIATION))`.
    const float scale = item.scale + volatile_rng.between(0.0f, item.scale_variation);
    if (from_save) return;
    const float roll = volatile_rng.between(item.min_value, item.max_value);
    if (difficulty >= 0 && difficulty <= 3) {
        const std::string graph = std::string(kGoldDropGraph) + difficulty_suffix(difficulty);
        const float curve = graphs.value(graph, static_cast<float>(level_number + 1), 0.0f);
        // `fmuls roll, 0.01f`, `fmuls` by the curve, `ceilf`, `fctiwz`.
        item.gold.amount = static_cast<int32_t>(std::ceil(curve * (roll * 0.01f)));
    }
    // `+0x230 = getScale()`, then `setScale(0.01)`: the pile grows from nothing.
    item.gold.scale = scale;
}

std::vector<int> roll_money(const LootCharacter* killer, const LootCharacter* dropper,
                            RunicRandom& volatile_rng, const LootPlacement* place) {
    // `CLevel::rollMoney(CCharacter*, CCharacter*, const Vector3&, bool)`.
    //
    std::vector<int> piles;
    if (dropper != nullptr && dropper->dont_drop_gold) return piles;
    for (int attempt = 0; attempt < 3; ++attempt) {
        // Every try rolls: the console has no `force` argument.
        const float roll = volatile_rng.between(0.0f, 1.0f);
        if (!(roll < 0.1f)) continue;
        const LootCharacter* measured = dropper != nullptr ? dropper : killer;
        int gold = 0;
        if (measured == nullptr) {
            gold = volatile_rng.integer_between(2, 12);
        } else {
            const float f = std::fma(static_cast<float>(measured->level_number), 0.25f, 1.0f);
            gold = volatile_rng.integer_between(static_cast<int>(f * 2.0f),
                                                static_cast<int>(f * 12.0f));
        }
        if (killer != nullptr) {
            // `fmuls` by the gold find, `fmuls` by -0.01f, `fctiwz`, `subf`.
            const float bonus = (killer->gold_find * static_cast<float>(gold)) * -0.01f;
            gold -= tl::formats::ppc_fctiwz(bonus);
        }
        piles.push_back(gold);
        const GoldPile pile = make_gold_pile(
            gold, volatile_rng, place != nullptr ? place->gold_sounds : GoldPileSounds{});
        const Vec3 at = place != nullptr ? place->at : Vec3{};
        const Vec3 where = random_open_item_position(
            at, 0.0f, 1.0f,
            place != nullptr ? place->map_passable : std::function<bool(const Vec3&)>{},
            volatile_rng);
        if (place != nullptr && place->add_gold) place->add_gold(pile, where);
    }
    return piles;
}

std::vector<ItemStats> create_units_by_spawn_class(
    const SpawnClass& spawn_class, uint32_t count, const SpawnCharacterContext* character,
    const SpawnCharacterContext* finder, int32_t level, int32_t flags, RunicRandom& game_rng,
    const SpawnContext& spawn_context, const CreateLootUnit& create_unit) {
    // `CResourceManager::createUnitsBySpawnClass(CSpawnClass*, ...)`.
    //
    std::vector<ItemStats> made;
    if (count == 0) return made;
    std::vector<bool> enchant_flags;  // never cleared between rolls (see the header)
    for (uint32_t roll = 0; roll < count; ++roll) {
        std::vector<std::string> units;
        spawn_class.roll_spawn_class(units, &enchant_flags, character, finder, level, 0xffffffffu,
                                     0, -1, 0, static_cast<uint32_t>(flags), game_rng,
                                     spawn_context);
        for (size_t i = 0; i < units.size(); ++i) {
            // The flags list read as `TArrayList` reads: past the end, entry 0.
            bool force = false;
            if (!enchant_flags.empty()) {
                force = i < enchant_flags.size() ? enchant_flags[i] : enchant_flags[0];
            }
            ItemStats unit;
            if (create_unit && create_unit(units[i], level, force, unit)) {
                made.push_back(std::move(unit));
            }
        }
    }
    return made;
}

LootDrop roll_treasure(const SpawnClass* barrel_treasure, int32_t level_number,
                       const LootCharacter* killer, const LootCharacter* dropper,
                       RunicRandom& volatile_rng, RunicRandom& game_rng,
                       const SpawnContext& spawn_context, const CreateLootUnit& create_unit,
                       const LootPlacement* place) {
    // `CLevel::rollTreasure(CCharacter*, CCharacter*, const Vector3&)`,
    // through `createUnitsBySpawnClass(const
    // std::wstring&, ...)`; `rollMoney`'s force flag clear.
    LootDrop drop;
    if (barrel_treasure != nullptr) {
        drop.items = create_units_by_spawn_class(
            *barrel_treasure, 1, nullptr, killer != nullptr ? &killer->spawn : nullptr,
            level_number, 0, game_rng, spawn_context, create_unit);
    }
    // `addUnit(unit, position)` for each, in the order they were made.
    if (place != nullptr && place->add_unit) {
        for (const ItemStats& unit : drop.items) place->add_unit(unit, place->at);
    }
    drop.gold = roll_money(killer, dropper, volatile_rng, place);
    return drop;
}

Vec3 spawned_unit_position(const ItemStats& unit, uint32_t index, const Vec3& at,
                           const LootPlacement* place, const UnitTypeHierarchy* unit_types,
                           RunicRandom& volatile_rng) {
    // `createUnitsBySpawnClassAtPositionInLevel`: the
    // radius is `i / 3` when above 0.5.
    const float third = static_cast<float>(index) / 3.0f;
    const float radius = 0.5f < third ? third : 0.5f;
    std::function<bool(const Vec3&)> open;
    if (place != nullptr) {
        open = [place](const Vec3& p) {
            return (!place->map_passable || place->map_passable(p)) &&
                   (!place->position_passable || place->position_passable(p));
        };
    }
    const Vec3 point = random_open_position(at, 0.0f, radius, open, volatile_rng);
    const bool takeable =
        unit_types != nullptr ? item_isa(unit, unit_type::kTakeable, *unit_types) : true;
    const bool gold =
        unit_types != nullptr ? item_isa(unit, unit_type::kGold, *unit_types) : unit.is_gold();
    if (takeable && !gold) return at;
    return Vec3{point.x, at.y + 0.5f, point.z};
}

std::vector<ItemStats> create_units_at_position(const SpawnClass& spawn_class, uint32_t count,
                                                const SpawnCharacterContext* character,
                                                const SpawnCharacterContext* finder, int32_t level,
                                                RunicRandom& volatile_rng, RunicRandom& game_rng,
                                                const SpawnContext& spawn_context,
                                                const CreateLootUnit& create_unit,
                                                const LootPlacement* place) {
    // `CResourceManager::createUnitsBySpawnClassAtPositionInLevel`:
    // `createUnitsBySpawnClass` with flags
    // 0, then each unit is placed.
    std::vector<ItemStats> made = create_units_by_spawn_class(
        spawn_class, count, character, finder, level, 0, game_rng, spawn_context, create_unit);
    const Vec3 at = place != nullptr ? place->at : Vec3{};
    for (size_t i = 0; i < made.size(); ++i) {
        const Vec3 where = spawned_unit_position(made[i], static_cast<uint32_t>(i), at, place,
                                                 spawn_context.unit_types, volatile_rng);
        if (place != nullptr && place->add_unit) place->add_unit(made[i], where);
    }
    return made;
}

LootDrop roll_death_drop(const DyingCharacter& dying, const LootCharacter* killer,
                         const SpawnClass* barrel_treasure, RunicRandom& volatile_rng,
                         RunicRandom& game_rng, const SpawnContext& spawn_context,
                         const CreateLootUnit& create_unit, const LootPlacement* place) {
    // The end of `CCharacter::die`.
    LootDrop drop;
    if (dying.is_player || dying.alignment == 1 || killer == nullptr || dying.no_drops) {
        return drop;
    }
    if (dying.treasure.spawn_class == nullptr) {
        return roll_treasure(barrel_treasure, dying.loot.level_number, killer, &dying.loot,
                             volatile_rng, game_rng, spawn_context, create_unit, place);
    }
    const int32_t count = volatile_rng.integer_between(dying.treasure.min, dying.treasure.max);
    // `die` hands it the dying character, the killer and level -1, which it
    // turns into the level's own number (`CLevel +0x1a8`).
    drop.items =
        create_units_at_position(*dying.treasure.spawn_class, static_cast<uint32_t>(count),
                                 &dying.loot.spawn, &killer->spawn, dying.loot.level_number,
                                 volatile_rng, game_rng, spawn_context, create_unit, place);
    // `rollMoney`'s force flag clear.
    drop.gold = roll_money(killer, &dying.loot, volatile_rng, place);
    return drop;
}

}  // namespace tl::core
