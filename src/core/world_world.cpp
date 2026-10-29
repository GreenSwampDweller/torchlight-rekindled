// The gameplay half of render/renderer_world.cpp (core/world.h).
#include "core/world.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace tl::core {

void World::configure_unit_ranges(tl::core::LevelScript& machine) {
    machine.set_unit_active_range(level_unit_active_range);
    machine.set_unit_near_range(level_unit_near_range);
    machine.set_unit_shadow_range(level_unit_shadow_range);
    machine.set_view_test([this](const tl::core::LevelScript& owner, int64_t node) {
        return unit_in_view(owner, node);
    });
    machine.set_item_near_list([this]() { item_near_list(); });
    // `CMoneyTaker::takeMoney`: the player's gold (`+0x444`) and
    // `CCharacter::giveGold`.
    tl::core::LevelScript::PlayerGold gold;
    gold.held = [this]() { return static_cast<int32_t>(player_inventory.gold()); };
    gold.give = [this](int32_t amount) {
        player_inventory.give_gold(amount);
        TL_LOG_INFO("script: a money taker takes %d gold, %d left", -amount,
                    player_inventory.gold());
    };
    machine.set_player_gold(std::move(gold));
}

bool World::in_town() const {
    // The loaded level's template `IS_TOWN` (+0x86) when a dungeon owns it; a
    // layout loaded on its own falls back to its path.
    if (!level_dungeon.empty()) return level_is_town;
    return lowered(level_reference).find("/town/") != std::string::npos;
}

void World::give_pet_spells_to_script() {
    if (pet_node_id == 0) return;
    // `learnSpell` adds each spell to the
    // pet's own skill manager, after the skills its unit file names.
    for (size_t i = 0; i < pet.skills().spells.size(); ++i) {
        const std::string& name = pet.skills().spells[i];
        if (name.empty()) continue;
        ensure_player_skill_files();
        const auto file = player_skill_files.find(tl::core::upper_ascii(name));
        if (file == player_skill_files.end()) continue;
        tl::core::LevelScript::UnitSkill skill;
        skill.known.def = &file->second->def;
        skill.known.entry.name = file->second->def.name;
        const int32_t level = std::max(1, pet.skills().spell_levels[i]);
        skill.known.invested = static_cast<uint32_t>(level);
        skill.known.effective = static_cast<uint32_t>(level);
        script.add_unit_skill(pet_node_id, skill);
    }
}

// PORT: the pet's state copied from the level machine that runs it into `Pet`.
void World::sync_pet_mirror() {
    if (pet_node_id == 0) return;
    tl::core::PetMirror mirror;
    script.unit_position(pet_node_id, mirror.position);
    script.unit_facing(pet_node_id, mirror.facing);
    script.unit_health(pet_node_id, mirror.health, mirror.max_health);
    mirror.ai_state = script.unit_ai_state(pet_node_id);
    mirror.running = script.unit_running(pet_node_id);
    mirror.pathing = script.unit_pathing(pet_node_id);
    mirror.near_death = script.unit_near_death(pet_node_id);
    mirror.town_timer = script.unit_town_timer(pet_node_id);
    mirror.transform_left = script.unit_transform_left(pet_node_id);
    mirror.transformed_from = script.unit_transformed_from(pet_node_id);
    pet.mirror(mirror);
}

// PORT: `UNITTYPES.HIE` read from the archive and handed to `UnitTypeHierarchy::parse`.
void World::load_unit_types() {
    if (unit_types_loaded) return;
    unit_types_loaded = true;
    const std::string path = archive_path_unique("UNITTYPES.HIE.adm");
    const std::vector<uint8_t> bytes = path.empty() ? std::vector<uint8_t>{} : read_resource(path);
    if (bytes.empty()) {
        TL_LOG_WARN("no media/UNITTYPES.HIE.adm: unit types are unknown");
        return;
    }
    const tl::formats::AdmFile file = tl::formats::parse_adm(bytes.data(), bytes.size());
    if (!file.ok) {
        TL_LOG_WARN("could not parse '%s': %s", path.c_str(), file.error.c_str());
        return;
    }
    unit_types.parse(file.root);
}

