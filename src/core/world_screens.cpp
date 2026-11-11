// The gameplay half of render/renderer_screens.cpp (core/world.h).
#include "core/run_config.h"
#include "core/world.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace tl::core {

/// The `PORTRAIT` key on a unit's own record, as CQuestDialogMenu::setNPC reads it for
/// CGameUI::getImageFromImageSet. None authored, none drawn.
std::string World::portrait_of(const std::string& unit) {
    // `unit_book` is keyed by the lower-cased name and only holds what a level
    // has already placed, so a quest giver who is not in this level - or who is
    // and has not been asked about yet - has to be indexed first.
    const std::string key = lowered(unit);
    index_unit(key);
    const tl::core::UnitStats* stats = unit_book.find(key);
    return stats != nullptr ? stats->portrait : std::string();
}

bool World::is_enchant_shrine(const std::string& unit) {
    TriggerProp prop;
    trigger_prop(unit, prop);
    if (prop.unittype.empty()) return false;
    load_unit_types();
    return unit_types.isa(prop.unittype, tl::core::unit_type::kEnchantShrine);
}

// A `Unit Trigger`'s prop that ISAs `FISHING HOLE` (0x7a): the test
// `CCharacter::getItemAI` makes before it
// calls `startFishing` instead of `interact` (core/fishing.h).
bool World::is_fishing_hole(const std::string& unit) {
    TriggerProp prop;
    trigger_prop(unit, prop);
    if (prop.unittype.empty()) return false;
    load_unit_types();
    return unit_types.isa(prop.unittype, tl::core::unit_type::kFishingHole);
}

void World::step_fishing(float delta, bool catch_press, bool stop_press) {
    using State = tl::core::FishingState;
    // `queueBlendAnimation`: the queued animation once the one-shot has run out.
    if (fishing_animation_once && !fishing_queued.empty() &&
        !fishing_animation_playing(fishing_animation)) {
        play_fishing_animation(fishing_queued, 1.0f, false, "");
    }

    switch (fishing_state) {
        case State::Off: return;
        case State::Cast:
            // `CCharacter::fishingAICast`:
            // 0x21 once the cast is over.
            if (!fishing_animation_playing("FISHING_CAST")) fishing_state = State::Waiting;
            break;
        case State::Waiting: {
            // CFishingMenu::onClick: false is catchFish (vtable +0x16c),
            // true stops unless in 0x22. INVENTED: the button-to-call dispatch.
            if (stop_press) {
                TL_LOG_INFO("fishing: stopped");
                stop_fishing();
                return;
            }
            if (catch_press) {
                catch_fish();
                break;
            }
            // `CPlayer::fishingAI`.
            const bool animating = fishing_animation_playing("FISHING_CAST") ||
                                   fishing_animation_playing("FISHING_BITE");
            if (tl::core::step_fishing(bobber, delta, animating, tl::core::volatile_random())) {
                // `CCharacter::fishingSplash`:
                // unless it plays already,
                // `FISHING_BITE` at half speed, `FISHING_IDLE` queued.
                if (!fishing_animation_playing("FISHING_BITE")) {
                    play_fishing_animation("FISHING_BITE", 0.5f, true, "FISHING_IDLE");
                }
                TL_LOG_INFO("fishing: a bite, %.3f s to catch it",
                            static_cast<double>(bobber.bite));
            }
            break;
        }
        case State::Catch:
            // `CPlayer::fishingAICatch`:
            // nothing while `FISHING_CATCH` plays.
            if (!fishing_animation_playing("FISHING_CATCH")) land_catch();
            break;
    }
}

