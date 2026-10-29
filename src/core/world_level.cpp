// The gameplay half of render/renderer_level.cpp (core/world.h).
#include "core/layout_text.h"
#include "core/level_memory.h"
#include "core/logic_names.h"
#include "core/ogre_math.h"
#include "core/skill_cast.h"
#include "core/unit_skills_reader.h"
#include "core/world.h"
#include "formats/ogre_mesh.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace tl::core {

namespace {

// A collision placement into the level's collision world; a room's piece waits for
// `end_room_placement`, which adds them in the order made.
void place_collision(World& world, const MeshPlacement& placement) {
    if (placement.made != 0 && placement.collision_group == 0 && world.collision_section >= 0) {
        world.room_piece_collision.push_back({world.collision_section, placement});
        return;
    }
    world.add_collision_placement(placement);
    if (placement.collision_group != 0) {
        world.collision.set_group_blocks(placement.collision_group, placement.collision_blocks);
    }
}

// A room piece's collision, numbered in the order the level made it (`end_room_placement` adds
// them so); a piece cast again after the bake is `recalculate`.
void add_room_piece_collision(World& world, std::vector<MeshPlacement>& out,
                              MeshPlacement placement, int depth, bool recalculate) {
    placement.room_piece = true;
    placement.recalculate = recalculate;
    placement.made = ++world.room_pieces_made;
    placement.linked = depth > 1;
    out.push_back(std::move(placement));
}

// CLevel::bakeCollision, handed `m_HasPathing` by loadRoomLayout: with
// pathing, a room without its `.pth` holds the random pieces back.
bool rooms_hold_random_pieces(const World& world, const tl::core::LevelTemplate& level_template,
                              const tl::core::LevelLayout& layout) {
    if (!level_template.has_pathing) return false;
    for (size_t chunk_i = 0; chunk_i < layout.instances.size(); ++chunk_i) {
        const std::string& piece = level_template.chunks[layout.pieces[chunk_i]].path;
        if (world.resolve_archive_path(piece + ".cmp").empty()) continue;
        if (piece.size() < 6 ||
            world.resolve_archive_path(piece.substr(0, piece.size() - 6) + "pth").empty()) {
            return true;
        }
    }
    return false;
}

// The rooms are placed: their seeding ends, and their pieces' collision goes in as
// CLevel::bakeCollision walks them, room by room, each `Room Piece` in the order made.
void end_room_placement(World& world) {
    world.layout_seeding = false;
    std::stable_sort(
        world.room_piece_collision.begin(), world.room_piece_collision.end(),
        [](const WorldState::RoomPieceCollision& a, const WorldState::RoomPieceCollision& b) {
            return a.placement.made < b.placement.made;
        });
    for (const WorldState::RoomPieceCollision& piece : world.room_piece_collision) {
        world.collision_section = piece.section;
        world.add_collision_placement(piece.placement);
    }
    world.collision_section = -1;
    world.room_piece_collision.clear();
}

// Node::_updateFromParentImpl: the object's node under its layout's, turned
// by the parent's quaternion and its own axes made one
// (`CPositionableObject::setOrientation`), all fused; the room's own node is (1, -0, -0, -0).
void node_place(const tl::formats::CmpNode& node, const ogre::Vector3& origin,
                const ogre::Quaternion& basis, ogre::Vector3& position,
                ogre::Quaternion& orientation) {
    const bool unturned = basis.w == 1.0f && basis.x == 0.0f && basis.y == 0.0f && basis.z == 0.0f;
    const tl::core::OgreQuaternion parent =
        unturned ? tl::core::kLayoutNodeOrientation
                 : tl::core::OgreQuaternion{basis.w, basis.x, basis.y, basis.z};
    const tl::core::Vec3 turned = tl::core::quaternion_rotate(
        parent, {node.position().x, node.position().y, node.position().z});
    position = ogre::Vector3(origin.x + turned.x, origin.y + turned.y, origin.z + turned.z);
    tl::core::OgreQuaternion own = tl::core::quaternion_derived(parent, {});
    if (node.vec3s.size() >= 4) {
        const auto axis = [&node](size_t i) {
            return tl::core::Vec3{node.vec3s[i].x, node.vec3s[i].y, node.vec3s[i].z};
        };
        own = tl::core::node_orientation(axis(2), axis(3), axis(1), parent);
    }
    orientation = ogre::Quaternion(own.w, own.x, own.y, own.z);
}

// `CPositionableObject::getPosition(true)` of an object under a Group: the layout
// holds where it stands, the scene keeps it as its offset from the Group's place, and the node gives
// it back as `group + (world - group)` (`_updateFromParentImpl`) - a last bit off
// where the two are far apart. A turned room or Group keeps the place as `node_place` makes it.
ogre::Vector3 derived_position(const std::map<int64_t, const tl::formats::CmpNode*>& by_id,
                               const tl::formats::CmpNode& node, const ogre::Vector3& origin,
                               const ogre::Quaternion& basis, int depth = 0) {
    ogre::Vector3 position;
    ogre::Quaternion orientation;
    node_place(node, origin, basis, position, orientation);
    const bool unturned = basis.w == 1.0f && basis.x == 0.0f && basis.y == 0.0f && basis.z == 0.0f;
    const auto parent = by_id.find(node.parent_id);
    if (!unturned || depth > 64 || parent == by_id.end() ||
        parent->second->type != tl::formats::cmp_type::kGroup ||
        parent->second->vec3s.size() >= 4) {
        return position;
    }
    const ogre::Vector3 at = derived_position(by_id, *parent->second, origin, basis, depth + 1);
    const ogre::Vector3 offset = position - at;
    return at + offset;
}

// The floor's collision bookkeeping, emptied before its rooms are placed.
void begin_floor_collision(World& world, const tl::core::LevelTemplate& level_template,
                           const tl::core::LevelLayout& layout) {
    world.passability_recalculate.clear();
    world.held_random_collision.clear();
    world.held_random_boxes.clear();
    world.room_piece_collision.clear();
    world.room_pieces_made = 0;
    world.hold_random_pieces = rooms_hold_random_pieces(world, level_template, layout);
}

// The floor's grid baked over its rooms; a held piece's faces join after the rooms' passes.
void bake_floor_passability(World& world) {
    const auto add_held = [&world] {
        for (const WorldState::HeldCollision& held : world.held_random_collision) {
            world.add_collision_mesh(held.path, held.to_world);
        }
        world.collision.build();
    };
    world.passability.bake(world.collision, &world.collision_sections,
                           &world.passability_recalculate,
                           world.hold_random_pieces ? &world.held_random_boxes : nullptr, add_held);
}

// `CParticlePreloader` +0x20 (`m_ParticleSystems`): the files loaded, for the whole run, and here
// what of each the load kept out (`World::roll_layout_groups` and LoadParticle's filter).
std::map<std::string, std::set<int64_t>>& loaded_particle_files() {
    static std::map<std::string, std::set<int64_t>> loaded;
    return loaded;
}

}  // namespace

/// Where a layout's rules file lives, or empty when it has none (read before placement
/// for the rim light: read_rim_light()).
// PORT: a layout path as the key the port's backdrop rules are kept under.
std::string World::backdrop_key(const std::string& layout_path) {
    std::string key = tl::core::upper_ascii(layout_path);
    for (char& c : key) {
        if (c == '\\') c = '/';
    }
    for (const char* suffix : {".CMP", ".ADM"}) {
        const size_t n = std::strlen(suffix);
        if (key.size() > n && key.compare(key.size() - n, n, suffix) == 0)
            key.resize(key.size() - n);
    }
    return key;
}

std::string World::rules_path_for(const std::string& layout_path) {
    // A generated floor hands over its stratum's RULESET itself, not a layout.
    if (layout_path.size() > 4) {
        std::string tail = layout_path.substr(layout_path.size() - 4);
        for (char& c : tail) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (tail == ".dat") {
            const std::string direct = resolve_archive_path(layout_path + ".adm");
            if (!direct.empty()) return direct;
        }
    }

    // A main-menu backdrop knows the rules it was laid out from.
    const auto backdrop = backdrop_rules.find(backdrop_key(layout_path));
    if (backdrop != backdrop_rules.end()) {
        const std::string path = resolve_archive_path(backdrop->second + ".adm");
        if (!path.empty()) return path;
    }

    // A main menu's rules: the layout's name plus RULES.DAT, in the parent directory,
    // found by unique basename.
    std::string stem = layout_path;
    const size_t slash = stem.find_last_of('/');
    if (slash != std::string::npos) stem = stem.substr(slash + 1);
    for (const char* suffix : {".cmp", ".LAYOUT", ".layout"}) {
        const size_t n = std::strlen(suffix);
        if (stem.size() > n && stem.compare(stem.size() - n, n, suffix) == 0) {
            stem.resize(stem.size() - n);
        }
    }

    std::string rules_path = archive_path_unique(stem + "RULES.DAT.adm");

    // A playable level's rules are `rules.dat` (the dungeon's RULESET), ambiguous by
    // basename, so resolved by path: the layout's directory or its parent.
    if (rules_path.empty()) {
        const size_t last = layout_path.find_last_of('/');
        const std::string own =
            (last == std::string::npos) ? std::string() : layout_path.substr(0, last + 1);

        std::string parent;
        if (own.size() >= 2) {
            const size_t up = own.find_last_of('/', own.size() - 2);
            if (up != std::string::npos) parent = own.substr(0, up + 1);
        }

        // Parent first: that is where the shipped levels keep it, and trying the
        // other order would log an "ambiguous" warning on every level that is
        // laid out normally.
        for (const std::string& dir : {parent, own}) {
            if (dir.empty() || !rules_path.empty()) continue;
            rules_path = resolve_archive_path(dir + "rules.dat.adm");
        }
    }

    return rules_path;
}

/// Read and parse a level's rules file, or report that it has none.
bool World::read_rules(const std::string& layout_path, tl::formats::AdmFile& out, bool quiet) {
    const std::string rules_path = rules_path_for(layout_path);
    if (rules_path.empty()) {
        if (!quiet) {
            TL_LOG_INFO("no rules file for '%s'; keeping the default lighting",
                        layout_path.c_str());
        }
        return false;
    }

    const std::vector<uint8_t> bytes = read_resource(rules_path);
    if (bytes.empty()) return false;

    out = tl::formats::parse_adm(bytes.data(), bytes.size());
    if (!out.ok) {
        if (!quiet) {
            TL_LOG_WARN("could not parse '%s': %s", rules_path.c_str(), out.error.c_str());
        }
        return false;
    }
    if (!quiet) {
        TL_LOG_INFO("rules %s: section %s, %zu records, %zu undecoded byte(s)", rules_path.c_str(),
                    out.section().c_str(), out.records().size(), out.undecoded_bytes);
    }
    return true;
}

// The level's rim light, read before the level is placed: every character needs it as
// it is placed. The default when the
// rules author none.
void World::read_rim_light(const std::string& rules_reference) {
    tl::formats::AdmFile rules;
    rim_light_texture = read_rules(rules_reference, rules, /*quiet=*/true)
                            ? rules.string_or("RIMLIGHT", kDefaultRimLight)
                            : std::string(kDefaultRimLight);
    TL_LOG_INFO("  rules rim light: '%s'", rim_light_texture.c_str());
}

const tl::formats::CmpScene* World::layout_file(const std::string& path) {
    const auto found = layout_files.find(path);
    if (found != layout_files.end()) return found->second.get();
    // `CEditorScene::loadScene` takes the
    // compiled file when there is one (`CFileSystem::getFileInfo`'s compressed
    // check), and reads the layout as a data group - a mod's text `.layout` -
    // only when there is not (`core/layout_text`).
    std::string source = path;
    if (!ends_with_lower(path, ".cmp")) {
        const std::string compiled = resolve_archive_path(path + ".cmp");
        if (!compiled.empty()) source = compiled;
    }
    const std::vector<uint8_t> bytes = read_resource(source);
    if (bytes.empty()) return nullptr;
    auto parsed = std::make_unique<const tl::formats::CmpScene>(
        tl::core::parse_layout_file(bytes.data(), bytes.size()));
    return layout_files.emplace(path, std::move(parsed)).first->second.get();
}

// CGameClient::loadLevel, after the level is created:
// the depth's MUST_BE_COMPLETED quests not complete are completed
// (CQuestManager::completeQuest), and MUST_BE_COMPLETED_OR_ACTIVE ones neither complete
// nor active are given (giveQuest(quest, nullptr, false), no banner), accepted and marked
// interacted.
void World::apply_floor_quests(const tl::core::DungeonDef& def, uint32_t depth) {
    if (depth >= def.depth_strata.size()) return;
    const uint32_t index = def.depth_strata[depth];
    if (index >= def.strata.size()) return;
    const tl::core::DungeonStrata& stratum = def.strata[index];
    if (stratum.must_be_completed.empty() && stratum.must_be_completed_or_active.empty()) return;
    load_quest_book();
    load_stat_graphs();
    for (const std::string& name : stratum.must_be_completed) {
        const tl::core::QuestDefinition* quest = quest_book.find(name);
        if (quest == nullptr) continue;
        if (quest_log.complete_quest(*quest)) {
            TL_LOG_INFO("floor quests: '%s' completed", quest->name.c_str());
        }
    }
    for (const std::string& name : stratum.must_be_completed_or_active) {
        const tl::core::QuestDefinition* quest = quest_book.find(name);
        if (quest == nullptr) continue;
        const tl::core::QuestStatus status = quest_log.state().status(quest->name);
        if (status == tl::core::QuestStatus::Complete || status == tl::core::QuestStatus::Active) {
            continue;
        }
        // Given with no NPC, so `CQuest +0x1c` keeps the constructor's -1:
        // `initializeQuestWithNPC` rolls a seed only for a non-null NPC.
        // `getDialog` then draws without reseeding `g_Rand`.
        const uint32_t seed = static_cast<uint32_t>(-1);
        if (!quest_log.accept(*quest, quest_level_of(*quest), seed, stat_graphs, -1,
                              /*resolve_units=*/false)) {
            continue;
        }
        if (tl::core::QuestEntry* held = quest_log.find(quest->name))
            held->dialog_interacted = true;
        TL_LOG_INFO("floor quests: '%s' given", quest->name.c_str());
    }
}

// `CTriggerUnit::setUnitModelName` ->
// `unitInit` for each `Unit Trigger` whose
// `MODEL` a unit file under `media/units/props/` names: its `MAXSTATES` and
// `LOOPTYPE`, and the `resetTrigger` it ends with.
void World::init_unit_triggers(tl::core::LevelScript& machine) {
    for (const tl::core::LevelScript::UnitTriggerView& view : machine.every_unit_trigger()) {
        if (view.unit.empty()) continue;
        TriggerProp prop;
        trigger_prop(view.unit, prop);
        if (!prop.found) continue;
        machine.init_unit_trigger(view.id, prop.max_states, prop.loop_type);
    }
}

void World::log_puzzles(const tl::core::LevelScript& script) {
    for (const tl::core::LevelScript::NodeRecord& n : script.nodes_of_type(46)) {
        const uint32_t count = script.puzzle_active_inputs(n.node);
        std::string order;
        for (uint32_t i = 0; i < count; ++i) {
            order += (i == 0 ? "" : ", ") + std::to_string(script.puzzle_order_at(n.node, i) + 1);
        }
        TL_LOG_INFO("  puzzle '%s': %u input(s), to be pulled in the order %s", n.name.c_str(),
                    count, order.c_str());
    }
}

// `CGameClient::storedUnitExists(long long)`: the travel
// record's state of this level (`CPlayer::getLevelSavedState`) holds a
// character of that object id.
bool World::level_stores_unit(int64_t id) {
    tl::core::TravelRecord* travel = config.travel;
    if (travel == nullptr || level_dungeon.empty()) return false;
    const tl::formats::LevelSaveState* state =
        tl::core::get_level_saved_state(*travel, level_dungeon, level_depth);
    if (state == nullptr) return false;
    for (const tl::formats::CharacterSaveState& saved : state->characters) {
        if (saved.object_id == id) return true;
    }
    return false;
}

void World::activate_room_machine(size_t index, const std::string& path) {
    NestedScript& machine = nested_scripts[index];
    std::vector<tl::core::ScriptEvent> events;
    machine.script.set_has_saved_state(level_has_saved_state);
    machine.script.set_stored_unit_exists([this](int64_t id) { return level_stores_unit(id); });
    machine.script.set_activation_listener(sound_listener);
    machine.script.level_activated(quest_log.state(), events);
    if (machine.owner == 0) {
        TL_LOG_INFO("  room script '%s' armed (no owning link): %zu event(s) on activation",
                    path.c_str(), events.size());
    } else if (!events.empty()) {
        TL_LOG_INFO("  linked machine '%s' activated with the level: %zu event(s)",
                    machine.owner_name.c_str(), events.size());
    }
    if (!events.empty()) apply_script_events(events, machine.nodes);
}

