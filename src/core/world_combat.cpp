// The gameplay half of render/renderer_combat.cpp (core/world.h).
#include "core/item_data.h"
#include "core/unit_resource.h"
#include "core/world.h"
#include "formats/unit_data.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace tl::core {

namespace {

/// The list's four resource groups (`ERESOURCE_GROUPS`), each a section of the PC table.
constexpr const char* kUnitSections[] = {"ITEMS", "MONSTERS", "PLAYERS", "PROPS"};

/// The PC releases' flattened table, `media/masterresourceunits.dat`. False when
/// the pak has none (the Xbox 360's) or it does not parse.
bool load_unit_table(const World& m, tl::formats::AdmFile& file) {
    const std::string path = m.resolve_archive_path("media/masterresourceunits.dat.adm");
    if (path.empty()) return false;
    const std::vector<uint8_t> bytes = m.read_resource(path);
    file = World::parse_data_group(bytes.data(), bytes.size());
    if (!file.ok) TL_LOG_WARN("could not parse '%s': %s", path.c_str(), file.error.c_str());
    return file.ok;
}

/// `UNITDATA.RAW`, as `CUnitResourceList`'s loader
/// files it: a nameless entry is not filed; a later entry replaces an earlier one of the
/// same GUID and, in its group, name. False when the pak has no list; `differing` counts
/// entries whose file names or numbers the unit otherwise.
bool file_unit_list(World& m, tl::formats::AdmFile& file, size_t& differing) {
    const std::string path = m.resolve_archive_path("media/unitdata.raw");
    if (path.empty()) return false;
    const std::vector<uint8_t> bytes = m.read_resource(path);
    const tl::formats::UnitData list = tl::formats::parse_unit_data(bytes.data(), bytes.size());
    if (!list.complete)
        TL_LOG_WARN("unitdata.raw ends early; %zu entries read", list.entries.size());

    std::map<int64_t, size_t> by_guid;
    std::map<std::pair<uint32_t, std::string>, size_t> by_name;
    std::vector<const tl::formats::UnitDataEntry*> filed;
    for (const tl::formats::UnitDataEntry& entry : list.entries) {
        if (entry.name.empty() || entry.group >= std::size(kUnitSections)) continue;
        const auto key = std::make_pair(entry.group, entry.name);
        if (const auto same = by_guid.find(entry.guid); same != by_guid.end())
            filed[same->second] = nullptr;
        if (const auto same = by_name.find(key); same != by_name.end())
            filed[same->second] = nullptr;
        by_guid[entry.guid] = filed.size();
        by_name[key] = filed.size();
        filed.push_back(&entry);
    }
    for (const tl::formats::UnitDataEntry* entry : filed) {
        if (entry == nullptr) continue;
        m.master_unit_by_guid[entry->guid] = entry->name;
        m.master_guid_by_unit[tl::core::upper_ascii(entry->name)] = entry->guid;
        const auto group = static_cast<tl::core::ResourceGroup>(entry->group);
        tl::formats::AdmFile unit_file = m.read_unit_file(entry->file, std::string(), &group);
        if (!unit_file.ok) continue;
        tl::formats::AdmSection section = std::move(unit_file.root);
        section.name = kUnitSections[entry->group];
        // The list keeps names in upper case; the files, as authored.
        const std::string file_guid = section.string_or("UNIT_GUID", "");
        if (tl::core::upper_ascii(section.string_or("NAME", "")) !=
                tl::core::upper_ascii(entry->name) ||
            std::strtoll(file_guid.c_str(), nullptr, 10) != entry->guid) {
            ++differing;
        }
        file.root.children.push_back(std::move(section));
    }
    return !filed.empty();
}

/// Units the master table has not got, from their own files (`CUnitResourceList::loadFile`):
/// beside a table only the items it lacks (`all_groups` false); with neither table nor
/// list, every kind. A `DONTCREATE` file is a base only. Returns the units added; `items`
/// counts the items among them.
std::string lower_ascii(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

/// The unit files a table already lists, by its `DATFILE`s, lower case and with
/// forward slashes.
std::set<std::string> tabled_unit_files(const tl::formats::AdmFile& file) {
    std::set<std::string> tabled;
    for (const tl::formats::AdmSection& section : file.root.children) {
        std::string dat = lower_ascii(section.string_or("DATFILE", ""));
        for (char& c : dat) c = c == '\\' ? '/' : c;
        if (!dat.empty()) tabled.insert(dat);
    }
    return tabled;
}

size_t add_unit_files(World& m, tl::formats::AdmFile& file, bool all_groups, size_t& items) {
    static const char* const kDirs[] = {"media/units/items/", "media/units/monsters/",
                                        "media/units/players/", "media/units/props/"};
    const std::set<std::string> tabled = tabled_unit_files(file);
    size_t added = 0;
    for (uint32_t group = 0; group < std::size(kDirs); ++group) {
        if (!all_groups && group != 0) break;
        const auto resource_group = static_cast<tl::core::ResourceGroup>(group);
        for (const std::string& unit_path : m.data_paths_under(kDirs[group])) {
            // `DATFILE` as the table spells it: upper case, without `.adm`.
            std::string dat = unit_path.substr(
                0, unit_path.size() - (ends_with_lower(unit_path, ".adm") ? 4 : 0));
            if (tabled.count(lower_ascii(dat)) != 0) continue;
            dat = tl::core::upper_ascii(dat);
            tl::formats::AdmFile unit_file = m.read_unit_file(dat, std::string(), &resource_group);
            if (!unit_file.ok || unit_file.root.name != "UNIT" ||
                unit_file.root.bool_or("DONTCREATE", false) ||
                unit_file.root.string_or("NAME", "").empty()) {
                continue;
            }
            tl::formats::AdmSection merged = std::move(unit_file.root);
            merged.name = kUnitSections[group];
            file.root.children.push_back(std::move(merged));
            ++added;
            if (group == 0) ++items;
        }
    }
    return added;
}

/// The mods' own unit files in the unit list: the PC rebuilds its table with them
/// (`masterresourceunits.dat`, which a mod's install notes say to delete). A unit the list
/// already has, by GUID, is the mod's file in its place; one it lacks is added. Returns the
/// units added; `items` counts the items among them.
size_t add_mod_unit_files(World& m, tl::formats::AdmFile& file, size_t& items) {
    static const char* const kDirs[] = {"media/units/items/", "media/units/monsters/",
                                        "media/units/players/", "media/units/props/"};
    size_t added = 0;
    for (uint32_t group = 0; group < std::size(kDirs); ++group) {
        const auto resource_group = static_cast<tl::core::ResourceGroup>(group);
        for (const std::string& unit_path : m.data_paths_under(kDirs[group])) {
            if (!m.mod_has_file(unit_path)) continue;
            std::string dat = unit_path.substr(
                0, unit_path.size() - (ends_with_lower(unit_path, ".adm") ? 4 : 0));
            tl::formats::AdmFile unit_file =
                m.read_unit_file(tl::core::upper_ascii(dat), std::string(), &resource_group);
            if (!unit_file.ok || unit_file.root.name != "UNIT" ||
                unit_file.root.bool_or("DONTCREATE", false)) {
                continue;
            }
            const std::string name = unit_file.root.string_or("NAME", "");
            const std::string guid = unit_file.root.string_or("UNIT_GUID", "");
            if (name.empty() || guid.empty()) continue;
            const int64_t value = std::strtoll(guid.c_str(), nullptr, 10);
            tl::formats::AdmSection merged = std::move(unit_file.root);
            merged.name = kUnitSections[group];
            if (m.master_unit_by_guid.count(value) != 0) {
                for (tl::formats::AdmSection& listed : file.root.children) {
                    if (listed.name == merged.name && listed.string_or("UNIT_GUID", "") == guid)
                        listed = merged;
                }
                continue;
            }
            m.master_unit_by_guid[value] = name;
            m.master_guid_by_unit[tl::core::upper_ascii(name)] = value;
            file.root.children.push_back(std::move(merged));
            ++added;
            if (group == 0) ++items;
        }
    }
    return added;
}

/// The table's and the scanned files' names and GUIDs, filed as the console's loader does:
/// a later record replaces an earlier one of the same GUID or name
/// (`CUnitResourceList::addNewItem`, only logs one).
void file_unit_names(World& m, const tl::formats::AdmFile& file) {
    for (const tl::formats::AdmSection& section : file.root.children) {
        const std::string name = section.string_or("NAME", "");
        const std::string guid = section.string_or("UNIT_GUID", "");
        if (name.empty() || guid.empty()) continue;
        const int64_t value = std::strtoll(guid.c_str(), nullptr, 10);
        m.master_unit_by_guid[value] = name;
        m.master_guid_by_unit[tl::core::upper_ascii(name)] = value;
    }
}

}  // namespace

/// Load the authored stat curves (`media/graphs/stats/`, `[POINT] X, Y` lists) that turn
/// a unit's percentages into numbers.
// PORT: `media/graphs/stats/` read from the archive into the port's curve table.
void World::load_stat_graphs() {
    if (stat_graphs_loaded) return;
    stat_graphs_loaded = true;
    const size_t curves = load_graph_directory("media/graphs/stats/", stat_graphs);
    TL_LOG_INFO("stat curves: %zu from media/graphs/stats/", curves);
}

void World::load_camera_shake_graphs() {
    if (camera_shake_graphs_loaded) return;
    camera_shake_graphs_loaded = true;
    const size_t curves = load_graph_directory("media/graphs/camerashakes/", camera_shake_graphs);
    TL_LOG_INFO("camera shake curves: %zu from media/graphs/camerashakes/", curves);
}

void World::load_game_speed() {
    if (game_speed_loaded) return;
    game_speed_loaded = true;
    for (const std::string& path : data_paths_under("media/graphs/camerashakes/")) {
        if (lowered(path).find("/decelerategamespeed.") == std::string::npos) continue;
        const std::vector<uint8_t> bytes = read_resource(path);
        const tl::formats::AdmFile file = parse_data_group(bytes.data(), bytes.size());
        if (!file.ok || file.section() != "LINE") break;
        game_speed_curve = tl::core::StatGraph::from(file.root);
        TL_LOG_INFO("game speed: curve '%s' from %s", game_speed_curve.name().c_str(),
                    path.c_str());
        return;
    }
    TL_LOG_WARN("game speed: no DecelerateGameSpeed curve under media/graphs/camerashakes/");
}

void World::slow_on_player_kill(bool target_dead) {
    // `CGameSpeed::getSingleton()->addSpeedModifier(0, 0.5, 1.0)`: type 0 slows by
    // the curve, over 0.5 s, at strength 1.
    if (!target_dead) return;
    load_game_speed();
    game_speed.add_modifier(static_cast<int32_t>(tl::core::GameSpeedType::Decelerate), 0.5f, 1.0f);
    TL_LOG_INFO("game speed: a killing blow slows the game (%zu modifier(s))", game_speed.size());
}

size_t World::load_graph_directory(const std::string& directory, tl::core::StatBook& book) const {
    size_t curves = 0;
    for (const std::string& path : data_paths_under(directory)) {
        const std::vector<uint8_t> bytes = read_resource(path);
        if (bytes.empty()) continue;
        const tl::formats::AdmFile file = parse_data_group(bytes.data(), bytes.size());
        if (!file.ok || file.section() != "LINE") continue;
        tl::core::StatGraph graph = tl::core::StatGraph::from(file.root);
        if (graph.empty()) continue;
        // Keyed by the curve's `NAME` (what `HEALTH_GRAPH` refers to) and by the file's stem.
        book.add(graph.name(), graph);
        std::string stem = path;
        const size_t slash = stem.find_last_of('/');
        if (slash != std::string::npos) stem = stem.substr(slash + 1);
        const size_t dot = stem.find('.');
        if (dot != std::string::npos) stem.resize(dot);
        book.add(stem, graph);
        ++curves;
    }
    return curves;
}

/// Read every quest in the game, once: `CQuestManager::loadQuests`
/// over `media/quests/`, each by `CQuest::loadQuestData`. All of them,
/// since an NPC offers whatever `getQuestForNPC` finds among all.
void World::load_quest_book() {
    if (quest_book_loaded) return;
    quest_book_loaded = true;

    size_t files = 0, lines = 0;
    for (const std::string& path : data_paths_under("media/quests/")) {
        const std::vector<uint8_t> bytes = read_resource(path);
        if (bytes.empty()) continue;
        const tl::formats::AdmFile file = parse_data_group(bytes.data(), bytes.size());
        // `CQuest::loadQuestData` refuses only a file that did not
        // load, not one whose top section is named otherwise.
        if (!file.ok) continue;

        tl::core::QuestDefinition quest;
        if (!tl::core::parse_quest_definition(file.root, quest)) continue;

        const auto time_lines = [&](std::vector<tl::core::DialogLine>& all) {
            for (tl::core::DialogLine& line : all) {
                line.seconds = sound_length(line.sound);
                ++lines;
            }
        };
        time_lines(quest.intro);
        time_lines(quest.ret);
        time_lines(quest.complete);
        time_lines(quest.passive);

        // The lines a `Quest Controller` opens, keyed by the quest's `NAME`
        // (`CQuestManager::getQuestByName`), not the file's stem; every quest,
        // in file order.
        {
            std::string key = quest.name;
            if (key.empty()) {
                key = path.substr(path.find_last_of('/') + 1);
                key.resize(key.size() - (sizeof(".dat.adm") - 1));
            }
            file.visit([&](const tl::formats::AdmSection& section) {
                if (section.name != "DIALOG") return;
                // `getQuestStrings`: a line naming nobody is
                // the `[DIALOG]` group's `UNITNAME`'s.
                const std::string speaker = section.string_or("UNITNAME", "");
                for (const tl::formats::AdmSection& entry : section.children) {
                    // `DETAILS` is the journal entry; nobody says it out loud.
                    if (entry.name == "DETAILS") continue;
                    // `CQuestDialog::parseDialogTag`.
                    tl::core::DialogLine line = tl::core::parse_dialog_line(entry);
                    if (line.unit.empty()) line.unit = speaker;
                    if (line.texts.empty()) continue;
                    line.seconds = sound_length(line.sound);
                    dialog_book.add(key, line);
                }
            });
        }

        quest_book.add(std::move(quest));
        ++files;
    }

    quest_log.set_book(&quest_book);
    quest_log.set_unit_namer(quest_unit_namer());
    quest_log.set_unit_world(quest_unit_world());
    quest_log.set_dungeon_source(quest_dungeon_source());
    quest_log.set_player_source(quest_player_source());
    quest_log.set_reward_world(quest_reward_world());
    // `CRandomNames::CRandomNames`, which `CMasterResourceManager`
    // makes from `media/randomnames.dat`.
    const std::vector<uint8_t> names = read_resource("media/RANDOMNAMES.DAT.adm");
    if (!names.empty()) {
        const tl::formats::AdmFile file = tl::formats::parse_adm(names.data(), names.size());
        if (file.ok) random_names.load(file.root);
    }
    if (random_names.empty()) TL_LOG_WARN("quests: no media/RANDOMNAMES.DAT; champions go unnamed");
    quest_log.set_random_names(&random_names);
    TL_LOG_INFO("quests: %zu from media/Quests/, %zu spoken line(s), %zu for quest controllers",
                files, lines, dialog_book.line_count());
}

// `CQuestUnitData::getBaseUnitDataGroup`, the half
// `getUnitName` prints (`CHierarchy::getTypeName`).
// INVENTED: a `SPAWNCLASS` or `SPAWNFROMUNITTYPE` objective
// (`getSpawnClassUnitDataLookingFor`) names nothing.
tl::core::QuestUnitNamer World::quest_unit_namer() {
    // `CQuestUnitData::getUnitName` past the champion
    // name: with `+0x44` set, the type's name; else `DISPLAYNAME`, or `NAME`.
    return [this](const tl::core::QuestObjective& objective) -> std::string {
        if (objective.unit_type_id != tl::core::unit_type::kNone) {
            load_unit_types();
            const std::string name = unit_types.name_of(objective.unit_type_id);
            return name.empty() ? objective.unit_type : name;
        }
        if (objective.resolved.empty()) return std::string();
        ensure_item_index();
        if (const tl::core::ItemStats* item = items.find(objective.resolved)) {
            return item->display_name.empty() ? item->name : item->display_name;
        }
        index_unit(objective.resolved);
        const tl::core::UnitStats* unit = unit_book.find(objective.resolved);
        if (unit == nullptr) return std::string();
        return unit->display_name.empty() ? objective.resolved : unit->display_name;
    };
}

/// Index every item in the game, once, from `media/masterresourceunits.dat.adm` (the
/// master unit table, `BASEFILE` chains merged): the way from an item's name to its mesh.
// PORT: a unit's GUID by name, from the port's item index.
int64_t World::unit_guid_of(const std::string& name) {
    ensure_item_index();
    const auto found = master_guid_by_unit.find(tl::core::upper_ascii(name));
    return found != master_guid_by_unit.end() ? found->second : -1;
}

// `CUnitResourceList::getDataGroupByGuid`: the GUID map.
// The console's refuses 0x00000000ffffffff and looks a 64-bit -1 up; neither finds a unit.
std::string World::unit_of_guid(int64_t guid) {
    ensure_item_index();
    const auto found = master_unit_by_guid.find(guid);
    return found != master_unit_by_guid.end() ? found->second : std::string();
}

tl::formats::AdmFile World::read_unit_file(const std::string& path, const std::string& first_root,
                                           const tl::core::ResourceGroup* group) const {
    tl::formats::AdmFile file;
    file.ok = false;
    bool first = true;
    bool first_read = false;
    tl::core::UnitFileHost host;
    // `CDataGroup::LoadFile`: the file's root group, or nothing.
    host.load = [&](const std::string& unit_path, tl::formats::AdmSection& out) {
        const bool is_first = first;
        first = false;
        std::string wanted = unit_path;
        for (char& c : wanted) {
            if (c == '\\') c = '/';
        }
        const bool compiled =
            wanted.size() > 4 && wanted.compare(wanted.size() - 4, 4, ".adm") == 0;
        const std::string resolved =
            compiled ? wanted : resolve_unit_link(wanted, first_root, is_first);
        if (resolved.empty()) return;
        const std::vector<uint8_t> bytes = read_resource(resolved);
        if (bytes.empty()) return;
        tl::formats::AdmFile link = parse_data_group(bytes.data(), bytes.size());
        if (!link.ok) return;
        if (is_first) first_read = true;
        out = std::move(link.root);
    };
    if (group != nullptr) {
        tl::core::load_file(path, *group, host, file.root);
    } else {
        tl::core::parse_child_file(path, true, host, file.root);
    }
    file.ok = first_read;
    return file;
}

/// `CCharacter::performAttack` ->
/// `rollAttack(level, target, weapon, 0, 1, 1, 7)` for
/// the player's blow, with the held weapon or bare-handed (`+0x390`).
float World::player_blow(int64_t target, float damage_scale, float soak_scale) {
    load_unit_types();
    if (!(has_weapon && weapon.combat_rolled)) {
        return player_unarmed_blow(target, damage_scale, soak_scale);
    }
    // `strength()`, `dexterity()` and
    // `magic()`, each with the character's effects and its
    // inventory's; the hands as `sync_equipped_weapon` leaves them.
    const tl::core::CharacterEffects total = player_total();
    const auto& hands = player_inventory.slots();
    const bool right_held = hands[tl::core::equip_location::kRightHand].count > 0;
    const bool left_held = hands[tl::core::equip_location::kLeftHand].count > 0;
    const tl::core::WeaponTraits traits = tl::core::held_weapon_traits(
        weapon, unit_types, right_held, left_held,
        left_held && unit_types.isa(hands[tl::core::equip_location::kLeftHand].item.unittype,
                                    tl::core::unit_type::kShield));
    const tl::core::Strike strike = tl::core::armed_strike(
        weapon, traits, player_character.strength(total), player_character.dexterity(total),
        player_character.magic(total), total, item_effect_context(), damage_scale, soak_scale);
    return player_strike(target, strike);
}

void World::roll_attack_effects(const tl::core::AttackEffectSources& sources, float knock_back,
                                float degrade, const tl::core::Vec3& from, const tl::core::Vec3& to,
                                tl::core::LevelScript* target_machine, int64_t target_node,
                                const tl::core::EffectSource& attacker) {
    // `rollAttack`.
    const bool player_target = target_machine == nullptr;
    if (player_target && target_node != 0) return;
    tl::core::EffectManager* manager =
        player_target ? &player_effects : unit_effect_manager(target_machine, target_node);
    if (manager == nullptr) return;
    const std::string who =
        player_target ? std::string("the player") : target_machine->unit_of(target_node);
    // The effects belong to the attacker (`+0x48`, `transferEffects`' owner).
    const tl::core::EffectSourceScope owned(manager, attacker);
    // `KNOCK BACK` above 0, `addNewEffect` with no gate.
    if (0.0f < knock_back) {
        const tl::core::Effect effect = tl::core::knock_back_effect(knock_back, from, to);
        manager->add_effect(effect);
        TL_LOG_INFO("effects: %s takes KNOCKBACK %.2f for %.0fs", who.c_str(),
                    static_cast<double>(effect.values[0]), static_cast<double>(effect.duration));
    }
    // `DEGRADE ARMOR` above 0.
    if (0.0f < degrade) {
        const tl::core::Effect effect = tl::core::degrade_armor_effect(degrade);
        manager->add_effect(effect);
        TL_LOG_INFO("effects: %s takes DEGRADEARMOR for %.0fs", who.c_str(),
                    static_cast<double>(effect.duration));
    }
    // the `TRANSFER` effects, `isEffectValidForUnit` (vtable +0x110), then
    // `applyEffectOnUnit` (+0x114) on the copy.
    load_unit_types();
    load_stat_graphs();
    const tl::core::ItemUseTarget target =
        player_target ? player_use_target() : unit_use_target(target_machine, target_node);
    const auto valid = [&](const tl::core::Effect& effect) {
        // DIVERGENCE: `transferEffects` leaves `isEffectValidForUnit`'s float
        // argument unset; only its sign is read, so the effect's base value stands in.
        const float value = tl::core::effect_base_value(effect, tl::core::EffectCalculate::Roll,
                                                        stat_graphs, tl::core::EffectOwner{});
        return tl::core::effect_valid_for_unit(target, nullptr, effect, value, unit_types);
    };
    const auto apply = [&](tl::core::Effect& made) {
        const tl::core::EffectTypeInfo* info = effect_types.at(made.type);
        const bool held = manager->offer_effect(made);
        TL_LOG_INFO("effects: %s %s the transferred %s", who.c_str(), held ? "holds" : "refuses",
                    info != nullptr ? info->name.c_str() : "?");
    };
    tl::core::transfer_attack_effects(sources, valid, apply);
}

void World::unit_attack_effects(int64_t attacker_node, tl::core::LevelScript* target_machine,
                                int64_t target_node) {
    // `rollAttack`'s tail for a placed character: its own manager (X360 `+0x17c`); the
    // pet carries its bag (`+0x3e8`). INVENTED: a monster's weapons are names only, so no
    // inventory or weapon of its own adds to `KNOCK BACK` or the transfers.
    tl::core::LevelScript* by = script_owning(attacker_node);
    if (by == nullptr) return;
    tl::core::Vec3 from;
    tl::core::Vec3 to;
    if (!by->unit_position(attacker_node, from)) return;
    if (target_machine == nullptr) {
        if (target_node != 0) return;
        to = player.position();
    } else if (!target_machine->unit_position(target_node, to)) {
        return;
    }
    tl::core::AttackEffectSources sources;
    tl::core::CharacterEffects total;
    if (pet_node_id != 0 && attacker_node == pet_node_id) {
        sources.own = &pet.effects();
        sources.has_inventory = true;
        sources.inventory = &pet.bag().effects();
        for (size_t location = 0; location < sources.worn.size(); ++location) {
            sources.worn[location] = pet.bag().equipped_item_effects(location);
        }
        total = pet_total();
    } else {
        sources.own = by->unit_effects(attacker_node);
        total = tl::core::CharacterEffects(sources.own);
    }
    if (sources.own == nullptr) return;
    roll_attack_effects(sources, total.effect_value(tl::core::kEffectTypeKnockBack, 7),
                        total.effect_value(tl::core::kEffectTypeDegradeArmor, 7), from, to,
                        target_machine, target_node,
                        tl::core::EffectSource{true, by, attacker_node});
}

float World::missile_reflect_share(int64_t unit) {
    // `CCharacter::getDmgToReflectFromMissile`: `MISSILE REFLECT` over
    // its own manager and, with an inventory, the 12 worn items'.
    std::array<const tl::core::EffectManager*, 12> worn{};
    if (unit == 0) {
        for (size_t location = 0; location < worn.size(); ++location) {
            worn[location] = player_inventory.equipped_item_effects(location);
        }
        return tl::core::dmg_to_reflect_from_missile(
            player_total().effect_value(tl::core::kEffectTypeMissileReflect, 7), &player_effects,
            true, worn, tl::core::volatile_random());
    }
    if (pet_node_id != 0 && unit == pet_node_id) {
        for (size_t location = 0; location < worn.size(); ++location) {
            worn[location] = pet.bag().equipped_item_effects(location);
        }
        return tl::core::dmg_to_reflect_from_missile(
            pet_total().effect_value(tl::core::kEffectTypeMissileReflect, 7), &pet.effects(), true,
            worn, tl::core::volatile_random());
    }
    // INVENTED: a placed monster's worn items are kept by name only, so its
    // own manager is all it reflects with.
    tl::core::LevelScript* machine = script_owning(unit);
    if (machine == nullptr) return 0.0f;
    const tl::core::EffectManager* own = machine->unit_effects(unit);
    if (own == nullptr) return 0.0f;
    return tl::core::dmg_to_reflect_from_missile(
        tl::core::CharacterEffects(own).effect_value(tl::core::kEffectTypeMissileReflect, 7), own,
        false, worn, tl::core::volatile_random());
}

void World::apply_damage_status(tl::core::LevelScript* machine, int64_t node, float amount,
                                const std::array<int, tl::core::kDamageTypeCount>& by_type,
                                int parts, int first_type, bool forced, int attacker_level,
                                const tl::core::EffectSource& attacker) {
    std::array<int, tl::core::kDamageTypeCount> own{};  // the player's `+0x620` is all 0
    if (machine != nullptr && !machine->unit_raw_defense(node, own)) return;
    const tl::core::DamageStatus status =
        tl::core::roll_damage_status(amount, by_type, parts, first_type, own, forced,
                                     machine == nullptr, tl::core::volatile_random());
    if (status.type < 0) return;
    tl::core::EffectManager* manager =
        machine != nullptr ? unit_effect_manager(machine, node) : &player_effects;
    if (manager == nullptr) return;
    // A held affix only redraws its duration; else `addAffix` first. DIVERGENCE: the
    // duration is drawn before the add, so a missing affix still costs the draw.
    const float duration =
        tl::core::volatile_random().between(status.duration_min, status.duration_max);
    const std::string who = machine != nullptr ? machine->unit_of(node) : "the character";
    // `addAffix(name, level, attacker)`: the attacker owns the
    // affix's effects.
    const tl::core::EffectSourceScope owned(manager, attacker);
    if (!manager->set_affix_duration(status.affix, duration)) {
        if (!manager->add_affix_by_name(status.affix, static_cast<uint32_t>(attacker_level),
                                        &affixes, duration)) {
            return;
        }
    }
    TL_LOG_INFO("effects: %s takes %s for %.2fs", who.c_str(), status.affix,
                static_cast<double>(duration));
}

// `CCharacter::applyDamage`, the reflection at its end;
// the flags test (`& 0x300`) is skipped, as no caller passes those bits.
void World::reflect_damage(const tl::core::CharacterEffects& victim, int64_t victim_node,
                           const std::string& victim_name, float amount, int64_t attacker) {
    if (!victim || attacker == tl::core::LevelScript::kNoAttacker) return;
    if (attacker == victim_node) return;
    // The attacker dying or dead (`+0x330` 5 or 6) is left alone.
    tl::core::LevelScript* by = nullptr;
    tl::core::Defender defender;
    if (attacker == 0) {
        if (player_health.dead) return;
        defender = player_defender();
    } else {
        by = script_owning(attacker);
        if (by == nullptr || by->unit_is_dead(attacker) || !by->unit_defender(attacker, defender)) {
            return;
        }
        defender.on_absorbed = [this, by, attacker] { show_unit_absorbed(by, attacker); };
    }
    const float flat = victim.effect_value(0x30, 7);
    if (flat == 0.0f && victim.effect_value(0x20, 7) == 0.0f) return;
    // DIVERGENCE: the original recurses while both sides reflect and live; cut at eight
    // so two undying characters return.
    if (8 <= reflect_depth) return;
    const float share = victim.effect_value(0x20, 7) / 100.0f;
    // `modifyDamage((int)(amount * share), 0, (int)amount, share)` on the attacker.
    const int part =
        tl::core::modify_damage(defender, static_cast<int>(amount * share), 0,
                                static_cast<int>(amount), share, tl::core::volatile_random());
    const float total = victim.effect_value(0x30, 7) + static_cast<float>(part);
    TL_LOG_INFO("combat: %s reflects %.1f of %.1f (%.0f%% + %.1f)", victim_name.c_str(),
                static_cast<double>(total), static_cast<double>(amount),
                static_cast<double>(share * 100.0f), static_cast<double>(flat));
    ++reflect_depth;
    if (attacker == 0) {
        hurt_player(total, victim_name, victim_node);
    } else {
        hurt_unit(attacker, by->unit_of(attacker), total, false, victim_node, by);
    }
    --reflect_depth;
}

/// How much life a placed unit has, worked out the first time it is asked for.
// INVENTED: its HEALTH_GRAPH (else the champion or monster curve) at its level, times a
// volatile roll between MINHP and MAXHP over 100.
World::Health& World::health_of(int64_t node, const std::string& unit) {
    const auto found = unit_health.find(node);
    if (found != unit_health.end()) return found->second;

    load_stat_graphs();
    const tl::core::UnitStats stats = stats_for_unit(unit);
    // The curve the unit names, the monster curve only if it names none; a champion
    // (own `.DAT` or promoted at spawn) takes the champion curve over both.
    const bool champion = stats.is_champion || spawn_marked_champion(node);
    const std::string health_curve =
        champion
            ? std::string(tl::core::kChampionMonsterHealthGraph) + tl::core::difficulty_suffix(1)
            : (stats.health_graph.empty() ? tl::core::kMonsterHealthGraph : stats.health_graph);
    const float curve =
        stat_graphs.value(health_curve, static_cast<float>(effective_monster_level(node)), 0.0f);
    // `MINHP` and `MAXHP` (percent) bracket the roll.
    float low = stats.min_health;
    float high = stats.max_health;
    if (high < low) high = low;
    Health health;
    health.max =
        curve > 0.0f ? curve * tl::core::volatile_random().between(low, high) / 100.0f : 0.0f;
    health.current = health.max;
    return unit_health.emplace(node, health).first->second;
}

// PORT: a skill's animation cue (`begin_skill_animation`) played on the player's entities.
void World::play_skill_cue(const tl::core::SkillAnimationCue& cue) {
    // `blendAnimation(animation, 0.2, speed, duration)`; the cross-fade is not drawn.
    if (cue.animation < 0 || static_cast<size_t>(cue.animation) >= player_catalog.size()) {
        skill_playback = SkillPlayback{};
        return;
    }
    const auto& entry = player_catalog[static_cast<size_t>(cue.animation)];
    if (!link_subject_animation(entry.first)) {
        skill_playback = SkillPlayback{};
        return;
    }
    skill_playback = SkillPlayback{};
    skill_playback.active = true;
    skill_playback.animation = entry.first;
    skill_playback.index = cue.animation;
    skill_playback.length = entry.second;
    skill_playback.speed = cue.speed;
    skill_playback.held = cue.duration;
    skill_playback.then_loop = cue.then_loop;
    // From its first frame, even when the same animation was already playing.
    player_animation.clear();
    follow_player_animation();
    TL_LOG_INFO("skills: '%s' plays '%s' (%.3fs at x%.2f, held %.3fs)",
                player_active_skill.skill.c_str(), entry.first.c_str(),
                static_cast<double>(entry.second), static_cast<double>(cue.speed),
                static_cast<double>(cue.duration));
}

tl::core::CreateLootUnit World::loot_unit_maker() {
    // `CResourceManager::createUnit` for an item.
    // INVENTED: a spawn class that rolls a monster or a prop makes nothing yet.
    return [this](const std::string& name, int32_t, bool force_enchant, tl::core::ItemStats& out) {
        ensure_item_index();
        const tl::core::ItemStats* data = items.find(name);
        if (data == nullptr) {
            TL_LOG_WARN("loot: '%s' is not an item; nothing made", name.c_str());
            return false;
        }
        out = create_item(*data, false, true, force_enchant);
        return true;
    };
}

bool World::is_breakable_unit(const std::string& unit) {
    const std::string key = lowered(unit);
    const auto cached = breakable_units.find(key);
    if (cached != breakable_units.end()) return cached->second;

    // `CResourceManager::createNewItem` makes a
    // `CBreakable` for `CREATEAS: BREAKABLE` (props' `base.dat` default).
    bool breakable = false;
    ensure_prop_index();
    const auto found = prop_files.find(key);
    if (found != prop_files.end()) {
        // The unit as the list holds it (`CUnitResourceList::parseChildFile`):
        // the file over its `BASEFILE`.
        const tl::formats::AdmFile file = read_unit_file(found->second);
        if (file.ok) {
            breakable = tl::core::upper_ascii(file.string_or("CREATEAS", "")) == "BREAKABLE";
        }
    }
    breakable_units.emplace(key, breakable);
    return breakable;
}

// `CItem::unitInit`: `NONSELECTABLE` into `+0x1f1`,
// which `findItemsOnscreen` and
// `findItemAtScreenCoordinates` skip: never targeted, never named.
bool World::non_selectable_prop(const std::string& unit) {
    const std::string key = lowered(unit);
    const auto cached = non_selectable_units.find(key);
    if (cached != non_selectable_units.end()) return cached->second;
    bool non_selectable = false;
    ensure_prop_index();
    const auto found = prop_files.find(key);
    if (found != prop_files.end()) {
        const tl::formats::AdmFile file = read_unit_file(found->second);
        if (file.ok) non_selectable = file.bool_or("NONSELECTABLE", false);
    }
    non_selectable_units.emplace(key, non_selectable);
    return non_selectable;
}

float World::player_unarmed_blow(int64_t target, float damage_scale, float soak_scale) {
    // `performAttack`: no weapon, `+0x390`, the attack built at the class's
    // `LEVEL` by `unitInit` - 1 from `CBaseUnit`'s constructor.
    // Later `setLevel` calls skip a player: bare hands keep level 1 damage.
    load_stat_graphs();
    const tl::core::AttackDescription attack = tl::core::own_attack(
        player_stats, player_stats.level, config.difficulty, /*player=*/true, stat_graphs);
    // `rollAttack` with no weapon: `strength()` as the share.
    const tl::core::CharacterEffects total = player_total();
    const tl::core::Strike strike = tl::core::unarmed_strike(
        attack, player_character.strength(total), total, damage_scale, soak_scale);
    return player_strike(target, strike);
}

void World::perform_unknown_skill(const std::string& name, uint32_t level) {
    // `CCharacter::performUnknownSkill(CSkill*)`: the
    // skill by name in the character's manager, else a copy put there
    // (`CBaseUnit::cloneSkill`, inlined).
    // `CSkillManager::getSkill(name)`: by name alone.
    const tl::core::SkillCopy* copy = nullptr;
    for (const tl::core::SkillCopy& held : player_proc_skills.copies()) {
        if (tl::core::upper_ascii(held.name) == tl::core::upper_ascii(name)) {
            copy = &held;
            break;
        }
    }
    if (copy == nullptr) copy = player_proc_skills.add(name, level, skill_file_lookup());
    if (copy == nullptr) {
        TL_LOG_WARN("use: the item's skill '%s' has no file", name.c_str());
        return;
    }
    ensure_player_skill_files();
    const auto file = player_skill_files.find(tl::core::upper_ascii(copy->name));
    if (file == player_skill_files.end() || subject_node == nullptr) return;
    // A skill with no animation is run at once (`executeSkill`).
    // INVENTED: one with an animation is run at once too (no shipped item skill has one).
    if (!file->second->def.base.animation.empty()) {
        TL_LOG_WARN("use: the item's skill '%s' has an animation; run at once", copy->name.c_str());
    }
    const float body =
        player_stats.radius > 0.0f ? player_stats.radius : tl::core::kDefaultCollisionRadius;
    const tl::core::Vec3 feet = player.position();
    const tl::core::Vec3 centre{feet.x, feet.y + body, feet.z};
    const tl::core::SkillCopy held = *copy;
    run_proc_skill(held, centre, centre, ProcTarget{}, "the item used");
}

// An effect's warp: `CGameClient::warpLevels`
// with the effect's dungeon and absolute depth (`warp_levels`, `warp_destination`).
void World::warp_by_effect(const std::string& dungeon, int32_t absolute) {
    const tl::core::TravelRecord none;
    const tl::core::TravelRecord& travel = config.travel != nullptr ? *config.travel : none;
    const tl::core::WarpRequest request = tl::core::warp_levels(dungeon, 0, absolute, travel);
    const tl::core::WarpDestination to = tl::core::warp_destination(request, warp_origin(), travel);
    if (!to.go) {
        TL_LOG_INFO("warp: the effect's '%s' leads nowhere", dungeon.c_str());
        return;
    }
    warp_pending = true;
    warp_dungeon = to.dungeon;
    // DIVERGENCE: a depth below 0 is loaded as depth 0, as for a Warper.
    warp_floor = std::max(0, to.depth) + 1;
    warp_level_delta = request.delta;
    warp_level_absolute = request.absolute;
    warp_waypoint = false;
    dungeon_transition_pending = true;
    TL_LOG_INFO("warp: an effect sends the player to '%s' depth %d", warp_dungeon.c_str(),
                to.depth);
}

tl::core::ItemUseTarget World::unit_use_target(tl::core::LevelScript* machine, int64_t node) {
    // What `CCharacter::isEffectValidForUnit` reads of a
    // placed character.
    tl::core::ItemUseTarget target;
    const std::string unit = machine->unit_of(node);
    const tl::core::UnitStats stats = stats_for_unit(unit);
    machine->unit_health(node, target.hp, target.max_hp);
    target.damageable = stats.damageable;
    target.unittype = stats.unittype;
    machine->unit_level(node, target.level);
    target.effects = machine->unit_effects(node);
    const int32_t ai = machine->unit_ai(node);
    target.dying_or_dead = ai == tl::core::ai_state::kDying || ai == tl::core::ai_state::kDead;
    if (SceneUnit* drawn = scene_unit_of(machine, node)) {
        load_unit_themes();
        target.stunned = drawn->themes.themes.has_unit_theme("STUNNED", &unit_themes);
    }
    // `EALIGNMENT`: `GOOD` 1, `EVIL` 2, `NEUTRAL` 0.
    switch (unit_side(node, stats)) {
        case tl::core::UnitStats::Alignment::Friendly: target.alignment = 1; break;
        case tl::core::UnitStats::Alignment::Hostile: target.alignment = 2; break;
        case tl::core::UnitStats::Alignment::Neutral: target.alignment = 0; break;
    }
    target.has_master = machine->unit_has_master(node);
    target.master_is_player = machine->unit_is_player_pet(node);
    return target;
}

// `CCharacter::awardExperience` for the player: the
// pets' share first (`LevelScript::award_pet_experience`), then
// `CharacterProgression::award_experience`.
bool World::award_player_experience(int amount, bool dying) {
    const tl::core::CharacterEffects total = player_total();
    int share = 0;
    if (player_character.pet_experience_share(amount, total, dying, share)) {
        for (const MachineView& view : machines()) {
            std::vector<int64_t> levelled;
            view.script->award_pet_experience(share, levelled);
            for (const int64_t id : levelled) {
                int level = 0;
                view.script->unit_level(id, level);
                TL_LOG_INFO("experience: %s rises to level %d", view.script->unit_of(id).c_str(),
                            level);
            }
        }
        // The animal's level and experience outlive the level it stands on.
        int level = 0;
        int32_t experience = 0;
        if (pet.alive() && pet_node_id != 0 && script.unit_level(pet_node_id, level) &&
            script.unit_experience(pet_node_id, experience)) {
            pet.set_progress(level, experience);
            sync_pet_mirror();
        }
    }
    return player_character.award_experience(amount, stat_graphs, total, false, dying);
}

const tl::core::LootCharacter* World::loot_character(int64_t attacker,
                                                     tl::core::LootCharacter& out) {
    // The killer: the player (attacker 0), the pet, or another character.
    // `CLevel::rollMoney` asks the killer alone for
    // `GOLD FIND` (0x33); `CSpawnClassData::rollSpawnClassData`
    // takes the largest `MAGIC FIND` (0x1b) of the finder, its master
    // and that master's master, each cut to an unsigned integer.
    out = tl::core::LootCharacter{};
    out.level_number = level_monster_level;
    if (attacker == tl::core::LevelScript::kNoAttacker) return nullptr;
    const auto whole = [](float value) {
        return static_cast<float>(static_cast<uint32_t>(static_cast<int64_t>(value)));
    };
    const tl::core::CharacterEffects players = player_total();
    if (attacker == 0) {
        // The player has no master.
        out.gold_find = players.effect_value(tl::core::item_effect::kGoldFind);
        // The roll reads the level the character is on (`SpawnCharacterContext::level`).
        out.spawn.level = level_monster_level;
        out.spawn.is_hardcore = config.hardcore;
        out.spawn.magic_find = whole(players.effect_value(tl::core::item_effect::kMagicFind));
    } else {
        // Its own totals: the animal's manager and bag, any other unit's
        // manager.
        tl::core::LevelScript* machine = script_owning(attacker);
        const tl::core::CharacterEffects own =
            attacker == pet_node_id
                ? pet_total()
                : tl::core::CharacterEffects(
                      machine != nullptr ? machine->unit_effects(attacker) : nullptr, nullptr);
        out.gold_find = own.effect_value(tl::core::item_effect::kGoldFind);
        float find = whole(own.effect_value(tl::core::item_effect::kMagicFind));
        // A unit whose master is the player: the player's, when it is larger.
        if (machine != nullptr && machine->unit_is_player_pet(attacker)) {
            find = std::max(find, whole(players.effect_value(tl::core::item_effect::kMagicFind)));
        }
        out.spawn.magic_find = find;
        out.spawn.level = level_monster_level;
    }
    return &out;
}

// `CCharacter::rollCritical`, the player's critical:
// `CCameraShake("CRITICAL", forward, 0.2)` (0.2), magnitude 0.7
// (0x3f333333), from the player's position.
void World::start_critical_shake() {
    load_camera_shake_graphs();
    const float yaw = player.facing();
    tl::core::CameraShake shake("CRITICAL", tl::core::Vec3f{std::sin(yaw), 0.0f, std::cos(yaw)},
                                0.2f, camera_shake_graphs);
    shake.magnitude = 0.7f;
    const ogre::Vector3 at =
        subject_node != nullptr ? view_position(subject_node) : ogre::Vector3::ZERO;
    shake.origin = tl::core::Vec3f{at.x, at.y, at.z};
    shake.has_origin = true;
    TL_LOG_INFO("camera: CRITICAL shake%s",
                shake.curve != nullptr ? "" : " has no curve; nothing moves");
    camera_shakes.add(shake);
}

void World::start_death_shake(const char* name, float duration, float magnitude,
                              const ogre::Vector3& centre) {
    load_camera_shake_graphs();
    // `die` hands the shake its own `+0x9c`, `getUp()`: a character stands
    // upright.
    tl::core::CameraShake shake(name, tl::core::Vec3f{0.0f, 1.0f, 0.0f}, duration,
                                camera_shake_graphs);
    shake.magnitude = magnitude;
    shake.origin = tl::core::Vec3f{centre.x, centre.y, centre.z};
    shake.has_origin = true;
    shake.falloff = true;
    TL_LOG_INFO("camera: %s shake%s", name,
                shake.curve != nullptr ? "" : " has no curve; nothing moves");
    camera_shakes.add(shake);
}

// What `CQuest::reinitializeQuest` reads for a quest's
// floor (`CPlayer::getMaxDepth`, depth count, `BOTTOMLESS`, `NO_RANDOM_QUESTS`). Each floor
// tried is `resetGameSeed(floor)`, then the dungeon's kept template, built on first asking.
tl::core::QuestDungeonSource World::quest_dungeon_source() {
    return [this](const std::string& upper_name) {
        tl::core::QuestDungeon out;
        tl::core::Dungeon* dungeon = loaded_dungeon_manager().dungeon_by_name(upper_name);
        if (dungeon == nullptr) return out;
        out.found = true;
        out.display_name = dungeon->def.display_name;
        out.max_depth =
            config.travel != nullptr ? tl::core::get_max_depth(*config.travel, upper_name) : -1;
        out.depth_count = static_cast<int>(dungeon->templates.size());
        out.bottomless = dungeon->def.bottomless;
        out.no_random_quests = [this, dungeon](int floor) {
            const tl::core::DungeonFiles files = archive_dungeon_files();
            tl::core::RunicRandom& g_rand = tl::core::game_random();
            tl::core::reset_game_seed(tl::core::game_seed(), floor, g_rand);
            const tl::core::LevelTemplate* found = dungeon->get_level_template_for_depth(
                static_cast<uint32_t>(std::max(0, floor)), files, g_rand);
            return found != nullptr && found->no_random_quests;
        };
        // `getRankOfMonstersOnFloor(floor, dungeon)`.
        out.rank_on_floor = [this, dungeon](int floor) {
            return tl::core::rank_of_monsters_on_floor(
                floor, &dungeon->def, config.travel, static_cast<int32_t>(player_character.level()),
                true);
        };
        // `CPlayer::clearDungeonHistory(DUNGEON)` and
        // `updateStoredLevels` on the level the player stands on.
        out.clear_history = [this, upper_name]() {
            if (config.travel == nullptr) return;
            tl::core::clear_dungeon_history(*config.travel, upper_name);
            tl::core::update_stored_levels(*config.travel, level_dungeon, level_depth, {},
                                           level_memory_rules());
        };
        return out;
    };
}

// What `CQuestRequirements::questRequirmentsHaveBeenMet`
// reads of the player: level, `CPlayer::getMaxDepth`, and the dungeon's kept template
// (built on `g_Rand` on first asking, no `resetGameSeed`). `on_level` is always true.
tl::core::QuestPlayerSource World::quest_player_source() {
    return [this]() {
        tl::core::QuestPlayer out;
        out.present = true;
        out.on_level = true;
        out.level = static_cast<uint32_t>(player_character.level());
        out.max_depth = [this](const std::string& dungeon) {
            return config.travel != nullptr ? tl::core::get_max_depth(*config.travel, dungeon) : -1;
        };
        out.template_name = [this](const std::string& dungeon, uint32_t depth, std::string& name) {
            tl::core::Dungeon* found = loaded_dungeon_manager().dungeon_by_name(dungeon);
            if (found == nullptr) return false;
            const tl::core::DungeonFiles files = archive_dungeon_files();
            const tl::core::LevelTemplate* level_template =
                found->get_level_template_for_depth(depth, files, tl::core::game_random());
            if (level_template == nullptr) return false;
            name = level_template->name;
            return true;
        };
        return out;
    };
}

void World::ensure_item_index() {
    if (items_loaded) return;
    items_loaded = true;

    // The unit list, three ways: the PC releases' flattened table; else the Xbox
    // 360's list, `UNITDATA.RAW`; else (a pak a mod has stripped) the unit files
    // themselves. Beside the PC table, the item files it lacks are added too.
    tl::formats::AdmFile file;
    const bool from_table = load_unit_table(*this, file);
    if (!from_table) {
        file = tl::formats::AdmFile{};
        file.ok = true;
        file.root.name = "UNITS";
    }
    size_t list_differing = 0;
    const bool from_list = !from_table && file_unit_list(*this, file, list_differing);
    size_t loose_items = 0;
    const size_t loose_units =
        from_list ? 0 : add_unit_files(*this, file, !from_table, loose_items);
    if (!from_list) file_unit_names(*this, file);
    size_t mod_items = 0;
    const size_t mod_units = add_mod_unit_files(*this, file, mod_items);
    if (mod_units != 0) {
        TL_LOG_INFO("unit index: %zu unit(s) from the mods (%zu item(s))", mod_units, mod_items);
    }

    // An item's own `[EFFECT]` blocks resolve their `TYPE` against the effect list.
    ensure_effect_data();

    for (const tl::formats::AdmSection& section : file.root.children) {
        if (section.name != "ITEMS") continue;
        tl::core::ItemStats item = tl::core::read_item_stats(section, effect_types);
        if (item.name.empty()) continue;
        items.add(item);
    }
    const size_t from_master = items.size();

    // Else the transcription (`tl::core::xbox_starting_fish()`), so a new character's fish
    // does not depend on owning the Xbox archive.
    bool transcribed = false;
    if (items.find(tl::core::xbox_starting_fish().name) == nullptr) {
        items.add(tl::core::xbox_starting_fish());
        transcribed = true;
    }
    const char* const fish =
        transcribed ? ", and the Xbox starting fish from its transcription" : "";
    if (from_table) {
        TL_LOG_INFO(
            "item index: %zu item(s) from masterresourceunits.dat (%zu of them from item "
            "files it has not got)%s",
            from_master, loose_items, fish);
    } else if (from_list) {
        TL_LOG_INFO(
            "item index: %zu item(s) from unitdata.raw (%zu units filed; %zu whose file "
            "names or numbers them otherwise)%s",
            from_master, master_unit_by_guid.size(), list_differing, fish);
    } else {
        TL_LOG_INFO(
            "item index: %zu item(s), from the pak's own unit files (%zu units in all; no "
            "masterresourceunits.dat or unitdata.raw)%s",
            from_master, loose_units, fish);
    }
}

float World::player_strike(int64_t target, const tl::core::Strike& strike) {
    tl::core::Defender defender;
    tl::core::LevelScript* owner = script_owning(target);
    if (owner == nullptr || !owner->unit_defender(target, defender)) {
        // INVENTED: a unit no level machine runs has no `setLevel` numbers
        // here; it takes the blow unarmoured.
        defender = tl::core::Defender{};
    }
    defender.on_absorbed = [this, owner, target] { show_unit_absorbed(owner, target); };
    // `rollAttack`: `getDmgToReflectFromMissile` above 0 with a ranged
    // weapon (`ISA 0x23`): `MISSILEREFLECT_SOUND`, the `MISSILE REFLECT VISUAL` effect on
    // the attacker (vtable +0x114), and no damage.
    if (owner != nullptr) {
        load_unit_types();
        const float reflected = missile_reflect_share(target);
        const bool ranged =
            strike.weapon && has_weapon &&
            unit_types.isa(unit_types.id_of(weapon.unittype), tl::core::unit_type::kRangedWeapon);
        if (0.0f < reflected && ranged) {
            ogre::Vector3 at = ogre::Vector3::ZERO;
            if (unit_node_position(target, at)) {
                play_sound(player_stats.sounds[tl::core::SoundSlot::MissileReflect], at);
            }
            const tl::core::EffectSourceScope owned(&player_effects,
                                                    tl::core::EffectSource{true, owner, target});
            player_effects.offer_effect(tl::core::missile_reflect_visual_effect(reflected));
            TL_LOG_INFO("combat: %s reflects the shot, %.2f of it", owner->unit_of(target).c_str(),
                        static_cast<double>(reflected));
            last_blow_critical = false;
            last_killing_blow = tl::core::KillingBlow{};
            return 0.0f;
        }
    }
    const tl::core::Blow blow =
        tl::core::roll_attack(strike, defender, tl::core::volatile_random());
    last_blow_critical = blow.critical;
    // What `rollAttack` hands `applyDamage`: the player-to-target direction on
    // the ground, turned by the jitter, and the knock-back force.
    last_killing_blow = tl::core::KillingBlow{};
    ogre::Vector3 at = ogre::Vector3::ZERO;
    if (subject_node != nullptr && unit_node_position(target, at)) {
        const ogre::Vector3 from = view_position(subject_node);
        last_killing_blow.has_direction = true;
        last_killing_blow.direction = tl::core::blow_direction(
            {from.x, from.y, from.z}, {at.x, at.y, at.z}, blow.direction_jitter);
    }
    last_killing_blow.force = blow.knockback;
    last_killing_blow.critical = blow.critical;
    last_killing_blow.glancing = blow.glancing;
    last_blow_glancing = blow.glancing;
    last_blow_blocked = blow.blocked;
    last_blow_interrupts = blow.interrupts;
    // `rollAttack`: `interrupt(target, true)`, then `applyDamageEffects`
    // ahead of `applyDamage`.
    if (owner != nullptr && blow.interrupts && 0 < blow.damage) {
        std::vector<tl::core::ScriptEvent> events;
        owner->interrupt_unit(target, quest_log.state(), events);
        if (NodeMap* nodes = nodes_of(owner); nodes != nullptr && !events.empty()) {
            apply_script_events(events, *nodes);
        }
    }
    if (owner != nullptr && 0 < blow.damage) {
        apply_damage_status(owner, target, static_cast<float>(blow.damage), blow.by_type,
                            blow.parts, strike.damage_type, blow.critical, player_character.level(),
                            tl::core::EffectSource{true, nullptr, 0});
    }
    // `rollAttack`: knock-back, degraded armour and `TRANSFER` effects,
    // whatever the damage came to. `KNOCK BACK`: the character's, then the weapon's and its
    // sockets', each `getEffectValue(DYNAMIC, 0xc, 0, 7)` (vtable +0x120).
    if (owner != nullptr && subject_node != nullptr) {
        const tl::core::CharacterEffects total = player_total();
        float knock_back = total.effect_value(tl::core::kEffectTypeKnockBack, 7);
        if (has_weapon && weapon.combat_rolled) {
            const tl::core::EffectManager::Context& items = item_effect_context();
            knock_back +=
                tl::core::item_effect_value(weapon, items, tl::core::EffectActivation::Dynamic,
                                            tl::core::kEffectTypeKnockBack, 7);
            for (const tl::core::ItemStats& socketed : weapon.socketed) {
                knock_back += tl::core::item_effect_value(socketed, items,
                                                          tl::core::EffectActivation::Dynamic,
                                                          tl::core::kEffectTypeKnockBack, 7);
            }
        }
        tl::core::AttackEffectSources sources;
        sources.own = &player_effects;
        sources.has_inventory = true;
        sources.inventory = &player_inventory.effects();
        for (size_t location = 0; location < sources.worn.size(); ++location) {
            sources.worn[location] = player_inventory.equipped_item_effects(location);
        }
        const ogre::Vector3 from = view_position(subject_node);
        roll_attack_effects(sources, knock_back,
                            total.effect_value(tl::core::kEffectTypeDegradeArmor, 7),
                            {from.x, from.y, from.z}, {at.x, at.y, at.z}, owner, target,
                            tl::core::EffectSource{true, nullptr, 0});
    }
    TL_LOG_INFO(
        "swing: rollAttack %d%s%s%s (max physical %d, armour %d, critical chance +%.1f, "
        "critical damage +%.1f%%)",
        blow.damage, blow.critical ? ", critical" : "", blow.glancing ? ", glancing" : "",
        blow.blocked ? ", blocked" : "", strike.max_physical, defender.armor,
        static_cast<double>(strike.critical_chance), static_cast<double>(strike.critical_percent));
    return static_cast<float>(blow.damage);
}

tl::core::Defender World::player_defender() {
    tl::core::Defender defense;
    const tl::core::CharacterEffects totals = player_total();
    // `modifyDamage`: `PERCENT DAMAGE TAKEN` over the effects and the
    // inventory's (`CCharacter::getEffectValue`), the shields off the player's
    // own manager.
    defense.effects = totals;
    defense.shields = &player_effects;
    defense.on_absorbed = [this] {
        if (subject_node != nullptr) show_absorbed(view_position(subject_node), player_stats);
    };
    defense.armor = player_character.armour_class(totals);
    defense.max_hp = static_cast<int>(player_health.max);
    defense.block_chance = static_cast<int>(player_character.block_chance(totals));
    defense.interrupt_resistance = totals.effect_value(0x88, 7);
    for (int type = 1; type < tl::core::kDamageTypeCount - 1; ++type) {
        defense.damage_defense[static_cast<size_t>(type)] =
            player_character.damage_defense(type, totals);
    }
    return defense;
}

/// The nearest hostile placed unit the character can reach, or 0: both radii plus the
/// larger of `REACH_BONUS` and the held weapon's `STRIKERANGE`.
int64_t World::hostile_in_reach(float extra) const {
    if (!subject_node) return 0;
    const ogre::Vector3 me = view_position(subject_node);

    int64_t best = 0;
    float best_distance = 0.0f;
    for (const auto& entry : scene_units) {
        const SceneUnit& unit = entry.second;
        if (unit.entity == nullptr || unit.node == 0) continue;
        if (!node_targetable(unit.node, view_parent_node(unit.entity))) continue;
        const tl::core::UnitStats stats = stats_for_unit(entry.first);
        if (!unit_hostile(unit.node, stats) || !stats.targetable) continue;
        const Ogre::SceneNode* at = view_parent_node(unit.entity);
        if (at == nullptr) continue;
        const ogre::Vector3 there = view_derived_position(at);
        const float dx = there.x - me.x;
        const float dz = there.z - me.z;
        const float distance = std::sqrt(dx * dx + dz * dz);
        if (distance > extra + stats.radius) continue;
        if (best == 0 || distance < best_distance) {
            best = unit.node;
            best_distance = distance;
        }
    }
    return best;
}

/// Take life off a placed unit, and kill it if that empties it. An `INVINCIBLE` unit
/// still shows the blow (and a critical); its health never moves.
void World::hurt_unit(int64_t node, const std::string& unit, float amount, bool is_crit,
                      int64_t attacker, tl::core::LevelScript* owner_hint,
                      const tl::core::KillingBlow* blow, bool struck) {
    if (node == 0 || amount <= 0.0f) return;
    // A critical the player landed shakes the camera (`rollCritical`'s
    // player branch; attacker 0 is the player).
    if (is_crit && attacker == 0) start_critical_shake();

    const tl::core::UnitStats target_stats = stats_for_unit(unit);
    const std::string display =
        target_stats.display_name.empty() ? unit : target_stats.display_name;

    // `rollCritical`:
    // "Critical Strike!" over the target, whoever struck it.
    auto critical_text = [&] {
        ogre::Vector3 at = ogre::Vector3::ZERO;
        if (is_crit && unit_node_position(node, at)) {
            add_combat_text(tl::core::CombatText::CriticalStrike, at);
        }
    };

    // A character the level's machine runs keeps its own hit points
    // (`CCharacter::applyDamage` / `modifyHP`, `LevelScript::damage_unit`).
    tl::core::LevelScript* owner = owner_hint != nullptr ? owner_hint : script_owning(node);
    if (owner != nullptr) {
        std::vector<tl::core::ScriptEvent> events;
        // A death here runs `kill_unit` next, which plays its sample after the procs.
        owner->set_defer_death_sample(true);
        const bool killed = owner->damage_unit(node, amount, attacker, quest_log.state(), events,
                                               nullptr, blow, struck);
        owner->set_defer_death_sample(false);
        float hp = 0.0f;
        float max = 0.0f;
        owner->unit_health(node, hp, max);
        critical_text();
        TL_LOG_INFO("combat: %s takes %.1f%s, %.1f of %.1f left", display.c_str(),
                    static_cast<double>(amount), is_crit ? " (CRITICAL)" : "",
                    static_cast<double>(hp), static_cast<double>(max));
        if (owner == &script) {
            if (!events.empty()) apply_script_events(events, scripted_nodes);
        } else {
            for (size_t i = 0; i < nested_scripts.size(); ++i) {
                // By index: the events can link a layout, a push_back that
                // invalidates a deque's iterators.
                NestedScript& machine = nested_scripts[i];
                if (&machine.script != owner || events.empty()) continue;
                apply_script_events(events, machine.nodes);
            }
        }
        if (killed) kill_unit(node, unit, /*fall=*/false, attacker);
        // `applyDamage`: still alive, it answers the attacker.
        if (!killed && 0.0f < hp) {
            reflect_damage(tl::core::CharacterEffects(owner->unit_effects(node)), node, display,
                           amount, attacker);
        }
        return;
    }

    if (target_stats.invincible) {
        critical_text();
        TL_LOG_INFO("combat: %s takes %.1f%s, unaffected (invincible)", display.c_str(),
                    static_cast<double>(amount), is_crit ? " (CRITICAL)" : "");
        return;
    }

    Health& health = health_of(node, unit);
    if (health.dead) return;
    if (!(health.max > 0.0f)) {
        // Nothing said how much life it has.
        TL_LOG_WARN("combat: '%s' has no health curve; the hit does nothing", unit.c_str());
        return;
    }
    health.current -= amount;
    TL_LOG_INFO("combat: %s takes %.1f%s, %.1f of %.1f left", display.c_str(),
                static_cast<double>(amount), is_crit ? " (CRITICAL)" : "",
                static_cast<double>(std::max(0.0f, health.current)),
                static_cast<double>(health.max));

    critical_text();

    if (health.current <= 0.0f) {
        health.current = 0.0f;
        health.dead = true;
        kill_unit(node, unit, /*fall=*/true, attacker);
    }
}

// PORT: where the dead unit fell, from its OGRE node, for `drop_death_loot_at`.
void World::drop_death_loot(int64_t node, const std::string& unit, int64_t attacker) {
    // Where it fell: the unit's own node, found by id (it may already count
    // as dead to the machine).
    ogre::Vector3 at;
    bool placed = false;
    auto consider = [&](const Ogre::Entity* entity, int64_t id) {
        if (placed || id != node || entity == nullptr) return;
        const Ogre::SceneNode* where = view_parent_node(entity);
        if (where == nullptr) return;
        at = view_derived_position(where);
        placed = true;
    };
    for (const auto& entry : scene_units) consider(entry.second.entity, entry.second.node);
    for (const auto& owner : spawned) {
        for (const SpawnedUnit& made : owner.second) consider(made.entity, made.id);
    }
    if (!placed) return;
    drop_death_loot_at(at, node, unit, attacker);
}

tl::core::LootPlacement World::loot_placement_at(const ogre::Vector3& at) {
    tl::core::LootPlacement place;
    place.at = tl::core::Vec3{at.x, at.y, at.z};
    place.gold_sounds = gold_sounds();
    place.map_passable = [this](const tl::core::Vec3& p) {
        return !passability.ready() || passability.map_passable(p);
    };
    place.position_passable = [this](const tl::core::Vec3& p) {
        return !passability.ready() || passability.position_passable(p);
    };
    place.add_unit = [this](const tl::core::ItemStats& made, const tl::core::Vec3& where) {
        const ogre::Vector3 to(where.x, where.y, where.z);
        if (made.is_gold()) {
            place_gold(made.gold, to, &made);
        } else {
            drop_item(made, 1, to);
        }
    };
    place.add_gold = [this](const tl::core::GoldPile& pile, const tl::core::Vec3& where) {
        const ogre::Vector3 to(where.x, where.y, where.z);
        place_gold(pile, to);
        play_sound("GoldPickup", to);  // `rollMoney`'s own `playDropSound`
    };
    return place;
}

void World::drop_death_loot_at(const ogre::Vector3& at, int64_t node, const std::string& unit,
                               int64_t attacker) {
    const tl::core::UnitStats stats = stats_for_unit(unit);
    if (!loot_tables_built) {
        loot_tables_built = true;
        build_spawn_context(loot_tables, /*items_by_type=*/true);
    }
    const tl::core::SpawnContext& context = loot_tables.context;

    // The dying character (`CCharacter::unitInit` and
    // `spawnUnitByIndex` for its flags).
    tl::core::DyingCharacter dying;
    dying.loot.level_number = level_monster_level;  // `+0x68 -> +0x18 -> +0x1a8`
    dying.loot.dont_drop_gold = stats.dont_drop_gold || no_loot_units.count(node) != 0;
    dying.loot.spawn.level = level_monster_level;
    dying.no_drops = no_loot_units.count(node) != 0;
    // `KALIGNMENT_STRINGS`: 0 `NEUTRAL`, 1 `GOOD`, 2 `EVIL`.
    const tl::core::UnitStats::Alignment side = unit_side(node, stats);
    dying.alignment = side == tl::core::UnitStats::Alignment::Friendly  ? 1
                      : side == tl::core::UnitStats::Alignment::Hostile ? 2
                                                                        : 0;
    if (!stats.treasure_class.empty() && context.get_spawn_class) {
        dying.treasure.spawn_class = context.get_spawn_class(stats.treasure_class);
    }
    dying.treasure.min = stats.treasure_min;
    dying.treasure.max = stats.treasure_max;

    tl::core::LootCharacter killer;
    const tl::core::LootCharacter* killed_by = loot_character(attacker, killer);

    const tl::core::LootPlacement place = loot_placement_at(at);
    const tl::core::CreateLootUnit create = loot_unit_maker();

    const tl::core::SpawnClass* barrel =
        context.get_spawn_class ? context.get_spawn_class("BARREL_TREASURE") : nullptr;
    const tl::core::LootDrop drop =
        tl::core::roll_death_drop(dying, killed_by, barrel, tl::core::volatile_random(),
                                  tl::core::game_random(), context, create, &place);
    int gold = 0;
    for (int pile : drop.gold) gold += pile;
    std::string names;
    for (const tl::core::ItemStats& item : drop.items) {
        names += (names.empty() ? "" : ", ") + item.name;
    }
    TL_LOG_INFO(
        "loot: %s leaves %zu item(s) and %zu pile(s) of gold (%d) from '%s' [%s]", unit.c_str(),
        drop.items.size(), drop.gold.size(), gold,
        dying.treasure.spawn_class != nullptr ? stats.treasure_class.c_str() : "BARREL_TREASURE",
        names.c_str());
}

void World::load_player_stats() {
    player_stats = tl::core::UnitStats{};
    // DIVERGENCE: a new player in the same process (New Game) drops the last one's
    // queued lines; the original's manager outlives its players and keeps its queue.
    voice_queue.clear();
    reported_voices.clear();
    tl::core::MovementStats stats;

    // The chosen class names its own file, spelled as a path with the console's copy
    // first (the basename is not unique); `build_player_skills()` reads the same roots.
    bool from_console_data = false;
    std::string reference = class_unit_path(player_class, &from_console_data);
    // Nobody chose one (a `--mesh` run): the subject's mesh stem. Never for a playable
    // class: `PLAYER.DAT.adm` is a sound definition file.
    if (reference.empty() && subject) {
        std::string stem = view_entity_mesh(subject);
        const size_t slash = stem.find_last_of('/');
        if (slash != std::string::npos) stem = stem.substr(slash + 1);
        const size_t dot = stem.find_last_of('.');
        if (dot != std::string::npos) stem.resize(dot);
        reference = archive_path_unique(stem + ".DAT.adm");
    }
    if (reference.empty()) {
        // The player base every class inherits from, spelled in full: BASE.DAT.adm is
        // not a unique basename.
        reference = resolve_archive_path("media/units/players/base.dat.adm");
    }

    bool have_walk = false, have_run = false;
    std::string from;
    std::string health_graph;

    bool have_attack_speed = false, have_min_hp = false, have_max_hp = false;
    bool have_min_damage = false, have_max_damage = false;
    bool have_reach = false, have_weapon_scale = false, have_shield_scale = false;

    class_profile = tl::core::ClassProfile{};
    class_profile.name = player_class;

    // `CResourceManager::createPlayer(name, fromSave)`:
    // the class's record from the list's `PLAYERS` group, its file over its `BASEFILE`
    // (`CUnitResourceList::parseChildFile`). The console root goes first.
    const tl::formats::AdmFile unit =
        reference.empty() ? tl::formats::AdmFile{}
                          : read_unit_file(reference, from_console_data ? "xbox/" : "");
    if (!reference.empty() && !unit.ok) {
        TL_LOG_WARN("could not read the class file '%s'", reference.c_str());
    }
    if (unit.ok) {
        from = reference;

        if (player_stats.themes_root.children.empty()) {
            for (const tl::formats::AdmSection& child : unit.root.children) {
                if (child.name == "THEMES") player_stats.themes_root.children.push_back(child);
            }
        }

        const float walking = unit.float_or("WALKINGSPEED", -1.0f);
        if (!have_walk && walking > 0.0f) {
            stats.walking_speed = walking;
            have_walk = true;
        }
        const float running = unit.float_or("RUNNINGSPEED", -1.0f);
        if (!have_run && running > 0.0f) {
            stats.running_speed = running;
            have_run = true;
        }

        auto take = [&unit](const char* key, float& into, bool& taken) {
            if (taken) return;
            const float value = unit.float_or(key, -1.0f);
            if (value >= 0.0f) {
                into = value;
                taken = true;
            }
        };
        take("ATTACKSPEED", player_stats.attack_speed, have_attack_speed);
        take("MINHP", player_stats.min_health, have_min_hp);
        take("MAXHP", player_stats.max_health, have_max_hp);
        take("MINDAMAGE", player_stats.min_damage, have_min_damage);
        take("MAXDAMAGE", player_stats.max_damage, have_max_damage);
        take("REACH_BONUS", player_stats.reach, have_reach);
        take("WEAPON_SCALE", player_stats.weapon_scale, have_weapon_scale);
        take("SHIELD_SCALE", player_stats.shield_scale, have_shield_scale);

        // The 67-slot sound bank; slots 42 to 65 are the voice bank, authored only by
        // the three class files.
        player_stats.sounds.inherit([&unit](const char* key) { return unit.string_or(key, ""); });

        if (unit.bool_or("USEWEAPONDAMAGE", false)) player_stats.use_weapon_damage = true;
        if (player_stats.display_name.empty()) {
            player_stats.display_name = tl::core::strip_markup(unit.string_or("DISPLAYNAME", ""));
        }
        if (player_stats.intro_cinematic.empty()) {
            player_stats.intro_cinematic = unit.string_or("INTRO_CINEMATIC", "");
        }

        // What `CPlayer::unitInit` reads off the record: the four attributes, `ARMOR`,
        // and the level curves.
        {
            const std::string name = class_profile.name;
            class_profile = tl::core::read_class_profile(unit.root);
            class_profile.name = name;
            health_graph = class_profile.health_graph;
        }

        // What the character is created holding.
        unit.visit([this](const tl::formats::AdmSection& section) {
            if (section.name != "EQUIPMENT") return;
            if (player_stats.right_hand.empty()) {
                player_stats.right_hand = section.string_or("RIGHTHAND", "");
            }
            if (player_stats.left_hand.empty()) {
                player_stats.left_hand = section.string_or("LEFTHAND", "");
            }
        });
    }

    player_stats.movement = stats;

    if (have_walk && have_run) {
        TL_LOG_INFO("movement: walk %.2f, run %.2f from %s", stats.walking_speed,
                    stats.running_speed, from.c_str());
    } else {
        TL_LOG_WARN("no WALKINGSPEED/RUNNINGSPEED in the unit chain; using %.2f/%.2f",
                    stats.walking_speed, stats.running_speed);
    }
    player.set_stats(stats);

    // The unit's `MAXHP` is a percentage of its class's curve, not hit points.
    load_stat_graphs();

    // Everything the stats screen needs about the class, in one place.
    class_profile.display_name = player_stats.display_name;
    class_profile.health_graph = health_graph;
    if (!health_graph.empty() && class_profile.mana_graph.empty()) {
        TL_LOG_WARN("no MANA_GRAPH on '%s'; the stats screen will show no mana",
                    player_class.c_str());
    }

    if (!health_graph.empty()) {
        const float curve =
            stat_graphs.value(health_graph, static_cast<float>(character_level), 0.0f);
        if (curve > 0.0f) {
            player_health.max = curve * player_stats.max_health / 100.0f;
            player_health.current = player_health.max;
            player_health.dead = false;
            TL_LOG_INFO("health: %.0f (%s at level %d, %.0f%% of it)",
                        static_cast<double>(player_health.max), health_graph.c_str(),
                        character_level, static_cast<double>(player_stats.max_health));
        }
    }
    // What the character and the narrator can say (logged: data without the class files
    // is silent).
    {
        size_t authored = 0;
        for (int i = static_cast<int>(tl::core::SoundSlot::VoicePetInventoryFull);
             i <= static_cast<int>(tl::core::SoundSlot::VoiceSpellLearned); ++i) {
            if (player_stats.sounds.has(static_cast<tl::core::SoundSlot>(i))) ++authored;
        }
        TL_LOG_INFO("voice bank: %zu of 24 lines authored (%s / %s / %s / %s for the pet)",
                    authored, voice_or_none(tl::core::SoundSlot::VoicePetDeparted),
                    voice_or_none(tl::core::SoundSlot::VoicePetReturned),
                    voice_or_none(tl::core::SoundSlot::VoicePetFled),
                    voice_or_none(tl::core::SoundSlot::VoicePetInventoryFull));
    }

    if (!(player_health.max > 0.0f)) {
        TL_LOG_WARN("no HEALTH_GRAPH for the character; it cannot be hurt");
    }

    // `CGameClient::setCreationClass`: the class picked
    // on New Game makes the player anew (`createPlayer`, alignment 1, `firstTimeSetup`).
    // Starting items and default equipment for this class (CPlayer::firstTimeSetup)
    ensure_item_index();
    load_unit_types();
    player_character.init(class_profile, stat_graphs,
                          config.player_name.empty() ? player_class : config.player_name);
    // `CBaseUnit::unitInit`, not from a save:
    // `reapplyEffects` puts the class's own `[EFFECT]`s on before the skills.
    refresh_skill_effects();
    for (const tl::core::Effect& effect : tl::core::unit_own_effects(unit.root, effect_types)) {
        player_effects.add_effect(effect);
    }
    player_effects.calculate_effect_values();
    build_player_skills();
    // `CPlayer::unitInit`: the first usable skill on its
    // slot, then the potions on their item slots.
    if (const tl::core::KnownSkill* active = player_skills.bind_default_skill_slot()) {
        TL_LOG_INFO("skills: '%s' on function slot %d", active->entry.name.c_str(),
                    tl::core::SkillBook::kDefaultSkillSlot);
    }
    bind_default_item_slots();
    start_player_passives();
    {
        const tl::core::CreateEquipment create = [this](const tl::core::ItemStats& data,
                                                        bool random_enchant) {
            return create_item(data, false, random_enchant);
        };
        // `loadAndEquipSpawnClass`'s roll, as `roll_unit_equipment` makes it.
        const tl::core::RollSpawnClass roll = [this](const std::string& spawn_class,
                                                     std::vector<std::string>& units,
                                                     std::vector<bool>& enchant) {
            SpawnTables tables;
            build_spawn_context(tables, /*items_by_type=*/false);
            const std::optional<tl::core::SpawnClass> loaded =
                load_spawn_class(spawn_class, tables.context);
            if (!loaded) return;
            loaded->roll_spawn_class(units, &enchant, nullptr, nullptr, /*level=*/-1, 0xffffffffu,
                                     /*level_bonus=*/0, /*rarity_override=*/-1, /*magic_find=*/0,
                                     /*flags=*/0x10, tl::core::game_random(), tables.context);
        };
        const tl::core::EnchantEquipment enchant = [this](tl::core::ItemStats& item) {
            tl::core::enchant_item(item, true, item_roll_context());
        };
        player_inventory.clear();
        // `CCharacter::unitInit` makes the class's own `[EQUIPMENT]` (`createDefaultEquipment`),
        // then `CPlayer::firstTimeSetup` the gifts.
        player_inventory.create_default_equipment(tl::core::read_default_equipment(unit.root),
                                                  items, unit_types, create, roll, enchant);
        tl::core::StartingGifts gifts;
        gifts.launch_promo = config.starting_fish;
        player_inventory.first_time_setup(items, unit_types, gifts, create);
    }
    refresh_inventory_effects();
    // `CPlayer::firstTimeSetup` ends on the welcome.
    //
    queue_tip(tl::core::ContextTip::Welcome);

    // Extra levels and points, only when asked for on the command line.
    for (int i = 0; i < config.extra_levels; ++i) player_character.level_up(stat_graphs);
    player_character.give_unspent_stat_points(config.extra_stat_points);
    player_character.give_unspent_skill_points(config.extra_skill_points);
    player_skills.set_skill_set(config.skill_set);
    if (config.foremen_kills > 0) {
        for (int i = 0; i < config.foremen_kills; ++i) {
            kill_unit(-1000 - i, "Ratlin Foreman");
        }
    }
    if (config.extra_levels > 0) {
        // The health curve is indexed by level, so a gained level must reach the orb.
        refresh_player_maxima();
        player_health.current = player_health.max;
        player_health.dead = false;
    }
    if (config.extra_levels > 0 || config.extra_stat_points > 0 || config.foremen_kills > 0) {
        TL_LOG_INFO(
            "character: level %d, %d unspent stat point(s), %llu xp, %.0f health, %.0f mana",
            player_character.level(), player_character.unspent_stat_points(),
            static_cast<unsigned long long>(player_character.experience()),
            static_cast<double>(player_character.max_hp()),
            static_cast<double>(player_character.max_mp()));
    }

    update_menu();
    refresh_game_hud();
}

bool World::begin_swing(int64_t target) {
    // `CCharacter::attack`: nothing while a skill holds
    // the character, and no new swing while one runs.
    if (tl::core::skill_holds_character(player_active_skill)) return false;
    if (swing.active || !subject) return false;
    // `CCharacter::updateAI` runs `selectAttack(0)` every frame before
    // `attack` picks a ranged or melee hand.
    select_player_attack(0);
    choose_player_attack_hand();

    // `CCharacter::selectAttack`: `findRandomAnimation`
    // of the attack's prefix as `calculateCombatStats`
    // writes it, upper-case (`RSLASH`, `POLEARM`).
    build_player_catalog();
    const int index =
        tl::core::model_find_random_animation(player_catalog, tl::core::upper_ascii(attack_class));
    const std::string chosen =
        index >= 0 ? player_catalog[static_cast<size_t>(index)].first : std::string();
    if (chosen.empty()) {
        TL_LOG_WARN("no '%s' animation beside the character's mesh; it cannot swing",
                    attack_class.c_str());
        return false;
    }

    if (!link_subject_animation(chosen)) return false;
    const float length = view_animation_length(subject, chosen);
    if (length < 0.0f) {
        TL_LOG_WARN("linked '%s' but the character has no state for it", chosen.c_str());
        return false;
    }

    if (target != 0) set_player_target(target);
    swing = Swing{};
    swing.active = true;
    swing.animation = chosen;
    swing.length = length;
    swing.target = target;
    // The blow lands on the frame the mesh's manifest times; untagged, halfway through.
    swing.hit_at =
        manifest_for(view_entity_mesh(subject)).seconds_of(chosen, "HIT", swing.length * 0.5f);
    // `CCharacter::attack`: an attack that uses the weapon, with a
    // weapon held, plays the character's slot 0x1a (`EFFORT_SOUND`), the grunt.
    if (has_weapon && subject_node != nullptr) {
        play_sound(player_stats.sounds[tl::core::SoundSlot::Effort], view_position(subject_node));
    }
    return true;
}

/// Advance the character's swing, and land it on the authored frame.
void World::step_swing(float delta) {
    if (!swing.active) return;

    const float swing_was = swing.elapsed;
    swing.elapsed += delta * swing_rate();
    // `CCharacter::updateAnimation`, key type 9
    // (`ATTACKSOUND`) while a swing runs: slot 10 (`ATTACK_SOUND`) of the weapon's bank
    // for an attack using it, else the character's own.
    if (subject != nullptr && subject_node != nullptr) {
        const std::string& attack_sound =
            has_weapon ? weapon.attack_sound : player_stats.sounds[tl::core::SoundSlot::Attack];
        for (const tl::core::AnimationTag& tag :
             manifest_for(view_entity_mesh(subject)).tags_for(swing.animation)) {
            const bool trail = tag.name == "SHOWWEAPONTRAIL" || tag.name == "HIDEWEAPONTRAIL";
            if (tag.name != "ATTACKSOUND" && !trail) continue;
            const float when = tag.seconds();
            if (!(swing_was < when && swing.elapsed >= when)) continue;
            if (trail) {
                // Keys 7 and 8: the weapon's trail.
                player_weapon_trail_key(tag);
                continue;
            }
            play_sound(tag.sound.empty() ? attack_sound : tag.sound, view_position(subject_node));
        }
    }
    if (!swing.landed && swing.elapsed >= swing.hit_at) {
        swing.landed = true;
        // `performAttack` first flips `+0x2b2`:
        // the next `selectAttack(0)` takes the other hand.
        alternate_hand = !alternate_hand;

        // `CCharacter::performAttack(0, flags 0, 1.0, 1.0, damage type 7)`,
        // from the `HIT` key (`updateAttack`). The console's
        // search cone, player reach and `facingTarget` are used.
        load_unit_types();
        const float strike = has_weapon ? weapon.strike_range : player_stats.strike_range;
        const bool ranged = has_weapon && unit_types.isa(unit_types.id_of(weapon.unittype),
                                                         tl::core::unit_type::kRangedWeapon);
        // a `RANGED WEAPON` (ISA 0x23) adds effect 0x4e to the reach. The console
        // looks within 30 degrees and takes anything nearer than 1.5.
        //
        const float near = 1.5f;
        const float angle = 30.0f;
        const float bonus = ranged ? player_total().effect_value(0x4e) : 0.0f;
        // The character's target (`+0x340`): a target that is no enemy takes
        // no blow at all; a dead one is none.
        int64_t target = player_target_node;
        tl::core::LevelScript* target_owner = target != 0 ? script_owning(target) : nullptr;
        if (target_owner == nullptr || target_owner->unit_is_dead(target)) target = 0;
        const bool refused = target != 0 && !target_owner->unit_is_enemy_of(target, nullptr, 0);
        int64_t hit = 0;
        // `inStrikeRange`: a ranged weapon skips the
        // height test and adds effect 0x4e; the console adds 0.5 for the player.
        //
        tl::core::AttackDescription attack;
        attack.strike_range = strike;
        tl::core::UnitStats body = player_stats;
        body.radius = player.body().radius;
        // (flag 0x10 clear): `executeProcs(0x4a, target)`, then
        // `doWeaponSkill`, whose skill started ends the blow.
        bool weapon_skill = false;
        if (!refused) {
            const ProcTarget aimed = proc_target_unit(target);
            player_execute_procs(tl::core::kEffectCastSkill, aimed);
            weapon_skill = player_weapon_skill(aimed);
        }
        // `CEquipment::fireMissiles(weapon, this, target)`; firing ends the blow.
        // The console aims at the nearest character within
        // 21 (nearer than 1.5, or within 30 degrees), and a `Wand` (ISA 0x62) at an item.
        bool fired = weapon_skill;
        if (!refused && !fired && has_weapon && !weapon.missile.empty() &&
            subject_node != nullptr) {
            int64_t aim = target;
            int aim_item = -1;
            if (aim == 0) {
                ViewUnit found;
                if (find_character_within_view(player.position(), view_orientation(subject_node),
                                               1.5f, 30.0f, 21.0f, 5, /*nearest=*/true, ViewUnit{},
                                               found) &&
                    !found.player) {
                    aim = found.node;
                } else if (unit_types.isa(unit_types.id_of(weapon.unittype),
                                          tl::core::unit_type::kWand)) {
                    aim_item = breakable_within_view(
                        30.0f, tl::core::attack_view_reach(player_stats.scale, player_stats.reach,
                                                           strike, player_stats.range_multiplier));
                }
            }
            fired = fire_weapon_missiles(aim, aim_item);
        }
        if (target != 0 && !refused && !fired) {
            // `inStrikeRange(target)`: the target takes it.
            tl::core::Vec3 at;
            target_owner->unit_position(target, at);
            if (tl::core::in_strike_range(body, attack, player.position(), at,
                                          target_owner->unit_radius(target), ranged, bonus,
                                          /*console_player=*/true)) {
                hit = target;
            }
        }
        if (hit == 0 && !refused && !fired && subject_node != nullptr) {
            // Otherwise the nearest character in view, `findCharacterWithinView`;
            // the player's alignment is 5, the `EVIL` side.
            const float reach =
                tl::core::attack_view_reach(player_stats.scale, player_stats.reach, strike + bonus,
                                            player_stats.range_multiplier);
            ViewUnit found;
            if (find_character_within_view(player.position(), view_orientation(subject_node), near,
                                           angle, reach, 5, /*nearest=*/true, ViewUnit{}, found) &&
                !found.player) {
                hit = found.node;
            } else {
                // None in view: the target, out of strike range or not.
                //
                hit = target;
            }
        }
        if (refused) {
            TL_LOG_INFO("swing: '%s' is not at an enemy", swing.animation.c_str());
        } else if (weapon_skill) {
            TL_LOG_INFO("swing: '%s' casts the weapon's skill", swing.animation.c_str());
        } else if (fired) {
            TL_LOG_INFO("swing: '%s' fires the weapon's '%s'", swing.animation.c_str(),
                        weapon.missile.c_str());
        } else {
            // `CCharacter::performAttack`: no character in view, so
            // `CLevel::findItemWithinView` for a breakable, which `interact` (+0x280) breaks.
            int found = -1;
            if (hit == 0) {
                const float view = tl::core::attack_view_reach(
                    player_stats.scale, player_stats.reach, strike, player_stats.range_multiplier);
                found = breakable_within_view(angle, view);
            }
            // Where the blow goes: the character's place (`getPosition(true)`)
            // and radius (`+0x194`), the breakable's, or nowhere.
            const tl::core::Vec3 here = player.position();
            tl::core::Vec3 at = here;
            float at_radius = 0.0f;
            bool aimed = false;
            if (hit != 0) {
                if (tl::core::LevelScript* owner = script_owning(hit)) {
                    aimed = owner->unit_position(hit, at);
                    at_radius = owner->unit_radius(hit);
                }
            } else if (found >= 0 && breakables[static_cast<size_t>(found)].node != nullptr) {
                const ogre::Vector3 p = view_position(breakables[static_cast<size_t>(found)].node);
                at = tl::core::Vec3{p.x, p.y, p.z};
                // `CBaseUnit::CBaseUnit`: a
                // breakable's `+0x194` is 0.3, never read from its data.
                at_radius = 0.3f;
                aimed = true;
            }
            // the wall test. A ranged weapon's shot, with its tracer;
            // anything else's three rays.
            bool walled = false;
            if (ranged) {
                const uint32_t type = unit_types.id_of(weapon.unittype);
                const bool tracer = unit_types.isa(type, tl::core::unit_type::kBow) ||
                                    unit_types.isa(type, tl::core::unit_type::kCrossbow) ||
                                    unit_types.isa(type, tl::core::unit_type::kRifle) ||
                                    unit_types.isa(type, tl::core::unit_type::kPistol);
                // The weapon model's tag point (`+0x1e0 -> +0x60`, its parent
                // node). INVENTED: with no model drawn, the character's feet.
                tl::core::Vec3 start = here;
                ogre::Vector3 hand;
                float height = 0.0f;
                if (view_held_item(weapon_entity, hand, height)) {
                    start = tl::core::Vec3{hand.x, hand.y, hand.z};
                }
                const float facing = player.facing();
                const tl::core::Vec3 forward{std::sin(facing), 0.0f, std::cos(facing)};
                // `RANGE_MULTIPLIER * strike range + effect 0x4e`.
                const float reach = player_stats.range_multiplier * strike + bonus;
                const tl::core::RangedShot shot =
                    tl::core::ranged_shot(&collision, start, here, body.radius,
                                          aimed ? &at : nullptr, at_radius, forward, reach);
                walled = shot.walled;
                if (tracer) player_shoot_tracer(start, shot.end);
                TL_LOG_INFO("swing: shot from (%.2f, %.2f, %.2f) to (%.2f, %.2f, %.2f)%s%s",
                            static_cast<double>(start.x), static_cast<double>(start.y),
                            static_cast<double>(start.z), static_cast<double>(shot.end.x),
                            static_cast<double>(shot.end.y), static_cast<double>(shot.end.z),
                            walled ? ", stopped by a wall" : "", tracer ? ", traced" : "");
            } else if (!collision.empty()) {
                walled = tl::core::melee_attack_walled(collision, here, player.facing(), at);
            }
            // `inStrikeRange(character, item, 0)` and
            // `facingTarget`; either failing ends the blow silently. With nothing to strike
            // `facingTarget` is false, so it ends here. An item
            // keeps the height test; the console's
            // `facingTarget` with the weapon's arc.
            const uint32_t held = has_weapon ? unit_types.id_of(weapon.unittype) : 0;
            const float arc = has_weapon ? tl::core::console_weapon_arc(
                                               unit_types.isa(held, tl::core::unit_type::kSword),
                                               unit_types.isa(held, tl::core::unit_type::kAxe),
                                               unit_types.isa(held, tl::core::unit_type::kMace),
                                               unit_types.isa(held, tl::core::unit_type::kPolearm),
                                               unit_types.isa(held, tl::core::unit_type::kStaff))
                                         : 0.0f;
            const bool item = hit == 0 && found >= 0;
            const bool reaches =
                aimed && (!item || !(2.5f < std::fabs(at.y - here.y))) &&
                tl::core::in_strike_range(body, attack, here, at, at_radius, ranged, bonus,
                                          /*console_player=*/true) &&
                tl::core::facing_target(player.facing(), here, at, player_stats.attack_angle, arc);
            if (!reaches) {
                // What the search passed by: the nearest enemy, for the log.
                std::string nearest = "no enemy listed";
                if (subject_node != nullptr) {
                    const ogre::Quaternion inverse = view_orientation(subject_node).Inverse();
                    float best = 9999.0f;
                    for (tl::core::LevelScript* machine : level_machines) {
                        std::vector<tl::core::LevelScript::Reachable> list;
                        machine->reachable_characters(list);
                        for (const tl::core::LevelScript::Reachable& r : list) {
                            if (r.alignment != tl::core::UnitStats::Alignment::Hostile) continue;
                            const ogre::Vector3 local =
                                inverse *
                                ogre::Vector3(r.center.x - here.x, 0.0f, r.center.z - here.z);
                            const float distance = std::sqrt(local.x * local.x + local.z * local.z);
                            if (best <= distance) continue;
                            best = distance;
                            char text[160];
                            std::snprintf(
                                text, sizeof(text),
                                "nearest enemy %s %.2f away, %.0f degrees off, %+.2f up",
                                r.unit.c_str(), static_cast<double>(distance),
                                static_cast<double>(std::atan2(local.x, local.z) * 57.295826f),
                                static_cast<double>(r.center.y - here.y));
                            nearest = text;
                        }
                    }
                }
                TL_LOG_INFO("swing: '%s' %s (%s)", swing.animation.c_str(),
                            aimed ? "does not reach its target" : "connects with nothing",
                            nearest.c_str());
            } else if (hit == 0 && found >= 0) {
                const ProcTarget broken = proc_target_breakable(found);
                break_breakable(static_cast<size_t>(found), /*by_player=*/true);
                // CCharacter::performAttack: incrementJournalStatistic(11, 1)
                count_journal(tl::core::JournalStatistic::BarrelsBroken);
                // broken, but a wall between makes the blow a miss.
                if (walled && subject_node != nullptr) {
                    play_sound(player_stats.sounds[tl::core::SoundSlot::Miss],
                               view_position(subject_node));
                } else if (!walled) {
                    // in the open, the item takes a struck character's procs
                    // and the blow its strike sound.
                    player_execute_procs(tl::core::kEffectCastSkillOnTarget, broken);
                    player_execute_procs(tl::core::kEffectCastSkillAtTarget, broken);
                    player_weapon_procs(broken);
                    if (subject_node != nullptr) {
                        play_sound(has_weapon ? weapon.strike_sound
                                              : player_stats.sounds[tl::core::SoundSlot::Strike],
                                   view_position(subject_node));
                    }
                }
            } else if (walled) {
                TL_LOG_INFO("swing: '%s' is stopped by a wall", swing.animation.c_str());
                // `performAttack`: a wall in the way: slot 0xf
                // (`MISS_SOUND`) of the character's bank.
                if (subject_node != nullptr) {
                    play_sound(player_stats.sounds[tl::core::SoundSlot::Miss],
                               view_position(subject_node));
                }
            } else {
                // `performAttack` sets no target: what the blow lands on is not
                // made the character's target.
                std::string unit;
                for (const auto& entry : scene_units) {
                    if (entry.second.node == hit) unit = entry.first;
                }
                // Where the struck unit stood, before the blow can take it away.
                const ProcTarget struck = proc_target_unit(hit);
                const float dealt = player_blow(hit);
                show_player_blow(hit, unit, dealt);
                hurt_unit(hit, unit, dealt, last_blow_critical, 0, nullptr, &last_killing_blow,
                          /*struck=*/true);
                // `rollAttack` true, then `executeProcs(0x4b)` and
                // `(0x4c)` at the struck unit and `doWeaponProcs`.
                player_execute_procs(tl::core::kEffectCastSkillOnTarget, struck);
                player_execute_procs(tl::core::kEffectCastSkillAtTarget, struck);
                player_weapon_procs(struck);
                // `performAttack`: slot 1 (`STRIKE_SOUND`) of the weapon's bank,
                // else the character's own.
                if (subject_node != nullptr) {
                    play_sound(has_weapon ? weapon.strike_sound
                                          : player_stats.sounds[tl::core::SoundSlot::Strike],
                               view_position(subject_node));
                }
                const tl::core::LevelScript* owner = script_owning(hit);
                slow_on_player_kill(owner != nullptr && owner->unit_is_dead(hit));
            }
        }
    }
    if (swing.elapsed >= swing.length) {
        // `CCharacter::update`: the attack's
        // time (`+0x378`) run out puts the hands' trails off.
        player_hide_hand_trails();
        swing = Swing{};
    }
}

// PORT: the player's model's animation list (`CGenericModel +0x1cc`), read once per mesh.
void World::build_player_catalog() {
    const std::string mesh = view_entity_mesh(subject);
    if (mesh.empty()) return;
    if (mesh == player_catalog_mesh) return;
    player_catalog_mesh = mesh;
    player_catalog.clear();

    // The `.ANIMATION` file beside the mesh, in its order; the index is what a skill's
    // `getAnimationIndex*` and a keyframe's `+0x28` hold.
    size_t missing = 0;
    player_catalog = model_catalog(mesh, subject_dir, &missing);
    TL_LOG_INFO("skills: the character's model lists %zu animation(s), %zu without a skeleton",
                player_catalog.size(), missing);
}

// `CCharacter::updateAnimation` handles
// the model's keys before the skill's: a `SPAWNPARTICLE` key (type 4, 5
// with STOP_ON_DEATH) starts its particle.
void World::fire_model_keys(const std::vector<tl::core::FiredKey>& keys,
                            const tl::core::AnimationManifest& manifest) {
    for (const tl::core::FiredKey& key : keys) {
        const std::vector<tl::core::AnimationTag>& tags =
            manifest.tags_for(skill_playback.animation);
        if (key.tag >= tags.size()) continue;
        if (key.type == 7 || key.type == 8) {
            // `SHOWWEAPONTRAIL`, `HIDEWEAPONTRAIL`.
            player_weapon_trail_key(tags[key.tag]);
            continue;
        }
        // `PLAYSOUND` (3), `FOOTSTEP` (6) and `ATTACKSOUND` (9): the key's own sound, else
        // the step or swing of the weapon's bank, or the character's.
        if (key.type == 3 || key.type == 6 || key.type == 9) {
            const tl::core::AnimationTag& tag = tags[key.tag];
            std::string sound = tag.sound;
            if (sound.empty() && key.type == 6) {
                sound = player_stats.sounds[tl::core::SoundSlot::Step];
            } else if (sound.empty() && key.type == 9) {
                sound = has_weapon ? weapon.attack_sound
                                   : player_stats.sounds[tl::core::SoundSlot::Attack];
            }
            if (subject_node != nullptr) play_sound(sound, view_position(subject_node));
            continue;
        }
        if (key.type != 4 && key.type != 5) continue;
        fire_particle_trigger(skill_playback.animation, key.tag, tags[key.tag]);
    }
}

void World::step_skill_animation(float delta) {
    if (!skill_playback.active || !(delta > 0.0f) || !subject) return;

    // The model's clock and its keys: `CGenericModel::updateAnimation`.
    // A held one-shot stops at its end; a loop wraps.
    const float from = skill_playback.time;
    float to = from + delta * skill_playback.speed;
    if (skill_playback.length > 0.0f && to > skill_playback.length) {
        to = skill_playback.held < 0.0f ? std::fmod(to, skill_playback.length)
                                        : skill_playback.length;
    }
    skill_playback.time = to;
    const tl::core::AnimationManifest& manifest = manifest_for(view_entity_mesh(subject));
    const std::vector<tl::core::FiredKey> keys = tl::core::keys_crossed(
        manifest.tags_for(skill_playback.animation), skill_playback.index, from, to);

    fire_model_keys(keys, manifest);

    // `CCharacter::updateSkillKeys`, then each group
    // through `CSkillManager::triggerSkillEvent`.
    const std::string name = player_active_skill.skill;
    const tl::core::SkillRunState* state = player_skill_runner.state(name);
    if (state != nullptr && !keys.empty()) {
        const std::vector<int> groups = tl::core::update_skill_keys(
            player_active_skill, state->animations, player_skill_runner.is_running(name),
            !player_death.dying, keys);
        for (int group : groups) {
            const int applied = player_skill_runner.trigger_event(
                group, player_cast_context(),
                [this](const std::string& n) -> const tl::core::KnownSkill* {
                    return player_skill(n);
                });
            TL_LOG_INFO(
                "skills: '%s' key at %.3fs fires %s, %d skill(s) applied, "
                "%zu affix(es) held, %zu effect(s)",
                skill_playback.animation.c_str(), static_cast<double>(to),
                group == tl::core::kSkillEventTrigger ? "EVENT_TRIGGER" : "EVENT_TRIGGER_TWO",
                applied, player_effects.affixes().size(), player_effects.all_effects().size());
        }
    }

    // `CCharacter::updateAnimation`: the
    // gait takes the model back once the cast flag (`+0x267`) is down and no swing runs.
    if (skill_playback.held < 0.0f && !player_active_skill.casting &&
        !(player_active_skill.animation_left > 0.0f && !player_active_skill.attack_ended)) {
        skill_playback = SkillPlayback{};
        return;
    }
    // The cue's hold: then the queued loop, or `IDLE` - which here is the
    // character's own gait again.
    if (skill_playback.held < 0.0f) return;
    skill_playback.held -= delta;
    if (skill_playback.held > 0.0f) return;
    if (skill_playback.then_loop >= 0) {
        tl::core::SkillAnimationCue loop;
        loop.animation = skill_playback.then_loop;
        loop.speed = 1.0f;  // `queueBlendAnimation(loop, true, 0.2, 1.0)`
        loop.duration = -1.0f;
        play_skill_cue(loop);
        return;
    }
    skill_playback = SkillPlayback{};
}

void World::step_active_skill(float dt) {
    const std::string name = player_active_skill.skill;
    const tl::core::KnownSkill* skill = player_skills.find_by_name(name);
    const tl::core::SkillRunState* state = player_skill_runner.state(name);
    const tl::core::SkillAnimations animations =
        state != nullptr ? state->animations : tl::core::SkillAnimations{};
    // `CSkill::getManaCostOT`: the level's `MANACOSTOT`, truncated.
    int64_t upkeep = 0;
    if (skill != nullptr && state != nullptr) {
        if (const tl::core::SkillProperty* property = skill->property(state->level)) {
            upkeep = static_cast<int64_t>(property->mana_cost_ot);
        }
    }
    const tl::core::ActiveSkillUpdate out = tl::core::update_active_skill(
        player_active_skill, animations, player_skill_runner.is_running(name), upkeep,
        player_character.mp(), dt);
    // `updateActiveSkill`'s `modifyMana(-cost)`.
    if (out.mana_spent > 0.0f) modify_player_mana(-out.mana_spent);
    if (out.hide_trails) player_hide_hand_trails();
    if (out.stop_all) {
        // `blendAnimation(loopEnd, 0.2, 1.0, -1)` with `IDLE` queued behind it,
        // then every skill stopped.
        if (out.loop_end >= 0 && static_cast<size_t>(out.loop_end) < player_catalog.size()) {
            tl::core::SkillAnimationCue end;
            end.animation = out.loop_end;
            end.duration = player_catalog[static_cast<size_t>(out.loop_end)].second;
            end.then_idle = true;
            play_skill_cue(end);
        } else {
            skill_playback = SkillPlayback{};
        }
        // `updateActiveSkill`: `stopAllSkills(false,...)`.
        player_skill_runner.stop_all(
            player_cast_context(),
            [this](const std::string& n) -> const tl::core::KnownSkill* { return player_skill(n); },
            /*force=*/false, /*passives=*/false);
        TL_LOG_INFO("skills: '%s' ran out of mana for its upkeep; every skill stopped",
                    name.c_str());
    }
}

/// A placed unit's life has run out. The fall is `Die`, then `Die1`: models name their
/// death animation either way and the unit data names neither.
void World::kill_unit(int64_t node, const std::string& unit, bool fall, int64_t attacker) {
    // Whatever the level hangs off this unit dying (`Monster Killed` wires).
    {
        // `CCharacter::die`: the death particle, by "Show Blood".
        ogre::Vector3 body = ogre::Vector3::ZERO;
        if (unit_node_position(node, body)) {
            play_death_particle(stats_for_unit(unit), body, character_ref(node));
        }
    }
    std::vector<tl::core::ScriptEvent> events;
    script.unit_killed(node, quest_log.state(), events);
    {
        // `CCharacter::die`:
        // `questEventFire(1, attacker, this)`.
        const auto tagged = quest_unit_tags.find(node);
        ogre::Vector3 body = ogre::Vector3::ZERO;
        unit_node_position(node, body);
        tl::core::QuestEventUnit dead = quest_event_unit(
            unit, stats_for_unit(unit).unittype, body,
            tagged == quest_unit_tags.end() ? tl::core::QuestUnitTag{} : tagged->second);
        // The character's name (`+0x4c0`): a champion's or a quest's
        // own over the unit's.
        const auto named = unit_names.find(node);
        const tl::core::UnitStats& dead_stats = stats_for_unit(unit);
        dead.shown_name = named != unit_names.end()
                              ? named->second
                              : (dead_stats.display_name.empty() ? unit : dead_stats.display_name);
        quest_event(quest_log.unit_defeated(dead));
    }

    if (!events.empty()) apply_script_events(events, scripted_nodes);

    index_unit(unit);

    // `CCharacter::die`: `stopAllSkills`,
    // `fireSkillsOnDeath`, then `executeProcs(0x69, attacker)`, the
    // `CAST SKILL ON DEATH` effects.
    if (tl::core::LevelScript* owner = script_owning(node)) {
        fire_unit_skills_on_death(*owner, node);
        const ProcTarget by = proc_target_attacker(attacker);
        unit_execute_procs(*owner, node, tl::core::kEffectCastSkillOnDeath, by.machine, by.node,
                           by.any() || attacker == 0);
        std::vector<tl::core::ScriptEvent> sample;
        owner->play_death_sample(node, sample);
        if (NodeMap* nodes = nodes_of(owner); nodes != nullptr && !sample.empty()) {
            apply_script_events(sample, *nodes);
        }
    }
    // a death with no blow to fly from and no burst:
    // `findRandomAnimation("DIE")` blended in.
    std::string animation;
    if (fall) {
        if (const tl::core::LevelScript* owner = script_owning(node)) {
            animation = owner->death_animation(node);
        }
    }
    const bool fell = !fall || view_unit_falls(node, animation);
    TL_LOG_INFO("combat: %s dies%s%s%s", unit.c_str(),
                fell ? "" : " (no death animation beside its mesh)",
                animation.empty() ? "" : ", playing ", animation.c_str());

    // The treasure at the end of `CCharacter::die`,
    // after `fireSkillsOnDeath` and before `applyDamage` pays the experience.
    drop_death_loot(node, unit, attacker);
    // after the money: whoever watches the unit hears that it died
    // (`unitBroadcastMessage(1)`, inline).
    unit_observers.broadcast({script_owning(node), node}, tl::core::unit_state::kDied);

    const tl::core::UnitStats stats = stats_for_unit(unit);
    if (unit_hostile(node, stats)) {
        const bool champion = stats.is_champion || spawn_marked_champion(node);

        // Experience: EXPERIENCE_MONSTER / EXPERIENCE_CHAMPIONMONSTER at the difficulty,
        // squared, at the monster's level (`monster_experience()`); the authored XP only
        // switches it on. The level is the floor's (`unit_spawn_level`), else
        // `character_level`.
        const int monster_level = effective_monster_level(node);
        // `applyDamage` pays the attacker: no attacker, no experience; nor from a unit
        // released with `GIVE XP` clear (+0x454 = 0).
        const bool pays =
            attacker != tl::core::LevelScript::kNoAttacker && no_xp_units.count(node) == 0;
        if (pays) {
            // CCharacter::awardExperience: incrementJournalStatistic(6, 1)
            count_journal(tl::core::JournalStatistic::MonstersDefeated);
            if (champion) {
                // CCharacter::awardExperience: incrementJournalStatistic(7, 1)
                count_journal(tl::core::JournalStatistic::ChampionsDefeated);
            }
        }
        const uint64_t xp = pays ? tl::core::monster_experience(monster_level, stats.xp, champion,
                                                                config.difficulty, stat_graphs)
                                 : 0;
        const bool leveled = award_player_experience(static_cast<int>(xp), player_health.dead);
        TL_LOG_INFO("experience: %s pays %llu, level %d at %llu/%llu", unit.c_str(),
                    static_cast<unsigned long long>(xp), player_character.level(),
                    static_cast<unsigned long long>(player_character.experience()),
                    static_cast<unsigned long long>(player_character.experience_gate()));
        if (leveled) player_levelled_up();

        update_menu();
        refresh_game_hud();
    }
}

}  // namespace tl::core