void World::set_godspeed(bool on) {
    god_mode = on;
    ensure_effect_data();
    // `CBaseUnit::removeAffix` / `addAffix`
    // on the player's manager, level 1 and
    // duration -1.
    const bool had = player_effects.delete_affix("PLAYER_SPEED_UP");
    bool has = false;
    if (on) has = player_effects.add_affix_by_name("PLAYER_SPEED_UP", 1, &affixes, -1.0f);
    const float running =
        tl::core::character_speed(player_stats.movement.running_speed, player_total());
    TL_LOG_INFO("debug: GODSPEED %s (god mode %s; speed boost %s; running speed %.2f)",
                on ? "on" : "off", god_mode ? "on" : "off",
                has   ? "added"
                : had ? "removed"
                : on  ? "NOT FOUND"
                      : "was not on",
                static_cast<double>(running));
}

void World::level_up_to(int level) {
    const int from = player_character.level();
    while (player_character.level() < level) {
        const int before = player_character.level();
        const uint64_t gate = player_character.experience_gate();
        const uint64_t held = player_character.experience();
        if (gate <= held) break;
        if (!award_player_experience(static_cast<int>(gate - held), player_health.dead) ||
            player_character.level() == before) {
            break;
        }
    }
    // Once, not once per level: one chime, one banner, one particle.
    if (player_character.level() != from) player_levelled_up();
    TL_LOG_INFO("debug: LEVEL %d asked, level %d -> %d", level, from, player_character.level());
}

void World::outfit_debug_staff() {
    ensure_item_index();
    load_unit_types();
    if (player_inventory.effect_context().types == nullptr) refresh_inventory_effects();

    // The highest item level at or under the character's; the first by `NAME`
    // among equals (the book's own order).
    const tl::core::ItemStats* best = nullptr;
    for (const auto& [key, data] : items.all()) {
        if (data.is_gold() || data.spawn_rarity <= 0) continue;
        if (!unit_types.isa(data.unittype, tl::core::unit_type::kStaff) ||
            !unit_types.isa(data.unittype, tl::core::unit_type::kWeapon)) {
            continue;
        }
        if (data.level > player_character.level()) continue;
        if (best == nullptr || data.level > best->level) best = &data;
    }
    if (best == nullptr) {
        TL_LOG_WARN("debug: no staff at or under level %d", player_character.level());
        return;
    }
    tl::core::ItemStats staff = create_item(*best, false, /*random_enchant_allowed=*/false);
    // A unique's authored affixes can leave the copy unidentified, which no
    // character that answers to requirements may wear.
    staff.identified = true;
    const std::string name = tl::core::item_name(staff);

    // The requirements with no reductions and the stats with no effects: the
    // most the wear can ask for, whatever is worn or taken off on the way.
    const tl::core::RequirementReductions none{};
    const tl::core::StatType order[4] = {tl::core::StatType::Strength,
                                         tl::core::StatType::Dexterity, tl::core::StatType::Magic,
                                         tl::core::StatType::Defense};
    const int required[4] = {tl::core::item_strength_requirement(staff, &none, unit_types),
                             tl::core::item_dexterity_requirement(staff, &none, unit_types),
                             tl::core::item_magic_requirement(staff, &none, unit_types),
                             tl::core::item_defense_requirement(staff, &none, unit_types)};
    const int points = player_character.unspent_stat_points();
    for (int i = 0; i < 4; ++i) {
        const int short_by = required[i] - player_character.base_stat(order[i]);
        const int moved = std::min(std::max(0, short_by), player_character.unspent_stat_points());
        player_character.move_stat_point(order[i], moved);
        if (moved < short_by) {
            TL_LOG_WARN("debug: %d point(s) short of '%s''s requirement %d", short_by - moved,
                        name.c_str(), required[i]);
        }
    }
    const tl::core::StatType spread[4] = {tl::core::StatType::Magic, tl::core::StatType::Defense,
                                          tl::core::StatType::Strength,
                                          tl::core::StatType::Dexterity};
    for (int i = 0; player_character.unspent_stat_points() > 0; i = (i + 1) % 4) {
        player_character.move_stat_point(spread[i], 1);
    }
    TL_LOG_INFO("debug: %d stat point(s) spent: str %d dex %d mag %d def %d (requires %d/%d/%d/%d)",
                points, player_character.base_stat(order[0]), player_character.base_stat(order[1]),
                player_character.base_stat(order[2]), player_character.base_stat(order[3]),
                required[0], required[1], required[2], required[3]);

    const int slot = player_inventory.pickup(staff, 1, unit_types, /*equip_when_full=*/false);
    if (slot < 0) {
        TL_LOG_WARN("debug: no room in the bag for '%s'; it goes on the floor", name.c_str());
        drop_item_near_player(staff, 1);
    } else {
        TL_LOG_INFO("debug: '%s' (%s, item level %d) in the bag at slot %d", name.c_str(),
                    best->name.c_str(), staff.level, slot);
        wear_player_item(static_cast<size_t>(slot));
    }
    sync_equipped_weapon();
    refresh_skill_effects();
    update_menu();
    update_character_preview(0.0f);
}