// CQuestManager::getNPCIcon: only an NPC (0x68); the
// dialog CQuest::getQuestDialog picks, with ICONABOVEHEAD (+0x75), gives the icon by kind
// (+0x70; CSWTCH.4688): intro 23, return 26, complete 22, passive none.
int World::npc_quest_icon(const std::string& unit, uint32_t type) {
    if (!unit_types.isa(type, tl::core::unit_type::kNpc)) return -1;
    load_quest_book();
    const tl::core::QuestDefinition* quest =
        quest_log.quest_for_npc(unit, stats_for_unit(unit).quests, tl::core::volatile_random());
    if (quest == nullptr) return -1;
    tl::core::QuestSection section = tl::core::QuestSection::Intro;
    const tl::core::DialogLine* line = quest_log.dialog_for(*quest, unit, &section);
    if (line == nullptr || !line->icon_above_head) return -1;
    switch (section) {
        case tl::core::QuestSection::Intro: return 23;
        case tl::core::QuestSection::Return: return 26;
        case tl::core::QuestSection::Complete: return 22;
        default: return -1;
    }
}

// PORT: which of the port's machines holds a unit (the level's own or a nested layout's).
tl::core::LevelScript* World::script_owning(int64_t node) {
    float hp = 0.0f;
    float max = 0.0f;
    if (script.unit_health(node, hp, max)) return &script;
    for (NestedScript& machine : nested_scripts) {
        if (machine.script.unit_health(node, hp, max)) return &machine.script;
    }
    return nullptr;
}

// PORT: a unit's side asked of the machine that holds it, else its unit data's.
tl::core::UnitStats::Alignment World::unit_side(int64_t node,
                                                const tl::core::UnitStats& stats) const {
    if (node != 0) {
        if (const auto side = script.unit_side(node)) return *side;
        for (const NestedScript& machine : nested_scripts) {
            if (const auto side = machine.script.unit_side(node)) return *side;
        }
    }
    return stats.alignment;
}

// PORT: whether a node is the player's pet, asked of the machines.
bool World::node_is_player_pet(int64_t node) const {
    if (node == 0) return false;
    if (script.unit_is_player_pet(node)) return true;
    for (const NestedScript& machine : nested_scripts) {
        if (machine.script.unit_is_player_pet(node)) return true;
    }
    return false;
}

void World::apply_pet_stance() {
    if (pet_node_id == 0) return;
    script.set_unit_stance(pet_node_id,
                           static_cast<int32_t>(tl::core::pet_stance_number(pet.stance())));
    // `CCharacter::setTarget(pet, nullptr)`: what a stance change does after
    // it, on the PC's buttons (`CGameUI::onClick`)
    // and the console's dialog alike.
    std::vector<tl::core::ScriptEvent> events;
    script.drop_unit_target(pet_node_id, events);
    if (!events.empty()) apply_script_events(events, scripted_nodes);
}

// PORT: the pet's OGRE entity destroyed and its machine unit removed.
void World::destroy_pet_entity() {
    if (pet_node_id != 0) script.remove_spawned_unit(pet_node_id);
    destroy_spawned_unit(pet_entity, pet_node_id);
    pet_entity = nullptr;
    pet_node = nullptr;
    pet_mesh.clear();
    pet_node_id = 0;
}

bool World::send_pet_to_town() {
    if (!pet.alive() || pet_node_id == 0) return false;
    if (in_town()) {
        // `CGameUI::onClick` has a "cannot depart" line for exactly this, and
        // `CCharacter::sendToTown` returns without doing anything on the same
        // condition. There is nowhere for it to go.
        TL_LOG_INFO("pet: it is already in town");
        return false;
    }
    // `sendToTown`: the `PET_TOWN_RETURN`
    // (0x60) percent of the pet and of its master.
    const float own = pet_total().effect_value(tl::core::item_effect::kPetTownReturn);
    const float master = player_total().effect_value(tl::core::item_effect::kPetTownReturn);
    std::vector<tl::core::ScriptEvent> events;
    if (!script.send_unit_to_town(pet_node_id, own, master, in_town(), quest_log.state(), events)) {
        return false;
    }
    if (!events.empty()) apply_script_events(events, scripted_nodes);
    sync_pet_mirror();
    // `CGameUI::onClick` says this the moment the button is pressed, not when
    // the animal actually disappears - so it is here and not on arrival.
    play_voice(tl::core::SoundSlot::VoicePetDeparted);
    TL_LOG_INFO("pet: off to town with %zu item(s); back in %s", pet.bag().size(),
                pet.town_clock().c_str());
    return true;
}

namespace {

// `CLayoutDescriptor::descriptorSceneActivated`: what the activation has fired by
// the link's turn is done (the console acts at once), then the machines reached through it are
// activated.
void activate_linked_machines(World& world, LevelScript& parent, int64_t link,
                              std::vector<ScriptEvent>& out) {
    World::NodeMap* nodes = world.nodes_of(&parent);
    if (nodes == nullptr) return;
    if (!out.empty()) {
        std::vector<ScriptEvent> fired;
        fired.swap(out);
        world.apply_script_events(fired, *nodes);
    }
    for (size_t i = 0; i < world.nested_scripts.size(); ++i) {
        const World::NestedScript& linked = world.nested_scripts[i];
        if (linked.retired || linked.turn_link != link || linked.owner_nodes != nodes) continue;
        world.activate_room_machine(i, linked.reference);
    }
}

}  // namespace

