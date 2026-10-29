// The gameplay half of render/renderer_session.cpp (core/world.h).
#include "core/saved_orientation.h"
#include "core/world.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace tl::core {

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
    const bool by_guid = found != nullptr;
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
    if (!by_guid) copy.display_name = found->display_name;
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
        out.push_back(view);
    }
    return out;
}

void World::set_level_identity(const std::string& dungeon, int32_t depth, bool is_town,
                               bool dont_store, bool allows_portals) {
    level_dungeon = tl::core::upper_ascii(dungeon);
    level_depth = depth;
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
void World::store_level_state() {
    tl::core::TravelRecord* travel = config.travel;
    if (travel == nullptr || level_dungeon.empty()) return;

    // `updateDungeonTracking` records the dungeon's salt (`CDungeon +0x58`).
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

    std::vector<MachineView> views = machines();

    // `CCharacter::unitInit` stocked every merchant when it was made; here
    // that waits for a frame in control, which a warp can come before.
    step_merchants(0.0f);

    // Step 5: formations not yet triggered (`CLevelState::addFormations`):
    // the layout and where the node stands, for each whose `+0x108`
    // is clear.
    for (const PendingFormation& formation : pending_formations) {
        if (formation.woken) continue;
        // `CFormationNodeSaveAndLoad::CFormationNodeSaveAndLoad(node)`:
        // pending is the node's `+0x108` negated,
        // the layout its file's path, the position its world one.
        tl::formats::FormationSaveState saved;
        saved.pending = 1;
        saved.layout = formation.reference;
        saved.position = {formation.at.x, formation.at.y, formation.at.z};
        state.formations.push_back(std::move(saved));
    }

    // Step 6: the items. The level keeps only Unit Trigger props, with their state
    // (CTriggerUnit::fillSaveState: +0x68 = +0x248).
    for (const MachineView& view : views) {
        for (const tl::core::LevelScript::NodeRecord& n :
             view.script->nodes_of_type(tl::formats::cmp_type::kUnitTrigger)) {
            tl::formats::ItemSaveState item;
            item.name = n.unit;
            item.object_id = n.node;
            item.hierarchy = view.hierarchy;
            item.from_layout = 1;
            item.enabled = n.enabled ? 1 : 0;
            item.position = {n.position.x, n.position.y, n.position.z};
            item.centre = item.position;
            // `CItem::fillSaveState`: +0x80 =
            // `+0x17c`, the room `getRoomIndexThatPositionIsIn` finds at the
            // item's centre the first time it is asked.
            item.room = tl::core::room_index_that_position_is_in(level_rooms, n.position);
            const int64_t key = instance_key(n.node, view.scene, view.hierarchy);
            tl::core::LevelScript::UnitTriggerView held;
            item.value = view.script->unit_trigger(n.node, held) ? held.state : 0;
            // `CItem::fillSaveState`: +0x5e =
            // the unit's `+0x18d`.
            item.collideable = trigger_not_collidable.count(key) == 0 ? 1 : 0;
            // One `populate` made (`place_populated_trigger`) is not the
            // layout's: it goes down as created, with its turn, and is made
            // again when the level is put back.
            const auto populated = populated_triggers.find(n.node);
            if (populated != populated_triggers.end()) {
                item.from_layout = 0;
                save_orientation(populated->second.facing, item.orientation);
            }
            state.items.push_back(std::move(item));
        }
    }
    // The map portals (`CPlayer::openMapPortal`): created items, each with the
    // `WARP` effect its manager holds (`CItemSaveState +0xe0`, the passive
    // list) and whether it is still on (`+0x5d`).
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
        item.effects[0].push_back(tl::core::effect_save_record(warp));
        state.items.push_back(std::move(item));
    }
    // INVENTED: the original keeps every item lying in the level (CItem::fillSaveState,
    // not read). Here name, place and count (`value`).
    for (const DroppedWorldItem& dropped : dropped_items) {
        if (dropped.flying || dropped.node == nullptr) continue;
        if (dropped.is_gold) {
            // CItemGold::fillSaveState: GUID -1, the
            // position, the orientation (+0x9c), the amount in +0x68; the name is not written.
            tl::formats::ItemSaveState gold;
            gold.unit_guid = -1;
            gold.from_layout = 0;
            gold.enabled = 1;
            gold.value = dropped.pile.amount;
            const ogre::Vector3 at = view_position(dropped.node);
            gold.position = {at.x, at.y, at.z};
            gold.centre = gold.position;
            save_orientation(dropped.rest, gold.orientation);
            state.items.push_back(std::move(gold));
            continue;
        }
        tl::formats::ItemSaveState item;
        item.name = dropped.item.name;
        item.unit_guid = dropped.item.unit_guid;
        item.from_layout = 0;
        item.enabled = 1;
        item.value = dropped.count;
        item.i170 = dropped.item.quest_guid;
        item.i178 = dropped.item.quest_unit_id;
        const ogre::Vector3 at = view_position(dropped.node);
        item.position = {at.x, at.y, at.z};
        item.centre = item.position;
        state.items.push_back(std::move(item));
    }

    // The breakables still standing.
    // INVENTED: a broken one is left out, the GUID not kept, and its spawner not rejoined.
    for (const BreakableWorld& b : breakables) {
        if (b.node == nullptr || !b.state.usable) continue;
        tl::formats::ItemSaveState item;
        item.name = b.unit;
        item.unit_guid = 0;
        item.from_layout = 0;
        item.enabled = 1;
        item.collideable = b.state.collideable ? 1 : 0;
        item.value = 1;
        const auto spawner = spawner_units.find(b.id);
        if (spawner != spawner_units.end()) item.spawner_id = spawner->second.spawner;
        const ogre::Vector3 at = view_position(b.node);
        item.position = {at.x, at.y, at.z};
        item.centre = item.position;
        save_orientation(view_orientation(b.node), item.orientation);
        state.items.push_back(std::move(item));
    }

    // Step 7: the characters, alive and not the pet.
    for (const MachineView& view : views) {
        for (const tl::core::LevelScript::CharacterRecord& c : view.script->living_characters()) {
            tl::formats::CharacterSaveState saved;
            // INVENTED: `createUnit` finds a unit by its `UNIT_GUID` (+0x20); this
            // engine's unit book is keyed by `NAME`, so the name is kept in +0x40.
            saved.name = c.unit;
            saved.object_id = c.node;
            saved.from_layout = c.from_layout ? 1 : 0;
            saved.enabled = 1;
            saved.level = c.level;
            saved.hp = c.hp;
            saved.depth = level_depth;
            // `CCharacter::fillSaveState`:
            // +0x184 = `+0x17c`, the room of the position it saves.
            saved.room = tl::core::room_index_that_position_is_in(level_rooms, c.position);
            saved.position = {c.position.x, c.position.y, c.position.z};
            saved.orientation = orientation_bytes(c.facing);
            for (const auto& entry : spawned) {
                for (const SpawnedUnit& unit : entry.second) {
                    if (unit.id == c.node) saved.spawner_id = entry.first;
                }
            }
            // `CCharacter::fillSaveState`:
            // the clock (`+0x388` into +0xf8) and, for a merchant, its
            // inventory, one `CItemSaveState` per occupied slot as the player's
            // are written.
            const auto write_inventory = [&saved](const tl::core::Inventory& inventory) {
                const std::vector<tl::core::InventorySlot>& slots = inventory.slots();
                for (size_t i = 0; i < slots.size(); ++i) {
                    if (slots[i].count <= 0) continue;
                    tl::formats::ItemSaveState item;
                    item.name = slots[i].item.name;
                    item.slot = static_cast<int32_t>(i);
                    item.value = slots[i].count;
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
            state.characters.push_back(std::move(saved));
        }
    }

    // Step 9: the automap.
    if (automap_initialized) view_store_automap(state);

    // Step 10: the logic nodes, per room layout.
    for (const MachineView& view : views) {
        view.script->store_logic_states(view.scene, view.hierarchy, state.logic);
    }

    const size_t characters = state.characters.size();
    const size_t items = state.items.size();
    const size_t logic = state.logic.size();

    // Step 11. The quests `updateStoredLevels` asks about (`CPlayer+0x868`) are
    // the random quests bound to a dungeon level, which this engine does not
    // generate, so none holds a level.
    tl::core::update_stored_levels(*travel, level_dungeon, level_depth, {}, level_memory_rules());
    TL_LOG_INFO(
        "level: %s depth %d remembered (%zu character(s), %zu item(s), %zu logic "
        "node(s)); %zu level(s) kept",
        level_dungeon.c_str(), level_depth, characters, items, logic, travel->levels.size());
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
    tl::core::TravelRecord* travel = config.travel;
    if (travel == nullptr || level_dungeon.empty()) return;
    const tl::formats::LevelSaveState* found =
        tl::core::get_level_saved_state(*travel, level_dungeon, level_depth);
    if (found == nullptr) return;
    const tl::formats::LevelSaveState state = *found;
    level_restored = true;

    std::vector<MachineView> views = machines();

    // 1. Logic node states, per room layout.
    for (MachineView& view : views) {
        std::vector<tl::core::ScriptEvent> events;
        view.script->restore_logic_states(view.scene, view.hierarchy, state.logic,
                                          quest_log.state(), events);
        if (!events.empty()) apply_script_events(events, *view.nodes);
    }

    // 2. The automap, when its size still matches.
    if (automap_initialized) view_restore_automap(state);

    // A saved monster's hands (CCharacter::applySaveState). DIVERGENCE: older port saves
    // hold no monster inventory; such a monster keeps (or rolls) what making it gives.
    const auto saved_hands = [this](const tl::formats::CharacterSaveState& saved,
                                    tl::core::LevelScript::CharacterRecord& record) {
        if (merchants.count(saved.object_id) != 0 || combiners.count(saved.object_id) != 0) return;
        const tl::core::UnitStats stats = stats_for_unit(saved.name);
        const bool rolls = !stats.spawn_right_hand.empty() || !stats.spawn_left_hand.empty();
        if (saved.inventory.empty() && rolls) return;
        record.equipment = true;
        for (const tl::formats::ItemSaveState& item : saved.inventory) {
            if (item.slot == tl::core::equip_location::kRightHand) record.right_hand = item.name;
            if (item.slot == tl::core::equip_location::kLeftHand) record.left_hand = item.name;
        }
        record.weapon_attack = weapon_attack_of(record.right_hand);
        record.left_weapon_attack = weapon_attack_of(record.left_hand, tl::core::Hand::Left);
    };

    // 3. Characters, in the state's order.
    std::map<tl::core::LevelScript*, std::set<int64_t>> kept;
    size_t placed = 0;
    size_t created = 0;
    for (const tl::formats::CharacterSaveState& saved : state.characters) {
        const ogre::Vector3 world(saved.position.x, saved.position.y, saved.position.z);
        const float world_yaw = yaw_of_bytes(saved.orientation);
        if (saved.from_layout != 0) {
            // `CLevel::getCharacterByOriginalGuid(id, -1)`: the first
            // character in the level with that id.
            bool matched = false;
            for (MachineView& view : views) {
                tl::core::Vec3 unused;
                if (!view.script->unit_position(saved.object_id, unused)) continue;
                tl::core::LevelScript::CharacterRecord record;
                record.node = saved.object_id;
                record.unit = saved.name;
                record.position = {world.x, world.y, world.z};
                record.facing = world_yaw;
                record.hp = saved.hp;
                record.level = saved.level;
                saved_hands(saved, record);
                std::vector<tl::core::ScriptEvent> events;
                view.script->apply_character_record(record, events);
                if (!events.empty()) apply_script_events(events, *view.nodes);
                restore_merchant(saved);
                restore_combiner(saved);
                kept[view.script].insert(saved.object_id);
                matched = true;
                ++placed;
                break;
            }
            if (!matched) {
                TL_LOG_WARN("level: unable to find editor character %lld (%s)",
                            static_cast<long long>(saved.object_id), saved.name.c_str());
            }
            continue;
        }
        // `CResourceManager::createUnit` at the saved position, then `applySaveState`.
        const int64_t owner = saved.spawner_id >= 0 ? saved.spawner_id : 0;
        const ogre::Quaternion facing(ogre::Radian(world_yaw), ogre::Vector3::UNIT_Y);
        if (spawn_unit(saved.name, world, owner, saved.level, saved.champion != 0,
                       /*spawn_anim=*/false, facing) == nullptr) {
            TL_LOG_WARN("level: could not bring back '%s'", saved.name.c_str());
            continue;
        }
        const SpawnedUnit& made = spawned[owner].back();
        tl::core::LevelScript::CharacterRecord record;
        record.node = made.id;
        record.unit = saved.name;
        record.position = {world.x, world.y, world.z};
        record.facing = world_yaw;
        record.hp = saved.hp;
        record.level = saved.level;
        saved_hands(saved, record);
        // `CResourceManager::createUnit` with `from_save` set: `unitInit`
        // skips `createDefaultEquipment`.
        const tl::core::LevelScript::UnitCreation creation =
            script.roll_unit_creation(saved.name, /*default_equipment=*/!record.equipment);
        script.add_spawned_unit(made.id, saved.name, tl::core::Vec3{world.x, world.y, world.z},
                                /*hunts=*/false, world_yaw, &creation);
        std::vector<tl::core::ScriptEvent> events;
        script.apply_character_record(record, events);
        if (!events.empty()) apply_script_events(events, scripted_nodes);
        ++created;
    }

    // What `CMonsterDescriptor::descriptorSceneActivated`
    // marked on a restored level that is not
    // a town, and the state did not claim, goes.
    size_t deleted = 0;
    if (!level_is_town) {
        for (MachineView& view : views) {
            std::vector<tl::core::ScriptEvent> events;
            deleted += view.script->delete_placed_monsters_except(kept[view.script], events).size();
            if (!events.empty()) apply_script_events(events, *view.nodes);
        }
    }

    // 4. Items: each `Unit Trigger` back in its state (`CTriggerUnit::applySaveState`
    // `setTriggerState(+0x68)`).
    size_t opened = 0;
    // DIVERGENCE: an older port's level state holds each trigger state one higher than
    // +0x248; a value above MAXSTATES - 1 marks such a state, and all are taken one lower.
    // A map portal the player opened here (CPlayer::openMapPortal): made again with its
    // WARP, on or off as it was left.
    load_unit_types();
    const auto map_portal_item = [this](const tl::formats::ItemSaveState& item) {
        if (item.from_layout != 0 || item.name.empty()) return false;
        // The answer is whether it has a mesh; a portal's body is a layout.
        TriggerProp prop;
        trigger_prop(item.name, prop);
        if (!prop.found) return false;
        return unit_types.isa(unit_types.id_of(prop.unittype), tl::core::unit_type::kMapPortal);
    };
    size_t map_portals_back = 0;
    for (const tl::formats::ItemSaveState& item : state.items) {
        if (!map_portal_item(item)) continue;
        std::string warp;
        for (const std::vector<tl::formats::EffectSaveState>& list : item.effects) {
            for (const tl::formats::EffectSaveState& effect : list) {
                if (warp.empty() && effect.i1c == tl::core::kEffectTypeWarp) warp = effect.names[0];
            }
        }
        if (add_map_portal(ogre::Vector3(item.position.x, item.position.y, item.position.z), warp,
                           item.enabled != 0)) {
            ++map_portals_back;
        }
    }
    if (map_portals_back > 0) TL_LOG_INFO("level state: %zu map portal(s) back", map_portals_back);

    int32_t stored_offset = 0;
    for (const tl::formats::ItemSaveState& item : state.items) {
        if (item.name.empty() || is_breakable_unit(item.name) || map_portal_item(item)) continue;
        TriggerProp named;
        trigger_prop(item.name, named);
        if (named.found && item.value > named.max_states - 1) stored_offset = 1;
    }
    if (stored_offset != 0) {
        TL_LOG_INFO(
            "level state: trigger states stored one higher (before this build), "
            "read one lower");
    }
    // The trigger props `populate` made (stored as created): made again where
    // they stood, then given the state they were left in - a chest opened
    // stays open. INVENTED: the original makes any created item through
    // `createUnit` and `applySaveState` in one pass over the list.
    size_t triggers_back = 0;
    for (const tl::formats::ItemSaveState& item : state.items) {
        if (item.from_layout != 0 || item.name.empty() || is_breakable_unit(item.name)) continue;
        if (map_portal_item(item)) continue;
        TriggerProp prop;
        if (!trigger_prop(item.name, prop)) continue;
        const int64_t id = next_spawned_id();
        if (!place_populated_trigger(
                item.name, ogre::Vector3(item.position.x, item.position.y, item.position.z),
                saved_orientation(item.orientation), id)) {
            continue;
        }
        ++triggers_back;
        for (MachineView& view : machines()) {
            bool here = false;
            for (const tl::core::LevelScript::NodeRecord& n :
                 view.script->nodes_of_type(tl::formats::cmp_type::kUnitTrigger)) {
                if (n.node == id) here = true;
            }
            if (!here) continue;
            std::vector<tl::core::ScriptEvent> events;
            if (item.collideable == 0) {
                tl::core::ScriptEvent e;
                e.action = tl::core::ScriptAction::TriggerCollidable;
                e.node_id = id;
                e.name = item.name;
                e.on = false;
                events.push_back(e);
            }
            view.script->restore_unit_trigger(id, item.value - stored_offset, events);
            if (!events.empty()) apply_script_events(events, *view.nodes);
            break;
        }
    }
    if (triggers_back > 0) {
        collision.build();
        register_door_avoidance();
        if (passability.ready()) nav.build(passability);
        views = machines();
    }

    // As restoreLevelState finds it: CLevel::getItemByOriginalGuid(+0x30, +0x38, +0x80,
    // +0x40): GUID, room
    // (getRoomIndexThatPositionIsIn), the copy with the state's hierarchy hash or the last.
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

    // `CItem::applySaveState` first: `+0x18d`
    // = the state's +0x5e, so a wall a `Not Collidable` took out stays out.
    for (const tl::formats::ItemSaveState& item : state.items) {
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
    for (const tl::formats::ItemSaveState& item : state.items) {
        if (item.from_layout == 0) continue;
        {
            const auto [view, id] = layout_item(item);
            if (view == nullptr) continue;
            // `applySaveState`'s forced `setTriggerState`, whose event puts the
            // prop in the state (see the `TriggerState` handler's DIVERGENCE).
            std::vector<tl::core::ScriptEvent> events;
            view->script->restore_unit_trigger(id, item.value - stored_offset, events);
            apply_script_events(events, *view->nodes);
            tl::core::LevelScript::UnitTriggerView after;
            if (view->script->unit_trigger(id, after) && after.state != after.first) {
                ++opened;
            }
        }
    }

    // The items on the ground (see `store_level_state`). INVENTED: an item's
    // quest tag is not in the stored record; one that an open, placed
    // objective of an active quest wants is tagged for it again.
    size_t breakables_back = 0;
    for (const tl::formats::ItemSaveState& item : state.items) {
        if (item.from_layout != 0) continue;
        // INVENTED: an item this port stored before gold was kept went down
        // with GUID -1 and its name, so only a nameless -1 is gold here.
        if (item.unit_guid == -1 && item.name.empty()) {
            // restoreLevelState: GUID -1 is CItemGold(rm, +0x68) at +0x84 (make_gold_pile
            // draws turn and size again), then CItemGold::applySaveState:
            // position, the saved orientation over the drawn turn, name,
            // amount.
            const tl::core::GoldPile pile =
                tl::core::make_gold_pile(item.value, tl::core::volatile_random(), gold_sounds());
            const size_t before = dropped_items.size();
            place_gold(pile, ogre::Vector3(item.position.x, item.position.y, item.position.z),
                       nullptr, /*drop_sound=*/false);
            const bool turned = std::any_of(item.orientation.begin(), item.orientation.end(),
                                            [](uint8_t b) { return b != 0; });
            if (turned && dropped_items.size() > before && dropped_items.back().node != nullptr) {
                const ogre::Quaternion facing = saved_orientation(item.orientation);
                dropped_items.back().rest = facing;
                view_set_orientation(dropped_items.back().node, facing);
            }
            continue;
        }
        if (map_portal_item(item)) continue;
        if (is_breakable_unit(item.name)) {
            // A breakable that was standing when the level was left: made
            // again where it stood (`createUnit`, `applySaveState`, `addItem`).
            const ogre::Quaternion facing = saved_orientation(item.orientation);
            const ogre::Vector3 at(item.position.x, item.position.y, item.position.z);
            if (place_breakable(item.name, at, facing, next_spawned_id(), &at)) ++breakables_back;
            continue;
        }
        const tl::core::ItemStats* stats = items.find(item.name);
        if (stats == nullptr) continue;
        // `CGameClient::restoreLevelState` makes it through `createUnit` with
        // `from_save` set.
        tl::core::ItemStats copy = create_item(*stats, true, true);
        copy.quest_guid = item.i170;
        copy.quest_unit_id = item.i178;
        place_ground_item(copy, std::max(1, static_cast<int>(item.value)),
                          ogre::Vector3(item.position.x, item.position.y, item.position.z),
                          ogre::Quaternion::IDENTITY, quest_tag_of_item(copy));
    }

    // 5. Formations: a `CFormationNode` for each saved one
    // (`CFormationNode::CFormationNode(rm, CFormationNodeSaveAndLoad*)`
    // `setLayoutFile`, `setPosition`), waiting as
    // before.
    size_t formations_back = 0;
    for (const tl::formats::FormationSaveState& saved : state.formations) {
        if (saved.pending == 0) continue;
        if (add_pending_formation(
                saved.layout, ogre::Vector3(saved.position.x, saved.position.y, saved.position.z),
                level_monster_level)) {
            ++formations_back;
        }
    }
    if (formations_back > 0) TL_LOG_INFO("level: %zu formation(s) wait again", formations_back);

    // `restoreLevelState`'s last step before `updateNPCIcons`: a
    // merchant with nothing in its Armor, Weapon and Spell panes restocks.
    populate_new_merchants();

    TL_LOG_INFO(
        "level: %s depth %d put back: %zu logic node(s), %zu placed and %zu created "
        "character(s), %zu deleted, %zu prop(s) reopened, %zu breakable(s), %zu chest(s) "
        "made again",
        level_dungeon.c_str(), level_depth, state.logic.size(), placed, created, deleted, opened,
        breakables_back, triggers_back);
    log_scene_census(" as put back");
}

}  // namespace tl::core