void World::debug_level_row(int level) {
    level_up_to(level);
    outfit_debug_staff();
}

/// Finish a quest and hand over what it pays: `CQuestManager::completeQuest`,
/// whose payout `pay_quest_completions`
/// carries out.
bool World::complete_quest(const std::string& name) {
    if (!quest_log.complete(name)) return false;
    quest_completed(name);
    return true;
}

// `CQuestDialogMenu::onClick` -> `CQuestManager::completeQuest`,
// with no test of the quest's state.
bool World::complete_quest_by_dialog(const std::string& name) {
    load_quest_book();
    const tl::core::QuestDefinition* quest = quest_book.find(name);
    if (quest == nullptr || !quest_log.complete_by_dialog(*quest)) return false;
    quest_completed(name);
    return true;
}

// What a `Quest Controller`'s change of a quest leads to past the level's machines:
// `CQuestController::setQuestComplete(true)` ends in
// `CQuestManager::completeQuest`; Force Not Accepted / Not
// Complete in `removeQuest(quest, false)` and `resetQuest`.
void World::quest_controller_changed(const tl::core::ScriptEvent& event) {
    if (event.status == tl::core::QuestStatus::Unknown) quest_log.drop(event.text);
    if (event.status == tl::core::QuestStatus::Complete) {
        load_quest_book();
        if (const tl::core::QuestDefinition* quest = quest_book.find(event.text)) {
            quest_log.apply_completion_consequences(*quest);
        }
        pay_quest_completions();
    }
    report_gates_waiting_on(event.text);
    // `giveQuest` / `completeQuest` both end in `updateAutomapIcons` and `updateNPCIcons`.
    update_automap_icons();
    update_npc_icons();
}

void World::quest_completed(const std::string& name) {
    pay_quest_completions();
    report_gates_waiting_on(name);
    // `CQuestManager::completeQuest`
    // refreshes the icons.
    update_automap_icons();
    update_npc_icons();
    update_menu();
    refresh_game_hud();
}

// `CQuest::giveRewardForQuest`, its payout: the log runs it
// before the quests it gives and `cleanUp`'s `reinitializeQuest` (`QuestLog::set_payout`).
void World::pay_quest_completion(const tl::core::QuestCompletion& done) {
    if (done.quest == nullptr) return;
    // CQuest::giveRewardForQuest: incrementJournalStatistic(4, 1)
    count_journal(tl::core::JournalStatistic::QuestsCompleted);
    if (done.held && done.quest->reward_spec.any()) reward_player(done);
    TL_LOG_INFO("quest complete: %s%s", done.quest->name.c_str(),
                done.held ? "" : " (not held - nothing to pay)");
    // No banner here: "Quest Complete" is questEventUpdate's; completeQuest's
    // setIsComplete sends event 4, which it skips for a complete quest.
}

