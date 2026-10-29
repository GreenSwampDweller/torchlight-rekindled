// The gameplay half of render/renderer_merchants.cpp (core/world.h).
#include "core/item_data.h"
#include "core/unit_resource.h"
#include "core/world.h"
#include "formats/unit_data.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace tl::core {

// PORT: a merchant as `populate_new_merchants_inventory` reads it, from its unit data.
tl::core::MerchantCharacter World::merchant_character(MerchantState& state, int64_t node,
                                                      const tl::core::UnitStats& stats) {
    if (!loot_tables_built) {
        loot_tables_built = true;
        build_spawn_context(loot_tables, /*items_by_type=*/true);
    }
    const tl::core::SpawnContext& context = loot_tables.context;
    tl::core::MerchantCharacter character;
    character.inventory = &state.inventory;
    character.is_merchant = true;
    character.is_gambler = state.gambler;
    character.data.player_level_factor = stats.merchant_level_factor;
    character.data.allows_uniques = stats.allows_merch_uniques;
    character.data.allows_magic = stats.allows_merch_magic;
    character.data.default_tab = stats.merchant_tab;
    if (!stats.treasure_class.empty() && context.get_spawn_class) {
        character.treasure.spawn_class = context.get_spawn_class(stats.treasure_class);
    }
    character.treasure.min = stats.treasure_min;
    character.treasure.max = stats.treasure_max;
    // The merchant is both the character and the finder of its own roll.
    character.spawn.is_merchant = true;
    character.spawn.level = level_monster_level;
    return character;
}

// PORT: the tables a merchant's stock roll reads, gathered.
void World::with_merchant_stock_context(
    const std::function<void(const tl::core::MerchantStockContext&)>& fn) {
    if (!loot_tables_built) {
        loot_tables_built = true;
        build_spawn_context(loot_tables, /*items_by_type=*/true);
    }
    const tl::core::ItemRollContext roll = item_roll_context();
    const uint32_t player_level = static_cast<uint32_t>(std::max(0, player_character.level()));
    tl::core::MerchantStockContext stock;
    stock.player_level = &player_level;
    stock.spawn_context = &loot_tables.context;
    stock.volatile_rng = &tl::core::volatile_random();
    stock.game_rng = &tl::core::game_random();
    stock.create_unit = loot_unit_maker();
    stock.roll = &roll;
    fn(stock);
}

void World::stock_merchant(MerchantState& state, int64_t node, const tl::core::UnitStats& stats) {
    // `CCharacter::unitInit`, ISA Merchant:
    // `CInventory(this, 0x2a)` and `addSection(3, 0x2a)`, `addSection(4,
    // 0x2a)`, then `generateMerchantInventory` when `+0x6c0` names a class.
    state.inventory = tl::core::make_merchant_inventory();
    tl::core::MerchantCharacter character = merchant_character(state, node, stats);
    if (character.treasure.spawn_class == nullptr) {
        // Not stocked (`unitInit` asks for `+0x6c0`); still a merchant.
        state.inventory.set_stacking(false);
        return;
    }
    tl::core::MerchantStockOutcome outcome;
    with_merchant_stock_context([&](const tl::core::MerchantStockContext& stock) {
        outcome = tl::core::generate_merchant_inventory(character, stock);
    });
    TL_LOG_INFO(
        "merchant: '%s' stocks %zu item(s) from '%s' (%u rolled at level %d, %u "
        "discarded)%s",
        state.unit.c_str(), outcome.slots.size(), stats.treasure_class.c_str(), outcome.count,
        outcome.level, outcome.discarded, state.gambler ? ", gambler" : "");
}

// `CCharacter::unitInit` of a character the population makes: a merchant is
// stocked as it is made (`generateMerchantInventory`, after
// `createDefaultEquipment`), held by `handle` until it is placed (`place_made_merchant`).
void World::stock_made_merchant(int64_t handle, const std::string& unit) {
    load_unit_types();
    const tl::core::UnitStats stats = stats_for_unit(unit);
    const uint32_t type = unit_types.id_of(stats.unittype);
    if (!unit_types.isa(type, tl::core::unit_type::kMerchant)) return;
    MerchantState& state = made_merchants[handle];
    state.unit = unit;
    state.gambler = unit_types.isa(type, tl::core::unit_type::kGambler);
    stock_merchant(state, handle, stats);
}

// The merchant `stock_made_merchant` held for `handle`, now the character `node`.
void World::place_made_merchant(int64_t handle, int64_t node) {
    const auto found = made_merchants.find(handle);
    if (found == made_merchants.end()) return;
    merchants[node] = std::move(found->second);
    merchant_checked.insert(node);
    made_merchants.erase(found);
}