void World::wire_script(tl::core::LevelScript& machine) {
    machine.set_animation_length([this](const std::string& unit, const std::string& animation) {
        return scene_animation_length(unit, animation);
    });
    load_stat_graphs();
    machine.set_stat_graphs(&stat_graphs);
    load_unit_types();
    machine.set_unit_types(&unit_types);
    if (!level_items.list) {
        level_items.list = [this]() {
            std::vector<tl::core::DamageShapeItem> out;
            for (size_t i = breakables.size(); i-- > 0;) {
                const BreakableWorld& b = breakables[i];
                // On the near list and active (`CLevel::updateItems`).
                //
                if (b.node == nullptr || !b.state.usable || !b.listed || !b.presence.active) {
                    continue;
                }
                const auto at = view_position(b.node);
                tl::core::DamageShapeItem item;
                item.id = b.id;
                item.at = {at.x, at.y, at.z};
                item.radius = tl::core::kBreakableRadius;
                // Its own `UNITTYPE` where the unit's data is at hand; every
                // one of them is a `BREAKABLE` at the least.
                item.unit_type = unit_types.id_of(stats_for_unit(b.unit).unittype);
                if (item.unit_type == tl::core::UnitTypeHierarchy::kUnknown) {
                    item.unit_type = tl::core::unit_type::kBreakable;
                }
                out.push_back(item);
            }
            return out;
        };
        level_items.interact = [this](int64_t item) { queue_item_interact(item); };
    }
    machine.set_level_items(&level_items);
    machine.set_level_monster_level(level_monster_level);
    machine.set_random_names(&random_names);
    machine.set_level_is_town(in_town());
    machine.set_difficulty(config.difficulty);
    machine.set_trace_player_name(player_class.empty() ? std::string("PLAYER") : player_class);
    machine.set_animation_catalog(
        [this](const std::string& unit) { return animation_catalog(unit); });
    machine.set_link_activation(
        [this, &machine](int64_t link, std::vector<tl::core::ScriptEvent>& out) {
            activate_linked_machines(*this, machine, link, out);
        });
    // The player's skill manager, for a `Skill Controller` on `PLAYERS`
    // (`CSkillController::update`).
    tl::core::LevelScript::PlayerSkills skills;
    skills.knows = [this](const std::string& name) {
        return player_skills.find_by_name(name) != nullptr;
    };
    skills.updating = [this](const std::string& name) {
        const tl::core::KnownSkill* skill = player_skills.find_by_name(name);
        return skill != nullptr && player_skill_runner.is_running(skill->entry.name);
    };
    skills.cast = [this](const std::string& name, int32_t level) {
        tl::core::KnownSkill* skill = player_skills.find_by_name(name);
        if (skill == nullptr) return;
        // `setSkillLevel` when the controller names a level.
        if (level > 0) {
            skill->invested = static_cast<uint32_t>(level);
            tl::core::SkillBook::calculate_effective_level(*skill, player_total());
        }
        TL_LOG_INFO("script: a skill controller casts '%s' on the player", name.c_str());
        cast_player_skill(*skill, true);
    };
    skills.stop = [this](const std::string& name, bool) {
        const tl::core::KnownSkill* skill = player_skills.find_by_name(name);
        if (skill != nullptr) player_skill_runner.stop(*skill, player_cast_context());
    };
    // INVENTED: addSkillByName / deleteSkill on the player (no shipped Skill Controller
    // learns or unlearns).
    skills.learn = [](const std::string& name) {
        TL_LOG_WARN("script: a skill controller would teach the player '%s' - not modelled",
                    name.c_str());
        return false;
    };
    skills.unlearn = [](const std::string& name) {
        TL_LOG_WARN("script: a skill controller would take '%s' from the player - not modelled",
                    name.c_str());
    };
    machine.set_player_skills(std::move(skills));
    // Every machine reads the one book, a room's as much as the level's.
    load_quest_book();
    machine.set_dialog(&dialog_book);
    // `CQuestManager::getDialogsForNPC`, for
    // a `Quest Controller` whose own quest has no line for its unit.
    machine.set_npc_dialog([this](const std::string& unit, std::string* quest_name,
                                  tl::core::QuestSection* section) -> const tl::core::DialogLine* {
        load_quest_book();
        const tl::core::QuestDefinition* quest = nullptr;
        const tl::core::DialogLine* line = quest_log.dialog_for_npc(
            unit, stats_for_unit(unit).quests, tl::core::volatile_random(), &quest, section);
        if (quest_name != nullptr) *quest_name = quest != nullptr ? quest->name : std::string();
        return line;
    });
    // `CQuest::getQuestDialog(CBaseUnit*)` on
    // a controller's quest.
    machine.set_quest_dialog(
        [this](const std::string& quest_name, const std::string& unit,
               tl::core::QuestSection* section) -> const tl::core::DialogLine* {
            load_quest_book();
            const tl::core::QuestDefinition* quest = quest_book.find(quest_name);
            return quest != nullptr ? quest_log.dialog_for(*quest, unit, section) : nullptr;
        });
    // `CQuestManager::getQuestForNPC`, asked
    // by `forceInteractiveDialog` for a line with no quest.
    machine.set_quest_for_npc([this](const std::string& unit) {
        load_quest_book();
        quest_log.quest_for_npc(unit, stats_for_unit(unit).quests, tl::core::volatile_random());
    });
    // A `Sound` node's bank: the mixer, and `CSoundObject::setSoundBankGuid`'s
    // definition from the sound tables.
    machine.set_sound_host(node_sound_host());
    machine.set_sound_slot(
        [this](const std::string& guid, tl::core::BankSample& slot, bool& loops) {
            ensure_sound_index();
            const SoundDefinition* definition = sound_by_guid(guid);
            if (definition == nullptr) return false;
            // `addSample` keeps some of the recordings (`bank_recordings`).
            for (const size_t file : tl::core::bank_recordings(*definition))
                slot.files.push_back(definition->files[file]);
            slot.radius = definition->radius;
            slot.volume = definition->volume;
            slot.chance = definition->play_chance;
            slot.variation = definition->volume_variation;
            loops = definition->loops;
            return true;
        });
    // `CQuestManager::getQuestByName`: a controller's quest.
    machine.set_quest_known([this](const std::string& quest_name) {
        load_quest_book();
        return quest_book.find(quest_name) != nullptr;
    });
    // `CGameGlobals::reload`:
    // `*_MONSTER_ATTENTION_BONUS` for the difficulty, added to a monster's
    // sight and motion radii by `unitInit`.
    {
        static const char* const kBonus[] = {
            "EASY_MONSTER_ATTENTION_BONUS", "NORMAL_MONSTER_ATTENTION_BONUS",
            "HARD_MONSTER_ATTENTION_BONUS", "VERYHARD_MONSTER_ATTENTION_BONUS"};
        float bonus = 0.0f;
        const std::vector<uint8_t> bytes = read_resource("media/GLOBALS.DAT.adm");
        if (!bytes.empty() && config.difficulty >= 0 && config.difficulty <= 3) {
            const tl::formats::AdmFile globals = tl::formats::parse_adm(bytes.data(), bytes.size());
            if (globals.ok) bonus = globals.float_or(kBonus[config.difficulty], 0.0f);
        }
        machine.set_attention_bonus(bonus);
    }
    // Every machine runs in the level's coordinates, so every one walks the
    // same grids and floor; see join_level_machines().
    machine.set_unit_skills(unit_skill_hooks());
    {
        // A unit's own effect manager's tables (`load_own_effects`).
        ensure_effect_data();
        load_stat_graphs();
        load_unit_themes();
        tl::core::EffectManager::Context tables;
        tables.types = &effect_types;
        tables.graphs = &stat_graphs;
        tables.theme_book = &unit_themes;
        machine.set_unit_effect_tables(tables);
        machine.set_affix_book(&affixes);
        ensure_sound_index();
        machine.set_sound_data(&sound_info);
        machine.set_audio_plays([this]() { return audio_enabled && tl::core::audio_available(); });
        machine.set_model_sounds([this](const std::string& unit) {
            const tl::core::UnitStats* stats = unit_book.find(unit);
            if (stats == nullptr || stats->mesh.empty()) return false;
            return tl::core::manifest_adds_sound_sample(manifest_for(stats->mesh), sound_info);
        });
    }
    machine.set_passability(&passability);
    machine.set_navigation(&nav);
    machine.set_collision(&collision);
    machine.set_level_machines(&level_machines);
    machine.set_active_range_mod(level_active_range_mod);
    configure_unit_ranges(machine);
    machine.set_trace(logic_trace_on());
    // Somebody is there to press OK only when the game is being played.
    machine.set_dialogs_wait_for_player(player_control && !config.headless);
    machine.set_equipment_roller(
        [this](const std::string& unit, tl::core::LevelScript::UnitCreation& made) {
            roll_unit_equipment(unit, made);
        });
    machine.set_unit_equipment(unit_equipment_hooks());
}

}  // namespace tl::core