void World::pay_quest_completions() {
    const std::vector<tl::core::QuestCompletion> done_list = quest_log.take_completions();
    for (const tl::core::QuestCompletion& done : done_list) {
        if (!done.paid) pay_quest_completion(done);
    }
    // `CQuest::giveRewardForQuest` ends in `cleanUp(true)`, after the payout:
    // what the quests take back (`QuestLog` queued it with each completion),
    // and the pets their lines made.
    apply_quest_item_removals();
    for (const tl::core::QuestCompletion& done : done_list) {
        if (done.quest != nullptr) release_quest_pets(done);
    }
    if (!done_list.empty()) {
        update_menu();
        refresh_game_hud();
    }
}

/// The banner and the narrator, when a quest is taken or finished: CQuestManager::giveQuest
/// and questEventUpdate,
/// slot 31 QUESTRECEIVED_SOUND or 32 QUESTCOMPLETED_SOUND on the player's bank,
/// then the big text and the quest's name for 2 s.
void World::announce_quest(const tl::core::QuestDefinition& quest, bool complete) {
    // `NO_NOTIFICATION` is the quest saying to keep quiet, and
    // `SHOWQUESTCOMPLETE` gates the completion half specifically.
    if (quest.no_notification) return;
    if (complete && !quest.show_quest_complete) return;

    set_big_text(complete ? "Quest Complete" : "New Quest", kQuestBannerSeconds);
    set_small_text(
        tl::core::strip_markup(quest.display_name.empty() ? quest.name : quest.display_name),
        kQuestBannerSeconds);

    // Two sounds from two banks: the narrator, and the quest
    // manager's own chime.
    play_ui(complete ? tl::core::InterfaceSound::QuestCompleted
                     : tl::core::InterfaceSound::QuestReceived);

    const tl::core::SoundSlot slot =
        complete ? tl::core::SoundSlot::QuestCompleted : tl::core::SoundSlot::QuestReceived;
    // Worth a line: a banner that does not come up and a narrator who does not
    // speak look identical from the sofa, and the two have different causes.
    TL_LOG_INFO("quest banner: '%s' / '%s' for %.1fs, %s -> '%s'", big_banner.text.c_str(),
                small_banner.text.c_str(), static_cast<double>(kQuestBannerSeconds),
                tl::core::kSoundSlotKeys[static_cast<size_t>(slot)],
                player_stats.sounds[slot].c_str());
    // Volume 1 and a second's delay: `giveQuest`,
    // `questEventUpdate` (both 1.0f).
    play_voice(slot, 1.0f, 1.0f);
    refresh_game_hud();
}

/// Report that a group in this level was gated on `quest`. Gates are evaluated only at
/// load, as in the original: CRandomGroup::getGroupCanBeCreated()
/// is called
/// only by CRandomGroupDescriptor::descriptorSceneLoaded and rollRandomGroups.
void World::report_gates_waiting_on(const std::string& quest) {
    if (quest.empty() || script_layout.nodes.empty()) return;

    size_t groups = 0;
    for (const tl::formats::CmpNode& node : script_layout.nodes) {
        if (node.type != tl::formats::cmp_type::kGroup) continue;
        if (script_gated.count(node.id) == 0) continue;
        // This group specifically, not any group that happens to be open now:
        // completing the intro opens several at once, and reporting the whole
        // set against each quest in turn says nothing.
        if (!tl::core::group_gates_on(node, quest)) continue;
        if (!tl::core::group_is_open(node, quest_log.state())) continue;
        ++groups;
    }
    if (groups == 0) return;
    if (!reported_gates.insert(quest).second) return;

    TL_LOG_INFO(
        "quest '%s': %zu group(s) this level held back would now be open - "
        "they stay closed until the level is loaded again, which is what the "
        "original does too (see report_gates_waiting_on)",
        quest.c_str(), groups);
}