// PORT: the level's merchants handed to `populate_new_merchants_inventory`, which holds the rule.
void World::populate_new_merchants() {
    load_unit_types();
    std::vector<tl::core::MerchantCharacter> characters;
    characters.reserve(merchants.size());
    for (auto& entry : merchants) {
        characters.push_back(
            merchant_character(entry.second, entry.first, stats_for_unit(entry.second.unit)));
    }
    std::vector<tl::core::MerchantCharacter*> list;
    list.reserve(characters.size());
    for (tl::core::MerchantCharacter& c : characters) list.push_back(&c);
    int restocked = 0;
    with_merchant_stock_context([&](const tl::core::MerchantStockContext& stock) {
        restocked = tl::core::populate_new_merchants_inventory(list, stock);
    });
    if (restocked > 0) TL_LOG_INFO("merchant: %d empty merchant(s) restocked", restocked);
}

void World::restore_merchant(const tl::formats::CharacterSaveState& saved) {
    // `CCharacter::applySaveState` on a
    // merchant.
    load_unit_types();
    const tl::core::UnitStats stats = stats_for_unit(saved.name);
    const uint32_t type = unit_types.id_of(stats.unittype);
    if (!unit_types.isa(type, tl::core::unit_type::kMerchant)) return;
    MerchantState state;
    state.unit = saved.name;
    state.gambler = unit_types.isa(type, tl::core::unit_type::kGambler);
    state.clock = saved.clock;  // `+0x388 = +0xf8`
    merchant_checked.insert(saved.object_id);
    if (tl::core::merchant_restocks_on_restore(state.clock,
                                               level_memory_rules().merchant_respawn_time)) {
        state.clock = 0.0f;
        stock_merchant(state, saved.object_id, stats);
        TL_LOG_INFO("merchant: '%s' restocked on return", state.unit.c_str());
        merchants[saved.object_id] = std::move(state);
        return;
    }
    const std::string key = level_dungeon + ":" + std::to_string(level_depth);
    const auto remembered = remembered_merchants.find(key);
    if (remembered != remembered_merchants.end()) {
        const auto kept = remembered->second.find(saved.object_id);
        if (kept != remembered->second.end()) {
            state.inventory = kept->second.inventory;
            merchants[saved.object_id] = std::move(state);
            return;
        }
    }
    // The saved items, each made again and put back in its slot; a gambler's
    // repriced as its own.
    ensure_item_index();
    load_stat_graphs();
    for (const tl::formats::ItemSaveState& item : saved.inventory) {
        const tl::core::ItemStats* data = items.find(item.name);
        if (data == nullptr) continue;
        tl::core::ItemStats copy = create_item(*data, true, true);
        const int count = std::max(1, item.value);
        int landed = item.slot;
        if (item.slot < 0 || !state.inventory.pickup_into_slot(copy, count, item.slot)) {
            landed = state.inventory.pickup(copy, count, unit_types);
        }
        if (landed < 0) continue;
        if (state.gambler) {
            if (tl::core::ItemStats* held =
                    state.inventory.item_in_slot(static_cast<size_t>(landed))) {
                tl::core::recalculate_item_price(*held, stat_graphs, unit_types, true);
            }
        }
    }
    state.inventory.set_stacking(false);
    merchants[saved.object_id] = std::move(state);
}

void World::step_merchants(float delta) {
    // `CCharacter::unitInit`'s Merchant branch, once per placed character.
    // It needs the player: the stock's level is a share of the player's.
    if (subject != nullptr) {
        for (const auto& entry : scene_units) {
            const SceneUnit& su = entry.second;
            if (su.entity == nullptr || su.node == 0) continue;
            if (merchants.count(su.node) != 0 || merchant_checked.count(su.node) != 0) continue;
            merchant_checked.insert(su.node);
            load_unit_types();
            const tl::core::UnitStats stats = stats_for_unit(entry.first);
            const uint32_t type = unit_types.id_of(stats.unittype);
            if (!unit_types.isa(type, tl::core::unit_type::kMerchant)) continue;
            MerchantState& state = merchants[su.node];
            state.unit = entry.first;
            state.gambler = unit_types.isa(type, tl::core::unit_type::kGambler);
            stock_merchant(state, su.node, stats);
        }
    }
    // `CCharacter::update`: `+0x388 += dt` while the level is live.
    for (auto& entry : merchants) entry.second.clock += delta;
}

// PORT: a merchant's state found by its unit's name in the port's scene.
World::MerchantState* World::merchant_named(const std::string& unit) {
    step_merchants(0.0f);
    const auto range = scene_units.equal_range(unit);
    for (auto it = range.first; it != range.second; ++it) {
        const auto found = merchants.find(it->second.node);
        if (found != merchants.end()) return &found->second;
    }
    return nullptr;
}

}  // namespace tl::core