// `CEditorScene::loadCompressedLayout`: every Group to
// `rollRandomGroups`, on `g_Rand`; returns the nodes not created, the groups and what is under them.
std::set<int64_t> World::roll_layout_groups(const tl::formats::CmpScene& layout,
                                            std::vector<int64_t>* created) {
    std::set<int64_t> gated;
    std::map<int64_t, const tl::formats::CmpNode*> by_id;
    for (const tl::formats::CmpNode& n : layout.nodes) by_id.emplace(n.id, &n);

    namespace group_field = tl::formats::cmp_field::group;
    std::string class_upper = player_class;
    for (char& c : class_upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    std::vector<tl::core::LayoutGroup> groups;
    for (const tl::formats::CmpNode& n : layout.nodes) {
        if (n.type != tl::formats::cmp_type::kGroup) continue;
        tl::core::LayoutGroup g;
        g.id = n.id;
        g.parent_id = n.parent_id;
        // CRandomGroup::canGroupBeCreatedBasedOffOfPlayerClass:
        // no CLASS, or CGameClient::getPlayerClassName() equal as
        // authored - the upper-cased NAME,
        // so only an upper-case CLASS matches.
        const tl::formats::CmpField* class_field = n.field(group_field::kClass);
        const std::string group_class =
            class_field != nullptr ? class_field->as_text() : std::string();
        const bool class_ok = group_class.empty() || group_class == class_upper;
        // `CRandomGroup::canGroupBeCreatedBasedOffOfDungeon`: a
        // `DUNGEON` (`+0x188`), on a level that has a dungeon name (`CLevel
        // +0x280`), must be that name exactly.
        const tl::formats::CmpField* dungeon_field = n.field(group_field::kDungeon);
        const std::string group_dungeon =
            dungeon_field != nullptr ? dungeon_field->as_text() : std::string();
        const bool dungeon_ok =
            group_dungeon.empty() || level_dungeon.empty() || group_dungeon == level_dungeon;
        // CRandomGroup::canGroupBeCreatedBasedOffOfDifficulty: a DIFFICULTY
        // MODE (+0x190) below 4 must be the game's (+0x38ec);
        // CRandomGroupDescriptor::getDifficultyIDByString:
        // EASY..VERY HARD are 0..3, anything else (the empty name) 0xe.
        const tl::formats::CmpField* mode_field = n.field(group_field::kDifficultyMode);
        const std::string mode_name = mode_field != nullptr ? mode_field->as_text() : std::string();
        const uint32_t group_mode = tl::core::difficulty_id_by_string(mode_name);
        const bool difficulty_ok =
            !(group_mode < 4u) || group_mode == static_cast<uint32_t>(config.difficulty);
        const bool quest_ok = tl::core::group_is_open(n, quest_log.state());
        g.can_be_created = class_ok && dungeon_ok && difficulty_ok && quest_ok;
        if (!g.can_be_created) {
            TL_LOG_DEBUG("  group '%s' not created:%s%s%s%s", n.name.c_str(),
                         class_ok ? "" : " class", dungeon_ok ? "" : " dungeon",
                         difficulty_ok ? "" : " difficulty", quest_ok ? "" : " quest");
        }
        if (const tl::formats::CmpField* f = n.field(group_field::kChoice)) {
            g.random_type = tl::core::random_group_type_from_string(f->as_text());
        }
        if (const tl::formats::CmpField* f = n.field(group_field::kNumber)) {
            g.number_of_picks = f->as_uint();
        }
        if (const tl::formats::CmpField* f = n.field(group_field::kRandomisation)) {
            g.random_weight = f->as_uint();
        }
        groups.push_back(g);
    }
    gated = tl::core::roll_random_groups(groups, tl::core::game_random(), created);

    if (!gated.empty()) {
        size_t hidden_by_quest = 0;
        for (const tl::formats::CmpNode& n : layout.nodes) {
            int64_t at = n.parent_id;
            for (int depth = 0; depth < 16 && at != -1; ++depth) {
                if (gated.count(at)) {
                    gated.insert(n.id);
                    ++hidden_by_quest;
                    break;
                }
                const auto it = by_id.find(at);
                if (it == by_id.end()) break;
                at = it->second->parent_id;
            }
        }
        TL_LOG_INFO("  %zu node(s) held back by %zu group(s) not created (gates or random choice)",
                    hidden_by_quest, gated.size() - hidden_by_quest);
    }
    return gated;
}

// `CLayout::loadLayoutFile` for a file the World does
// not place as it is loaded: counted and seeded as `place_layout` is, its groups rolled,
// its links loaded in turn. A particle link goes through `CParticlePreloader::LoadParticle`,
// whose own `CLayout` neither counts nor seeds.
bool World::count_layout_load(const std::string& reference) {
    std::string path = resolve_archive_path(reference);
    if (path.empty()) path = resolve_archive_path(reference + ".cmp");
    if (path.empty()) return false;
    const tl::formats::CmpScene* const scene = layout_file(path);
    if (scene == nullptr || !scene->ok) return false;
    if (!layouts_in_progress.insert(path).second) return false;
    if (layout_stack_depth == 0) layout_child_count = 0;
    ++layout_stack_depth;
    if (layout_seeding) {
        ++layout_child_count;
        tl::core::game_random().set_seed(static_cast<int32_t>(
            layout_stack_depth + layout_child_count + static_cast<uint32_t>(layout_level_seed)));
        TL_LOG_DEBUG("layout load counted: %s stack %u counter %u", reference.c_str(),
                     layout_stack_depth - 1, layout_child_count);
    }
    const std::set<int64_t> gated = roll_layout_groups(*scene);
    // `CEditorScene::GetObjectsCreatedByADescriptor(L"Timeline")`.
    bool timeline = false;
    for (const tl::formats::CmpNode& node : scene->nodes) {
        if (gated.count(node.id) != 0) continue;
        if (node.type == tl::formats::cmp_type::kTimeline) timeline = true;
        if (node.type != tl::formats::cmp_type::kLayoutLink &&
            node.type != tl::formats::cmp_type::kTimelineLink) {
            continue;
        }
        const std::string link = node.reference(".LAYOUT");
        if (!link.empty()) count_layout_load(link);
    }
    --layout_stack_depth;
    layouts_in_progress.erase(path);
    return timeline;
}

// `CBaseUnit::unitInit` makes each `[SKILL]` of a prop
// (`CSkill::CSkill`), its effects rolled as made.
void World::roll_prop_skills(const std::string& unit) {
    ensure_prop_index();
    const auto found = prop_files.find(lowered(unit));
    if (found != prop_files.end()) {
        const tl::formats::AdmFile file = read_unit_file(found->second);
        if (file.ok) {
            ensure_player_skill_files();
            ensure_effect_data();
            load_stat_graphs();
            const std::vector<tl::core::LevelScript::UnitSkill> skills = tl::core::read_unit_skills(
                file, [this](const std::string& upper_name) -> const tl::core::SkillDef* {
                    const auto skill = player_skill_files.find(upper_name);
                    return skill == player_skill_files.end() ? nullptr : &skill->second->def;
                });
            tl::core::SkillConstruction with;
            with.types = &effect_types;
            with.unit_types = &unit_types;
            with.affixes = &affixes;
            with.rolls.graphs = &stat_graphs;
            with.rolls.rng = &tl::core::volatile_random();
            with.game_rng = &tl::core::game_random();
            with.layout_adds_sample = [this](const std::string& event_file) {
                ensure_sound_index();
                const tl::formats::CmpScene* scene = layout_file(event_file);
                return scene != nullptr && tl::core::layout_adds_sound_sample(*scene, sound_info);
            };
            with.load_event_file = [this](const std::string& event_file) {
                count_layout_load(event_file);
            };
            for (const tl::core::LevelScript::UnitSkill& block : skills) {
                if (block.known.def != nullptr)
                    tl::core::roll_skill_construction(*block.known.def, with);
            }
        }
    }
}

// `CTriggerUnit::unitInit`: `CItem::unitInit` ->
// `CBaseUnit::unitInit` makes each `[SKILL]` (`CSkill::CSkill`),
// then the `LAYOUTFILE` is loaded into a kind-0 `CLayout`, then each
// `LAYOUT_STATE_n` into a kind-2 one; a state layout that made no
// `Timeline` is made again as kind 1, which the preloader loads.
void World::trigger_unit_loads(const std::string& unit, const TriggerProp& prop,
                               bool skills_rolled) {
    if (!skills_rolled) roll_prop_skills(unit);
    const std::string own = prop_layout_path(unit);
    if (!own.empty()) count_layout_load(own);
    // `CTriggerUnit::addSoundsToState`, twice: a sound found is a
    // `CSoundBank::addSample`, which leaves `g_Rand` at its low word.
    ensure_sound_index();
    for (const std::string& sound : prop.state_sound) {
        if (sound.empty() || sound_info.find(upper_ascii(sound)) == nullptr) continue;
        tl::core::game_random().set_seed(
            static_cast<int32_t>(static_cast<uint32_t>(tl::core::game_random().state())));
    }
    for (const std::string& state : prop.state_layout) {
        if (!state.empty()) count_layout_load(state);
    }
}

// `CParticlePreloader::LoadParticle`: a `CParticle`'s file is
// loaded the first time its upper-cased path is asked for in the run, into the preloader's own
// `CLayout` (no reseed, no count); every later `CParticle` of it copies that load's systems. What
// the load kept out: the groups it rolled, and every object but a `Particle` (with what hangs
// from it), a `Sound` and a `Camera Shake` (`DeleteAllObjectsFromScene`). Null when the file
// cannot be read. `added_sample`: this call loaded it, and a `Sound` of it named a sound.
const std::set<int64_t>* World::load_particle_file(const std::string& path, bool* added_sample) {
    if (added_sample != nullptr) *added_sample = false;
    std::map<std::string, std::set<int64_t>>& loaded = loaded_particle_files();
    const std::string key = backdrop_key(path);
    if (const auto found = loaded.find(key); found != loaded.end()) return &found->second;
    const tl::formats::CmpScene* const layout = layout_file(path);
    if (layout == nullptr || !layout->ok) return nullptr;

    const std::set<int64_t> rolled = roll_layout_groups(*layout);
    std::set<int64_t>& kept_out = loaded.emplace(key, rolled).first->second;
    std::map<int64_t, const tl::formats::CmpNode*> by_id;
    for (const tl::formats::CmpNode& n : layout->nodes) by_id.emplace(n.id, &n);
    size_t dropped = 0;
    for (const tl::formats::CmpNode& n : layout->nodes) {
        namespace type = tl::formats::cmp_type;
        if (n.type == type::kParticle || n.type == type::kSound || n.type == type::kCameraShake ||
            n.type == type::kGroup) {
            continue;
        }
        bool under_particle = false;
        int64_t at = n.parent_id;
        for (int up = 0; up < 16 && at != -1 && !under_particle; ++up) {
            const auto it = by_id.find(at);
            if (it == by_id.end()) break;
            under_particle = it->second->type == type::kParticle;
            at = it->second->parent_id;
        }
        if (under_particle) continue;
        if (kept_out.insert(n.id).second) ++dropped;
    }
    if (dropped > 0) {
        TL_LOG_INFO(
            "  a particle's file: %zu node(s) not kept (only Particle, Sound and Camera Shake are)",
            dropped);
    }

    // Its objects are all made before the filter deletes them, in file order: a `Sound` that
    // names a sound adds the sample (`CSoundBank::addSample`, `g_Rand` left at
    // its low word), and a particle link loads its own file through the preloader.
    ensure_sound_index();
    for (const tl::formats::CmpNode& node : layout->nodes) {
        if (rolled.count(node.id) != 0) continue;
        if (node.type == tl::formats::cmp_type::kSound) {
            const tl::formats::CmpField* f = node.field(tl::formats::cmp_field::sound::kGuid);
            const std::string text = f != nullptr ? f->as_text() : std::string();
            const int64_t guid = text.empty() ? -1 : std::strtoll(text.c_str(), nullptr, 10);
            if (guid != -1 && sound_info.find_by_guid(guid) != nullptr) {
                tl::core::game_random().set_seed(
                    static_cast<int32_t>(static_cast<uint32_t>(tl::core::game_random().state())));
                if (added_sample != nullptr) *added_sample = true;
            }
        } else if (node.type == tl::formats::cmp_type::kParticleLink) {
            const std::string inner = layout_archive_path(node.reference(".LAYOUT"));
            bool inner_added = false;
            if (!inner.empty()) load_particle_file(inner, &inner_added);
            if (inner_added && added_sample != nullptr) *added_sample = true;
        }
    }
    return &kept_out;
}

// `CLevel::loadRoomLayout`, cases 8 and 9, after the bake:
// `positionUpdated`, then `setEnabled(isEnabled())`, whose `updatePathNodes`
// lays an enabled `Path Node Occupation` box or circle on the map. Kept in node order.
void World::collect_path_blockers(const tl::formats::CmpScene& layout,
                                  const std::set<int64_t>& gated, const ogre::Vector3& origin,
                                  const ogre::Quaternion& basis) {
    namespace f = tl::formats::cmp_field::property_node;
    for (const tl::formats::CmpNode& node : layout.nodes) {
        if (node.type != tl::formats::cmp_type::kMarker || !node.has_position()) continue;
        if (gated.count(node.id) != 0) continue;
        const tl::formats::CmpField* kind = node.field(f::kType);
        const std::string text = kind != nullptr ? kind->as_text() : std::string();
        const bool circle = text == "Path Node Occupation Circle";
        if (!circle && text != "Path Node Occupation Box") continue;
        const tl::formats::CmpField* enabled = node.field(f::kEnabled);
        if (enabled != nullptr && enabled->count() == 1 && !enabled->as_bool()) continue;
        const auto number = [&node](size_t index) {
            const tl::formats::CmpField* field = node.field(index);
            return field != nullptr && field->count() == 1 ? field->as_float() : 0.0f;
        };
        ogre::Vector3 position;
        ogre::Quaternion orientation;
        node_place(node, origin, basis, position, orientation);
        PathBlocker b;
        b.circle = circle;
        b.centre = {position.x, position.y, position.z};
        const float width = number(f::kBoxWidth);
        const float length = number(f::kBoxLength);
        b.scale_x = width != 0.0f ? width : number(f::kRadius);
        const ogre::Vector3 x = orientation.xAxis();
        const ogre::Vector3 y = orientation.yAxis();
        const ogre::Vector3 z = orientation.zAxis();
        const tl::core::NoSpawnRegion box = tl::core::property_node_bounds(
            b.centre, {x.x, x.y, x.z}, {y.x, y.y, y.z}, {z.x, z.y, z.z}, b.scale_x,
            length != 0.0f ? length : 1.0f);
        b.low = box.min;
        b.high = box.max;
        loaded_path_blockers.push_back(b);
    }
}

// The ruleset's LEVELPARTICLE layout (a Sound and a Layout Link Particle), placed once:
// `CGameClient::loadLevel` loads it after `CLevel::populate`
// (`CLayout::loadLayoutFile`), keeps it (+0x1080), and `updateIngame`
// puts it at the player every frame.
void World::place_level_particle(
    const std::string& level_particle_ref,
    std::vector<std::pair<std::string, ogre::Vector3>>& placed_pieces) {
    if (level_particle_ref.empty()) return;
    std::string level_particle_path = resolve_archive_path(level_particle_ref);
    if (level_particle_path.empty()) {
        level_particle_path = resolve_archive_path(level_particle_ref + ".cmp");
    }
    const size_t systems_before = view_particle_mark();
    if (!level_particle_path.empty() &&
        place_layout(level_particle_path, ogre::Vector3::ZERO, ogre::Quaternion::IDENTITY,
                     /*depth=*/1)) {
        placed_pieces.emplace_back(level_particle_path, ogre::Vector3::ZERO);
        const size_t riding = view_level_particle_node(systems_before);
        TL_LOG_INFO("  LEVELPARTICLE '%s': %zu system node(s) follow the player",
                    level_particle_ref.c_str(), riding);
    } else {
        TL_LOG_WARN("  LEVELPARTICLE '%s' named but could not be placed",
                    level_particle_ref.c_str());
    }
}

// The rooms' activation `CLevel::populate` ends with (`setActivated`, room order), then
// `CTriggerUnit::setActiveInLevel(true)` of their triggers: the collision
// onto the map grid after every population test, and the route grid again after.
void World::activate_deferred_rooms() {
    defer_room_activation = false;
    for (const auto& [index, path] : deferred_room_machines) activate_room_machine(index, path);
    deferred_room_machines.clear();
    if (!door_trigger_nodes.empty()) {
        register_door_avoidance();
        if (passability.ready()) nav.build(passability);
    }
}

// `CLevel::loadRoomLayout` after `bakeCollision`: the loaded layouts'
// enabled path blockers onto the map, before the population.
void World::lay_loaded_path_blockers() {
    for (const PathBlocker& b : loaded_path_blockers) set_path_blocker_cells(b, true);
    path_blockers_laid = !loaded_path_blockers.empty();
}

bool World::place_layout(const std::string& reference, const ogre::Vector3& origin,
                         const ogre::Quaternion& basis, int depth, uint32_t load_seed_arg) {
    // A nested layout may reference an ancestor: the set catches a cycle, the depth limit
    // a long chain. Also whether a layout link placed this file.
    struct LinkGuard {
        bool* slot;
        bool previous;
        ~LinkGuard() { *slot = previous; }
    } link_guard{&placing_under_link, placing_under_link};
    placing_under_link = std::exchange(link_places_next_layout, false);
    // A `CParticle`'s file: `CParticlePreloader::LoadParticle`.
    //
    const bool as_cparticle = std::exchange(cparticle_places_next_layout, false);

    constexpr int kMaxDepth = 8;
    if (depth > kMaxDepth) {
        TL_LOG_WARN("layout nesting deeper than %d, stopping at '%s'", kMaxDepth,
                    reference.c_str());
        return false;
    }

    // A layout the port made itself (`place_populated_trigger`) is in
    // `layout_files` under its own name, not in the archive.
    const bool made_here = reference.rfind("populate:", 0) == 0;
    std::string path = made_here ? reference : resolve_archive_path(reference);
    if (path.empty()) path = resolve_archive_path(reference + ".cmp");
    if (path.empty()) {
        TL_LOG_WARN("no layout '%s' in the archive", reference.c_str());
        return false;
    }

    if (!layouts_in_progress.insert(path).second) {
        TL_LOG_WARN("layout '%s' references itself; not recursing", path.c_str());
        return false;
    }

    const tl::formats::CmpScene* const parsed = layout_file(path);
    if (parsed == nullptr) {
        layouts_in_progress.erase(path);
        return false;
    }

    const tl::formats::CmpScene& layout = *parsed;
    if (!layout.ok) {
        TL_LOG_ERROR("could not parse '%s': %s", path.c_str(), layout.error.c_str());
        layouts_in_progress.erase(path);
        return false;
    }

    // resyncs is how much of the file this parser did not understand. Logged
    // rather than swallowed: a silently skipped subtree is missing geometry
    // that looks exactly like geometry the level never had.
    TL_LOG_INFO("layout %s: %zu nodes, %zu resync(es)", path.c_str(), layout.nodes.size(),
                layout.resyncs);

    // `CLayout::loadLayoutFile`: the load
    // stack, and the reseed a dungeon room piece gets. The child counter
    // restarts when an outermost load begins; while `layout_seeding` is set
    // each load bumps it and seeds `g_Rand` with its argument + stack depth +
    // counter + the level seed. A room piece passes `1 << index`, a nested
    // `Layout Link` 0 (`CLayoutDescriptor::Set_loadLayoutFile`).
    if (layout_stack_depth == 0) layout_child_count = 0;
    ++layout_stack_depth;
    struct LoadStackPop {
        uint32_t& count;
        ~LoadStackPop() { --count; }
    } load_stack_pop{layout_stack_depth};
    if (rng_trace_file != nullptr) {
        // `--rng-trace`: which file this load is, beside the original's `L` lines
        // from the gdb trace of the original (depth there is the count before the push).
        std::fprintf(rng_trace_file, "L %s depth=%u child=%u seeded=%d\n", reference.c_str(),
                     layout_stack_depth - 1, layout_child_count + (layout_seeding ? 1u : 0u),
                     layout_seeding ? 1 : 0);
        std::fflush(rng_trace_file);
    }
    if (layout_seeding) {
        ++layout_child_count;
        tl::core::game_random().set_seed(
            static_cast<int32_t>(load_seed_arg + layout_stack_depth + layout_child_count +
                                 static_cast<uint32_t>(layout_level_seed)));
        TL_LOG_DEBUG("layout load counted: %s stack %u counter %u", reference.c_str(),
                     layout_stack_depth - 1, layout_child_count);
    }

    if (depth == 0) {
        // Only the rim light here: the lighting acts on the scenery placed below, so it
        // stays at the end.
        read_rim_light(path);
    }

    // Quest-gated subtrees, skipped: a Group naming a quest in one of five fields holds
    // its subtree back, evaluated against the quest state (src/core/quest.h). The random
    // choice among child groups: CEditorScene::loadCompressedLayout
    // hands every Group to rollRandomGroups, which drops failed gates
    // (CRandomGroup::getGroupCanBeCreated; core/random_groups.h).
    // A `CParticle`'s file is the preloader's one load of it (`load_particle_file`).
    std::set<int64_t> gated;
    std::vector<int64_t> created_groups;
    if (as_cparticle) {
        if (const std::set<int64_t>* kept_out = load_particle_file(path)) gated = *kept_out;
    } else {
        gated = roll_layout_groups(layout, &created_groups);
    }

    // The layout's Animation Controller nodes, by the unit (`MONSTER`) they drive; they
    // have no position, so the walk below would not see them. START ON LOAD starts at once.
    std::map<std::string, std::string> unit_animations;
    for (const tl::formats::CmpNode& n : layout.nodes) {
        if (n.type != tl::formats::cmp_type::kAnimationController) continue;
        if (gated.count(n.id)) continue;
        namespace controller = tl::formats::cmp_field::animation_controller;

        const tl::formats::CmpField* start = n.field(controller::kStartOnLoad);
        if (!start || start->count() != 1 || !start->as_bool()) continue;

        const tl::formats::CmpField* unit = n.field(controller::kUnit);
        const tl::formats::CmpField* animation = n.field(controller::kAnimation);
        if (!unit || !animation) continue;

        const std::string unit_name = unit->as_text();
        const std::string animation_name = animation->as_text();
        if (unit_name.empty() || animation_name.empty()) continue;

        unit_animations[unit_name] = animation_name;
        TL_LOG_INFO("  animation controller '%s': %s plays '%s' on load", n.name.c_str(),
                    unit_name.c_str(), animation_name.c_str());
    }

    // The units this layout places, read before anything is placed: a `Monster`
    // that authors no `SCALE` of its own takes the one in its unit `.DAT`, and
    // that decision is made while the mesh is being placed.
    if (depth == 0) {
        load_unit_stats(layout, gated);
    } else {
        // A room's own Monsters are added to the level's book, not replacing it (a
        // generated floor has no top-level file).
        for (const tl::formats::CmpNode& node : layout.nodes) {
            if (node.type != tl::formats::cmp_type::kMonster) continue;
            if (gated.count(node.id) != 0) continue;
            const tl::formats::CmpField* field =
                node.field(tl::formats::cmp_field::monster::kMonster);
            if (field == nullptr || field->as_text().empty()) continue;
            if (!index_unit(field->as_text())) {
                TL_LOG_WARN("  unit '%s' in %s has no .DAT this engine could read",
                            field->as_text().c_str(), path.c_str());
            }
        }
    }

    // A linked layout with a script is a machine in a file, owning everything it places;
    // each instance gets its own node map (NestedScript). Saved and restored, because a
    // linked layout may link another.
    NodeMap* outer_sink = node_sink;
    size_t nested_script_index = nested_scripts.size();
    // A room needs a machine even with nothing wired when it places a Unit Trigger
    // (CTriggerUnit::interact() needs one), a Monster (one of the level's characters,
    // CLevel +0x98), a Unit Spawner (SPAWN ON CREATE:
    // CUnitSpawnerDescriptor::descriptorSceneActivated,
    // here level_activated) or a Sound (CSoundObjectDescriptor, core/sound_object.h).
    bool owns_a_trigger = false;
    bool owns_a_monster = false;
    bool owns_a_spawner = false;
    bool owns_a_sound = false;
    if (depth > 0) {
        for (const tl::formats::CmpNode& n : layout.nodes) {
            if (n.type == tl::formats::cmp_type::kUnitTrigger) owns_a_trigger = true;
            if (n.type == tl::formats::cmp_type::kMonster) owns_a_monster = true;
            if (n.type == tl::formats::cmp_type::kUnitSpawner) owns_a_spawner = true;
            if (n.type == tl::formats::cmp_type::kSound) owns_a_sound = true;
        }
    }
    const bool owns_a_script = depth > 0 && !as_cparticle &&
                               (!layout.logic_groups.empty() || !layout.timelines.empty() ||
                                owns_a_trigger || owns_a_monster || owns_a_spawner || owns_a_sound);
    if (owns_a_script) {
        NestedScript machine;
        machine.reference = path;
        machine.owner = nested_link;
        machine.turn_link = turn_link;
        machine.owner_name = nested_link_name;
        machine.owner_nodes = outer_sink != nullptr ? outer_sink : &scripted_nodes;
        // The file's placement: `origin + basis * local`, the same transform
        // place_mesh() gives every scene node of it.
        {
            tl::core::LayoutFrame frame;
            const ogre::Vector3 x = basis * ogre::Vector3::UNIT_X;
            const ogre::Vector3 y = basis * ogre::Vector3::UNIT_Y;
            const ogre::Vector3 z = basis * ogre::Vector3::UNIT_Z;
            frame.origin = {origin.x, origin.y, origin.z};
            frame.x = {x.x, x.y, x.z};
            frame.y = {y.x, y.y, y.z};
            frame.z = {z.x, z.y, z.z};
            machine.script.set_frame(frame);
        }
        machine.scene = placing_scene;
        machine.hierarchy = placing_hierarchy;
        nested_scripts.push_back(std::move(machine));
        node_sink = &nested_scripts.back().nodes;
        level_machines.push_back(&nested_scripts.back().script);
        // Wired before the file's objects are made: its Monsters' units are made with it as
        // their nodes are (below).
        nested_scripts.back().script.set_units(&unit_book);
        wire_script(nested_scripts.back().script);
    }
    struct SinkGuard {
        NodeMap** slot;
        NodeMap* previous;
        ~SinkGuard() { *slot = previous; }
    } sink_guard{&node_sink, outer_sink};
    // The links of a file with a machine are made before its machine is; they are
    // activated after it, as before the links were made in the file's order.
    const size_t deferred_at = deferred_room_machines.size();
    const bool outer_defer = defer_room_activation;
    if (owns_a_script) defer_room_activation = true;

    std::vector<MeshPlacement> mesh_placements;
    std::vector<MeshPlacement> collision_placements;
    // A room piece whose variant is rolled after the file is loaded: its placements, kept in
    // the file's order, wait for the mesh.
    struct PendingVariant {
        size_t mesh = 0;
        size_t collision = std::string::npos;
        const std::vector<std::string>* variants = nullptr;
        const std::vector<std::string>* collisions = nullptr;
        int64_t guid = 0;
        size_t file_index = 0;  ///< the node's place in the file: the walk's order
    };
    std::vector<PendingVariant> pending_variants;
    struct NestedLayout {
        std::string reference;
        ogre::Vector3 origin;
        ogre::Quaternion basis;
        int64_t owner = 0;

        /// The link node itself, and what it is called. A linked layout with a
        /// script of its own belongs to this node: the parent's `Play` on it is
        /// what runs the machine inside. See NestedScript.
        int64_t link = 0;
        std::string link_name;

        /// A `Layout Link Particle`: the original loads its file through
        /// `CParticlePreloader::LoadParticle`,
        /// on the preloader's own `CLayout`,
        /// not as a nested load of the level's layout.
        bool particle = false;

        /// A particle link authored `START ON LOAD: false`:
        /// `CLayoutDescriptor::descriptorSceneActivated` calls
        /// `CLayout::stop(false)` on it, so its systems wait for `Start Particle`.
        bool stopped = false;

        /// A `Layout Link Timeline` left at `START ON LOAD` (its default):
        /// the same `descriptorSceneActivated` calls `CLayout::start` on it,
        /// `callFunctionOnObjects(0)`, which
        /// plays each of the linked layout's `LAYOUT CONTROL` timelines from
        /// the beginning.
        bool starts = false;

        /// A `Layout Link Timeline` authored `START ON LOAD: false`: the same
        /// call is `CLayout::stop(false)` on it. See the `held` branch below.
        bool held = false;

        /// A `Layout Link Timeline`'s `DURATION MODIFY` (`CLayout +0x1b4`, 0
        /// from the constructor).
        float duration_modify = 0.0f;
    };
    std::vector<NestedLayout> nested_layouts;
    size_t nested = 0;
    size_t particles = 0;
    size_t skipped_room_pieces = 0;
    std::vector<int64_t> skipped_room_guids;

    // `CDescriptorProp::setData` -> `CLayoutDescriptor::Set_loadLayoutFile`:
    // a link's file is loaded as the link is made, in the order the
    // file makes its objects, before the next object.
    const auto place_nested = [&](const NestedLayout& nested_layout) {
        const int64_t outer = particle_owner;
        const int64_t outer_link = nested_link;
        const std::string outer_link_name = nested_link_name;
        const int64_t outer_turn_link = turn_link;
        if (owns_a_script) turn_link = nested_layout.link;
        particle_owner = nested_layout.owner;
        nested_link = nested_layout.link;
        nested_link_name = nested_layout.link_name;
        // A particle layout does not reseed: entered through
        // CLayoutDescriptor::Set_loadLayoutFile, CLayout::loadLayoutFile neither reseeds
        // nor bumps the child count, and CParticlePreloader::LoadParticle loads the file.
        const bool outer_seeding = layout_seeding;
        if (nested_layout.particle) layout_seeding = false;
        const bool outer_particle_file = placing_particle_file;
        if (nested_layout.particle) placing_particle_file = true;
        link_places_next_layout = true;
        // A particle link's `CLayout` is of kind 1 (`Set_CLayoutDescriptor
        // KLAYOUT_TYPE_PARTICLEParam2`), so its file
        // is a `CParticle` once particles are cached; before that (the title's
        // menu) it is a live layout of its own.
        const bool link_cparticle = nested_layout.particle && view_caching_particles();
        cparticle_places_next_layout = link_cparticle;
        const int64_t outer_hierarchy = placing_hierarchy;
        placing_hierarchy |= nested_layout.link;
        const size_t systems_before = view_particle_mark();
        const size_t machines_before = nested_scripts.size();
        if (place_layout(nested_layout.reference, nested_layout.origin, nested_layout.basis,
                         depth + 1)) {
            ++nested;
        }
        cparticle_places_next_layout = false;
        // `CLayout::setDurationModification`: the link's `DURATION
        // MODIFY`, set once its file is loaded and before anything starts it.
        if (0.0f < nested_layout.duration_modify) {
            for (size_t i = machines_before; i < nested_scripts.size(); ++i) {
                NestedScript& machine = nested_scripts[i];
                if (machine.owner != nested_layout.link) continue;
                machine.script.set_duration_modification(nested_layout.duration_modify);
            }
        }
        if (nested_layout.starts) {
            // CLayoutDescriptor::descriptorSceneActivated: then CLayout::start
            // on the machine this link just made, not every machine sharing its id.
            for (size_t i = machines_before; i < nested_scripts.size(); ++i) {
                NestedScript& machine = nested_scripts[i];
                if (machine.owner != nested_layout.link) continue;
                std::vector<tl::core::ScriptEvent> inner;
                machine.running = true;
                machine.script.play_layout_timelines(quest_log.state(), inner, /*backwards=*/false);
                if (!inner.empty()) apply_script_events(inner, machine.nodes);
                TL_LOG_INFO("  linked machine '%s' starts on load: %zu event(s)",
                            machine.owner_name.c_str(), inner.size());
            }
        } else if (nested_layout.held) {
            // CLayout::initOnLoad(): a link that does not
            // start on load takes START ON LOAD off every timeline of its layout.
            for (size_t i = machines_before; i < nested_scripts.size(); ++i) {
                NestedScript& machine = nested_scripts[i];
                if (machine.owner != nested_layout.link) continue;
                machine.script.init_on_load();
            }
        }
        // One `Layout Link Particle` is one `CParticle` over the whole file.
        // Its systems learn their link, start or wait, as a `CParticle` (the renderer's).
        view_settle_link_particles(
            systems_before, nested_layout.owner, nested_layout.reference, nested_layout.origin,
            nested_layout.basis,
            LinkParticleSettle{nested_layout.particle, link_cparticle, nested_layout.stopped});
        placing_hierarchy = outer_hierarchy;
        layout_seeding = outer_seeding;
        placing_particle_file = outer_particle_file;
        particle_owner = outer;
        nested_link = outer_link;
        nested_link_name = outer_link_name;
        turn_link = outer_turn_link;
    };

    size_t hidden = 0;
    // `Unit Trigger`s whose unit names a `LAYOUTFILE`, placed once the nodes
    // are: see below.
    struct TriggerOwnLayout {
        int64_t node = 0;
        std::string path;
        ogre::Vector3 position;
        ogre::Quaternion orientation;
    };
    std::vector<TriggerOwnLayout> trigger_own_layouts;
    ensure_sound_index();
    // `CRoomPiece::vfunc_2`: a piece with a Group above it in its own file whose
    // `CHOICE` is not ALL (`CRandomGroup::m_eRandomType`, +0x108) is cast again after the bake,
    // a linked layout's pieces as a room's own.
    std::map<int64_t, const tl::formats::CmpNode*> node_by_id;
    for (const tl::formats::CmpNode& n : layout.nodes) node_by_id.emplace(n.id, &n);
    const auto under_random_group = [&node_by_id](const tl::formats::CmpNode& piece) {
        int64_t at = piece.parent_id;
        for (int up = 0; up < 64 && at != -1; ++up) {
            const auto it = node_by_id.find(at);
            if (it == node_by_id.end()) return false;
            const tl::formats::CmpNode& n = *it->second;
            if (n.type == tl::formats::cmp_type::kGroup) {
                const tl::formats::CmpField* f = n.field(tl::formats::cmp_field::group::kChoice);
                if (f != nullptr && tl::core::random_group_type_from_string(f->as_text()) != 0)
                    return true;
            }
            at = n.parent_id;
        }
        return false;
    };
    collect_path_blockers(layout, gated, origin, basis);
    // `CEditorScene::loadCompressedLayout` makes the Groups, then the objects
    // under none, then each made group's objects (a node's nearest Group ancestor) in the order
    // `rollRandomGroups` left the groups, each group's in the file's order.
    std::vector<const tl::formats::CmpNode*> made_order;
    {
        std::map<int64_t, const tl::formats::CmpNode*> node_of;
        for (const tl::formats::CmpNode& n : layout.nodes) node_of.emplace(n.id, &n);
        std::map<int64_t, size_t> group_rank;
        for (size_t i = 0; i < created_groups.size(); ++i) group_rank.emplace(created_groups[i], i);
        const auto rank_of = [&](const tl::formats::CmpNode& n) -> size_t {
            if (n.type == tl::formats::cmp_type::kGroup) return 0;
            int64_t at = n.parent_id;
            for (size_t guard = 0; at != -1 && guard < layout.nodes.size(); ++guard) {
                const auto it = node_of.find(at);
                if (it == node_of.end()) break;
                if (it->second->type == tl::formats::cmp_type::kGroup) {
                    const auto made = group_rank.find(at);
                    return made == group_rank.end() ? group_rank.size() + 2 : made->second + 2;
                }
                at = it->second->parent_id;
            }
            return 1;
        };
        std::vector<std::pair<size_t, const tl::formats::CmpNode*>> keyed;
        keyed.reserve(layout.nodes.size());
        for (const tl::formats::CmpNode& n : layout.nodes) keyed.emplace_back(rank_of(n), &n);
        std::stable_sort(keyed.begin(), keyed.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& [rank, n] : keyed) made_order.push_back(n);
    }
    for (const tl::formats::CmpNode* made_node : made_order) {
        const tl::formats::CmpNode& node = *made_node;
        // `CSoundObject::setSoundBankGuid`, as the object is made: a GUID
        // that names a sound is a `CSoundBank::addSample`, which leaves
        // `g_Rand` at its low word.
        if (node.type == tl::formats::cmp_type::kSound && gated.count(node.id) == 0 &&
            !as_cparticle) {
            if (const tl::formats::CmpField* f = node.field(tl::formats::cmp_field::sound::kGuid)) {
                const std::string text = f->as_text();
                const int64_t guid = text.empty() ? -1 : std::strtoll(text.c_str(), nullptr, 10);
                if (guid != -1 && sound_info.find_by_guid(guid) != nullptr) {
                    tl::core::game_random().set_seed(static_cast<int32_t>(
                        static_cast<uint32_t>(tl::core::game_random().state())));
                }
            }
        }
        if (!node.has_position()) continue;
        if (gated.count(node.id)) continue;

        // `CMonsterDescriptor::Set_createNewCharacter`: a Monster's unit is
        // made, its level set, as its node is - in the file's creation pass, after the links
        // made before it and before the walk rolls the pieces.
        if (node.type == tl::formats::cmp_type::kMonster && owns_a_script) {
            const tl::formats::CmpField* who =
                node.field(tl::formats::cmp_field::monster::kMonster);
            if (who != nullptr && !who->as_text().empty()) {
                tl::core::LevelScript& machine = nested_scripts[nested_script_index].script;
                machine.preset_unit_creation(node.id, machine.roll_unit_creation(who->as_text()));
            }
        }

        // Field 0 is VISIBLE on every descriptor; a hidden node is placed hidden
        // (MeshPlacement::start_hidden) so a later Show finds it. A Monster's VISIBLE is
        // field 12 (LevelScript::build reads it there too).
        bool node_start_hidden = false;
        const size_t visible_field = node.type == tl::formats::cmp_type::kMonster
                                         ? tl::formats::cmp_field::monster::kVisible
                                         : tl::formats::cmp_field::linked::kVisible;
        if (const tl::formats::CmpField* visible = node.field(visible_field)) {
            if (visible->count() == 1 && !visible->as_bool()) {
                ++hidden;
                node_start_hidden = true;
            }
        }

        // A node's position and basis are in its layout's frame: the offset is rotated
        // before it is added, and the rotations compose.
        ogre::Vector3 position;
        ogre::Quaternion orientation;
        node_place(node, origin, basis, position, orientation);
        position = derived_position(node_by_id, node, origin, basis);

        // `CLevel::loadRoomLayout`: a
        // `Property Node` typed `Quest Item` (12) joins `+0x108`, `Quest Boss`
        // (13) joins `+0xf0`.
        if (node.type == tl::formats::cmp_type::kMarker) {
            if (const tl::formats::CmpField* kind =
                    node.field(tl::formats::cmp_field::property_node::kType)) {
                const std::string text = kind->as_text();
                // CLevel::loadRoomLayout, cases 4 and 5:
                // automap tile 0x14 or 0x15, facing (1, 0, -1), shown once explored.
                if (automap_initialized && (text == "Entrance" || text == "Exit")) {
                    view_automap_add_tile(text == "Entrance" ? 0x14 : 0x15, position,
                                          ogre::Vector3(1.0f, 0.0f, -1.0f), false, false);
                    TL_LOG_INFO("automap: %s marker at (%.2f, %.2f)", text.c_str(),
                                static_cast<double>(position.x), static_cast<double>(position.z));
                }
                if (text == "Quest Item") quest_item_markers.push_back({position, orientation});
                if (text == "Quest Boss") quest_boss_markers.push_back({position, orientation});
                // `Town Portal` (7) is `+0x14c` and `Waypoint Start` (15) `+0x158`,
                // whichever was met last.
                if (text == "Town Portal") {
                    town_portal_spot = position;
                    has_town_portal_spot = true;
                }
                if (text == "Waypoint Start") waypoint_spot = position;
                // Entry type 6's start: case 14 (`Waypoint`, the waygate's own
                // node) and case 15 alike set `+0x140`, the last one met.
                if (text == "Waypoint" || text == "Waypoint Start") {
                    waypoint_entry_spot = position;
                    has_waypoint_entry_spot = true;
                }
                // `No Spawn Region` (3) joins `+0xd8` after `positionUpdated`
                // has put its box through `updateBoundingBox`, in the
                // level's frame: the node's world position and the columns of its
                // world orientation's matrix. Scale as `apply_path_blocker` reads it.
                if (text == "No Spawn Region") {
                    namespace f = tl::formats::cmp_field::property_node;
                    const auto number = [&node](size_t index) {
                        const tl::formats::CmpField* field = node.field(index);
                        return field != nullptr && field->count() == 1 ? field->as_float() : 0.0f;
                    };
                    const float width = number(f::kBoxWidth);
                    const float length = number(f::kBoxLength);
                    const ogre::Vector3 x = orientation.xAxis();
                    const ogre::Vector3 y = orientation.yAxis();
                    const ogre::Vector3 z = orientation.zAxis();
                    no_spawn_regions.push_back(tl::core::property_node_bounds(
                        {position.x, position.y, position.z}, {x.x, x.y, x.z}, {y.x, y.y, y.z},
                        {z.x, z.y, z.z}, width != 0.0f ? width : number(f::kRadius),
                        length != 0.0f ? length : 1.0f));
                }
            }
        }
        // ... and every `Warper` with a `DUNGEON` (`+0x110`, non-empty): tile
        // 0x15, or 0x22 for one marked `WAYPOINT` (`+0x109`), at the warper
        // (the loop over `GetObjectsCreatedByADescriptor("Warper")`).
        if (automap_initialized && node.type == tl::formats::cmp_type::kWarper) {
            namespace w = tl::formats::cmp_field::warper;
            const tl::formats::CmpField* dungeon = node.field(w::kDungeonName);
            if (dungeon != nullptr && !dungeon->as_text().empty()) {
                const tl::formats::CmpField* waypoint = node.field(w::kWaypoint);
                const bool is_waypoint =
                    waypoint != nullptr && waypoint->count() == 1 && waypoint->as_bool();
                view_automap_add_tile(is_waypoint ? 0x22 : 0x15, position,
                                      ogre::Vector3(1.0f, 0.0f, -1.0f), false, false);
                TL_LOG_INFO("automap: warper to '%s' marked at (%.2f, %.2f)",
                            dungeon->as_text().c_str(), static_cast<double>(position.x),
                            static_cast<double>(position.z));
            }
        }

        if (node.type == tl::formats::cmp_type::kItem) {
            place_layout_item(node, position);
            continue;
        }

        // A prop's authored scale: Generic Model fields 6-8, Room Piece 11-13, default 1.
        // A Monster's unit may carry a scale the layout does not repeat.
        float unit_scale = 1.0f;
        if (node.type == tl::formats::cmp_type::kMonster) {
            namespace mf = tl::formats::cmp_field::monster;
            if (const tl::formats::CmpField* who = node.field(mf::kMonster)) {
                unit_scale = stats_for_unit(who->as_text()).scale;
            }
        }
        const ogre::Vector3 node_scale = tl::core::layout_scale(node, unit_scale);

        // A `Particle`'s MESH is its entity renderer's (`CParticleTechWrapper::
        // setModelPath`), not a model of the
        // layout's.
        std::string mesh_ref =
            node.type == tl::formats::cmp_type::kParticle ? std::string() : node.reference(".MESH");

        // A Unit Trigger names a unit, not a mesh: its geometry is that unit's
        // (trigger_prop()).
        TriggerProp trigger;
        std::string trigger_unit;
        ogre::Vector3 trigger_scale = node_scale;
        if (mesh_ref.empty() && node.type == tl::formats::cmp_type::kUnitTrigger) {
            if (const tl::formats::CmpField* model =
                    node.field(tl::formats::cmp_field::unit_trigger::kModel)) {
                trigger_unit = model->as_text();
            }
            const bool has_model = !trigger_unit.empty() && trigger_prop(trigger_unit, trigger);
            if (!trigger_unit.empty()) {
                const std::string own = prop_layout_path(trigger_unit);
                if (!own.empty()) {
                    trigger_own_layouts.push_back({node.id, own, position, orientation});
                }
            }
            if (!trigger_unit.empty()) {
                // `CBaseUnit::unitInit`: the
                // unit's own scale, `SCALE + randomBetweenVolatile(0,
                // SCALE_VARIATION)`, drawn as it is made - here, or by
                // `populate` for a prop it made (`made_trigger_scale`).
                const auto drawn = made_trigger_scale.find(node.id);
                trigger_scale *= drawn != made_trigger_scale.end()
                                     ? drawn->second
                                     : trigger.scale + tl::core::volatile_random().between(
                                                           0.0f, trigger.scale_variation);
                // Then its `[SKILL]`s (a chest's traps), their effects rolled.
            }
            if (!trigger_unit.empty() && trigger.found)
                trigger_unit_loads(trigger_unit, trigger, made_trigger_scale.count(node.id) != 0);
            if (has_model) {
                mesh_ref = trigger.mesh;
            } else {
                // Nothing to draw, but a unit with a collision model still
                // blocks (`CTriggerUnit::getUnitCollisionModel`) until
                // a `Not Collidable` takes it out.
                if (!trigger_unit.empty() && !trigger.collision.empty() && trigger.collideable) {
                    const std::string collision_path = resolve_archive_path(trigger.collision);
                    if (collision_path.empty()) {
                        TL_LOG_WARN("  '%s' names collision '%s', not in the archive",
                                    trigger_unit.c_str(), trigger.collision.c_str());
                    } else {
                        MeshPlacement collision_placement;
                        collision_placement.mesh = collision_path;
                        collision_placement.node_id = node.id;
                        collision_placement.position = position;
                        collision_placement.orientation = orientation;
                        collision_placement.scale = trigger_scale;
                        collision_placement.collision_group = instance_key(
                            node.id, node_sink != nullptr ? *node_sink : scripted_nodes);
                        collision_placement.collision_blocks = trigger.block;
                        if (trigger.occupies_nodes) {
                            door_trigger_nodes.push_back(collision_placement.collision_group);
                        }
                        collision_placements.push_back(std::move(collision_placement));
                        TL_LOG_INFO("  unit trigger '%s' blocks with %s at (%.2f, %.2f, %.2f)",
                                    trigger_unit.c_str(), collision_path.c_str(),
                                    static_cast<double>(position.x),
                                    static_cast<double>(position.y),
                                    static_cast<double>(position.z));
                    }
                }
                trigger_unit.clear();
            }
        }

        if (!mesh_ref.empty()) {
            const std::string mesh_path = resolve_archive_path(mesh_ref);
            if (mesh_path.empty()) {
                TL_LOG_WARN("  no '%s' in the archive", mesh_ref.c_str());
                continue;
            }

            MeshPlacement placement;
            placement.mesh = mesh_path;
            placement.node_id = node.id;
            placement.position = position;
            placement.orientation = orientation;
            placement.scale = trigger_unit.empty() ? node_scale : trigger_scale;
            placement.start_hidden = node_start_hidden;
            // A Room Piece is made by CRoomPieceDescriptor::CreateObject,
            // a Generic Model by CGenericModelDescriptor::CreateObject,
            // constructor defaults before the properties.
            placement.room_piece = node.type == tl::formats::cmp_type::kRoomPiece;

            // The trigger prop's resting state and collision, authored on the unit; `unit`
            // registers it in scene_units so its Triggered reaches it.
            if (!trigger_unit.empty()) {
                placement.unit = trigger_unit;
                // The looping key when the unit authors one, otherwise the
                // state animation *once* - see TriggerProp. Otherwise a door
                // is left looping its `closing` animation.
                placement.animation =
                    trigger.resting_loop.empty() ? trigger.resting : trigger.resting_loop;
                placement.loops = !trigger.resting_loop.empty();
                placement.animation_rate = 1.0f;
                placement.trigger_resting_layout = trigger.state_layout[0];
                if (!trigger.collision.empty() && trigger.collideable) {
                    const std::string collision_path = resolve_archive_path(trigger.collision);
                    if (collision_path.empty()) {
                        TL_LOG_WARN("  '%s' names collision '%s', not in the archive",
                                    trigger_unit.c_str(), trigger.collision.c_str());
                    } else {
                        MeshPlacement collision_placement = placement;
                        collision_placement.mesh = collision_path;
                        collision_placement.animation.clear();
                        collision_placement.trigger_resting_layout.clear();
                        // Tagged with the node so Triggered can take it out again; one
                        // group per placed copy (instance_key()).
                        collision_placement.collision_group = instance_key(
                            node.id, node_sink != nullptr ? *node_sink : scripted_nodes);
                        collision_placement.collision_blocks = trigger.block;
                        // A map grid candidate once passability.bake() has run
                        // (door_trigger_nodes); not one authoring OCCUPIESNODES:False
                        // (CBaseUnit::addToAvoidanceMap, +0x18f).
                        if (trigger.occupies_nodes) {
                            door_trigger_nodes.push_back(collision_placement.collision_group);
                        }
                        collision_placements.push_back(std::move(collision_placement));
                    }
                }
                TL_LOG_INFO("  unit trigger '%s' draws %s at (%.2f, %.2f, %.2f)%s",
                            trigger_unit.c_str(), mesh_path.c_str(),
                            static_cast<double>(position.x), static_cast<double>(position.y),
                            static_cast<double>(position.z),
                            trigger.resting.empty() ? "" : ", resting state authored");
            }

            // What the layout asks this thing to play: Monster and Generic Model carry
            // ANIMATION, LOOPS and SPEED (play_scene_animation()).
            {
                size_t animation_index = 0, loops_index = 0, speed_index = 0, unit_index = 0;
                if (node.type == tl::formats::cmp_type::kMonster) {
                    namespace monster = tl::formats::cmp_field::monster;
                    // Somebody, not something: see `MeshPlacement::is_unit`.
                    placement.is_unit = true;
                    animation_index = monster::kAnimation;
                    loops_index = monster::kLoops;
                    speed_index = monster::kSpeed;
                    unit_index = monster::kMonster;
                } else if (node.type == tl::formats::cmp_type::kModel) {
                    namespace model = tl::formats::cmp_field::model;
                    animation_index = model::kAnimation;
                    loops_index = model::kLoops;
                    speed_index = model::kSpeed;
                }
                if (animation_index != 0) {
                    if (const tl::formats::CmpField* f = node.field(animation_index)) {
                        placement.animation = f->as_text();
                    }
                    if (const tl::formats::CmpField* f = node.field(loops_index)) {
                        if (f->count() == 1) placement.loops = f->as_bool();
                    }
                    if (const tl::formats::CmpField* f = node.field(speed_index)) {
                        if (f->count() == 1) {
                            const float value = f->as_float();
                            // A Monster's: CCharacter::setAnimationSpeed
                            // (X360 inline in the SPEED setter) keeps it as
                            // authored; ANIMATION, SPEED, LOOPS give playAnimation(index,
                            // LOOPS, SPEED) (ModelAnimator::set_animation_*), here by name.
                            if (node.type == tl::formats::cmp_type::kMonster ||
                                (value > 0.0f && value <= 100.0f)) {
                                placement.animation_rate = value;
                            }
                        }
                    }
                    if (unit_index != 0) {
                        if (const tl::formats::CmpField* f = node.field(unit_index)) {
                            placement.unit = f->as_text();
                        }
                    }
                    // A controller elsewhere in the layout wins: it is the later,
                    // more specific instruction, and it is how a monster placed
                    // in its bind pose is told to lie down dead.
                    const auto controlled = unit_animations.find(placement.unit);
                    if (!placement.unit.empty() && controlled != unit_animations.end()) {
                        placement.animation = controlled->second;
                    }
                }

                // INVENTED: a Monster with nothing else to play (or `bind`) plays its
                // IDLE: no unit data names an animation, the meshes ship an idle, and the
                // engine holds the name.
                if (node.type == tl::formats::cmp_type::kMonster &&
                    names_the_bind_pose(placement.animation)) {
                    placement.animation = kUnitIdleAnimation;
                    placement.idle_fallback = true;
                    placement.loops = true;
                    placement.animation_rate = 1.0f;
                }
            }

            // A Light is a pool drawn into the light map only. CLight:
            // scale (1, 0.25, 1), Y held at 0.25,
            // CLight::setRotation by ANGLE, setBitmapFile
            // the FILE mesh. CLevel::bake adds it
            // to static geometry on queue 0x5f once per LIGHT DENSITY (+0x100).
            // (CLightDescriptor::descriptorSceneActivated;
            // updateLightDensity is editor-only.)
            // CLightDescriptor::CreateObject;
            // CLight::CLight gives the fallbacks below.
            if (node.type == tl::formats::cmp_type::kLight) {
                using namespace tl::formats::cmp_field::light;
                float scale_x = 1.0f;
                float scale_z = 1.0f;
                float angle = 0.0f;
                if (const tl::formats::CmpField* f = node.field(kScaleX)) {
                    if (f->count() == 1) scale_x = f->as_float();
                }
                if (const tl::formats::CmpField* f = node.field(kScaleZ)) {
                    if (f->count() == 1) scale_z = f->as_float();
                }
                if (const tl::formats::CmpField* f = node.field(kAngle)) {
                    if (f->count() == 1) angle = f->as_float();
                }
                placement.light_density = 1;
                if (const tl::formats::CmpField* f = node.field(kBrightness)) {
                    if (f->count() == 1) placement.light_density = f->as_uint();
                }
                placement.scale = ogre::Vector3(scale_x, 0.25f, scale_z);
                placement.orientation =
                    basis * ogre::Quaternion(ogre::Degree(angle), ogre::Vector3::UNIT_Y);
                TL_LOG_INFO(
                    "  light %s at (%.2f, %.2f) scale (%.2f, %.2f) angle %.0f "
                    "density %u",
                    mesh_path.c_str(), position.x, position.z, scale_x, scale_z,
                    static_cast<double>(angle), placement.light_density);
            }

            mesh_placements.push_back(std::move(placement));
            continue;
        }

        // The four `Layout Link` kinds: `CLayoutDescriptor::CreateObject`
        // makes a `CLayout` of the
        // descriptor's own variant (`+0x170`).
        const std::string layout_ref = node.reference(".LAYOUT");
        if (!layout_ref.empty()) {
            const bool particle_link = node.type == tl::formats::cmp_type::kParticleLink;
            bool stopped = false;
            if (particle_link) {
                const tl::formats::CmpField* start =
                    node.field(tl::formats::cmp_field::particle_link::kStartOnLoad);
                stopped = start != nullptr && start->count() == 1 && !start->as_bool();
            }
            nested_layouts.push_back(NestedLayout{layout_ref, position, orientation,
                                                  particle_link ? node.id : particle_owner, node.id,
                                                  node.name, particle_link, stopped});
            if (node.type == tl::formats::cmp_type::kTimelineLink) {
                // `CLayout::setStartOnLoad`:
                // `+0x1a0`, 1 from the constructor; the rest of it runs in the
                // editor alone.
                const tl::formats::CmpField* start =
                    node.field(tl::formats::cmp_field::timeline_link::kStartOnLoad);
                nested_layouts.back().starts =
                    start == nullptr || start->count() != 1 || start->as_bool();
                nested_layouts.back().held = !nested_layouts.back().starts;
                const tl::formats::CmpField* modify =
                    node.field(tl::formats::cmp_field::timeline_link::kDurationModify);
                if (modify != nullptr && modify->count() == 1) {
                    nested_layouts.back().duration_modify = modify->as_float();
                }
            }
            place_nested(nested_layouts.back());
            continue;
        }

        if (node.type == tl::formats::cmp_type::kParticle) {
            if (config.no_particles) continue;
            placing_layout_path = path;
            if (place_particle(layout, node, position, orientation)) ++particles;
            continue;
        }

        if (node.type == tl::formats::cmp_type::kRoomPiece) {
            // A room piece names a 64-bit GUID as a decimal string (field 8), and
            // in field 9 (`PIECE`) either the word RANDOM or the variant index the
            // level designer picked. The GUID is looked up in the tileset index.
            load_tileset();

            int64_t guid = 0;
            bool have_guid = false;
            for (const std::string& text : node.strings) {
                if (text.empty()) continue;
                const bool numeric = (text[0] == '-' || (text[0] >= '0' && text[0] <= '9')) &&
                                     text.find_first_not_of("-0123456789", 1) == std::string::npos;
                if (!numeric) continue;
                errno = 0;
                char* end = nullptr;
                const long long value = std::strtoll(text.c_str(), &end, 10);
                if (end && *end == '\0' && errno == 0) {
                    guid = static_cast<int64_t>(value);
                    have_guid = true;
                    break;
                }
            }

            // GUID -1 names no piece (two of Necropolis 1's): nothing to place.
            // `CRoomPiece::setRoomPieceGuid`
            // asks for no record and makes nothing of it.
            if (have_guid && guid == -1) continue;
            // Any other GUID the index lacks is drawn as the ERROR piece:
            // CRoomPieceDataInformation::_GetRoomPieceCoreDataByGuid
            // answers the default.
            if (have_guid && tileset_known.count(guid) == 0 && tileset_default_guid != 0) {
                TL_LOG_WARN("  room piece %lld is not in the index; drawn as ERROR",
                            static_cast<long long>(guid));
                guid = tileset_default_guid;
            }
            auto piece = have_guid ? tileset.find(guid) : tileset.end();
            if (piece == tileset.end()) {
                // A piece with COLLISIONFILE and no FILE (ParseRoomPieceData
                // fills the two lists apart): no variant to roll; its collision
                // is the first listed.
                const auto only_collision =
                    have_guid ? tileset_collision.find(guid) : tileset_collision.end();
                if (only_collision == tileset_collision.end()) {
                    ++skipped_room_pieces;
                    if (skipped_room_guids.size() < 4) skipped_room_guids.push_back(guid);
                    continue;
                }
                // Its `initRoomPiece` in the walk still draws: no mesh is a count of 0, neither
                // 1 nor above the variant, so `randomIntegerBetween(0, -1)`.
                pending_variants.push_back({0, std::string::npos, nullptr, nullptr, guid,
                                            static_cast<size_t>(made_node - layout.nodes.data())});
                bool collides = true;
                if (const tl::formats::CmpField* enabled =
                        node.field(tl::formats::cmp_field::room_piece::kCollisionEnabled)) {
                    if (enabled->count() == 1) collides = enabled->as_bool();
                }
                if (!collides) continue;
                const std::string& collision_file = only_collision->second.front();
                const std::string collision_path = resolve_archive_path(collision_file);
                if (collision_path.empty()) {
                    TL_LOG_WARN("  room piece %lld names collision '%s', not in the archive",
                                static_cast<long long>(guid), collision_file.c_str());
                    ++collision_skipped;
                    continue;
                }
                MeshPlacement collision_placement;
                collision_placement.mesh = collision_path;
                collision_placement.node_id = node.id;
                collision_placement.position = position;
                collision_placement.orientation = orientation;
                collision_placement.scale = node_scale;
                collision_placement.start_hidden = node_start_hidden;
                collision_placement.has_mesh = false;
                add_room_piece_collision(*this, collision_placements,
                                         std::move(collision_placement), depth, false);
                continue;
            }

            // CRoomPiece::initRoomPiece: the variant
            // (+0x138) is PIECE, or 0xffffffff for RANDOM; one not below the count becomes 0,
            // and with several meshes it is rolled, randomIntegerBetween(0, count - 1) on
            // g_Rand - after the file is loaded (below, `pending_variants`).
            const std::vector<std::string>& variants = piece->second;
            // `CRoomPiece::CRoomPiece`: no
            // variant (`+0x138` -1, `RANDOM`), baked (`+0x150`), colliding
            // (`+0x153`), on the map (`+0x155`), visible - each fallback here
            // and below.
            uint32_t variant = 0xffffffffu;
            if (const tl::formats::CmpField* piece_field =
                    node.field(tl::formats::cmp_field::room_piece::kVisual)) {
                const std::string authored = piece_field->as_text();
                if (!authored.empty() &&
                    authored.find_first_not_of("0123456789") == std::string::npos) {
                    variant = static_cast<uint32_t>(std::strtoul(authored.c_str(), nullptr, 10));
                }
            }
            const uint32_t variant_count = static_cast<uint32_t>(variants.size());
            const bool rolled = variant_count <= variant && variant_count != 1;
            if (variant_count <= variant) variant = 0;
            const std::string& variant_file = variants[variant];
            const std::string mesh_path =
                rolled ? std::string() : resolve_archive_path(variant_file);
            if (!rolled && mesh_path.empty()) {
                TL_LOG_WARN("  room piece %lld names '%s', which is not in the archive",
                            static_cast<long long>(guid), variant_file.c_str());
                ++skipped_room_pieces;
                continue;
            }
            // Room-piece geometry is authored in world coordinates; the node position is
            // still applied (a small designer offset).
            MeshPlacement piece_placement;
            piece_placement.mesh = mesh_path;
            piece_placement.node_id = node.id;
            piece_placement.position = position;
            piece_placement.orientation = orientation;
            piece_placement.scale = node_scale;
            piece_placement.start_hidden = node_start_hidden;

            // The piece's collision at the same transform; COLLISION ENABLED is field 5,
            // default true.
            bool collides = true;
            if (const tl::formats::CmpField* enabled =
                    node.field(tl::formats::cmp_field::room_piece::kCollisionEnabled)) {
                if (enabled->count() == 1) collides = enabled->as_bool();
            }

            // The collision variant, as `initRoomPiece` picks it: the rolled index
            // when the record lists as many collision meshes as meshes, the first
            // one otherwise.
            const auto collision_entry = tileset_collision.find(guid);
            size_t collision_index = std::string::npos;
            if (collides && collision_entry != tileset_collision.end()) {
                const std::vector<std::string>& collisions = collision_entry->second;
                const std::string& collision_file =
                    collisions.size() == variants.size() ? collisions[variant] : collisions.front();
                const std::string collision_path = resolve_archive_path(collision_file);
                if (rolled && collisions.size() == variants.size()) {
                    collision_index = collision_placements.size();
                    add_room_piece_collision(*this, collision_placements, piece_placement, depth,
                                             under_random_group(node));
                } else if (collision_path.empty()) {
                    TL_LOG_WARN("  room piece %lld names collision '%s', not in the archive",
                                static_cast<long long>(guid), collision_file.c_str());
                    ++collision_skipped;
                } else {
                    MeshPlacement collision_placement = piece_placement;
                    collision_placement.mesh = collision_path;
                    // A piece with collision is one of `populate`'s sections.
                    add_room_piece_collision(*this, collision_placements,
                                             std::move(collision_placement), depth,
                                             under_random_group(node));
                }
            }

            piece_placement.room_piece = true;
            if (const tl::formats::CmpField* bake =
                    node.field(tl::formats::cmp_field::room_piece::kBake);
                bake != nullptr && bake->count() == 1) {
                piece_placement.bake = bake->as_bool();
            }
            if (const auto phase = tileset_render_phase.find(guid);
                phase != tileset_render_phase.end()) {
                piece_placement.render_phase = phase->second;
            }
            if (rolled) {
                pending_variants.push_back(
                    {mesh_placements.size(), collision_index, &variants,
                     collision_index != std::string::npos ? &collision_entry->second : nullptr,
                     guid, static_cast<size_t>(made_node - layout.nodes.data())});
            }
            mesh_placements.push_back(std::move(piece_placement));
        }
    }

    // `CEditorScene::loadCompressedLayout` ends with a walk over the scene's
    // objects in the order they were made, `CRoomPieceDescriptor::DescriptorObjectHasBeenInited`
    // on each room piece: `initRoomPiece` rolls there, after every link of the
    // file is loaded. While a layout loads, `setRoomPieceGuid` does not.
    {
        std::vector<size_t> unplaced;
        std::stable_sort(pending_variants.begin(), pending_variants.end(),
                         [](const PendingVariant& a, const PendingVariant& b) {
                             return a.file_index < b.file_index;
                         });
        for (const PendingVariant& pending : pending_variants) {
            if (pending.variants == nullptr) {
                tl::core::game_random().integer_between(0, -1);
                continue;
            }
            const std::vector<std::string>& variants = *pending.variants;
            const auto variant = static_cast<uint32_t>(
                tl::core::game_random().integer_between(0, static_cast<int>(variants.size()) - 1));
            const std::string mesh_path = resolve_archive_path(variants[variant]);
            if (mesh_path.empty()) {
                TL_LOG_WARN("  room piece %lld names '%s', which is not in the archive",
                            static_cast<long long>(pending.guid), variants[variant].c_str());
                ++skipped_room_pieces;
                unplaced.push_back(pending.mesh);
            } else {
                mesh_placements[pending.mesh].mesh = mesh_path;
            }
            if (pending.collision != std::string::npos) {
                const std::string& collision_file = (*pending.collisions)[variant];
                const std::string collision_path = resolve_archive_path(collision_file);
                if (collision_path.empty()) {
                    TL_LOG_WARN("  room piece %lld names collision '%s', not in the archive",
                                static_cast<long long>(pending.guid), collision_file.c_str());
                    ++collision_skipped;
                }
                collision_placements[pending.collision].mesh = collision_path;
            }
        }
        std::sort(unplaced.begin(), unplaced.end());
        for (auto it = unplaced.rbegin(); it != unplaced.rend(); ++it) {
            mesh_placements.erase(mesh_placements.begin() + static_cast<std::ptrdiff_t>(*it));
        }
        collision_placements.erase(
            std::remove_if(collision_placements.begin(), collision_placements.end(),
                           [](const MeshPlacement& c) { return c.mesh.empty(); }),
            collision_placements.end());
    }

    // The meshes made and stood in the scene, their animations started (the renderer's).
    size_t animated_props = 0;
    size_t idled = 0;
    view_place_meshes(mesh_placements, animated_props, idled);
    const size_t meshes = mesh_placements.size();

    // Collision meshes are loaded for their triangles only, never placed in the scene.
    for (const auto& placement : collision_placements) place_collision(*this, placement);

    // CTriggerUnit::unitInit: a LAYOUTFILE
    // gives a kind-0 CLayout (+0x240, X360 +0x1e8) under the trigger, shown, started; its
    // systems are the trigger's (CTriggerUnit::setVisible decides a Hide).
    if (!trigger_own_layouts.empty()) {
        // Counted as the unit was made (`trigger_unit_loads`); placing it here does not count.
        const bool outer_seeding = layout_seeding;
        layout_seeding = false;
        NodeMap& sink = (node_sink != nullptr ? *node_sink : scripted_nodes);
        for (const TriggerOwnLayout& own : trigger_own_layouts) {
            const size_t before = view_particle_mark();
            place_particle_layout(own.path, own.position, own.orientation, nullptr,
                                  ParticleStart::Live);
            view_own_trigger_particles(before, own.node, sink);
            TL_LOG_INFO("  unit trigger %lld carries its own layout %s (%zu system(s))",
                        static_cast<long long>(own.node), own.path.c_str(),
                        view_particle_mark() - before);
        }
        layout_seeding = outer_seeding;
    }

    // A Group places no mesh, so it is indexed onto its placed descendants' scene nodes:
    // a wire naming the group reaches them. Scoped to this layout's map; a nested link's
    // nodes are in its own NestedScript::nodes, as a wire cannot cross a file.
    {
        NodeMap& sink = (node_sink != nullptr ? *node_sink : scripted_nodes);
        std::map<int64_t, int64_t> parent_of;
        std::set<int64_t> group_ids;
        for (const tl::formats::CmpNode& n : layout.nodes) {
            parent_of.emplace(n.id, n.parent_id);
            if (n.type == tl::formats::cmp_type::kGroup) group_ids.insert(n.id);
        }

        const std::vector<std::pair<int64_t, Ogre::SceneNode*>> leaves(sink.begin(), sink.end());
        size_t group_entries = 0;
        for (const auto& leaf : leaves) {
            int64_t at = leaf.first;
            for (int depth = 0; depth < 16; ++depth) {
                const auto it = parent_of.find(at);
                if (it == parent_of.end() || it->second == -1) break;
                at = it->second;
                if (group_ids.count(at)) {
                    sink.emplace(at, leaf.second);
                    ++group_entries;
                }
            }
        }
        if (group_entries > 0) {
            TL_LOG_INFO(
                "  %zu group-ancestor entr%s added so Hide/Show can address a "
                "Group by id",
                group_entries, group_entries == 1 ? "y" : "ies");
        }
    }

    // What this file's wires and timelines can reach (Group entries included, gated
    // groups too), so bake_static_scenery() leaves it drawn on its own.
    {
        const NodeMap& sink = (node_sink != nullptr ? *node_sink : scripted_nodes);
        std::set<int64_t> addressed;
        for (const tl::formats::CmpLogicGroup& group : layout.logic_groups) {
            for (const tl::formats::CmpLogicObject& member : group.objects) {
                addressed.insert(member.object_id);
            }
        }
        for (const tl::formats::CmpTimeline& timeline : layout.timelines) {
            for (const tl::formats::CmpTimelineTarget& target : timeline.targets) {
                addressed.insert(target.object_id);
            }
        }
        for (const int64_t id : addressed) {
            const auto range = sink.equal_range(id);
            for (auto it = range.first; it != range.second; ++it) {
                scripted_scene_nodes.insert(it->second);
            }
        }
    }

    if (owns_a_script) {
        NestedScript& machine = nested_scripts[nested_script_index];
        machine.script.set_units(&unit_book);
        wire_script(machine.script);
        std::vector<int64_t> made_ids;
        made_ids.reserve(made_order.size());
        for (const tl::formats::CmpNode* n : made_order) made_ids.push_back(n->id);
        machine.script.set_made_order(std::move(made_ids));
        machine.script.build(layout, gated);
        init_unit_triggers(machine.script);
        log_puzzles(machine.script);
        TL_LOG_INFO(
            "  linked machine '%s' from %s: %zu object(s), %zu wire(s), "
            "%zu timeline(s)",
            machine.owner_name.c_str(), path.c_str(), machine.script.object_count(),
            machine.script.wire_count(), machine.script.timeline_count());

        // A machine with no owning Layout Link (nested_link == 0) can never be started
        // from outside (ScriptAction::NestedTimeline), so it activates itself as placed,
        // as a depth-0 layout does.
        if (nested_link == 0) {
            machine.running = true;
            // DIVERGENCE: headless with no pad, a Unit Trigger is answered by walking up
            // to it (enable_proximity_unit_triggers()).
            if (config.headless && !config.pad) {
                machine.script.enable_proximity_unit_triggers(
                    [this](const std::string& unit) { return is_enchant_shrine(unit); });
            }
        } else {
            // Activated with its level: CLayoutDescriptor::descriptorSceneActivated
            // calls CEditorScene::setActivated on
            // every linked layout, and CLayoutDescriptor::update steps each with
            // CLayout::update every frame.
            machine.running = true;
        }
        // A generated floor's rooms are activated by `CLevel::populate`
        // once the level is populated
        // (`defer_room_activation`).
        defer_room_activation = outer_defer;
        deferred_room_machines.emplace(
            deferred_room_machines.begin() + static_cast<std::ptrdiff_t>(deferred_at),
            nested_script_index, path);
        if (!defer_room_activation) {
            const std::vector<std::pair<size_t, std::string>> held(
                deferred_room_machines.begin() + static_cast<std::ptrdiff_t>(deferred_at),
                deferred_room_machines.end());
            deferred_room_machines.resize(deferred_at);
            for (const auto& [index, file] : held) activate_room_machine(index, file);
        }
    }

    if (skipped_room_pieces > 0) {
        std::string guids;
        for (const int64_t guid : skipped_room_guids) {
            guids += (guids.empty() ? "" : ", ") + std::to_string(guid);
        }
        TL_LOG_WARN("  %zu room piece(s) unresolved against the tileset index (%s)",
                    skipped_room_pieces, guids.c_str());
    }
    if (hidden > 0) TL_LOG_INFO("  %zu node(s) hidden by VISIBLE", hidden);
    TL_LOG_INFO(
        "  placed %zu mesh(es), %zu particle system(s), %zu nested layout(s), "
        "%zu animated (%zu of them on the default idle)",
        meshes, particles, nested, animated_props, idled);

    // The level's script (past the last node; CmpScene::logic_groups), reported even
    // when empty.
    if (layout.script_records > 0 || !layout.logic_groups.empty()) {
        TL_LOG_INFO("  script: %zu record(s), %zu logic group(s), %zu timeline(s)%s",
                    layout.script_records, layout.logic_groups.size(), layout.timelines.size(),
                    layout.script_undecoded > 0 ? " (some not decoded)" : "");
        if (layout.script_undecoded > 0) {
            TL_LOG_WARN("  %zu trailing record(s) of a kind this reader does not decode",
                        layout.script_undecoded);
        }
    }

    place_merge_layouts(path, origin, basis, depth);
    if (depth == 0) finish_level_layout(path, layout, gated, origin);

    layouts_in_progress.erase(path);
    return true;
}

// `place_layout` at depth 0, once the whole tree is placed: the player's start, the viewer, the
// collision and the grids, then the level script built and activated.
void World::finish_level_layout(const std::string& path, const tl::formats::CmpScene& layout,
                                const std::set<int64_t>& gated, const ogre::Vector3& origin) {
    // Where the player stands, recorded before the lighting is applied:
    // the light map starts there (aim_light_map()).
    if (const tl::formats::CmpNode* start = find_spawn(layout)) {
        if (start->has_position()) {
            player_start = origin + ogre::Vector3(start->position().x, start->position().y,
                                                  start->position().z);
            have_player_start = true;
        }
    }
    apply_environment(path);
    // The camera and the sky; the subject stood at the layout's Player Start
    // (player_start_yaw its facing).
    view_place_viewer(layout, origin);

    // `CGameClient::loadLevel` ends in
    // `CLevel::updateNPCIcons`; the player is not in control yet, so it
    // runs on the first frame that it is.
    npc_icons_dirty = true;

    if (automap_initialized) {
        update_automap_icons();
        if (have_player_start) view_automap_player(player_start);
    }
    log_scene_census(" as placed");

    // The level script, built once the whole tree is placed so a wire can find what
    // it drives. Top-level only; nested graphs run as their own machines.
    script_layout = layout;
    script_gated = gated;
    initial_quests = quest_log.state();
    script_ready = true;

    // Index the collision soup once the whole tree, nested layouts and all,
    // has contributed to it.
    collision.build();
    if (collision.empty()) {
        TL_LOG_WARN(
            "  no collision geometry: none of this layout's pieces names a "
            "COLLISIONFILE, so nothing will block a character");
    } else {
        TL_LOG_INFO("  collision: %zu face(s) from %zu mesh(es), %zu of them nocollide",
                    collision.triangle_count(), collision_meshes, collision.nocollide_count());
    }

    // Where a character may be routed, after the whole tree has contributed. A level
    // with no generated chunks is cast whole (CLevel::bakeCollision).
    nav.clear();
    passability.clear();
    if (!collision.empty()) {
        const unsigned long nav_started = view_milliseconds();
        passability.bake(collision, nullptr);
        const unsigned long nav_took = view_milliseconds() - nav_started;
        log_passability(passability, static_cast<double>(nav_took) / 1000.0);
        if (!passability_dump_path.empty()) {
            write_passability_dump(passability, passability_dump_path);
        }
        // Before `nav.build()`, which takes a one-time snapshot of the map
        // grid's open cells - a door registered afterwards would be solid
        // in `passability` but walkable in `nav` forever.
        register_door_avoidance();
        nav.build(passability);
        TL_LOG_INFO("  navigation: %zu of %zu cell(s) walkable at %.2f units", nav.walkable_count(),
                    nav.cell_count(), static_cast<double>(nav.cell_size()));
        apply_path_blockers(layout, gated);
        loaded_path_blockers.clear();
    } else {
        door_trigger_nodes.clear();
    }
    // `CLevel::loadRoomLayout`: the map, drawn from the
    // grid just baked (none without one); a town's whole (its town flag).
    view_automap_create();
    // A hunting monster routes through the same grid the player does,
    // and stands on the same floor - see LevelScript::set_collision().
    script.set_navigation(&nav);
    script.set_collision(&collision);

    // The script runs after the navigation grid: its Level Activated wires may switch
    // pathing blockers. Before the build, which makes the layout's characters.
    script.set_equipment_roller(
        [this](const std::string& unit, tl::core::LevelScript::UnitCreation& made) {
            roll_unit_equipment(unit, made);
        });
    script.set_unit_equipment(unit_equipment_hooks());
    // The level's observer map (`CLevel +0x200`) is the level's own.
    unit_observers.clear();
    skill_observers.clear();
    script.build(layout, gated);
    init_unit_triggers(script);
    log_puzzles(script);
    // `CGameClient::loadLevel`: once the level's
    // layout is built, `setSeed(0)` and `setSeedVolatile(0)` - the clock.
    tl::core::reseed_level_generators();
    // A Unit Trigger is acted on with A through the console's second target pass.
    // DIVERGENCE: with nobody holding a
    // pad (headless), walking up to it fires it.
    if (config.headless && !config.pad) {
        script.enable_proximity_unit_triggers(
            [this](const std::string& unit) { return is_enchant_shrine(unit); });
    }

    // And the level's hostile units go looking for the player. Nothing in
    // a layout wires this - a dungeon's monsters carry no wiring at all -
    // because in the original it is what being hostile *means*.
    if (const size_t armed = script.arm_hostiles()) {
        TL_LOG_INFO("  %zu hostile unit(s) will come after the player", armed);
    }
    if (script.wire_count() > 0 || script.trigger_count() > 0 || script.has_sound_nodes()) {
        TL_LOG_INFO(
            "  level script: %zu object(s), %zu wire(s), %zu trigger(s), "
            "%zu timeline(s)",
            script.object_count(), script.wire_count(), script.trigger_count(),
            script.timeline_count());

        std::vector<tl::core::ScriptEvent> events;
        logic_trace_text("# level load\n");
        script.set_has_saved_state(level_has_saved_state);
        script.set_stored_unit_exists([this](int64_t id) { return level_stores_unit(id); });
        script.set_activation_listener(sound_listener);
        script.level_activated(quest_log.state(), events);
        flush_logic_trace(script);
        apply_script_events(events, scripted_nodes);
        if (!events.empty()) {
            TL_LOG_INFO("  level activated: %zu event(s) fired", events.size());
        }
    }

    if (collision_skipped > 0) {
        TL_LOG_WARN("  %zu collision mesh(es) named but not loaded", collision_skipped);
    }

    log_memory("after the layout");
}

void World::place_layout_item(const tl::formats::CmpNode& node, const ogre::Vector3& at) {
    // CEquipment::createNewEquipment via CEquipment::convertEquipment:
    // the ITEMS record of that name, unitInit(data, false) on a fresh
    // CItem. No such item, nothing.
    const tl::formats::CmpField* name = node.field(tl::formats::cmp_field::item::kItem);
    if (name == nullptr || name->as_text().empty()) return;
    ensure_item_index();
    const tl::core::ItemStats* data = items.find(name->as_text());
    if (data == nullptr) {
        TL_LOG_WARN("layout item '%s': no item named '%s'", node.name.c_str(),
                    name->as_text().c_str());
        return;
    }
    const tl::core::ItemStats made = create_item(*data, false, false);
    // `descriptorSceneActivated`: outside the editor, each item is marked as
    // the layout's (`+0x191`) and `CLevel::addItem(item, its position, true)`
    // puts it in the level - for a `CEquipment`, on the ground and tossed from
    // there (`drop_item`).
    TL_LOG_INFO("layout item '%s': %s", node.name.c_str(), name->as_text().c_str());
    drop_item(made, 1, at);
}

/// Find the authored marker inside a placed exit chunk, if it has one: the chunk links
/// a shared prop (EXIT_NODE.LAYOUT or a `_PORTAL_NODE` one) holding a Property Node, a
/// Warper, a Unit Trigger and a Logic Group wiring Triggered to Activate Warper. Only the
/// marker's position and radius are read; one Layout Link deep.
bool World::find_chunk_marker(const std::string& chunk_cmp_path, const ogre::Vector3& chunk_origin,
                              const std::string& wanted_type, ogre::Vector3& world_position,
                              float& radius, ChunkMarkerExtra* extra) {
    const tl::formats::CmpScene* const parsed = layout_file(chunk_cmp_path);
    if (parsed == nullptr || !parsed->ok) return false;
    const tl::formats::CmpScene& scene = *parsed;

    namespace node_field = tl::formats::cmp_field::property_node;
    for (const tl::formats::CmpNode& link : scene.nodes) {
        if (link.type != tl::formats::cmp_type::kLayoutLink) continue;
        const std::string reference = link.reference(".layout");
        if (reference.empty()) continue;
        // The reference names the layout, not the compiled file: resolved in two steps as
        // place_layout() does for a nested Layout Link.
        std::string sub_path = resolve_archive_path(reference);
        if (sub_path.empty()) sub_path = resolve_archive_path(reference + ".cmp");
        if (sub_path.empty()) continue;
        const tl::formats::CmpScene* const parsed_sub = layout_file(sub_path);
        if (parsed_sub == nullptr || !parsed_sub->ok) continue;
        const tl::formats::CmpScene& sub = *parsed_sub;

        const ogre::Vector3 link_local =
            link.has_position()
                ? ogre::Vector3(link.position().x, link.position().y, link.position().z)
                : ogre::Vector3::ZERO;
        const ogre::Quaternion link_turn = node_orientation(link);
        // A node's forward as the level reads it: the z axis of its world
        // orientation, flattened and normalised (`CLevel::loadRoomLayout`
        // ahead of its `Property Node` switch).
        const auto flat_forward = [&link_turn](const ogre::Quaternion& own) {
            ogre::Vector3 forward = (link_turn * own) * ogre::Vector3::UNIT_Z;
            forward.y = 0.0f;
            const float length = forward.length();
            if (1e-08f < length) forward /= length;
            return forward;
        };

        for (const tl::formats::CmpNode& inner : sub.nodes) {
            if (inner.type != tl::formats::cmp_type::kMarker) continue;
            const tl::formats::CmpField* type_field = inner.field(node_field::kType);
            if (type_field == nullptr || lowered(type_field->as_text()) != wanted_type) continue;

            const ogre::Vector3 inner_local =
                inner.has_position()
                    ? ogre::Vector3(inner.position().x, inner.position().y, inner.position().z)
                    : ogre::Vector3::ZERO;
            world_position = chunk_origin + link_local + link_turn * inner_local;
            const tl::formats::CmpField* radius_field = inner.field(node_field::kRadius);
            radius = (radius_field != nullptr && radius_field->count() > 0)
                         ? radius_field->as_float()
                         : 3.0f;
            if (extra != nullptr) {
                extra->forward = flat_forward(node_orientation(inner));
                // The Warper the same layout holds: where placePlayerAtWarpToDungeonFloor
                // stands a player who came through its
                // twin, turned its getForwardAbsolute().
                for (const tl::formats::CmpNode& warper : sub.nodes) {
                    if (warper.type != tl::formats::cmp_type::kWarper) continue;
                    const ogre::Vector3 warper_local =
                        warper.has_position()
                            ? ogre::Vector3(warper.position().x, warper.position().y,
                                            warper.position().z)
                            : ogre::Vector3::ZERO;
                    extra->has_warper = true;
                    extra->warper_position = chunk_origin + link_local + link_turn * warper_local;
                    ogre::Vector3 own_forward = ogre::Vector3::UNIT_Z;
                    if (const tl::formats::CmpField* f =
                            warper.field(tl::formats::cmp_field::warper::kForward)) {
                        if (f->count() == 3) {
                            own_forward = {f->as_float(0), f->as_float(1), f->as_float(2)};
                        }
                    }
                    extra->warper_forward = link_turn * own_forward;
                    break;
                }
            }
            return true;
        }
    }
    return false;
}

bool World::place_dungeon_floor(const std::string& dungeon_name, int floor_number) {
    loaded_path_blockers.clear();
    path_blockers_laid = false;
    // `CDungeonManager::getDungeonByName`:
    // the process's dungeon of that `NAME`, with the salt and the floor
    // templates it has kept.
    tl::core::DungeonManager& manager = loaded_dungeon_manager();
    tl::core::Dungeon* named = manager.dungeon_by_name(dungeon_name);
    if (named == nullptr) {
        // DIVERGENCE (tooling): `--dungeon` has always taken a file of
        // `media/dungeons/` as well; such a name is read for its `NAME`.
        const std::string def_path = archive_path_unique(dungeon_name + ".DAT.adm");
        const std::vector<uint8_t> def_bytes =
            def_path.empty() ? std::vector<uint8_t>() : read_resource(def_path);
        if (!def_bytes.empty()) {
            const tl::core::DungeonDef by_file = tl::core::parse_dungeon_def(
                tl::formats::parse_adm(def_bytes.data(), def_bytes.size()));
            if (by_file.ok) named = manager.dungeon_by_name(by_file.name);
        }
    }
    if (named == nullptr || !named->def.ok) {
        TL_LOG_ERROR("no dungeon called '%s' in the archive", dungeon_name.c_str());
        return false;
    }
    tl::core::Dungeon& dungeon = *named;
    const tl::core::DungeonDef& def = dungeon.def;

    // `CGameClient::setCurrentDungeon`. A
    // warp has set it already (`Renderer::leave_level`); a floor loaded with
    // none before it (`--dungeon`, the Choose Level list) sets it here.
    tl::core::GameSeed& seed = tl::core::game_seed();
    seed.current_dungeon = &dungeon;

    // The first floor is depth 0 (STRATA0).
    const uint32_t depth = static_cast<uint32_t>(std::max(1, floor_number) - 1);
    // Before the level is placed, so its quest gates see what this gives.
    apply_floor_quests(def, depth);

    const tl::core::DungeonFiles dungeon_files = archive_dungeon_files();

    // CGameClient::loadLevel: resetGameSeed(depth) (game
    // seed, the dungeon's salt, depth), then the depth's template, kept by the dungeon;
    // building one draws room pieces from that stream.
    tl::core::RunicRandom& g_rand = tl::core::game_random();
    tl::core::reset_game_seed(seed, static_cast<int32_t>(depth), g_rand);
    const tl::core::DungeonStrata* source = nullptr;
    tl::core::LevelTemplate* found_template =
        dungeon.get_level_template_for_depth(depth, dungeon_files, g_rand, &source);
    if (found_template == nullptr || source == nullptr || found_template->chunks.empty()) {
        TL_LOG_ERROR("dungeon '%s' floor %d: no level template with room pieces", def.name.c_str(),
                     floor_number);
        return false;
    }
    tl::core::LevelTemplate& level_template = *found_template;
    const tl::core::DungeonStrata& strata = *source;
    // The level seed `CLevel::loadRoomLayout` seeds and prints.
    const int32_t level_seed =
        tl::core::level_seed(tl::core::kLevelSeedSetting, seed, static_cast<int32_t>(depth));
    // `CLevel+0x280` is the dungeon's upper-cased `NAME` and `+0x1a4` the depth;
    // the stratum lays `IS_TOWN` and a set `DONTSTORE` onto the template
    // (`CDungeon::getStrataTemplate`).
    set_level_identity(def.name, static_cast<int32_t>(depth), strata.is_town,
                       level_template.dont_store || strata.dont_store, strata.allow_portals);
    level_allows_pet_return = strata.allow_pet_return;
    level_cinematic = level_template.cinematic;
    const std::string& ruleset_path = strata.ruleset;
    // Its rooms are placed at depth 1, below place_layout()'s own reading.
    read_rim_light(ruleset_path);
    const tl::core::LevelTemplate* own_template =
        dungeon.templates[std::min<size_t>(depth, dungeon.templates.size() - 1)].get();
    const std::string redirected =
        own_template != nullptr ? own_template->redirect_dungeon : std::string();
    TL_LOG_INFO("dungeon '%s': floor %d is depth %u of %zu, ruleset '%s'%s%s", def.name.c_str(),
                floor_number, depth, def.depth_strata.size(), ruleset_path.c_str(),
                redirected.empty() ? "" : ", a random floor of ", redirected.c_str());
    TL_LOG_INFO("Seeding: %d", level_seed);

    // `CGameClient::loadLevel` empties a `RANDOMIZED` template's pool before
    // every load, so each visit to a floor generates afresh.
    if (level_template.randomized) level_template.delete_layouts();
    const tl::core::LevelLayout* layout =
        tl::core::choose_room_layout(level_template, g_rand, level_seed);
    if (layout == nullptr || layout->instances.empty()) {
        TL_LOG_ERROR("could not generate a floor for '%s' from '%s'", def.name.c_str(),
                     ruleset_path.c_str());
        return false;
    }
    const int entrance_index = 0;
    int exit_index = -1;
    TL_LOG_INFO("dungeon '%s' floor %d: %zu chunk(s), one of %zu layout(s)", def.name.c_str(),
                floor_number, layout->instances.size(), level_template.layouts.size());
    for (size_t i = 0; i < layout->instances.size(); ++i) {
        const tl::core::ChunkInstance& chunk = layout->instances[i];
        const tl::core::ChunkType& type = level_template.type_at(chunk.type_index);
        if (exit_index < 0 && type.exit) exit_index = static_cast<int>(i);
        TL_LOG_INFO("  chunk %zu: '%s' at (%.2f, %.2f, %.2f), piece '%s'%s%s", i, type.name.c_str(),
                    chunk.x, chunk.y, chunk.z,
                    level_template.chunks[layout->pieces[i]].path.c_str(),
                    static_cast<int>(i) == entrance_index ? " [entrance]" : "",
                    static_cast<int>(i) == exit_index ? " [exit]" : "");
    }

    // CLevel::loadRoomLayout: the start (+0x140) is the
    // Entrance node, facing (+0x164; CCharacter::setToward) its
    // forward, flattened; +Z without one.
    const ogre::Vector3 entrance_facing = ogre::Vector3::UNIT_Z;
    dungeon_entrance_warp_at = dungeon_exit_warp_at = ogre::Vector3::ZERO;
    dungeon_entrance_warp_forward = dungeon_exit_warp_forward = ogre::Vector3::UNIT_Z;

    level_reference = "dungeon:" + def.name;

    // CGameClient::loadLevel: monster level
    // is computed on every load, before loadRoomLayout and before
    // setSeedVolatile(0) - a room's placed monsters are made at it
    // (`CMonsterDescriptor::CreateObject`).
    tl::core::TravelRecord* travel = config.travel;
    const int32_t player_lvl = 0 < arriving_player_level
                                   ? arriving_player_level
                                   : static_cast<int32_t>(player_character.level());
    const bool has_player = travel != nullptr;
    const int32_t floor_rank = tl::core::rank_of_monsters_on_floor(static_cast<int32_t>(depth),
                                                                   &def, travel, player_lvl, true);
    const int32_t floor_monster_level = tl::core::level_monster_level(
        &level_template, floor_rank, has_player, player_lvl, tl::core::volatile_random());
    const bool has_history = travel != nullptr && tl::core::has_dungeon_history(*travel, def.name);
    TL_LOG_INFO("  monster level: %d (rank %d, player level %d%s)", floor_monster_level, floor_rank,
                player_lvl, has_history ? ", history" : "");
    level_monster_level = floor_monster_level;

    size_t placed_chunks = 0;
    std::string exit_chunk_path;
    ogre::Vector3 exit_chunk_origin = ogre::Vector3::ZERO;
    std::string entrance_chunk_path;
    ogre::Vector3 entrance_chunk_origin = ogre::Vector3::ZERO;
    // Every chunk's chosen piece and where it landed, for reading its Sound nodes later.
    std::vector<std::pair<std::string, ogre::Vector3>> placed_pieces;
    // CLevel::loadRoomLayout sets CResourceManager+0x42
    // and passes 1 << index: a piece's groups roll from level seed + (1 << index) + 2.
    // Each load in the piece is counted as it is made, links and units in the file's order.
    // The rooms' machines wait for the population (`activate_room_machine`).
    defer_room_activation = true;
    deferred_room_machines.clear();
    level_chunk_origins.clear();
    collision_sections.clear();
    begin_floor_collision(*this, level_template, *layout);
    layout_seeding = true;
    layout_level_seed = level_seed;
    for (size_t chunk_i = 0; chunk_i < layout->instances.size(); ++chunk_i) {
        const tl::core::ChunkInstance& chunk = layout->instances[chunk_i];
        const std::string& piece = level_template.chunks[layout->pieces[chunk_i]].path;
        const std::string piece_path = resolve_archive_path(piece + ".cmp");
        if (piece_path.empty()) {
            TL_LOG_WARN("  room piece '%s' is not in the archive; leaving a gap", piece.c_str());
            continue;
        }
        const ogre::Vector3 origin(chunk.x, chunk.y, chunk.z);
        level_chunk_origins.push_back(origin);
        // Each placed room is one of `CLevel`'s room layouts, and passability is
        // cast per room over the box its pieces' collision makes.
        collision_sections.emplace_back();
        // `CLevel::bakeCollision` hands `calculatePassability` the room's
        // path, which looks for `<path less LAYOUT>pth`.
        if (6 <= piece.size()) {
            const std::string pth = resolve_archive_path(piece.substr(0, piece.size() - 6) + "pth");
            if (!pth.empty()) collision_sections.back().cached = read_resource(pth);
        }
        level_rooms.emplace_back();
        collision_section = static_cast<int>(collision_sections.size()) - 1;
        placing_scene = static_cast<uint32_t>(chunk_i);
        placing_hierarchy = 0;
        if (place_layout(piece_path, origin, ogre::Quaternion::IDENTITY, /*depth=*/1,
                         1u << (chunk_i & 31u))) {
            ++placed_chunks;
            placed_pieces.emplace_back(piece_path, origin);
            if (static_cast<int>(chunk_i) == exit_index) {
                exit_chunk_path = piece_path;
                exit_chunk_origin = origin;
            }
            if (static_cast<int>(chunk_i) == entrance_index) {
                entrance_chunk_path = piece_path;
                entrance_chunk_origin = origin;
            }
        }
        collision_section = -1;
    }
    end_room_placement(*this);
    if (placed_chunks == 0) {
        defer_room_activation = false;
        deferred_room_machines.clear();
        TL_LOG_ERROR("dungeon '%s' floor %d: not one room piece could be placed", def.name.c_str(),
                     floor_number);
        return false;
    }
    TL_LOG_INFO("  %zu of %zu room(s) placed", placed_chunks, layout->instances.size());

    // Lighting, CAMERAMULT and fog over every room: apply_environment() takes the STRATA's
    // RULESET (rules_path_for()).
    apply_environment(strata.ruleset);

    // Collision and the navigation grid over the whole floor (place_layout()'s depth 0
    // tail), before the player is stood: the grid finds where.
    collision.build();
    TL_LOG_INFO("  collision: %zu face(s) from %zu mesh(es), %zu of them nocollide",
                collision.triangle_count(), collision_meshes, collision.nocollide_count());
    nav.clear();
    passability.clear();
    if (!collision.empty()) {
        const unsigned long bake_started = view_milliseconds();
        bake_floor_passability(*this);
        for (const tl::core::PassabilityMap::Box& box : collision_sections) {
            TL_LOG_DEBUG("  room box (%.3f, %.3f)..(%.3f, %.3f)%s", static_cast<double>(box.low.x),
                         static_cast<double>(box.low.z), static_cast<double>(box.high.x),
                         static_cast<double>(box.high.z),
                         box.cached.empty() ? "" : ", baked cells");
        }
        const unsigned long bake_took = view_milliseconds() - bake_started;
        log_passability(passability, static_cast<double>(bake_took) / 1000.0);
        if (!passability_dump_path.empty()) {
            write_passability_dump(passability, passability_dump_path);
        }
        // The doors' and triggers' collision reaches the map grid as the rooms activate, after
        // the population (below).
        nav.build(passability);
        TL_LOG_INFO("  navigation: %zu of %zu cell(s) walkable at %.2f units", nav.walkable_count(),
                    nav.cell_count(), static_cast<double>(nav.cell_size()));
        lay_loaded_path_blockers();
    } else {
        door_trigger_nodes.clear();
    }
    view_automap_create();
    script.set_navigation(&nav);
    script.set_collision(&collision);

    const float chunk_w = level_template.chunk_width_basis * level_template.tile_basis;
    const float chunk_h = level_template.chunk_height_basis * level_template.tile_basis;

    // The floor's extent: x in [origin - w/2, origin + (WIDTH - 1) * w + w/2] as
    // CLevelTemplateData::intersectsChunks bounds it, z in [origin, origin + HEIGHT * h].
    float min_x = 1e9f, max_x = -1e9f, min_z = 1e9f, max_z = -1e9f;
    for (const tl::core::ChunkInstance& chunk : layout->instances) {
        const tl::core::ChunkType& type = level_template.type_at(chunk.type_index);
        min_x = std::min(min_x, chunk.x - chunk_w * 0.5f);
        max_x = std::max(max_x, chunk.x + (static_cast<float>(type.width) - 0.5f) * chunk_w);
        min_z = std::min(min_z, chunk.z);
        max_z = std::max(max_z, chunk.z + static_cast<float>(type.height) * chunk_h);
    }

    // INVENTED (no marker for either): the player stands on walkable ground in the
    // entrance room, facing the first corridor, and able to reach the exit (ground_near()
    // spirals out; nav.find_path() checks reachability).
    auto ground_near = [&](const ogre::Vector3& centre, ogre::Vector3& out) -> bool {
        if (!nav.ready()) return false;
        constexpr int kSteps = 16;
        const float span = std::max(max_x - min_x, max_z - min_z);
        const int rings = 1 + static_cast<int>(span / (nav.cell_size() * 1.5f));
        for (int ring = 0; ring < rings; ++ring) {
            const float radius = static_cast<float>(ring) * (nav.cell_size() * 1.5f);
            for (int step = 0; step < kSteps; ++step) {
                const float a = 6.2831853f * static_cast<float>(step) / static_cast<float>(kSteps);
                const float x = centre.x + (ring == 0 ? 0.0f : std::cos(a) * radius);
                const float z = centre.z + (ring == 0 ? 0.0f : std::sin(a) * radius);
                if (!nav.walkable(x, z)) continue;
                const tl::core::Vec3 sample = nav.sample_at(x, z);
                out = ogre::Vector3(sample.x, sample.y, sample.z);
                return true;
            }
        }
        return false;
    };
    auto to_vec3 = [](const ogre::Vector3& v) { return tl::core::Vec3{v.x, v.y, v.z}; };

    // Where run()'s exit check triggers: the authored marker (find_exit_marker()) when the
    // chunk's piece has one, else the chunk's grounded origin.
    bool exit_marker_found = false;
    ogre::Vector3 exit_forward = ogre::Vector3::UNIT_Z;
    float exit_marker_radius = 4.0f;
    if (exit_index >= 0) {
        const tl::core::ChunkInstance& exit_chunk =
            layout->instances[static_cast<size_t>(exit_index)];
        dungeon_exit_point = ogre::Vector3(exit_chunk.x, exit_chunk.y, exit_chunk.z);
        if (!exit_chunk_path.empty()) {
            ogre::Vector3 marker_pos;
            float marker_radius = 3.0f;
            ChunkMarkerExtra exit_extra;
            if (find_chunk_marker(exit_chunk_path, exit_chunk_origin, "exit", marker_pos,
                                  marker_radius, &exit_extra)) {
                dungeon_exit_warp_at =
                    exit_extra.has_warper ? exit_extra.warper_position : marker_pos;
                dungeon_exit_warp_forward =
                    exit_extra.has_warper ? exit_extra.warper_forward : exit_extra.forward;
                dungeon_exit_point = marker_pos;
                exit_marker_radius = marker_radius;
                exit_marker_found = true;
                exit_forward = exit_extra.forward;
                TL_LOG_INFO("  exit marker at (%.2f, %.2f, %.2f), radius %.1f", marker_pos.x,
                            marker_pos.y, marker_pos.z, static_cast<double>(marker_radius));
            } else {
                TL_LOG_WARN(
                    "  no authored exit marker in '%s'; falling back to the nearest "
                    "walkable ground to its chunk's centre",
                    exit_chunk_path.c_str());
            }
        }
    }

    {
        const tl::core::ChunkInstance& entrance = layout->instances[0];
        ogre::Vector3 facing = entrance_facing;
        const ogre::Vector3 entrance_origin(entrance.x, entrance.y, entrance.z);

        // On the stairs: the entrance chunk's ENTRANCE_NODE `Entrance` Property Node; the
        // nearest ground to the chunk's centre as fallback.
        ogre::Vector3 spawn_at = entrance_origin;
        bool grounded = false;
        if (!entrance_chunk_path.empty()) {
            ogre::Vector3 marker_pos;
            float marker_radius = 3.0f;
            ChunkMarkerExtra entrance_extra;
            if (find_chunk_marker(entrance_chunk_path, entrance_chunk_origin, "entrance",
                                  marker_pos, marker_radius, &entrance_extra)) {
                facing = entrance_extra.forward;
                dungeon_entrance_warp_at =
                    entrance_extra.has_warper ? entrance_extra.warper_position : marker_pos;
                dungeon_entrance_warp_forward = entrance_extra.has_warper
                                                    ? entrance_extra.warper_forward
                                                    : entrance_extra.forward;
                spawn_at = marker_pos;
                grounded = true;
                dungeon_entrance_point = marker_pos;
                dungeon_entrance_radius = marker_radius;
                float surface = 0.0f;
                if (collision.ground_under(spawn_at.x, spawn_at.z, spawn_at.y + 8.0f, surface)) {
                    spawn_at.y = surface;
                }
                TL_LOG_INFO("  entrance marker at (%.2f, %.2f, %.2f)", spawn_at.x, spawn_at.y,
                            spawn_at.z);
            } else {
                TL_LOG_WARN(
                    "  no authored entrance node in '%s'; standing at the nearest walkable "
                    "ground to its chunk's centre",
                    entrance_chunk_path.c_str());
            }
        }
        if (!grounded) grounded = ground_near(entrance_origin, spawn_at);

        // Reachability over the passability grid is logged, not enforced: a
        // floor whose exit no route reaches is a defect to look at, not
        // something to paper over by moving the player.
        if (grounded && exit_index > entrance_index) {
            const tl::core::ChunkInstance& exit_chunk =
                layout->instances[static_cast<size_t>(exit_index)];
            ogre::Vector3 exit_ground;
            if (ground_near(ogre::Vector3(exit_chunk.x, exit_chunk.y, exit_chunk.z), exit_ground)) {
                // The trigger point substitutes for the marker only when there is none; the
                // connectivity check still wants a walkable point near the exit.
                if (!exit_marker_found) dungeon_exit_point = exit_ground;
                const ogre::Vector3 route_to = exit_marker_found ? dungeon_exit_point : exit_ground;
                if (nav.find_path(to_vec3(spawn_at), to_vec3(route_to)).empty()) {
                    // A closed door is a wall here too (register_door_avoidance()), so a
                    // floor behind one reads unreachable; named rather than silenced.
                    if (!door_avoidance_added.empty()) {
                        TL_LOG_WARN(
                            "  no pathfinder route from the entrance to this floor's exit "
                            "(%zu closed door/chest/trigger collision model(s) present - may "
                            "open the way on approach)",
                            door_avoidance_added.size());
                    } else {
                        TL_LOG_WARN("  no pathfinder route from the entrance to this floor's exit");
                    }
                } else {
                    TL_LOG_INFO("  a route leads from the entrance to the exit");
                }
            }
        }

        if (!grounded) {
            // No nav grid at all, or the whole floor is ungrounded: a plain
            // ray straight down, the way stand_subject_at() does for an
            // authored marker.
            ogre::Vector3 probe(spawn_at.x + facing.x * 6.0f, 20.0f, spawn_at.z + facing.z * 6.0f);
            float surface = 0.0f;
            if (ground_under(probe, surface)) {
                probe.y = surface;
                spawn_at = probe;
                grounded = true;
            }
        }
        if (!grounded) {
            TL_LOG_WARN("  no ground found anywhere on the generated floor; standing at y=0");
        }

        // CLevel::loadRoomLayout: entered with type 0
        // (from below) the level starts at its Exit node, turned as it is (performWarp
        // then stands the player at the matching Warper).
        if (level_entry_type == 0 && exit_marker_found) {
            spawn_at = dungeon_exit_point;
            facing = exit_forward;
            float surface = 0.0f;
            if (collision.ground_under(spawn_at.x, spawn_at.z, spawn_at.y + 8.0f, surface)) {
                spawn_at.y = surface;
            }
            grounded = true;
            TL_LOG_INFO("  entered from below: the start is the exit node");
        }

        view_stand_subject(spawn_at, facing);
        player_start = spawn_at;
        if (!facing.isZeroLength()) player_start_yaw = std::atan2(facing.x, facing.z);
        have_player_start = true;
        TL_LOG_INFO(
            "  subject at the dungeon entrance (%.2f, %.2f, %.2f), facing (%.2f, 0, %.2f)%s",
            spawn_at.x, spawn_at.y, spawn_at.z, facing.x, facing.z, grounded ? "" : ", ungrounded");
    }

    // A clean level-script slate: build() on an empty scene resets the machine's tables.
    script.set_equipment_roller(
        [this](const std::string& unit, tl::core::LevelScript::UnitCreation& made) {
            roll_unit_equipment(unit, made);
        });
    script.set_unit_equipment(unit_equipment_hooks());
    unit_observers.clear();
    skill_observers.clear();
    script.build(tl::formats::CmpScene{}, {});

    // build() leaves units_ alone and load_unit_stats() never runs for a generated floor,
    // so the unit book is set here (stats_for(), COLLIDEABLE and the rest).
    script.set_units(&unit_book);
    wire_script(script);
    script_ready = true;
    for (MachineView& view : machines()) view.script->set_level_monster_level(level_monster_level);
    for (MachineView& view : machines()) view.script->set_random_names(&random_names);

    // CGameClient::loadLevel, the room layout loaded:
    // resetGameSeed(depth), CLevelTemplateData::chooseUnitSpawners, then setSeed(0) and
    // setSeedVolatile(0) - the clock - before CLevel::populate.
    tl::core::reset_game_seed(seed, static_cast<int32_t>(depth), g_rand);
    tl::core::choose_unit_spawners(level_template, archive_spawn_class_lookup(), g_rand);
    const tl::core::SpawnSlots& spawn_classes = level_template.chosen_spawn_classes;
    // What a hooked fish rolls (`CPlayer::fishingAICatch` reads the level's
    // template, `+0x1d8 -> +0x5a8`).
    level_fish_spawn_class = spawn_classes[6];
    if (level_template.spawn_classes_randomized[0]) {
        TL_LOG_INFO("  monster spawn class '%s' chosen from '%s'", spawn_classes[0].c_str(),
                    level_template.spawn_classes[0].c_str());
    }
    tl::core::reseed_level_generators();

    // `CLevel::populate`: its sections, `clearOutRoomPieces`, the loading
    // screen's first update, then the random population.
    view_update_loading();
    // CGameClient::loadLevel calls
    // CLevel::populate(false) on a remembered level: no random population;
    // its units come from the state.
    if (!level_has_saved_state) populate_level(level_template, floor_monster_level, placed_pieces);

    // `CLevel::populate` ends with
    // `CQuestManager::populate`, outside the random-population branch: a
    // remembered level runs it too, and `+0xb9` keeps a placed objective from
    // being placed twice.
    populate_quests();
    view_update_loading();

    // ... and its last act: every room scene activated (setActivated, through
    // CLayoutDescriptor::descriptorSceneActivated) in room order;
    // CGameClient::loadLevel the same for LEVELPARTICLE after populate.
    activate_deferred_rooms();
    view_update_loading();
    // `findParticles` (the level's particle layouts listed), then populate's last update.
    view_update_loading();
    place_level_particle(level_template.level_particle, placed_pieces);

    if (const size_t armed = script.arm_hostiles()) {
        TL_LOG_INFO("  %zu hostile unit(s) will come after the player", armed);
    }

    // The music and the HUD, as load_layout() does once its layout is placed.
    start_level_audio(strata.ruleset);
    // The template's own name and flag, which a stratum's `NAME_OVERRIDE` has
    // already been laid over (`CDungeon::getStrataTemplate`)
    // - the ruleset file read above does not know
    // about the override.
    level_has_rules = true;
    level_name_authored = tl::core::strip_markup(level_template.level_name);
    level_shows_floor = level_template.show_floor_number;
    level_display_name =
        tl::core::level_display_name(level_name_authored, level_shows_floor, level_depth);
    raise_arrival_banner();
    refresh_game_hud();

    // Arms run()'s exit check: exit_marker_radius is the marker's RADIUS when found;
    // INVENTED: 4 without one.
    dungeon_active = true;
    dungeon_active_name = def.name;
    dungeon_active_floor = floor_number;
    dungeon_active_floors_total = static_cast<int>(def.depth_strata.size());
    dungeon_exit_radius = exit_marker_radius;
    dungeon_transition_pending = false;
    TL_LOG_INFO("  exit at (%.2f, %.2f, %.2f), radius %.1f - floor %d of %d", dungeon_exit_point.x,
                dungeon_exit_point.y, dungeon_exit_point.z,
                static_cast<double>(dungeon_exit_radius), floor_number,
                dungeon_active_floors_total);

    restore_level_state();
    bake_static_scenery();

    log_memory("after the dungeon floor");
    return true;
}

// The collision mesh read once (`CCollisionModel::loadModel`): every
// submesh's faces, each taking its submesh's material (100 for `nocollide`, else the submesh's index), in
// the mesh's own space; a submesh with no geometry still takes its index, and a corner past the vertex count
// drops its triangle. The bounds are over every vertex the mesh holds.
const World::CollisionModel& World::collision_model(const std::string& path) {
    const auto found = collision_models.find(path);
    if (found != collision_models.end()) return found->second;
    CollisionModel& model = collision_models[path];
    std::vector<uint8_t> bytes = read_resource(path);
    if (bytes.empty()) {
        const std::string resolved = resolve_archive_path_quiet(path);
        if (!resolved.empty()) bytes = read_resource(resolved);
    }
    if (bytes.empty()) {
        model.state = CollisionModel::State::Threw;
        model.error = "Cannot locate resource " + path;
        return model;
    }
    const tl::formats::OgreMesh mesh = tl::formats::parse_ogre_mesh(bytes.data(), bytes.size());
    if (!mesh.ok) {
        model.state = CollisionModel::State::Threw;
        model.error = mesh.error;
        return model;
    }
    model.state = CollisionModel::State::Loaded;
    uint32_t submesh_index = 0;
    for (const tl::formats::OgreMeshSubmesh& sub : mesh.submeshes) {
        CollisionModel::Submesh piece;
        piece.material = tl::core::collision_material(sub.material, submesh_index);
        ++submesh_index;
        const tl::formats::OgreMeshVertices& vertices =
            sub.uses_shared_vertices ? mesh.shared : sub.vertices;
        if (!vertices.present || vertices.positions.empty() || sub.indices.empty()) continue;
        piece.points.resize(vertices.count);
        for (size_t v = 0; v < vertices.count; ++v) {
            piece.points[v] =
                ogre::Vector3(vertices.positions[v * 3], vertices.positions[v * 3 + 1],
                              vertices.positions[v * 3 + 2]);
        }
        const size_t triangles = sub.indices.size() / 3;
        piece.corners.reserve(triangles * 3);
        for (size_t t = 0; t < triangles; ++t) {
            const size_t a = sub.indices[t * 3 + 0];
            const size_t b = sub.indices[t * 3 + 1];
            const size_t c = sub.indices[t * 3 + 2];
            if (a >= piece.points.size() || b >= piece.points.size() || c >= piece.points.size())
                continue;
            piece.corners.push_back(static_cast<uint32_t>(a));
            piece.corners.push_back(static_cast<uint32_t>(b));
            piece.corners.push_back(static_cast<uint32_t>(c));
        }
        model.submeshes.push_back(std::move(piece));
    }
    // Every vertex set's positions: the shared one, then each submesh's own.
    bool have = false;
    const auto take = [&](const tl::formats::OgreMeshVertices& v) {
        if (!v.present) return;
        for (size_t i = 0; i + 2 < v.positions.size(); i += 3) {
            const float x = v.positions[i], y = v.positions[i + 1], z = v.positions[i + 2];
            if (!have) {
                model.low = {x, y, z};
                model.high = model.low;
                have = true;
                continue;
            }
            model.low = {std::min(model.low.x, x), std::min(model.low.y, y),
                         std::min(model.low.z, z)};
            model.high = {std::max(model.high.x, x), std::max(model.high.y, y),
                          std::max(model.high.z, z)};
        }
    };
    take(mesh.shared);
    for (const tl::formats::OgreMeshSubmesh& sub : mesh.submeshes) {
        if (!sub.uses_shared_vertices) take(sub.vertices);
    }
    model.has_bounds = have;
    return model;
}

/// Fold one collision mesh into the level's collision soup, in world space.
///
/// The mesh is loaded but never made into an entity: a collision mesh is an
/// untextured stand-in for the geometry beside it, and putting one in the scene
/// would drape the level in grey. All that is wanted is its triangles.
///
/// Nothing is logged per mesh on purpose. A cave tileset gives every tile its
/// own collision file, so a real dungeon would put hundreds of lines in a log
/// whose whole value is that a person can read it; the counts are reported once
/// by place_layout() instead. A mesh that will not load *is* logged, because
/// that is a hole in the floor.
void World::add_collision_mesh(const std::string& path, const ogre::Matrix4& to_world,
                               int64_t group, const ogre::Vector3* piece_at, bool recalculate) {
    collision.set_group(group);
    struct GroupGuard {
        tl::core::CollisionWorld& world;
        ~GroupGuard() { world.set_group(0); }
    } group_guard{collision};

    const CollisionModel& model = collision_model(path);
    if (model.state == CollisionModel::State::Threw) {
        TL_LOG_WARN("  collision mesh '%s' would not load: %s", path.c_str(), model.error.c_str());
        ++collision_skipped;
        return;
    }
    if (model.state == CollisionModel::State::Missing) {
        ++collision_skipped;
        return;
    }

    // `bakeCollision`'s held pieces (`hold_random_pieces`): their faces join after the rooms' passes.
    const bool held = recalculate && hold_random_pieces;
    if (held) {
        held_random_collision.push_back({path, to_world});
    }
    // CCollisionList::addCollisionList and the box's corners put each point through
    // `Ogre::Matrix4::operator*(const Vector3&)`, fused.
    tl::core::OgreMatrix4 transform;
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column) transform.m[row][column] = to_world[row][column];
    const auto through = [&transform](const ogre::Vector3& v) {
        const tl::core::Vec3 r = tl::core::matrix_transform(transform, {v.x, v.y, v.z});
        return ogre::Vector3(r.x, r.y, r.z);
    };
    if (!held) {
        std::vector<ogre::Vector3> points;
        for (const CollisionModel::Submesh& piece : model.submeshes) {
            points.resize(piece.points.size());
            for (size_t v = 0; v < piece.points.size(); ++v) points[v] = through(piece.points[v]);
            for (size_t i = 0; i + 2 < piece.corners.size(); i += 3) {
                const ogre::Vector3& a = points[piece.corners[i + 0]];
                const ogre::Vector3& b = points[piece.corners[i + 1]];
                const ogre::Vector3& c = points[piece.corners[i + 2]];
                collision.add_triangle({a.x, a.y, a.z}, {b.x, b.y, b.z}, {c.x, c.y, c.z},
                                       piece.material);
            }
        }
        ++collision_meshes;
    }

    // CLevel::bakeCollision: a room layout's
    // passability box is the union, over its collision-enabled pieces, of four
    // corners of each collision list's own bounds put through the piece's
    // transform - its minimum, its maximum, (max x, min y, min z) and (min x,
    // max y, max z).
    if (group == 0 && collision_section >= 0 &&
        collision_section < static_cast<int>(collision_sections.size()) && model.has_bounds) {
        const tl::core::Vec3& low = model.low;
        const tl::core::Vec3& high = model.high;
        const ogre::Vector3 corners[4] = {
            through(ogre::Vector3(low.x, low.y, low.z)),
            through(ogre::Vector3(high.x, high.y, high.z)),
            through(ogre::Vector3(high.x, low.y, low.z)),
            through(ogre::Vector3(low.x, high.y, high.z)),
        };
        tl::core::Vec3 box_low{corners[0].x, corners[0].y, corners[0].z};
        tl::core::Vec3 box_high = box_low;
        for (const ogre::Vector3& c : corners) {
            box_low = {std::min(box_low.x, c.x), std::min(box_low.y, c.y),
                       std::min(box_low.z, c.z)};
            box_high = {std::max(box_high.x, c.x), std::max(box_high.y, c.y),
                        std::max(box_high.z, c.z)};
        }
        collision_sections[static_cast<size_t>(collision_section)].merge(box_low, box_high);
        if (recalculate) {
            tl::core::PassabilityMap::Box box;
            box.merge(box_low, box_high);
            (held ? held_random_boxes : passability_recalculate).push_back(std::move(box));
        }
    }

    // `CLevel::populate`: a section per room piece with a model, its
    // position plus the collision mesh's own bounds - no turn, no scale - with
    // the height pushed 1000 either way. The first corner is the list's `+0x1c`,
    // its minimum (`calculateFaceBounds` hands
    // `+0x1c` to `MATH::expandBounds` as the low end), and the rolls between the corners
    // run from it.
    if (piece_at != nullptr && model.has_bounds) {
        tl::core::PopulateSection section;
        section.first = {piece_at->x + model.low.x, piece_at->y + model.low.y - 1000.0f,
                         piece_at->z + model.low.z};
        section.second = {piece_at->x + model.high.x, piece_at->y + model.high.y + 1000.0f,
                          piece_at->z + model.high.z};
        population_sections.push_back(section);
        TL_LOG_DEBUG("  populate section %zu: %s at (%a, %a, %a)", population_sections.size() - 1,
                     path.c_str(), static_cast<double>(piece_at->x),
                     static_cast<double>(piece_at->y), static_cast<double>(piece_at->z));
        if (collision_section >= 0 && collision_section < static_cast<int>(level_rooms.size())) {
            level_rooms[static_cast<size_t>(collision_section)].add(section);
        }
    }
}

// A collision placement's triangles into the level's collision world, at its pose.
void World::add_collision_placement(const MeshPlacement& placement) {
    if (placement.room_piece && placement.has_mesh && !placement.linked) {
        TL_LOG_DEBUG("  section piece: node %lld", static_cast<long long>(placement.node_id));
    }
    ogre::Matrix4 to_world;
    to_world.makeTransform(placement.position, placement.scale, placement.orientation);
    add_collision_mesh(placement.mesh, to_world, placement.collision_group,
                       placement.room_piece && placement.has_mesh && !placement.linked
                           ? &placement.position
                           : nullptr,
                       placement.recalculate);
}

}  // namespace tl::core