/// The unit a quest's `TREASURECLASS` rolls to, or empty. CQuestRewards::calculateRewards:
/// CSpawnClassParser::getSpawnClass, CSpawnClass::rollSpawnClass,
/// CResourceManager::createUnit. A class picking by UNITTYPE gives empty. The quest's own
/// seed, so offer and payout agree.
std::vector<std::string> World::quest_reward_items(const tl::core::QuestDefinition& quest,
                                                   int level, int32_t seed) {
    std::vector<std::string> handed;
    // The units the quest's offer already made (`CQuestRewards +0x44`), when
    // an NPC has offered it.
    const tl::core::QuestEntry* held = quest_log.find(quest.name);
    const tl::core::QuestLog::Offered* rolled = quest_log.offered(quest);
    const std::vector<tl::core::QuestRewardUnit>* made =
        held != nullptr && !held->reward_units.empty()       ? &held->reward_units
        : rolled != nullptr && !rolled->reward_units.empty() ? &rolled->reward_units
                                                             : nullptr;
    if (made != nullptr) {
        for (const tl::core::QuestRewardUnit& unit : *made) handed.push_back(unit.unit);
        return handed;
    }
    // +0x30, the [UNITREWARD] units, from the groups CQuestRewards::parseRewardTag
    // names. DIVERGENCE: a fifth group reads past the
    // four names on the console; here nothing. INVENTED: IDENTIFY not applied.
    for (const tl::core::QuestRewardSpec::UnitReward& reward : quest.reward_spec.unit_rewards) {
        if (reward.unit.empty()) continue;
        bool found = false;
        if (reward.resource_group == 0) {
            ensure_item_index();
            found = items.find(reward.unit) != nullptr;
        } else if (reward.resource_group < 4) {
            index_unit(reward.unit);
            found = unit_book.find(reward.unit) != nullptr;
        }
        if (found) handed.push_back(reward.unit);
    }
    // `+0x28`, `TREASURECLASS`: rolled for the player (`getPlayer` twice, as
    // character and finder) at the quest's level plus -1..2, with the quest's
    // seed plus 0x272b, no flags.
    if (!quest.reward_spec.treasure_class.empty()) {
        SpawnTables tables;
        build_spawn_context(tables, /*items_by_type=*/true);
        std::optional<tl::core::SpawnClass> table =
            load_spawn_class(quest.reward_spec.treasure_class, tables.context);
        if (table) {
            tl::core::RunicRandom rng(0u);
            const tl::core::TreasureRoll roll = tl::core::quest_treasure_roll(level, seed, rng);
            tl::core::SpawnCharacterContext player;
            player.level = player_character.level();
            // INVENTED: magic find (`getEffectValue(0x1b, 7)` on the player
            // and the pets) is not summed here; 0 stands in.
            std::vector<std::string> rolled;
            std::vector<bool> enchant;
            table->roll_spawn_class(rolled, &enchant, &player, &player, roll.level, roll.seed,
                                    /*level_bonus=*/0, /*rarity_override=*/-1, /*magic_find=*/0,
                                    /*flags=*/0, rng, tables.context);
            handed.insert(handed.end(), rolled.begin(), rolled.end());
        }
    }
    return handed;
}

// INVENTED: one item, the first `CQuestRewards::calculateRewards` hands over
// (`quest_reward_items`): the port's offer window and log keep a single reward
// tile.
std::string World::quest_reward_item(const tl::core::QuestDefinition& quest, int level,
                                     int32_t seed) {
    const std::vector<std::string> handed = quest_reward_items(quest, level, seed);
    return handed.empty() ? std::string() : handed.front();
}

// PORT: the reward item's icon looked up for the port's journal and offer window.
void World::resolve_reward_item(tl::core::QuestEntry& entry) {
    if (entry.definition == nullptr) return;
    entry.reward_item = quest_reward_item(*entry.definition, entry.level, entry.seed);
    if (entry.reward_item.empty()) return;
    ensure_item_index();
    if (const tl::core::ItemStats* item = items.find(entry.reward_item)) {
        entry.reward_icon = item->icon;
    }
}

