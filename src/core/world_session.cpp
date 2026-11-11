// The gameplay half of render/renderer_session.cpp (core/world.h).
#include "core/saved_orientation.h"
#include "core/world.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <unordered_map>

namespace tl::core {

namespace {

// A name the restore takes for a pile of gold: `N Gold`, or none.
bool saved_gold_name(const std::string& name) {
    return name.empty() || (name.size() > 5 && name.compare(name.size() - 5, 5, " Gold") == 0 &&
                            std::isdigit(static_cast<unsigned char>(name[0])) != 0);
}

}  // namespace

tl::core::LevelMemoryRules World::level_memory_rules() const {
    tl::core::LevelMemoryRules rules;
    const std::vector<uint8_t> bytes = read_resource("media/GLOBALS.DAT.adm");
    if (bytes.empty()) return rules;
    const tl::formats::AdmFile globals = parse_data_group(bytes.data(), bytes.size());
    if (!globals.ok) return rules;
    rules.max_level_memory = static_cast<int32_t>(
        globals.integer_or("MAX_LEVEL_MEMORY", static_cast<uint32_t>(rules.max_level_memory)));
    rules.level_respawn_time = static_cast<int32_t>(
        globals.integer_or("LEVEL_RESPAWN_TIME", static_cast<uint32_t>(rules.level_respawn_time)));
    rules.merchant_respawn_time = static_cast<int32_t>(globals.integer_or(
        "MERCHANT_RESPAWN_TIME", static_cast<uint32_t>(rules.merchant_respawn_time)));
    return rules;
}

std::optional<tl::core::ItemStats> World::item_from_save(const tl::formats::ItemSaveState& item) {
    ensure_item_index();
    ensure_effect_data();
    load_unit_types();
    load_stat_graphs();
    load_unit_themes();
    // `createUnit(+0x60, ...)`: the item by its GUID. INVENTED: a record this
    // port wrote before it saved the GUID has -1 there and the item's `NAME`
    // in +0x10, so the name is tried next, then as a display name.
    const tl::core::ItemStats* found =
        item.unit_guid != -1 ? items.find_by_guid(item.unit_guid) : nullptr;
    if (found == nullptr) found = items.find(item.name);
    if (found == nullptr) found = items.find_by_display_name(item.name);
    if (found == nullptr) {
        TL_LOG_WARN("save: no item called '%s'", item.name.c_str());
        return std::nullopt;
    }
    // `CResourceManager::createUnit(guid, level, true, false)`:
    // `createItem` by GUID from a save with no random enchant allowed;
    // `unitInit` rolls as usual, but adds no
    // `[EFFECT]` blocks, no `BLOCK_CHANCE` and no enchant.
    tl::core::ItemStats copy = create_item(*found, true, false);
    // CEquipment::applySaveState. INVENTED: a record
    // without a GUID keeps the name the lookup found it by.
    tl::core::apply_item_save_state(
        copy, item, effect_types, &unit_themes, item_effect_context(), unit_types,
        [this](const tl::formats::ItemSaveState& socketed) { return item_from_save(socketed); },
        &tl::core::volatile_random());
    // `CItem::applySaveState`: the name (`+0x1b0`) is the record's (+0x10),
    // a quest's own over the data's.
    copy.display_name = item.name;
    if (!copy.saved_effects.empty() || !copy.socketed.empty() || copy.enchant_count != 0) {
        TL_LOG_INFO(
            "save: '%s' back with %zu effect(s), %zu of %d socket(s) filled, "
            "enchanted %u time(s), armour %d, damage %d",
            copy.name.c_str(), copy.saved_effects.size(), copy.socketed.size(), copy.sockets,
            copy.enchant_count, copy.graph_ac, copy.damage);
    }
    return copy;
}

// PORT: an item's save record gathered for `fill_item_save_state`, which holds the rule.
tl::formats::ItemSaveState World::item_save_state(const tl::core::InventorySlot& held,
                                                  int32_t slot) {
    ensure_effect_data();
    load_unit_types();
    load_stat_graphs();
    load_unit_themes();
    // The item's own manager, as `Inventory` builds it (an item is not a
    // character), with the theme table so a theme's GUID can be written.
    tl::core::EffectManager::Context context;
    context.types = &effect_types;
    context.graphs = &stat_graphs;
    context.owner.present = false;
    context.theme_book = &unit_themes;
    const bool equipped =
        slot >= 0 && tl::core::Inventory::is_equip_location(static_cast<size_t>(slot));
    return tl::core::fill_item_save_state(held.item, held.count, slot, equipped, context,
                                          unit_types);
}

// `CCinematics::reload`: every `*.dat` under
// `media/cinematics/`.
void World::load_cinematics() {
    if (cinematics_loaded) return;
    cinematics_loaded = true;
    for (const std::string& path : data_paths_under("media/cinematics/")) {
        const std::vector<uint8_t> bytes = read_resource(path);
        if (bytes.empty()) continue;
        const tl::formats::AdmFile file = parse_data_group(bytes.data(), bytes.size());
        if (!file.ok) {
            TL_LOG_WARN("cinematic: could not parse '%s': %s", path.c_str(), file.error.c_str());
            continue;
        }
        cinematics.add(tl::core::load_cinematic(file.root));
    }
    TL_LOG_INFO("cinematics: %zu read", cinematics.size());
}

// `CCinematicMenu::update`: the music sinks
// to a quarter of its volume under a cinematic and comes back once it has
// closed (`CSoundManager::updateAudioLevels`, the music volume times `+0xf8`).
void World::step_cinematic_music(float delta) {
    const float level = tl::core::cinematic_music_level(cinematic_music, delta, cinematic_open);
    if (level == cinematic_music) return;
    cinematic_music = level;
    tl::core::audio_set_channel_gain(tl::core::Channel::Music, level);
}

std::vector<World::MachineView> World::machines() {
    std::vector<MachineView> out;
    MachineView own;
    own.script = &script;
    own.nodes = &scripted_nodes;
    out.push_back(own);
    for (NestedScript& machine : nested_scripts) {
        if (machine.retired) continue;
        MachineView view;
        view.script = &machine.script;
        view.nodes = &machine.nodes;
        view.scene = machine.scene;
        view.hierarchy = machine.hierarchy;
        view.owner = machine.owner;
        view.particle = machine.particle;
        out.push_back(view);
    }
    return out;
}

void World::set_level_identity(const std::string& dungeon, int32_t depth, bool is_town,
                               bool dont_store, bool allows_portals) {
    level_dungeon = tl::core::upper_ascii(dungeon);
    level_depth = depth;
    level_chunk_names.clear();
    level_monster_level = 1;
    level_fish_spawn_class.clear();
    // A warp is `setAIState` out of whatever the player was doing.
    if (fishing_state != tl::core::FishingState::Off) stop_fishing();
    level_is_town = is_town;
    level_dont_store = dont_store;
    level_allows_portals = allows_portals;
    // A dungeon floor sets it after this (`place_dungeon_floor`); the town and a
    // plain layout keep false.
    level_allows_pet_return = false;
    level_restored = false;
    level_has_saved_state = config.travel != nullptr && !level_dungeon.empty() &&
                            tl::core::has_level_saved_state(*config.travel, level_dungeon, depth);
    TL_LOG_INFO("level: %s depth %d%s%s", level_dungeon.c_str(), depth, is_town ? ", a town" : "",
                level_has_saved_state ? ", remembered" : "");
    // What the last level left in the scene: a level change should leave nothing.
    log_scene_census(" left by the last level");
}

// CPlayer::storeLevelSavedState
// `CPlayer::storeLevelSavedState`'s item walk on every item lying in the level.
void World::store_dropped_items(
    std::vector<std::pair<uint64_t, tl::formats::ItemSaveState>>& made) {
    for (const DroppedWorldItem& dropped : dropped_items) {
        if (dropped.flying || dropped.node == nullptr) continue;
        if (dropped.is_gold) {
            // CItemGold::fillSaveState: GUID -1, the
            // position, the orientation, the name (vtable +0x144) and the amount; the rest is
            // the record's own, enabled clear among it.
            tl::formats::ItemSaveState gold;
            gold.unit_guid = -1;
            gold.name = std::to_string(dropped.pile.amount) + " Gold";
            gold.from_layout = 0;
            gold.enabled = 0;
            gold.value = dropped.pile.amount;
            const ogre::Vector3 at = view_position(dropped.node);
            gold.position = {at.x, at.y, at.z};
            gold.centre = gold.position;
            save_orientation(dropped.rest, gold.orientation);
            made.emplace_back(dropped.made, std::move(gold));
            continue;
        }
        // Vtable +0x13c, `CEquipment::fillSaveState(state, -1, false)`, then
        // `CItem::fillSaveState`'s place, orientation and room.
        tl::core::InventorySlot held;
        held.item = dropped.item;
        held.count = dropped.count;
        tl::formats::ItemSaveState item = item_save_state(held, -1);
        item.from_layout = 0;
        const ogre::Vector3 at = view_position(dropped.node);
        item.position = {at.x, at.y, at.z};
        item.centre = item.position;
        save_orientation(dropped.rest, item.orientation);
        item.room = tl::core::room_index_that_position_is_in(level_rooms, {at.x, at.y, at.z});
        // A spawner's item: `CItem::fillSaveState` writes its spawner (`+0x148`), and its
        // hierarchy is 0 as a breakable's.
        if (const auto spawner = spawner_units.find(dropped.unit_id);
            dropped.unit_id != 0 && spawner != spawner_units.end()) {
            item.spawner_id = spawner->second.spawner;
            item.hierarchy = 0;
        }
        made.emplace_back(dropped.made, std::move(item));
    }
}

// Step 5 of `storeLevelSavedState`: formations not yet triggered (`CLevelState::addFormations`):
// the layout and where the node stands, for each whose `+0x108`
// is clear.
void World::store_pending_formations(tl::formats::LevelSaveState& state) {
    for (const PendingFormation& formation : pending_formations) {
        if (formation.woken) continue;
        // `CFormationNodeSaveAndLoad::CFormationNodeSaveAndLoad(node)`:
        // pending is the node's `+0x108` negated,
        // the layout its file's path, the position `getPosition(true)`: with no parent, the
        // node's own `+0x70`, where `setPosition` put it.
        tl::formats::FormationSaveState saved;
        saved.pending = 1;
        saved.layout = formation.reference;
        saved.position = {formation.placed.x, formation.placed.y, formation.placed.z};
        state.formations.push_back(std::move(saved));
    }
}

// The machines with the room's linked files that made none (StoredLayout): what such a file links
// hangs from its own links.
std::vector<World::MachineView> World::with_stored_layouts(const std::vector<MachineView>& views) {
    std::vector<MachineView> walk_views = views;
    for (StoredLayout& stored : stored_layouts) {
        if (!stored.built) continue;
        MachineView view;
        view.script = &stored.script;
        view.scene = stored.scene;
        view.hierarchy = stored.hierarchy;
        view.owner = stored.owner;
        walk_views.push_back(view);
    }
    return walk_views;
}

// The layout's `Unit Trigger`s in the order they were made: room by room, each room's file in its
// order, a linked layout's at its link; each with the index of its view.
std::vector<std::pair<size_t, tl::core::LevelScript::NodeRecord>> World::made_trigger_nodes(
    const std::vector<MachineView>& views) {
    std::vector<std::pair<size_t, tl::core::LevelScript::NodeRecord>> made_nodes;
    std::vector<uint32_t> rooms;
    for (const MachineView& view : views) {
        if (std::find(rooms.begin(), rooms.end(), view.scene) == rooms.end())
            rooms.push_back(view.scene);
    }
    std::vector<bool> walked(views.size(), false);
    std::function<void(size_t)> walk = [&](size_t v) {
        walked[v] = true;
        const MachineView& view = views[v];
        std::unordered_map<int64_t, tl::core::LevelScript::NodeRecord> triggers;
        for (tl::core::LevelScript::NodeRecord& n :
             view.script->nodes_of_type(tl::formats::cmp_type::kUnitTrigger))
            triggers.emplace(n.node, std::move(n));
        for (const int64_t id : view.script->node_order()) {
            const auto t = triggers.find(id);
            if (t != triggers.end()) made_nodes.emplace_back(v, t->second);
            for (size_t k = 0; k < views.size(); ++k) {
                if (!walked[k] && views[k].scene == view.scene && views[k].owner == id) walk(k);
            }
        }
    };
    for (const uint32_t room : rooms) {
        for (size_t v = 0; v < views.size(); ++v)
            if (!walked[v] && views[v].scene == room && views[v].owner == 0) walk(v);
        for (size_t v = 0; v < views.size(); ++v)
            if (!walked[v] && views[v].scene == room) walk(v);
    }
    return made_nodes;
}

// One of the layout's `Unit Trigger` props into the level's items; one `populate` made into
// `populated`, with its turn.
void World::store_layout_item(
    const MachineView& view, const tl::core::LevelScript::NodeRecord& n,
    tl::formats::LevelSaveState& state,
    std::vector<std::pair<uint64_t, tl::formats::ItemSaveState>>& populated) {
    tl::formats::ItemSaveState item;
    TriggerProp prop;
    trigger_prop(n.unit, prop);
    // `CItem::fillSaveState`: +0x08 = the unit's name (`+0x1b0`),
    // +0x90 = its GUID (`+0x168`), +0x80 = its spawner (`+0x148`, none here); its parent
    // is the layout's owner: the link for a linked layout, whatever group holds it there,
    // and the room's own object (a run id) for the room's file.
    item.name = prop.found && !prop.display_name.empty() ? prop.display_name : n.unit;
    item.unit_guid = unit_guid_of(n.unit);
    // A trigger whose unit names nothing (the vault's "Temp Lever", no MODEL) made no item.
    if (item.unit_guid == -1) return;
    item.object_id = n.node;
    item.parent_id = view.owner != 0 ? view.owner : (n.parent != 0 ? n.parent : -1);
    item.spawner_id = -1;
    item.hierarchy = view.hierarchy;
    item.from_layout = 1;
    item.enabled = n.enabled ? 1 : 0;
    // +0xb4 = `getPosition(false)`, where it stands in its own file; +0xc0 =
    // `getPosition(true)`.
    item.position = {n.local_position.x, n.local_position.y, n.local_position.z};
    item.centre = {n.position.x, n.position.y, n.position.z};
    // `CItem::fillSaveState`: +0x80 =
    // `+0x17c`, the room `getRoomIndexThatPositionIsIn` finds at the
    // item's centre the first time it is asked.
    item.room = tl::core::room_index_that_position_is_in(level_rooms, n.position);
    const int64_t key = instance_key(n.node, view.scene, view.hierarchy);
    tl::core::LevelScript::UnitTriggerView held;
    item.value = view.script->unit_trigger(n.node, held) ? held.state : 0;
    // `CItem::fillSaveState`: +0x5e =
    // the unit's `+0x18d`.
    item.collideable =
        (!prop.found || prop.collideable) && trigger_not_collidable.count(key) == 0 ? 1 : 0;
    // One `populate` made (`place_populated_trigger`) is not the
    // layout's: it goes down as created, with its turn, and is made
    // again when the level is put back.
    const auto trigger = populated_triggers.find(n.node);
    if (trigger != populated_triggers.end()) {
        item.from_layout = 0;
        // +0x80 = its spawner (`+0x148`), one that released it or took it back.
        if (const auto spawner = spawner_units.find(n.node); spawner != spawner_units.end())
            item.spawner_id = spawner->second.spawner;
        item.position = item.centre;
        save_orientation(trigger->second.facing, item.orientation);
        populated.emplace_back(trigger->second.made, std::move(item));
        return;
    }
    state.items.push_back(std::move(item));
}

// The map portals (`CPlayer::openMapPortal`): created items, each with the
// `WARP` effect its manager holds (`CItemSaveState +0xe0`, the passive
// list) and whether it is still on (`+0x5d`).
void World::store_portal_items(std::vector<tl::formats::ItemSaveState>& out) {
    for (const OpenPortal& portal : open_portals) {
        if (portal.warp.empty()) continue;
        tl::formats::ItemSaveState item;
        item.name = portal.unit;
        item.from_layout = 0;
        item.enabled = portal.enabled ? 1 : 0;
        item.position = {portal.at.x, portal.at.y, portal.at.z};
        item.centre = item.position;
        tl::core::LiveEffect warp;
        warp.effect.type = tl::core::kEffectTypeWarp;
        warp.effect.activation = tl::core::EffectActivation::Passive;
        warp.effect.duration = tl::core::kDurationAlways;
        warp.effect.save = true;
        warp.effect.name = portal.warp;
        // `CTriggerUnit::fillSaveState` copies every effect of the manager as
        // `CCharacter::fillSaveState` copies a saved one: three rolls on `g_RandVolatile`.
        load_stat_graphs();
        for (int roll = 0; roll < 3; ++roll) {
            tl::core::roll_effect_value(warp.effect, stat_graphs, tl::core::EffectOwner{},
                                        tl::core::volatile_random());
        }
        if (warp.effect.rolled_value) warp.value = *warp.effect.rolled_value;
        item.effects[0].push_back(tl::core::effect_save_record(warp));
        out.push_back(std::move(item));
    }
}

// The breakables still standing: a broken
// one is left out (`ISA(0x1d)` and `+0x1cc`). The count is not written: the console's record
// keeps what the heap held there. INVENTED: its spawner is not rejoined.
void World::store_standing_breakables(
    std::vector<std::pair<uint64_t, tl::formats::ItemSaveState>>& populated) {
    for (const BreakableWorld& b : breakables) {
        if (b.node == nullptr || !b.state.usable) continue;
        tl::formats::ItemSaveState item;
        TriggerProp prop;
        trigger_prop(b.unit, prop);
        item.name = prop.found && !prop.display_name.empty() ? prop.display_name : b.unit;
        item.unit_guid = unit_guid_of(b.unit);
        item.from_layout = 0;
        item.enabled = 1;
        item.collideable = b.state.collideable ? 1 : 0;
        item.value = 1;
        item.hierarchy = 0;
        item.spawner_id = -1;
        const auto spawner = spawner_units.find(b.id);
        if (spawner != spawner_units.end()) item.spawner_id = spawner->second.spawner;
        const ogre::Vector3 at = view_position(b.node);
        item.position = {at.x, at.y, at.z};
        item.centre = item.position;
        item.room = tl::core::room_index_that_position_is_in(level_rooms, {at.x, at.y, at.z});
        save_orientation(view_orientation(b.node), item.orientation);
        populated.emplace_back(b.made, std::move(item));
    }
}

// One living character's record (`CCharacter::fillSaveState`)
// but its effects.
tl::formats::CharacterSaveState World::character_save_state(
    const MachineView& view, const tl::core::LevelScript::CharacterRecord& c) {
    tl::formats::CharacterSaveState saved;
    // `CCharacter::fillSaveState`: +0x18 = the unit's GUID (`+0x168`).
    saved.unit_guid = unit_guid_of(c.unit);
    // +0x40 = the character's name (`+0x4c0`): a champion's or a quest's own over
    // `DISPLAYNAME`.
    const auto named = unit_names.find(c.node);
    const std::string& shown = stats_for_unit(c.unit).display_name;
    saved.name = named != unit_names.end()  ? named->second
                 : !c.champion_name.empty() ? c.champion_name
                 : shown.empty()            ? c.unit
                                            : shown;
    saved.object_id = c.node;
    saved.from_layout = c.from_layout ? 1 : 0;
    saved.enabled = 1;
    saved.level = c.level;
    saved.hp = c.hp;
    saved.depth = level_depth;
    // `CCharacter::fillSaveState`:
    // +0x184 = `+0x17c`, the room of the position it saves.
    saved.room = tl::core::room_index_that_position_is_in(level_rooms, c.position);
    // `getPosition()` at +0x74: a character's position is its sphere's centre, its feet
    // plus its collision radius.
    saved.position = {c.position.x, c.position.y + view.script->unit_radius(c.node), c.position.z};
    saved.orientation = orientation_bytes(c.facing);
    // +0x60 = its spawner (`+0x148`): the one that released it, or took it back on a restore.
    if (const auto spawner = spawner_units.find(c.node); spawner != spawner_units.end())
        saved.spawner_id = spawner->second.spawner;
    for (const auto& entry : spawned) {
        for (const SpawnedUnit& unit : entry.second) {
            // `populate`'s phases (negative) and a restored unit with no spawner (0) are this
            // engine's keys, not a spawner.
            if (unit.id == c.node && entry.first > 0) saved.spawner_id = entry.first;
        }
    }
    // `CCharacter::fillSaveState`:
    // the clock (`+0x388` into +0xf8) and, for a merchant, its
    // inventory, one `CItemSaveState` per occupied slot as the player's
    // are written.
    // Each slot through `CEquipment::fillSaveState` (vtable +0x13c), as the player's: the full
    // record, its effects cloned with their three rolls.
    const auto write_inventory = [&](const tl::core::Inventory& inventory) {
        const std::vector<tl::core::InventorySlot>& slots = inventory.slots();
        for (size_t i = 0; i < slots.size(); ++i) {
            if (slots[i].count <= 0) continue;
            tl::formats::ItemSaveState item = item_save_state(slots[i], static_cast<int32_t>(i));
            item.enabled = 1;
            saved.inventory.push_back(std::move(item));
        }
    };
    const auto merchant = merchants.find(c.node);
    if (merchant != merchants.end()) {
        saved.clock = merchant->second.clock;
        write_inventory(merchant->second.inventory);
        remembered_merchants[level_dungeon + ":" + std::to_string(level_depth)][c.node] =
            merchant->second;
    }
    // What a monster holds, in its equip locations (0 and 1 are all the
    // shipped data gives one).
    if (merchant == merchants.end() && combiners.count(c.node) == 0) {
        const auto write_hand = [&saved](const std::string& name, int location) {
            if (name.empty()) return;
            tl::formats::ItemSaveState item;
            item.name = name;
            item.slot = location;
            item.value = 1;
            item.enabled = 1;
            saved.inventory.push_back(std::move(item));
        };
        write_hand(c.right_hand, tl::core::equip_location::kRightHand);
        write_hand(c.left_hand, tl::core::equip_location::kLeftHand);
    }
    // A Transmuter keeps what was left in its slots the same way.
    const auto combiner = combiners.find(c.node);
    if (combiner != combiners.end()) {
        write_inventory(combiner->second.inventory);
        remembered_combiners[level_dungeon + ":" + std::to_string(level_depth)][c.node] =
            combiner->second;
    }
    // +0x58 = the master's id (`+0x5a4`).
    saved.master_id = c.master;
    return saved;
}

// `CLevel +0x5C` holds the newest first, walked twice: the characters with no master, then
// those with one (the player's pets left out above). What a restore made is newer than all the
// load made.
void World::order_stored_characters(
    tl::formats::LevelSaveState& state,
    std::vector<std::pair<tl::core::LevelScript*, int64_t>>& stored_units) {
    std::reverse(state.characters.begin(), state.characters.end());
    std::reverse(stored_units.begin(), stored_units.end());
    {
        std::vector<size_t> order(state.characters.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::stable_partition(order.begin(), order.end(), [&](size_t i) {
            return restore_made_units.count(stored_units[i].second) != 0;
        });
        std::stable_partition(order.begin(), order.end(),
                              [&state](size_t i) { return state.characters[i].master_id == -1; });
        std::vector<tl::formats::CharacterSaveState> characters;
        std::vector<std::pair<tl::core::LevelScript*, int64_t>> units;
        for (const size_t i : order) {
            characters.push_back(std::move(state.characters[i]));
            units.push_back(stored_units[i]);
        }
        state.characters = std::move(characters);
        stored_units = std::move(units);
    }
}

// `CCharacter::fillSaveState` ends with the manager's three lists: each
// effect marked `SAVE` (`+0x2b`) copied - `CEffect::clone` with `setOwner(source's owner,
// true)`, the copy constructor's `calculateBaseValue(0)`, then `setOwner(null, true)` - three
// rolls on `g_RandVolatile`, the copy's last value kept in the record.
void World::store_character_effects(
    tl::formats::LevelSaveState& state,
    const std::vector<std::pair<tl::core::LevelScript*, int64_t>>& stored_units) {
    load_stat_graphs();
    for (size_t i = 0; i < state.characters.size(); ++i) {
        tl::core::EffectManager* effects =
            stored_units[i].first->unit_effects(stored_units[i].second);
        if (effects == nullptr) continue;
        const tl::core::EffectOwner owner = effects->context().owner;
        for (int list = 0; list < 3; ++list) {
            for (const tl::core::LiveEffect& live :
                 effects->effects(static_cast<tl::core::EffectActivation>(list))) {
                if (!live.effect.save) continue;
                tl::core::LiveEffect copy = live;
                // `clone` takes the effect's own owner (`CEffect +0x40`): the
                // caster that laid it, else the unit itself; its level, not the manager's.
                tl::core::EffectOwner own = owner;
                const std::optional<int32_t> level =
                    live.source.set
                        ? event_caster_level(static_cast<tl::core::LevelScript*>(
                                                 const_cast<void*>(live.source.machine)),
                                             live.source.node)
                        : event_caster_level(stored_units[i].first, stored_units[i].second);
                if (level.has_value()) own.level = static_cast<uint32_t>(*level);
                if (own.present) {
                    tl::core::set_owner_level(copy.effect, static_cast<int32_t>(own.level));
                }
                tl::core::roll_effect_value(copy.effect, stat_graphs, own,
                                            tl::core::volatile_random());
                tl::core::roll_effect_value(copy.effect, stat_graphs, own,
                                            tl::core::volatile_random());
                tl::core::roll_effect_value(copy.effect, stat_graphs, tl::core::EffectOwner{},
                                            tl::core::volatile_random());
                if (copy.effect.rolled_value) copy.value = *copy.effect.rolled_value;
                state.characters[i].effects[static_cast<size_t>(list)].push_back(
                    tl::core::effect_save_record(copy));
            }
        }
    }
}

// The machines in the walk's order with the files that made none (StoredLayout) among them:
// `GetObjectsCreatedByADescriptor` takes a scene's `Layout Link`s, then its
// `Layout Link Particle`s, then its `Layout Link Timeline`s, each recursing, so within a scene they
// go by the links they were placed under.
std::vector<World::MachineView> World::logic_store_views(const std::vector<MachineView>& views) {
    std::vector<MachineView> stored_views;
    std::vector<const std::vector<uint64_t>*> paths;
    static const std::vector<uint64_t> kRoot;
    {
        size_t next_stored = 0;
        const auto stored_before = [&](size_t nested) {
            for (; next_stored < stored_layouts.size() &&
                   stored_layouts[next_stored].before <= nested;
                 ++next_stored) {
                StoredLayout& stored = stored_layouts[next_stored];
                if (!stored.built || stored.level_particle || stored.particle) continue;
                MachineView view;
                view.script = &stored.script;
                view.scene = stored.scene;
                view.hierarchy = stored.hierarchy;
                view.owner = stored.owner;
                stored_views.push_back(view);
                paths.push_back(&stored.link_path);
            }
        };
        stored_views.push_back(views.front());
        paths.push_back(&kRoot);
        for (size_t i = 0; i < nested_scripts.size(); ++i) {
            stored_before(i);
            NestedScript& machine = nested_scripts[i];
            if (machine.retired || machine.level_particle || machine.particle) continue;
            MachineView view;
            view.script = &machine.script;
            view.nodes = &machine.nodes;
            view.scene = machine.scene;
            view.hierarchy = machine.hierarchy;
            view.owner = machine.owner;
            stored_views.push_back(view);
            paths.push_back(&machine.link_path);
        }
        stored_before(static_cast<size_t>(-1));
    }
    std::vector<uint32_t> scenes;
    std::vector<size_t> order(stored_views.size());
    for (size_t i = 0; i < stored_views.size(); ++i) {
        order[i] = i;
        if (std::find(scenes.begin(), scenes.end(), stored_views[i].scene) == scenes.end())
            scenes.push_back(stored_views[i].scene);
    }
    const auto scene_rank = [&](size_t i) {
        return std::find(scenes.begin(), scenes.end(), stored_views[i].scene) - scenes.begin();
    };
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        if (scene_rank(a) != scene_rank(b)) return scene_rank(a) < scene_rank(b);
        return *paths[a] < *paths[b];
    });
    std::vector<MachineView> sorted;
    sorted.reserve(order.size());
    for (const size_t i : order) sorted.push_back(stored_views[i]);
    return sorted;
}

