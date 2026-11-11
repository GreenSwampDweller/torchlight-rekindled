#include "core/item_save.h"

#include "core/combat.h"
#include "core/item_effects.h"
#include "core/item_name.h"
#include "core/item_roll.h"
#include "core/random.h"

#include <cstring>

namespace tl::core {

namespace {

// PORT: a float's bits, for the save file.
int32_t float_bits(float value) {
    int32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// PORT: a float from its bits, from the save file.
float bits_float(int32_t bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// `STRINGS::StringUpper`, ASCII. PORT: case folding for the port's narrow strings.
std::string upper_ascii(std::string text) {
    for (char& c : text) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return text;
}

}  // namespace

tl::formats::EffectSaveState effect_save_record(const LiveEffect& live) {
    // `CItemSaveState::save`, one effect of a
    // list.
    const Effect& e = live.effect;
    tl::formats::EffectSaveState r;
    r.names = {e.name, e.link_name, e.particle_fx, e.graph_override};
    r.b31 = e.exclusive ? 1 : 0;
    r.i1c = e.type;
    r.i14 = static_cast<int32_t>(e.damage_type);
    r.b33 = e.save ? 1 : 0;
    r.b32 = e.second_value ? 1 : 0;
    r.b30 = e.b30 ? 1 : 0;
    r.b37 = e.has_graphs ? 1 : 0;
    r.i20 = static_cast<int32_t>(e.activation);
    r.i10 = static_cast<int32_t>(e.level);
    // The theme's GUID (`+0x130 -> +0x30`), -1 without one.
    r.theme_guid = live.theme != nullptr ? live.theme->guid : -1;
    r.i3c = e.i3c;
    r.i40 = e.i40;
    r.i44 = e.i44;
    r.i24 = static_cast<int32_t>(e.duration);  // truncated
    r.i18 = float_bits(live.elapsed);
    r.i38 = float_bits(e.f38);
    r.ic0 = float_bits(live.value);
    for (size_t i = 0; i < r.ic4.size(); ++i) r.ic4[i] = float_bits(e.values[i]);
    return r;
}

ItemStats::SavedEffect effect_from_save_record(const tl::formats::EffectSaveState& r,
                                               const EffectTypeTable& types,
                                               const UnitThemeBook* themes) {
    // `CItemSaveState::load`, the effect loop: the value
    // constructor, then `initValues`
    // resets the rest - including +0x37, which the constructor had just set.
    ItemStats::SavedEffect saved;
    Effect& e = saved.effect;
    e.type = r.i1c;
    e.b30 = r.b30 != 0;
    e.activation = static_cast<EffectActivation>(r.i20);
    e.duration = static_cast<float>(r.i24);
    e.f38 = bits_float(r.i38);
    e.has_graphs = false;
    // `setFX`.
    e.particle_fx = r.names[2];
    if (r.b37 != 0) {
        // `CEffect::setGraphs`: its loop fills a graph only
        // while +0x37 is clear and sets it after the first, so only graph 0 is set.
        e.graph_override = r.names[3];
        const EffectTypeInfo* info = types.at(e.type);
        std::string graph = info != nullptr ? info->graphs[0] : std::string();
        if (!e.graph_override.empty()) graph = e.graph_override;
        e.graphs[0] = graph;
        e.has_graphs = true;
    }
    // The four numbers; `calculateBaseValue` after each is overwritten below.
    for (size_t i = 0; i < r.ic4.size(); ++i) e.values[i] = bits_float(r.ic4[i]);
    // The theme with that GUID, first match, or none.
    if (r.theme_guid != -1 && themes != nullptr) {
        if (const UnitTheme* theme = themes->get_unit_theme(r.theme_guid))
            e.unit_theme = theme->name;
    }
    e.damage_type = static_cast<DamageType>(r.i14);
    saved.elapsed = bits_float(r.i18);
    // The record's +0x3c goes into both +0x3c and +0x40; its +0x40 is read
    // and dropped.
    e.i3c = r.i3c;
    e.i40 = r.i3c;
    e.i44 = r.i44;
    // +0x10: -1, or anything above 1000 (unsigned), is 0.
    const uint32_t level = static_cast<uint32_t>(r.i10);
    e.level = r.i10 == -1 || level > 1000u ? 0u : level;
    e.second_value = r.b32 != 0;
    e.save = r.b33 != 0;
    e.exclusive = r.b31 != 0;
    e.name = upper_ascii(r.names[0]);
    e.link_name = upper_ascii(r.names[1]);
    saved.value = bits_float(r.ic0);
    return saved;
}

tl::formats::ItemSaveState fill_item_save_state(const ItemStats& item, int32_t count, int32_t slot,
                                                bool equipped,
                                                const EffectManager::Context& effects,
                                                const UnitTypeHierarchy& unit_types) {
    // `CEquipment::fillSaveState`. A gem's affix effects are
    // taken off the manager first (`clearOutAffixEffects`) and put
    // back last (`CEffectManager::addAffixEffectsBackIn`);
    // the port's manager is rebuilt below, so they are only left out.
    const bool gem = item_isa(item, unit_type::kSocketable, unit_types) &&
                     !item_isa(item, unit_type::kRandomMagicSocketable, unit_types);
    tl::formats::ItemSaveState state;
    // `CItem::fillSaveState`: the GUID, the display name,
    // the slot and the equipped byte; a held item's position is the constructor's.
    state.unit_guid = item.unit_guid;
    state.name = item.display_name.empty() ? item.name : item.display_name;
    state.slot = slot;
    state.b5c = equipped ? 1 : 0;
    // The unit's spawner, -1 for an item no spawner made.
    state.spawner_id = -1;
    // The unit's orientation, the identity for a held item; written as the
    // file's floats.
    for (int i = 0; i < 4; ++i) {
        const float one = 1.0f;
        std::memcpy(state.orientation.data() + 4 * (i * 5), &one, sizeof one);
    }
    state.enabled = 1;
    state.collideable = item.collideable ? 1 : 0;
    state.i170 = item.quest_guid;
    state.i178 = item.quest_unit_id;
    state.i28 = item.heirloom_level;
    state.value = count;  // the stack
    state.b75 = item.identified ? 1 : 0;
    state.i70 = item.sockets;
    state.i6c = static_cast<int32_t>(item.enchant_count);
    // The armour and the damage the copy keeps, -1 for each it has not got.
    //
    state.i7c = item.kept_armour;
    state.i78 = item.kept_damage;
    // `getFullItemName(true)` refreshes the two name parts, and the record takes them.
    // DIVERGENCE: refreshed on a copy, since the held item is const here; it shows only if an
    // item later loses the affix that worded it.
    ItemStats copy = item;
    item_full_name(copy, true, unit_types);
    state.s20 = copy.name_part_2e8;
    state.s18 = copy.name_part_2e0;
    // `createElementalDamages`, on the copy
    // for the same reason.
    create_elemental_damages(copy, effects, unit_types);
    // The socketed items, each at slot -1, not equipped.
    for (const ItemStats& socketed : item.socketed) {
        state.sockets.push_back(fill_item_save_state(socketed, 1, -1, false, effects, unit_types));
    }
    // The manager's three lists, in order, each effect copied.
    const EffectManager manager = item_effect_manager(copy, effects);
    for (int list = 0; list < 3; ++list) {
        for (const LiveEffect& live : manager.effects(static_cast<EffectActivation>(list))) {
            if (gem && !live.affix.empty()) continue;
            // The copy (`CEffect(const CEffect*)`: `clone`'s `setOwner(owner, true)`, then its own
            // `calculateBaseValue(0)`) and `setOwner(null, true)`: three rolls, after which the copy
            // takes the source's value back, so the record is the source's.
            if (effects.graphs != nullptr) {
                Effect copied = live.effect;
                roll_effect_value(copied, *effects.graphs, effects.owner, volatile_random());
                roll_effect_value(copied, *effects.graphs, effects.owner, volatile_random());
                roll_effect_value(copied, *effects.graphs, EffectOwner{}, volatile_random());
            }
            state.effects[static_cast<size_t>(list)].push_back(effect_save_record(live));
        }
    }
    // The damage bonuses, one pair per damage type, in the list's order.
    for (const int type : copy.damage_types) {
        state.pairs.push_back(
            {static_cast<float>(copy.damage_bonus[static_cast<size_t>(type)]), type});
    }
    return state;
}

void apply_item_save_state(ItemStats& copy, const tl::formats::ItemSaveState& state,
                           const EffectTypeTable& types, const UnitThemeBook* themes,
                           const EffectManager::Context& effects,
                           const UnitTypeHierarchy& unit_types, const SocketedFromSave& socketed,
                           RunicRandom* rolls) {
    // `CEquipment::applySaveState`, first
    // `CItem::applySaveState`: the name and the quest it belongs to.
    copy.from_save = true;
    copy.display_name = state.name;
    copy.quest_guid = state.i170;
    copy.quest_unit_id = state.i178;
    copy.heirloom_level = state.i28;
    copy.identified = state.b75 != 0;
    copy.name_part_2e8 = state.s20;
    copy.name_part_2e0 = state.s18;
    copy.enchant_count = static_cast<uint32_t>(state.i6c);
    copy.sockets = state.i70;
    // The kept damage and armour over the rolled ones, each unless -1.
    if (state.i78 != -1) copy.damage = copy.kept_damage = state.i78;
    if (state.i7c != -1) copy.graph_ac = copy.kept_armour = state.i7c;
    // Each socketed item with a GUID: `createUnit` and its own `applySaveState` (the caller's
    // `socketed`), then `addContainerItem`.
    for (const tl::formats::ItemSaveState& gem : state.sockets) {
        if (gem.unit_guid == -1 || !socketed) continue;
        std::optional<ItemStats> made = socketed(gem);
        if (made) add_container_item(copy, std::move(*made), unit_types);
    }
    // The three lists, each effect through `addNewEffect` with its value kept. An effect with no
    // owner gets the item (`CEffect::setOwner`), whose `calculateBaseValue(0)`
    // draws its roll before the saved value is laid back over it.
    copy.saved_effects.clear();
    for (const std::vector<tl::formats::EffectSaveState>& list : state.effects) {
        for (const tl::formats::EffectSaveState& record : list) {
            copy.saved_effects.push_back(effect_from_save_record(record, types, themes));
            if (rolls != nullptr && effects.graphs != nullptr) {
                Effect drawn = copy.saved_effects.back().effect;
                roll_effect_value(drawn, *effects.graphs, EffectOwner{}, *rolls);
            }
        }
    }
    // The damage bonuses emptied, `addDamageBonus(type, amount)` per saved pair, then
    // `createElementalDamages`.
    copy.damage_bonus = {};
    for (const std::pair<float, int32_t>& pair : state.pairs) {
        if (pair.second < 0 || kDamageTypeCount <= pair.second) continue;
        add_damage_bonus(copy, pair.second, static_cast<int>(pair.first));
    }
    create_elemental_damages(copy, effects, unit_types);
    // `recalculatePrice` and `setRequirements`;
    // a loaded copy has no owner to price it for.
    if (effects.graphs != nullptr) {
        recalculate_item_price(copy, *effects.graphs, unit_types, false);
        set_item_requirements(copy, *effects.graphs, unit_types);
    }
    // A record of version 0x19 or later ends on `calculateInherentDamages`,
    // the kept damage split again; an older one - a record kept in memory over
    // a warp, as a rule - rolls it anew (`recalculateInherentDamages`).
    // DIVERGENCE: the port's own saves, written at the PC's version (23), take the first path.
    const bool weapon = item_isa(copy, unit_type::kWeapon, unit_types);
    if (state.version >= 0x19 || state.version == tl::formats::kSaveFileVersion ||
        rolls == nullptr || effects.graphs == nullptr) {
        split_weapon_damage(copy, weapon);
    } else {
        roll_weapon_damage(copy, weapon, *effects.graphs, *rolls, copy.heirloom_level);
    }
}

}  // namespace tl::core