// `CQuestDialogMenu::onClick` on the unit of
// the player's talk.
void World::talk_menu_closed(int machine, int64_t node, int32_t menu_event, bool complete_line,
                             const std::string& quest) {
    tl::core::LevelScript* owner = node != 0 ? machine_script(machine) : nullptr;
    if (owner == nullptr) {
        // A unit no machine runs has nothing to hear; the quest still completes.
        if (complete_line) complete_quest_by_dialog(quest);
        return;
    }
    std::vector<tl::core::ScriptEvent> events;
    owner->menu_closed_on(node, menu_event, complete_line, quest, quest_log.state(), events);
    apply_machine_events(machine, events);
}

void World::finish_talk_window(int32_t menu_event) {
    // A [COMPLETE] line closing finishes the quest: CQuestDialogMenu::onClick
    // calls CQuestManager::completeQuest on that branch.
    const bool complete_line =
        menu_event == 2 && offered_section == tl::core::QuestSection::Complete;
    const std::string quest = offered_quest != nullptr ? offered_quest->name : std::string();
    const tl::core::QuestDefinition* definition = offered_quest;
    const tl::core::DialogLine* line = offered_line;
    const int machine = talk_menu_machine;
    const int64_t node = talk_menu_node;
    close_quest_offer();
    talk_menu_closed(machine, node, menu_event, complete_line, quest);
    // `onClick`'s `OK` (10): `completeQuest` for a `[COMPLETE]` line, then
    // `giveOrRemoveItems`. `Accept` hands them over in
    // accept_offered_quest(); `Decline` (8) never does.
    if (menu_event == 2 && definition != nullptr && line != nullptr) {
        hand_dialog_items(*definition, *line);
    }
}

/// Take the quest the window offered (`CQuestDialogMenu::acceptQuest`).
///
void World::tell_player_motion(tl::core::LevelScript& machine) {
    // What a pet's `idleAI` reads off its master: `+0xb4` (sin, 0, cos) of the
    // facing, `+0x264` moving, `+0x321` a stick pushed past the walking speed,
    // `walkingSpeed()`/`runningSpeed()` (the speeds `step_player` last set),
    // and `+0x340`, the player's target.
    machine.set_player_motion(
        tl::core::Vec3{std::sin(player.facing()), 0.0f, std::cos(player.facing())}, player.moving(),
        player.moving() && player.speed() > player.stats().walking_speed);
    machine.set_player_speeds(player.stats().walking_speed, player.stats().running_speed);
    // `CGameClient +0x38d0 == 1`, a game being played (a player under control,
    // as `kill_player` reads it), and the interface's half of
    // `getUIIsInCinematic`.
    machine.set_game_playing(player_control);
    machine.set_ui_cinematic(ui_in_cinematic());
    // `player_target_node` is dropped once that unit dies or leaves.
    tl::core::LevelScript* owner =
        player_target_node != 0 ? script_owning(player_target_node) : nullptr;
    if (owner == nullptr || owner->unit_is_dead(player_target_node)) {
        player_target_node = 0;
        owner = nullptr;
    }
    machine.set_player_attack_target(owner, owner != nullptr ? player_target_node : 0);
}

bool World::enchant_shrine_spent() const {
    // `+0x1f0`, cleared when the one-shot reached its last state.
    const tl::core::LevelScript* owner =
        enchant_shrine_machine == kLevelMachine
            ? &script
            : (enchant_shrine_machine >= 0 &&
                       static_cast<size_t>(enchant_shrine_machine) < nested_scripts.size()
                   ? &nested_scripts[static_cast<size_t>(enchant_shrine_machine)].script
                   : nullptr);
    if (owner == nullptr) return false;
    for (const tl::core::LevelScript::UnitTriggerView& view : owner->every_unit_trigger()) {
        if (view.id == enchant_shrine_node) return !view.armed;
    }
    return false;
}