// Step 10 of `storeLevelSavedState`: the logic nodes, per room layout: each descriptor kind over every layout the room
// links (its machines, in order) before the next kind.
void World::store_logic_nodes(const std::vector<MachineView>& stored_views,
                              tl::formats::LevelSaveState& state) {
    std::vector<uint32_t> scenes;
    for (const MachineView& view : stored_views) {
        if (std::find(scenes.begin(), scenes.end(), view.scene) == scenes.end())
            scenes.push_back(view.scene);
    }
    for (const uint32_t scene : scenes) {
        for (int kind = 0; kind < tl::core::LevelScript::store_descriptors(); ++kind) {
            for (const MachineView& view : stored_views) {
                if (view.scene != scene) continue;
                view.script->store_logic_states(view.scene, view.hierarchy, state.logic, kind,
                                                view.owner != 0 ? view.owner : -1);
            }
        }
    }
}

void World::store_level_state() {
    tl::core::TravelRecord* travel = config.travel;
    if (travel == nullptr || level_dungeon.empty()) return;
    const auto step = [this](tl::core::StoreStep s) {
        if (store_step_listener) store_step_listener(s);
    };

    // `updateDungeonTracking` records the dungeon's salt (`CDungeon +0x58`).
    step(tl::core::StoreStep::Header);
    const tl::core::Dungeon* tracked = loaded_dungeon_manager().dungeon_by_name(level_dungeon);
    tl::formats::LevelSaveState& state = tl::core::begin_store_level_saved_state(
        *travel, level_dungeon, level_depth, level_is_town, level_dont_store,
        player_character.level(), tracked != nullptr ? &tracked->salt : nullptr);

    // `storeLevelSavedState` writes journal slot 2 itself:
    // `+0x7e4 = max(+0x7e4, +0x7d8)`, the deepest depth just updated, which
    // `begin_store_level_saved_state` keeps as `deepest_depth_max`.
    const int32_t explored =
        player_character.journal().value(tl::core::JournalStatistic::LevelsExplored);
    if (travel->deepest_depth_max != explored) {
        player_character.journal().set(tl::core::JournalStatistic::LevelsExplored,
                                       travel->deepest_depth_max);
        TL_LOG_INFO("journal: LevelsExplored = %d", travel->deepest_depth_max);
    }

    step(tl::core::StoreStep::Prepare);
    std::vector<MachineView> views = machines();

    // `CCharacter::unitInit` stocked every merchant when it was made; here
    // that waits for a frame in control, which a warp can come before.
    step_merchants(0.0f);

    // Step 5: formations not yet triggered.
    step(tl::core::StoreStep::Formations);
    store_pending_formations(state);

    // Step 6: the items. The level keeps only Unit Trigger props, with their state
    // (CTriggerUnit::fillSaveState: +0x68 = +0x248).
    step(tl::core::StoreStep::Items);
    std::vector<std::pair<uint64_t, tl::formats::ItemSaveState>> populated_items;
    {
        const std::vector<MachineView> walk_views = with_stored_layouts(views);
        for (const auto& [v, n] : made_trigger_nodes(walk_views))
            store_layout_item(walk_views[v], n, state, populated_items);
    }
    std::vector<tl::formats::ItemSaveState> portal_items;
    store_portal_items(portal_items);
    store_standing_breakables(populated_items);
    // A dropped item takes its turn among them: a restored level makes its gold and its
    // breakables in one pass over the state, in the state's order.
    const size_t first_dropped = populated_items.size();
    store_dropped_items(populated_items);
    // `getIsQuestUnit` on each item walked marks the state (+0xb1): a quest's
    // item (`+0x170` set). INVENTED: its `ISA 0x67` half is not tested.
    for (size_t i = first_dropped; i < populated_items.size(); ++i) {
        const tl::formats::ItemSaveState& item = populated_items[i].second;
        if (item.unit_guid != -1 && item.i170 != -1) state.holds_quest_item = 1;
    }
    const uint64_t dropped_from =
        first_dropped < populated_items.size()
            ? std::min_element(populated_items.begin() + first_dropped, populated_items.end(),
                               [](const auto& a, const auto& b) { return a.first < b.first; })
                  ->first
            : UINT64_MAX;
    std::stable_sort(populated_items.begin(), populated_items.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    // INVENTED: a map portal (no turn of its own) goes just before the first dropped item.
    bool portals_in = false;
    const auto put_portals = [&] {
        for (tl::formats::ItemSaveState& portal : portal_items)
            state.items.push_back(std::move(portal));
        portals_in = true;
    };
    for (auto& made : populated_items) {
        if (!portals_in && made.first >= dropped_from) put_portals();
        state.items.push_back(std::move(made.second));
    }
    if (!portals_in) put_portals();
    // The level's item list (`CLevel +0x60`) holds the newest first: the order of creation, reversed.
    std::reverse(state.items.begin(), state.items.end());

    // Step 7: the characters, alive and not the pet.
    step(tl::core::StoreStep::Characters);
    std::vector<std::pair<tl::core::LevelScript*, int64_t>> stored_units;
    for (const MachineView& view : views) {
        for (const tl::core::LevelScript::CharacterRecord& c : view.script->living_characters()) {
            stored_units.emplace_back(view.script, c.node);
            state.characters.push_back(character_save_state(view, c));
        }
    }
    order_stored_characters(state, stored_units);
    store_character_effects(state, stored_units);

    // Step 8: the chunk names (`CLevel +0x1e8`'s `CChunk +0x20`), one per piece of the layout.
    step(tl::core::StoreStep::Chunks);
    state.chunks = level_chunk_names;

    // Step 9: the automap.
    step(tl::core::StoreStep::Automap);
    if (automap_initialized) view_store_automap(state);

    // Step 10: the logic nodes, per room layout.
    step(tl::core::StoreStep::Logic);
    store_logic_nodes(logic_store_views(views), state);

    const size_t characters = state.characters.size();
    const size_t items = state.items.size();
    const size_t logic = state.logic.size();

    // Step 11. The quests `updateStoredLevels` asks about (`CPlayer+0x868`) are
    // the random quests bound to a dungeon level, which this engine does not
    // generate, so none holds a level.
    step(tl::core::StoreStep::Kept);
    tl::core::update_stored_levels(*travel, level_dungeon, level_depth, {}, level_memory_rules());
    step(tl::core::StoreStep::Done);
    TL_LOG_INFO(
        "level: %s depth %d remembered (%zu character(s), %zu item(s), %zu logic "
        "node(s)); %zu level(s) kept",
        level_dungeon.c_str(), level_depth, characters, items, logic, travel->levels.size());
}

// `createUnit` makes a unit by its GUID (an item's +0x90, a character's +0x18); the name a record
// keeps is the one shown. This engine makes a unit by its `NAME`, so the GUID is turned into one.
void World::name_saved_units(tl::formats::LevelSaveState& state) {
    for (tl::formats::ItemSaveState& item : state.items) {
        const std::string unit = unit_of_guid(item.unit_guid);
        if (!unit.empty()) item.name = unit;
    }
    for (tl::formats::CharacterSaveState& saved : state.characters) {
        const std::string unit = unit_of_guid(saved.unit_guid);
        if (!unit.empty()) saved.name = unit;
    }
}

// 5. Formations: a `CFormationNode` for each saved one
// (`CFormationNode::CFormationNode(rm, CFormationNodeSaveAndLoad*)`
// `setLayoutFile`, `setPosition`), waiting as
// before.
void World::restore_pending_formations(const tl::formats::LevelSaveState& state) {
    size_t formations_back = 0;
    for (const tl::formats::FormationSaveState& saved : state.formations) {
        if (saved.pending == 0) continue;
        const ogre::Vector3 set_at(saved.position.x, saved.position.y, saved.position.z);
        if (add_pending_formation(saved.layout, dropped_to_ground(set_at), level_monster_level,
                                  set_at)) {
            ++formations_back;
        }
    }
    if (formations_back > 0) TL_LOG_INFO("level: %zu formation(s) wait again", formations_back);
}

// A saved monster's hands (CCharacter::applySaveState): what its saved inventory holds, none
// when it holds nothing - made from the save, it rolls no default equipment.
void World::saved_character_hands(const tl::formats::CharacterSaveState& saved,
                                  tl::core::LevelScript::CharacterRecord& record) {
    if (merchants.count(saved.object_id) != 0 || combiners.count(saved.object_id) != 0) return;
    record.equipment = true;
    for (const tl::formats::ItemSaveState& item : saved.inventory) {
        if (item.slot == tl::core::equip_location::kRightHand) record.right_hand = item.name;
        if (item.slot == tl::core::equip_location::kLeftHand) record.left_hand = item.name;
    }
    record.weapon_attack = weapon_attack_of(record.right_hand);
    record.left_weapon_attack = weapon_attack_of(record.left_hand, tl::core::Hand::Left);
}

// `CCharacter::applySaveState`: each saved item of a
// slot up to 1000 with a GUID is made again (`createUnit(guid, 1, true, false)`), given its
// record and picked up; a merchant's goes another way. Only the making's draws matter here: the
// hands are the record's (`saved_character_hands`).
void World::make_saved_inventory(const tl::formats::CharacterSaveState& saved) {
    if (merchants.count(saved.object_id) != 0 || combiners.count(saved.object_id) != 0) return;
    // A made merchant is not yet in `merchants` here (it is keyed by its made id after this):
    // the unit's type says so, as `applySaveState`'s branch asks it.
    load_unit_types();
    const uint32_t type = unit_types.id_of(stats_for_unit(saved.name).unittype);
    if (unit_types.isa(type, tl::core::unit_type::kMerchant)) return;
    for (const tl::formats::ItemSaveState& item : saved.inventory) {
        if (static_cast<uint32_t>(item.slot) > 1000 || item.unit_guid == -1) continue;
        (void)item_from_save(item);
    }
}

// Then each of the record's three effect lists: every effect through
// `CBaseUnit::addNewEffect`, whose copy's `setOwner` gives it the unit and draws its roll, then
// the saved value laid back over it and `activateEffect`.
void World::restore_saved_effects(const tl::formats::CharacterSaveState& saved,
                                  tl::core::LevelScript& machine, int64_t node) {
    const bool any =
        std::any_of(saved.effects.begin(), saved.effects.end(),
                    [](const std::vector<tl::formats::EffectSaveState>& l) { return !l.empty(); });
    if (!any) return;
    ensure_effect_data();
    load_stat_graphs();
    load_unit_themes();
    tl::core::EffectOwner own;
    if (const std::optional<int32_t> level = event_caster_level(&machine, node)) {
        own.present = true;
        own.level = static_cast<uint32_t>(*level);
    }
    tl::core::EffectManager* effects = machine.unit_effects(node);
    if (effects == nullptr) {
        const tl::core::EffectManager::Context* tables = machine.unit_effect_tables();
        if (tables == nullptr) return;
        tl::core::EffectManager::Context context = *tables;
        context.owner = own;
        effects = machine.ensure_unit_effects(node, context);
        if (effects == nullptr) return;
    }
    for (const std::vector<tl::formats::EffectSaveState>& list : saved.effects) {
        for (const tl::formats::EffectSaveState& record : list) {
            tl::core::ItemStats::SavedEffect back =
                tl::core::effect_from_save_record(record, effect_types, &unit_themes);
            if (own.present)
                tl::core::set_owner_level(back.effect, static_cast<int32_t>(own.level));
            tl::core::roll_effect_value(back.effect, stat_graphs, own, tl::core::volatile_random());
            back.effect.rolled_value = back.value;
            tl::core::LiveEffect& live = effects->add_effect(back.effect);
            live.value = back.value;
            live.elapsed = back.elapsed;
        }
    }
}

// `CCharacter::applySaveState` ends by rejoining a saved master:
// the level's character of that id, if it is one, takes the unit as a
// pet (`addPet`). The master comes first in the state, so it stands by then. This engine gives a
// made unit a new id, so the saved one is looked up here.
void World::rejoin_saved_master(const tl::formats::CharacterSaveState& saved,
                                tl::core::LevelScript& machine, int64_t node, NodeMap& nodes,
                                RestorePass& pass) {
    pass.by_saved_id[saved.object_id] = {&machine, node};
    if (saved.master_id == -1) return;
    const auto master = pass.by_saved_id.find(saved.master_id);
    if (master == pass.by_saved_id.end()) return;
    std::vector<tl::core::ScriptEvent> events;
    machine.set_unit_master(node, *master->second.first, master->second.second, quest_log.state(),
                            events);
    if (!events.empty()) apply_script_events(events, nodes);
}

// One character of the state, in its order: the layout's own found again, any other made again.
void World::restore_saved_character(const tl::formats::CharacterSaveState& saved,
                                    std::vector<MachineView>& views, RestorePass& pass) {
    // `CCharacter::applySaveState`: the character's name (`+0x4c0`) is the
    // record's (+0x40), a champion's own included; `name_saved_units` renamed the copy only.
    const std::string& saved_name =
        pass.found->characters[static_cast<size_t>(&saved - pass.state->characters.data())].name;
    // The record holds the sphere's centre (`store_level_state`); the feet are a radius below,
    // the unit's own (its numbers read first, as making it reads them).
    index_unit(saved.name);
    const ogre::Vector3 world(
        saved.position.x, saved.position.y - script.unit_radius_of(saved.name), saved.position.z);
    const float world_yaw = yaw_of_bytes(saved.orientation);
    tl::core::LevelScript::CharacterRecord record;
    record.unit = saved.name;
    record.position = {world.x, world.y, world.z};
    record.facing = world_yaw;
    record.hp = saved.hp;
    record.level = saved.level;
    saved_character_hands(saved, record);
    // What `applySaveState` does after the place: name, inventory, effects, master.
    const auto apply = [&](tl::core::LevelScript& machine, int64_t node, NodeMap& nodes) {
        record.node = node;
        std::vector<tl::core::ScriptEvent> events;
        machine.apply_character_record(record, events);
        if (!events.empty()) apply_script_events(events, nodes);
        if (!saved_name.empty()) unit_names[node] = saved_name;
        make_saved_inventory(saved);
        restore_saved_effects(saved, machine, node);
        rejoin_saved_master(saved, machine, node, nodes, pass);
        // `CUnitSpawner::addSpawnedUnit`: the saved spawner
        // (+0x58) takes the character back, as an item's does.
        rejoin_saved_spawner(node, saved.spawner_id, /*character=*/true);
    };
    if (saved.from_layout != 0) {
        // `CLevel::getCharacterByOriginalGuid(id, -1)`: the first character in the
        // level with that id.
        for (MachineView& view : views) {
            tl::core::Vec3 unused;
            if (!view.script->unit_position(saved.object_id, unused)) continue;
            apply(*view.script, saved.object_id, *view.nodes);
            restore_merchant(saved);
            restore_combiner(saved);
            pass.kept[view.script].insert(saved.object_id);
            ++pass.placed;
            return;
        }
        TL_LOG_WARN("level: unable to find editor character %lld (%s)",
                    static_cast<long long>(saved.object_id), saved.name.c_str());
        return;
    }
    // `CResourceManager::createUnit` at the saved position, then `applySaveState`.
    const int64_t owner = saved.spawner_id >= 0 ? saved.spawner_id : 0;
    const ogre::Quaternion facing(ogre::Radian(world_yaw), ogre::Vector3::UNIT_Y);
    if (spawn_unit(saved.name, world, owner, saved.level, saved.champion != 0,
                   /*spawn_anim=*/false, facing) == nullptr) {
        TL_LOG_WARN("level: could not bring back '%s'", saved.name.c_str());
        return;
    }
    const int64_t made = spawned[owner].back().id;
    // `CResourceManager::createUnit` with `from_save` set: `unitInit` skips
    // `createDefaultEquipment`.
    const tl::core::LevelScript::UnitCreation creation =
        script.roll_unit_creation(saved.name, /*default_equipment=*/!record.equipment);
    script.add_spawned_unit(made, saved.name, tl::core::Vec3{world.x, world.y, world.z},
                            /*hunts=*/false, world_yaw, &creation);
    apply(script, made, scripted_nodes);
    // A made merchant or combiner is keyed by the id this engine gave it.
    tl::formats::CharacterSaveState as_made = saved;
    as_made.object_id = made;
    restore_merchant(as_made);
    restore_combiner(as_made);
    restore_made_units.insert(made);
    ++pass.created;
}

// `CItem::applySaveState`: a saved spawner (+0x80) takes the item back
// (`CUnitSpawner::addSpawnedUnit`), every `Unit Spawner` of the level's scenes with that id.
void World::rejoin_saved_spawner(int64_t unit_id, int64_t spawner_id, bool character) {
    if (spawner_id == -1) return;
    for (MachineView& view : machines()) {
        for (const tl::core::LevelScript::NodeRecord& n :
             view.script->nodes_of_type(tl::formats::cmp_type::kUnitSpawner)) {
            if (n.node != spawner_id) continue;
            spawner_units[unit_id] = SpawnerUnit{view.script, n.node};
            // `addSpawnedUnit`: the spawner's own list (`MAX ALLOWED` counts it).
            if (tl::core::UnitSpawner* spawner = view.script->unit_spawner(n.node)) {
                tl::core::SpawnedUnitRef ref;
                ref.kind =
                    character ? tl::core::SpawnedKind::Character : tl::core::SpawnedKind::Item;
                ref.id = unit_id;
                spawner->take_back(ref);
            }
        }
    }
}

// A map portal the player opened here (CPlayer::openMapPortal); the answer is whether its
// prop is a map portal, as a portal's body is a layout.
bool World::saved_map_portal(const tl::formats::ItemSaveState& item) {
    if (item.from_layout != 0 || item.name.empty()) return false;
    TriggerProp prop;
    trigger_prop(item.name, prop);
    if (!prop.found) return false;
    return unit_types.isa(unit_types.id_of(prop.unittype), tl::core::unit_type::kMapPortal);
}

// Each map portal made again with its WARP, on or off as it was left.
void World::restore_map_portals(const tl::formats::LevelSaveState& state) {
    size_t back = 0;
    for (const tl::formats::ItemSaveState& item : state.items) {
        if (!saved_map_portal(item)) continue;
        std::string warp;
        for (const std::vector<tl::formats::EffectSaveState>& list : item.effects) {
            for (const tl::formats::EffectSaveState& effect : list) {
                if (warp.empty() && effect.i1c == tl::core::kEffectTypeWarp) warp = effect.names[0];
            }
        }
        if (add_map_portal(ogre::Vector3(item.position.x, item.position.y, item.position.z), warp,
                           item.enabled != 0)) {
            ++back;
        }
    }
    if (back > 0) TL_LOG_INFO("level state: %zu map portal(s) back", back);
}

// DIVERGENCE: an older port's level state holds each trigger state one higher than +0x248; a
// value above MAXSTATES - 1 marks such a state, and all are taken one lower.
int32_t World::stored_trigger_offset(const tl::formats::LevelSaveState& state) {
    int32_t offset = 0;
    for (const tl::formats::ItemSaveState& item : state.items) {
        if (item.name.empty() || is_breakable_unit(item.name) || saved_map_portal(item)) continue;
        TriggerProp named;
        trigger_prop(item.name, named);
        if (named.found && item.value > named.max_states - 1) offset = 1;
    }
    if (offset != 0) {
        TL_LOG_INFO(
            "level state: trigger states stored one higher (before this build), read one lower");
    }
    return offset;
}

// Each layout `Unit Trigger` back in its state (`CTriggerUnit::applySaveState`,
// `setTriggerState(+0x68)`), as restoreLevelState finds it:
// CLevel::getItemByOriginalGuid(+0x30, +0x38, +0x80, +0x40):
// GUID, room (getRoomIndexThatPositionIsIn), the copy with the state's
// hierarchy hash or the last.
void World::restore_layout_items(std::vector<MachineView>& views, RestorePass& pass) {
    std::vector<std::pair<MachineView*, int64_t>> layout_items;
    std::vector<tl::core::OriginalGuidUnit> layout_guids;
    for (MachineView& view : views) {
        for (const tl::core::LevelScript::UnitTriggerView& t : view.script->every_unit_trigger()) {
            layout_items.emplace_back(&view, t.id);
            layout_guids.push_back({t.id, view.hierarchy, t.at});
        }
    }
    const auto layout_item =
        [&](const tl::formats::ItemSaveState& item) -> std::pair<MachineView*, int64_t> {
        const int found = tl::core::find_item_by_original_guid(
            layout_guids, level_rooms, item.object_id, item.room, item.hierarchy);
        if (found < 0) return {nullptr, 0};
        return layout_items[static_cast<size_t>(found)];
    };
    // `CItem::applySaveState` first: `+0x18d` = the state's
    // +0x5e, so a wall a `Not Collidable` took out stays out.
    for (const tl::formats::ItemSaveState& item : pass.state->items) {
        if (item.from_layout == 0 || item.collideable != 0) continue;
        // The handler skips a copy with no collision group (instance_key()).
        const auto [view, id] = layout_item(item);
        if (view == nullptr) continue;
        tl::core::ScriptEvent e;
        e.action = tl::core::ScriptAction::TriggerCollidable;
        e.node_id = id;
        e.name = item.name;
        e.on = false;
        std::vector<tl::core::ScriptEvent> events{e};
        apply_script_events(events, *view->nodes);
    }
    for (const tl::formats::ItemSaveState& item : pass.state->items) {
        if (item.from_layout == 0) continue;
        const auto [view, id] = layout_item(item);
        if (view == nullptr) continue;
        // `applySaveState`'s forced `setTriggerState`, whose event puts the prop in the state
        // (see the `TriggerState` handler's DIVERGENCE).
        std::vector<tl::core::ScriptEvent> events;
        view->script->restore_unit_trigger(id, item.value - pass.stored_offset, events);
        apply_script_events(events, *view->nodes);
        tl::core::LevelScript::UnitTriggerView after;
        if (view->script->unit_trigger(id, after) && after.state != after.first) ++pass.opened;
    }
}

// A trigger prop `populate` made (stored as created), made again where it stood in the state's
// one pass: `createUnit` with `from_save` -> `CTriggerUnit::unitInit` ->
// `CBaseUnit::unitInit`, its scale drawn and its `[SKILL]`s made,
// then `applySaveState` gives it the state it was left in - a chest opened stays open.
void World::restore_created_trigger(const tl::formats::ItemSaveState& item, const TriggerProp& prop,
                                    RestorePass& pass) {
    const float scale =
        prop.scale + tl::core::volatile_random().between(0.0f, prop.scale_variation);
    roll_prop_skills(item.name);
    const int64_t id = next_spawned_id();
    if (!place_populated_trigger(item.name,
                                 ogre::Vector3(item.position.x, item.position.y, item.position.z),
                                 saved_orientation(item.orientation), id, scale)) {
        return;
    }
    rejoin_saved_spawner(id, item.spawner_id);
    if (const auto made = populated_triggers.find(id); made != populated_triggers.end())
        made->second.made = pass.turn(item);
    ++pass.triggers;
    for (MachineView& view : machines()) {
        const std::vector<tl::core::LevelScript::NodeRecord> triggers =
            view.script->nodes_of_type(tl::formats::cmp_type::kUnitTrigger);
        if (std::none_of(triggers.begin(), triggers.end(),
                         [id](const tl::core::LevelScript::NodeRecord& n) { return n.node == id; }))
            continue;
        std::vector<tl::core::ScriptEvent> events;
        if (item.collideable == 0) {
            tl::core::ScriptEvent e;
            e.action = tl::core::ScriptAction::TriggerCollidable;
            e.node_id = id;
            e.name = item.name;
            e.on = false;
            events.push_back(e);
        }
        view.script->restore_unit_trigger(id, item.value - pass.stored_offset, events);
        if (!events.empty()) apply_script_events(events, *view.nodes);
        break;
    }
    // `CLevel::addItem`: the chest's collision is there for the items after it.
    collision.build();
    register_door_avoidance();
    if (passability.ready()) nav.build(passability);
}

// restoreLevelState: GUID -1 is CItemGold(rm, +0x68) at +0x84 (make_gold_pile draws turn and
// size again), then CItemGold::applySaveState: position, the
// saved orientation over the drawn turn, name, amount.
void World::restore_saved_gold(const tl::formats::ItemSaveState& item, const RestorePass& pass) {
    const tl::core::GoldPile pile =
        tl::core::make_gold_pile(item.value, tl::core::volatile_random(), gold_sounds());
    const size_t before = dropped_items.size();
    place_gold(pile, ogre::Vector3(item.position.x, item.position.y, item.position.z), nullptr,
               /*drop_sound=*/false);
    if (dropped_items.size() <= before) return;
    dropped_items.back().made = pass.turn(item);
    const bool turned = std::any_of(item.orientation.begin(), item.orientation.end(),
                                    [](uint8_t b) { return b != 0; });
    if (turned && dropped_items.back().node != nullptr) {
        const ogre::Quaternion facing = saved_orientation(item.orientation);
        dropped_items.back().rest = facing;
        view_set_orientation(dropped_items.back().node, facing);
    }
}

// One created item of the state, in its order (see `store_level_state`). INVENTED: an item's
// quest tag is not in the stored record; one that an open, placed objective of an active quest
// wants is tagged for it again.
void World::restore_ground_item(const tl::formats::ItemSaveState& item, RestorePass& pass) {
    // INVENTED: an item this port stored before gold was kept went down with GUID -1 and its
    // name, so only a -1 named as gold (or not at all) is gold here.
    if (item.unit_guid == -1 && saved_gold_name(item.name)) {
        restore_saved_gold(item, pass);
        return;
    }
    if (saved_map_portal(item)) return;
    if (!is_breakable_unit(item.name)) {
        TriggerProp prop;
        if (trigger_prop(item.name, prop)) {
            restore_created_trigger(item, prop, pass);
            return;
        }
    } else {
        // A breakable that was standing when the level was left: made again where it stood
        // (`createUnit`, `applySaveState`, `addItem`).
        const ogre::Quaternion facing = saved_orientation(item.orientation);
        const ogre::Vector3 at(item.position.x, item.position.y, item.position.z);
        const int64_t id = next_spawned_id();
        if (!place_breakable(item.name, at, facing, id, &at)) return;
        rejoin_saved_spawner(id, item.spawner_id);
        for (BreakableWorld& b : breakables) {
            if (b.id == id) b.made = pass.turn(item);
        }
        ++pass.breakables;
        return;
    }
    // `CGameClient::restoreLevelState` makes it through `createUnit` by its GUID with
    // `from_save` set, then `applySaveState`.
    std::optional<tl::core::ItemStats> made = item_from_save(item);
    if (!made) return;
    tl::core::ItemStats& copy = *made;
    copy.quest_guid = item.i170;
    copy.quest_unit_id = item.i178;
    const size_t before = dropped_items.size();
    place_ground_item(copy, std::max(1, static_cast<int>(item.value)),
                      ogre::Vector3(item.position.x, item.position.y, item.position.z),
                      saved_orientation(item.orientation), quest_tag_of_item(copy));
    if (dropped_items.size() <= before) return;
    dropped_items.back().made = pass.turn(item);
    // `CItem::applySaveState` after `addItem`'s snap: `setPosition(+0x84)`,
    // then the record's +0x8d through vtable +0x1c, whose `snapToGround` draws again from the
    // record's spot (rs13's ray log: two rays per restored item, both at the saved point).
    if (item.enabled != 0 && dropped_items.back().node != nullptr) {
        const ogre::Vector3 again =
            snap_item_to_ground(ogre::Vector3(item.position.x, item.position.y, item.position.z));
        view_set_position(dropped_items.back().node, again);
    }
    if (item.spawner_id != -1) {
        const int64_t id = next_spawned_id();
        dropped_items.back().unit_id = id;
        rejoin_saved_spawner(id, item.spawner_id);
    }
}

// CGameClient::restoreLevelState
void World::restore_level_state() {
    // The level stands: `CLevel::addCharacter`
    // has dropped every character it was
    // given onto the floor (`dropToGround(4.0)`), before any remembered state
    // moves one.
    for (MachineView& view : machines()) {
        std::vector<tl::core::ScriptEvent> events;
        view.script->drop_characters_to_ground(events);
        if (!events.empty()) apply_script_events(events, *view.nodes);
    }
    apply_machine_path_blockers();
    TL_LOG_INFO("  no-spawn: %zu region(s)", no_spawn_regions.size());
    level_restored = false;
    restore_made_units.clear();
    tl::core::TravelRecord* travel = config.travel;
    if (travel == nullptr || level_dungeon.empty()) return;
    const tl::formats::LevelSaveState* found =
        tl::core::get_level_saved_state(*travel, level_dungeon, level_depth);
    if (found == nullptr) return;
    tl::formats::LevelSaveState state = *found;
    level_restored = true;
    if (restore_listener) restore_listener(true);
    name_saved_units(state);
    RestorePass pass;
    pass.state = &state;
    pass.found = found;

    std::vector<MachineView> views = machines();

    // 1. Logic node states, per room layout. A `Layout Link Particle`'s file holds none.
    for (MachineView& view : views) {
        if (view.particle) continue;
        std::vector<tl::core::ScriptEvent> events;
        view.script->restore_logic_states(view.scene, view.hierarchy, state.logic,
                                          quest_log.state(), events);
        if (!events.empty()) apply_script_events(events, *view.nodes);
    }

    // 2. The automap, when its size still matches.
    if (automap_initialized) view_restore_automap(state);

    // 3. Characters, in the state's order.
    for (const tl::formats::CharacterSaveState& saved : state.characters)
        restore_saved_character(saved, views, pass);

    // What `CMonsterDescriptor::descriptorSceneActivated`
    // marked on a restored level that is not
    // a town, and the state did not claim, goes.
    size_t deleted = 0;
    if (!level_is_town) {
        for (MachineView& view : views) {
            std::vector<tl::core::ScriptEvent> events;
            deleted +=
                view.script->delete_placed_monsters_except(pass.kept[view.script], events).size();
            if (!events.empty()) apply_script_events(events, *view.nodes);
        }
    }

    // 4. Items. `restoreLevelState` makes every created item in one pass over the state, so each
    // takes the turn of its place there (`CLevel::addItem`'s order).
    pass.base = items_made;
    load_unit_types();
    restore_map_portals(state);
    pass.stored_offset = stored_trigger_offset(state);
    restore_layout_items(views, pass);
    for (const tl::formats::ItemSaveState& item : state.items) {
        if (item.from_layout == 0) restore_ground_item(item, pass);
    }
    items_made = std::max(items_made, pass.base + state.items.size());

    // 5. Formations.
    restore_pending_formations(state);

    // `restoreLevelState`'s last step before `updateNPCIcons`: a
    // merchant with nothing in its Armor, Weapon and Spell panes restocks.
    populate_new_merchants();
    if (restore_listener) restore_listener(false);

    TL_LOG_INFO(
        "level: %s depth %d put back: %zu logic node(s), %zu placed and %zu created "
        "character(s), %zu deleted, %zu prop(s) reopened, %zu breakable(s), %zu chest(s) "
        "made again",
        level_dungeon.c_str(), level_depth, state.logic.size(), pass.placed, pass.created, deleted,
        pass.opened, pass.breakables, pass.triggers);
    log_scene_census(" as put back");
}

}  // namespace tl::core