void World::land_catch() {
    // `CPlayer::fishingAICatch`.
    std::string text;
    if (!bobber.hooked) {
        text = tl::core::fishing_catch_text(false, false, std::string());
    } else {
        // The level's fish class, or `FISH_SPAWN` for a level with no template
        // (`+0x1d8` null); one roll, the player as character and finder, level
        // -1, flags 0 (`createUnitsBySpawnClass`).
        const std::string class_name =
            level_fish_spawn_class.empty() ? std::string("FISH_SPAWN") : level_fish_spawn_class;
        if (!loot_tables_built) {
            loot_tables_built = true;
            build_spawn_context(loot_tables, /*items_by_type=*/true);
        }
        const tl::core::SpawnContext& context = loot_tables.context;
        const tl::core::SpawnClass* spawn_class =
            context.get_spawn_class ? context.get_spawn_class(class_name) : nullptr;
        tl::core::SpawnCharacterContext me;
        me.level = player_character.level();
        me.is_hardcore = config.hardcore;
        // INVENTED: magic find (`getEffectValue(0x1b, 7)` on the player and the
        // pets) is not summed; 0 stands in, as for the quest rewards.
        std::vector<tl::core::ItemStats> made;
        if (spawn_class != nullptr) {
            made = tl::core::create_units_by_spawn_class(*spawn_class, 1, &me, &me, -1, 0,
                                                         tl::core::game_random(), context,
                                                         loot_unit_maker());
        } else {
            TL_LOG_WARN("fishing: no spawn class '%s'", class_name.c_str());
        }
        load_unit_types();
        bool made_a_fish = false;
        std::string name;
        for (const tl::core::ItemStats& item : made) {
            name = tl::core::item_name(item);
            if (unit_types.isa(item.unittype, tl::core::unit_type::kFish)) made_a_fish = true;
            // `CInventory::pickupEquipment(item, true)`, and the level at the
            // player's feet when the bag has no room.
            if (player_inventory.pickup(item, 1, unit_types) < 0) drop_item_near_player(item, 1);
            TL_LOG_INFO("fishing: '%s' from '%s'", item.name.c_str(), class_name.c_str());
        }
        text = tl::core::fishing_catch_text(true, made_a_fish, name);
        // CPlayer::fishingAICatch: incrementJournalStatistic(14, 1)
        count_journal(tl::core::JournalStatistic::FishCaught);
        update_menu();
        refresh_game_hud();
    }
    open_modal(tl::core::fishing_dialog_title(), text);
    TL_LOG_INFO("fishing: \"%s\"", text.c_str());
    stop_fishing();
}

// CQuestManager::giveQuest, CQuestManager::removeQuest,
// CQuestManager::completeQuest:
// the quest controllers. DIVERGENCE: handed over once a frame after the change
// (the script's own Force Accept/Complete go at once, LevelScript::quest_notice).
void World::deliver_quest_notices() {
    // The quests an accept or a completion has just made complete through
    // their lists of names: the banner `questEventUpdate` puts up for them.
    const std::vector<const tl::core::QuestDefinition*> finished = quest_log.take_finished();
    if (!finished.empty()) quest_event(finished);
    const std::vector<tl::core::QuestNotice> notices = quest_log.take_notices();
    for (const tl::core::QuestNotice& notice : notices) {
        for (const MachineView& view : machines()) {
            std::vector<tl::core::ScriptEvent> events;
            view.script->quest_notice(notice, quest_log.state(), events);
            if (!events.empty() && view.nodes != nullptr) apply_script_events(events, *view.nodes);
        }
    }
}

// What `CQuestRewards::calculateRewards`
// reaches outside the quest for: `createUnit`, the `TREASURECLASS` roll on
// `g_Rand` for the player as character and finder, `enchant(true)`, the
// difficulty.
tl::core::QuestRewardWorld World::quest_reward_world() {
    tl::core::QuestRewardWorld world;
    // A level and a player: the port's quests are offered and paid in play.
    world.ready = [] { return true; };
    load_stat_graphs();
    world.graphs = &stat_graphs;
    world.difficulty = [this] { return config.difficulty; };
    world.create_unit = [this](const std::string& unit, uint32_t resource_group,
                               tl::core::QuestRewardUnit& out) {
        out.unit = unit;
        if (resource_group != 0) {
            // INVENTED: `createUnit` of a monster, player or prop (its own
            // `unitInit` draws) is not made; the port hands over items only.
            index_unit(unit);
            return unit_book.find(unit) != nullptr;
        }
        ensure_item_index();
        const tl::core::ItemStats* item = items.find(unit);
        if (item == nullptr) return false;
        // `createUnit(rm, data, 0, false)` -> `createItem(guid, false, true,
        // false)`: `+0x20a` set, no forced enchant.
        out.is_item = true;
        out.item = create_item(*item, false, true, false);
        return true;
    };
    world.roll_spawn_class = [this](const std::string& spawn_class, int32_t level, uint32_t seed,
                                    std::vector<std::string>& units, std::vector<bool>& enchant) {
        SpawnTables tables;
        build_spawn_context(tables, /*items_by_type=*/true);
        std::optional<tl::core::SpawnClass> table = load_spawn_class(spawn_class, tables.context);
        if (!table) return;
        tl::core::SpawnCharacterContext player;
        player.level = player_character.level();
        // INVENTED: magic find (`getEffectValue(0x1b, 7)` on the player and
        // the pets) is not summed here; 0 stands in.
        table->roll_spawn_class(units, &enchant, &player, &player, level, seed,
                                /*level_bonus=*/0, /*rarity_override=*/-1, /*magic_find=*/0,
                                /*flags=*/0, tl::core::game_random(), tables.context);
    };
    world.enchant = [this](tl::core::ItemStats& item) {
        tl::core::enchant_item(item, true, item_roll_context());
    };
    return world;
}

bool World::accept_offered_quest(const tl::core::QuestDefinition& quest) {
    const uint32_t seed = static_cast<uint32_t>(offer_seed(quest));
    if (take_quest(quest, quest_level_of(quest), seed)) {
        if (tl::core::QuestEntry* held = quest_log.find(quest.name)) {
            held->dialog_interacted = true;
            resolve_reward_item(*held);
        }
        TL_LOG_INFO("quest accepted: %s", quest.name.c_str());
        announce_quest(quest, false);
        // `CQuestDialog::giveOrRemoveItems`,
        // which the Accept runs, once a line: the items, then with
        // `MAKE_PET_ON_ACCEPT` (+0x78) the speaker becomes the player's pet
        // (`CCharacter::addPet`) and its NPC icon is worked out again.
        const bool first_time = offered_line == nullptr || hand_dialog_items(quest, *offered_line);
        if (offered_makes_pet && first_time) {
            tl::core::LevelScript* owner =
                offered_by_node != 0 ? script_owning(offered_by_node) : nullptr;
            std::vector<tl::core::ScriptEvent> events;
            if (owner != nullptr &&
                owner->make_player_pet(offered_by_node, quest_log.state(), events)) {
                TL_LOG_INFO("pet: %s follows the player", offered_by.c_str());
                if (!events.empty()) apply_script_events(events, scripted_nodes);
            } else {
                TL_LOG_WARN("pet: %s (node %lld) is no character a machine runs; it stays",
                            offered_by.c_str(), static_cast<long long>(offered_by_node));
            }
        }
        // `CQuestManager::giveQuest` ends in
        // `updateAutomapIcons` and `CLevel::updateNPCIcons`.
        update_automap_icons();
        update_npc_icons();
    } else {
        // `CQuestDialogMenu::onClick` opens the "Your quest log is
        // full!" modal here; this port has no modal, so the refusal
        // is the `Error` sound and a log line.
        play_ui(tl::core::InterfaceSound::Error);
        TL_LOG_WARN("your quest log is full: '%s' not taken", quest.name.c_str());
        update_menu();
        return false;
    }
    update_menu();
    return true;
}

}  // namespace tl::core
