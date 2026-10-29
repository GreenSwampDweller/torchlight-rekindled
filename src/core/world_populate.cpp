// The level's population (core/world.h): `CLevel::populate`'s phases, the units each rolls,
// their minions and formations. Split from world_level.cpp.
#include "core/layout_text.h"
#include "core/level_memory.h"
#include "core/logic_names.h"
#include "core/ogre_math.h"
#include "core/skill_cast.h"
#include "core/unit_skills_reader.h"
#include "core/world.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace tl::core {

// `CLevel::randomOpenPosition(p, radius, true)`, as the population
// phases ask it.
tl::core::Vec3 World::populate_open_position(const tl::core::Vec3& at, float radius) {
    return tl::core::random_open_position(
        at, 0.0f, radius,
        [this](const tl::core::Vec3& p) {
            return !passability.ready() ||
                   (passability.map_passable(p) && passability.position_passable(p) &&
                    !tl::core::is_in_no_spawn_region(no_spawn_regions, p, 0.0f));
        },
        tl::core::volatile_random());
}

// CCharacter::createPathfinder(CLevel&), called by
// CLevel::addCharacter: MINION_SPAWNCLASS (+0x6b8)
// rolled once - rollSpawnClass(out, null, this, this, -1, 0xffffffff, 0, -1, 0, 0) on
// g_Rand - and cleared.
std::vector<std::string> World::roll_unit_minions(const std::string& unit, int level) {
    std::vector<std::string> rolled;
    const std::string spawn_class = stats_for_unit(unit).minion_spawnclass;
    if (spawn_class.empty()) return rolled;
    SpawnTables tables;
    build_spawn_context(tables, /*items_by_type=*/false);
    const std::optional<tl::core::SpawnClass> loaded =
        load_spawn_class(spawn_class, tables.context);
    // `getSpawnClassByName` finding none leaves `+0x6b8` null.
    if (!loaded) return rolled;
    tl::core::SpawnCharacterContext self;
    self.level = level;
    loaded->roll_spawn_class(rolled, nullptr, &self, &self, -1, 0xffffffffu, /*level_bonus=*/0,
                             /*rarity_override=*/-1, /*magic_find=*/0, /*flags=*/0,
                             tl::core::game_random(), tables.context);
    return rolled;
}

// `CCharacter::createPathfinder(CLevel&)`'s
// minions for a unit: rolled at its level and placed (`place_unit_minions`).
void World::spawn_unit_minions(int64_t master, int64_t owner) {
    const std::string unit = script.unit_of(master);
    if (unit.empty()) return;
    int level = 0;
    if (!script.unit_level(master, level)) level = level_monster_level;
    place_unit_minions(master, owner, roll_unit_minions(unit, level));
}

// `[FORMATION]`s of a template's `FORMATIONS` file (`CLevelTemplateData +0x5b0`),
// as `CLevel::populateFormations` reads them:
// every child of the file in order, its `FILE` and its `WEIGHT` (1 when
// unauthored).
std::vector<tl::core::FormationChoice> World::formation_choices(const std::string& table) const {
    std::vector<tl::core::FormationChoice> choices;
    const std::string path = resolve_archive_path(table + ".adm");
    if (path.empty()) return choices;
    const std::vector<uint8_t> bytes = read_resource(path);
    if (bytes.empty()) return choices;
    const tl::formats::AdmFile file = tl::formats::parse_adm(bytes.data(), bytes.size());
    if (!file.ok) return choices;
    for (const tl::formats::AdmSection& child : file.root.children) {
        tl::core::FormationChoice choice;
        choice.file = child.string_or("FILE", "");
        choice.weight = static_cast<int32_t>(child.integer_or("WEIGHT", 1u));
        choices.push_back(std::move(choice));
    }
    return choices;
}

// `CResourceManager::createUnit` and
// `CLevel::addItem` for a `CREATEAS: TRIGGER`
// prop: the port lays down a layout of one `Unit Trigger` node (the header).
bool World::place_populated_trigger(const std::string& unit, const ogre::Vector3& at,
                                    const ogre::Quaternion& facing, int64_t id, float scale) {
    TriggerProp prop;
    if (!trigger_prop(unit, prop)) return false;

    using tl::formats::CmpField;
    const auto vec = [](float x, float y, float z) {
        CmpField f;
        for (float v : {x, y, z}) {
            uint32_t bits = 0;
            std::memcpy(&bits, &v, sizeof bits);
            f.words.push_back(bits);
        }
        return f;
    };
    const auto number = [](uint32_t value) {
        CmpField f;
        f.words.push_back(value);
        return f;
    };
    // A string field: UTF-16LE with a NUL, padded to whole dwords.
    const auto text = [](const std::string& value) {
        const size_t dwords = ((value.size() + 1) * 2 + 3) / 4;
        CmpField f;
        f.words.assign(dwords, 0u);
        for (size_t i = 0; i < value.size(); ++i) {
            f.words[i / 2] |= static_cast<uint32_t>(static_cast<unsigned char>(value[i]))
                              << (16 * (i % 2));
        }
        return f;
    };

    tl::formats::CmpNode node;
    node.type = tl::formats::cmp_type::kUnitTrigger;
    node.name = unit;
    node.id = id;
    node.parent_id = -1;
    node.root = true;
    // `unit_trigger`'s fields in their order: the position and the three axes
    // (the layout is put down already turned), enabled, the unit, and the
    // rest at what an unauthored node holds.
    node.fields = {vec(0.0f, 0.0f, 0.0f),
                   vec(0.0f, 0.0f, 1.0f),
                   vec(1.0f, 0.0f, 0.0f),
                   vec(0.0f, 1.0f, 0.0f),
                   number(1),
                   text(unit),
                   number(0),
                   text(""),
                   text(""),
                   number(0),
                   number(0)};
    node.vec3s = {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
    node.strings = {unit};

    auto scene = std::make_unique<tl::formats::CmpScene>();
    scene->ok = true;
    scene->nodes.push_back(std::move(node));
    const std::string key = "populate:" + unit + "#" + std::to_string(id);
    layout_files[key] = std::move(scene);
    made_trigger_scale[id] = scale;
    const bool placed = place_layout(key, at, facing, /*depth=*/1);
    made_trigger_scale.erase(id);
    if (!placed) return false;
    populated_triggers[id] = PopulatedTrigger{unit, facing};
    return true;
}

// The same function's loop: each unit made at the level's monster
// level, turned randomBetweenVolatile(0, 360), a character placed at
// randomOpenPositionRange(master, 1, 4, true) and made the master's pet.
//
void World::place_unit_minions(int64_t master, int64_t owner,
                               const std::vector<std::string>& minions) {
    for (const std::string& name : minions) {
        // Not a character: made and left out of the level.
        ensure_monster_index();
        if (monster_files.count(lowered(name)) == 0) continue;
        index_unit(name);
        const tl::core::LevelScript::UnitCreation creation = script.roll_unit_creation(name);
        const float yaw = tl::core::volatile_random().between(0.0f, 360.0f) * 0.017453292f;
        tl::core::Vec3 from;
        if (!script.unit_position(master, from)) return;
        const tl::core::Vec3 at = tl::core::random_open_position(
            from, 1.0f, 4.0f,
            [this](const tl::core::Vec3& p) {
                return !passability.ready() ||
                       (passability.map_passable(p) && passability.position_passable(p) &&
                        !tl::core::is_in_no_spawn_region(no_spawn_regions, p, 0.0f));
            },
            tl::core::volatile_random());
        // `CLevel::addCharacter(minion, at, false)`: on the ground.
        const ogre::Quaternion turn(ogre::Radian(yaw), ogre::Vector3::UNIT_Y);
        const ogre::Vector3 place = character_to_ground(ogre::Vector3(at.x, at.y, at.z));
        if (spawn_unit(name, place, owner, level_monster_level, /*champion=*/false,
                       /*spawn_anim=*/false, turn, creation.scale) == nullptr) {
            continue;
        }
        const int64_t id = spawned[owner].back().id;
        const ogre::Vector3 forward = turn * ogre::Vector3::UNIT_Z;
        script.add_spawned_unit(id, name, tl::core::Vec3{place.x, place.y, place.z},
                                /*hunts=*/false, std::atan2(forward.x, forward.z), &creation);
        int made_level = 0;
        if (0 < level_monster_level &&
            !(script.unit_level(id, made_level) && made_level == level_monster_level)) {
            apply_spawn_level(id);
        }
        // ... which rolls a minion's own minions, before its master takes it.
        spawn_unit_minions(id, owner);
        add_monster_pet(script, id, script, master);
        TL_LOG_INFO("  minion: %s beside %s at (%.2f, %.2f, %.2f)", name.c_str(),
                    script.unit_of(master).c_str(), static_cast<double>(place.x),
                    static_cast<double>(place.y), static_cast<double>(place.z));
    }
}

// `populateSectionOfLevel`: a champion's name is `CRandomNames::generateName(true)`,
// drawn on the game generator once the character is in the level, and `makeChampion` takes it.
void World::make_populated_champion(int64_t id, const std::string& unit) {
    load_quest_book();
    if (!random_names.empty()) {
        unit_names[id] = random_names.generate_name(true, tl::core::game_random());
        TL_LOG_INFO("  champion: %s is '%s'", unit.c_str(), unit_names[id].c_str());
    }
    // `makeChampion` on it: the champion's hit points,
    // armour, experience and fame, and its size.
    script.make_unit_champion(id);
}

int World::populate_units(const std::string& spawn_class, uint32_t wanted, int level,
                          tl::core::Randomizer& sections, const PopulatePhase& phase) {
    if (wanted == 0) return 0;
    if (population_sections.empty()) {
        TL_LOG_WARN("  %s: no room piece made a section; none placed", phase.label);
        return 0;
    }

    SpawnTables tables;
    build_spawn_context(tables, /*items_by_type=*/false);
    std::optional<tl::core::SpawnClass> loaded;
    if (!spawn_class.empty()) loaded = load_spawn_class(spawn_class, tables.context);
    // No such class (`NONE` is what a template names for a phase it leaves
    // out): the phase still runs, as the original's does, and places nothing.
    if (!loaded) TL_LOG_INFO("  %s: no spawn class '%s'", phase.label, spawn_class.c_str());

    tl::core::RunicRandom& g_rand = tl::core::game_random();
    tl::core::RunicRandom& volatile_rolls = tl::core::volatile_random();

    // `CLevel +0xf0`, the `Quest Boss` property nodes, and how often each has
    // been stood on (`CPropertyNode +0x114`).
    std::vector<tl::core::SpawnMarker> markers;
    if (phase.markers) {
        for (const QuestMarker& marker : quest_boss_markers) {
            tl::core::SpawnMarker m;
            m.position = {marker.position.x, marker.position.y, marker.position.z};
            m.orientation = {marker.orientation.w, marker.orientation.x, marker.orientation.y,
                             marker.orientation.z};
            m.taken = marker.taken;
            markers.push_back(m);
        }
    }

    const auto turn_of = [](const tl::core::PopulatedFacing& facing) {
        if (facing.from_marker) {
            return ogre::Quaternion(facing.orientation[0], facing.orientation[1],
                                    facing.orientation[2], facing.orientation[3]);
        }
        return ogre::Quaternion(ogre::Radian(facing.yaw), ogre::Vector3::UNIT_Y);
    };

    int breakables = 0;
    int triggers = 0;
    std::map<std::string, int> counts;
    // What `createUnit` drew for a unit, by handle, until it is added.
    std::map<int64_t, tl::core::LevelScript::UnitCreation> made_characters;
    std::map<int64_t, float> made_scales;
    int64_t handles = 0;

    tl::core::PopulateWorld world;
    world.volatile_rolls = &volatile_rolls;
    world.markers = phase.markers ? &markers : nullptr;
    world.roll = [&](const std::string&) {
        // `rollSpawnClass(out, null, null, null, CLevel +0x1a8, 0xffffffff, 0, -1, 0, 0)`:
        // on `g_Rand` as it stands.
        std::vector<std::string> rolled;
        if (!loaded) return rolled;
        loaded->roll_spawn_class(rolled, nullptr, nullptr, nullptr, level, 0xffffffffu,
                                 /*level_bonus=*/0, /*rarity_override=*/-1, /*magic_find=*/0,
                                 /*flags=*/0, g_rand, tables.context);
        return rolled;
    };
    world.random_open_position = [this](const tl::core::Vec3& at, float radius) {
        return populate_open_position(at, radius);
    };
    world.in_no_spawn_region = [this](const tl::core::Vec3& at, float radius) {
        return tl::core::is_in_no_spawn_region(no_spawn_regions, at, radius);
    };
    world.passable_between = [this](const tl::core::Vec3& from, const tl::core::Vec3& to) {
        return !passability.ready() || passability.passable_between(from, to, /*map_only=*/false);
    };
    world.create = [&](const std::string& unit) {
        tl::core::PopulatedUnit made;
        made.handle = ++handles;
        ensure_prop_index();
        const auto prop_file = prop_files.find(lowered(unit));
        if (prop_file == prop_files.end()) {
            // Not a prop: a character (the props class's `Mimic` among them).
            ensure_monster_index();
            if (monster_files.count(lowered(unit)) == 0) return tl::core::PopulatedUnit{};
            // Its numbers, which making it reads.
            index_unit(unit);
            made.kind = tl::core::PopulatedKind::Character;
            made_characters[made.handle] = script.roll_unit_creation(unit);
            stock_made_merchant(made.handle, unit);
            return made;
        }
        const tl::formats::AdmFile file = read_unit_file(prop_file->second);
        if (!file.ok) return tl::core::PopulatedUnit{};
        made.kind = tl::core::PopulatedKind::Item;
        if (is_breakable_unit(unit)) {
            // `CBaseUnit::unitInit`, through
            // `CItem::unitInit`: the scale. A breakable (`BREAKABLE` ->
            // `CONTAINER` -> `ITEM`) never blocks.
            const tl::core::BreakableData data = tl::core::read_breakable(file);
            made_scales[made.handle] = tl::core::breakable_scale(data, volatile_rolls);
            roll_prop_skills(unit);  // then its `[SKILL]`s
            breakable_unit_loads(data);
            return made;
        }
        // A `CREATEAS: TRIGGER` prop (the chests).
        TriggerProp prop;
        if (!trigger_prop(unit, prop)) {
            TL_LOG_WARN("  %s: '%s' is neither a breakable nor a trigger prop", phase.label,
                        unit.c_str());
            return tl::core::PopulatedUnit{};
        }
        // `CBaseUnit::unitInit` draws its
        // scale as it is made.
        made_scales[made.handle] = prop.scale + volatile_rolls.between(0.0f, prop.scale_variation);
        roll_prop_skills(unit);
        // `+0x18d`, `+0x19c` and `ISA 0x20`: a collideable chest.
        made.blocks = prop.collideable && !prop.collision.empty() &&
                      unit_types.isa(prop.unittype, tl::core::unit_type::kInteractable);
        return made;
    };
    world.add_character = [&](const tl::core::PopulatedUnit& made, const std::string& unit,
                              const tl::core::Vec3& at, const tl::core::PopulatedFacing& facing,
                              bool champion) {
        const auto creation = made_characters.find(made.handle);
        if (creation == made_characters.end()) return false;
        // `populateSectionOfLevel`: the champion's name and `makeChampion`
        // first, then `CLevel::addCharacter(unit, at, false)`,
        // which drops it to the ground: the drop's draws come after the champion's.
        const ogre::Quaternion turn = turn_of(facing);
        const ogre::Vector3 forward = turn * ogre::Vector3::UNIT_Z;
        const ogre::Vector3 start(at.x, at.y, at.z);
        if (spawn_unit(unit, start, phase.owner, level, champion, /*spawn_anim=*/false, turn,
                       creation->second.scale) == nullptr) {
            return false;
        }
        const SpawnedUnit placed = spawned[phase.owner].back();  // copied: minions join this list
        place_made_merchant(made.handle, placed.id);
        script.add_spawned_unit(placed.id, unit, tl::core::Vec3{at.x, at.y, at.z},
                                /*hunts=*/false, std::atan2(forward.x, forward.z),
                                &creation->second);
        // Made at the level's monster level already; only a champion's
        // `level + 1` sets it again.
        const auto wanted_level = unit_spawn_level.find(placed.id);
        int made_level = 0;
        if (wanted_level != unit_spawn_level.end() && wanted_level->second.level > 0 &&
            !(script.unit_level(placed.id, made_level) &&
              made_level == wanted_level->second.level)) {
            apply_spawn_level(placed.id);
        }
        if (champion) make_populated_champion(placed.id, unit);
        ++counts[unit];
        // `addCharacter` -> `createPathfinder`: its minions, rolled after the
        // name `makeChampion` took, each placed and dropped before the unit's own drop.
        spawn_unit_minions(placed.id, phase.owner);
        // Then `addCharacter`'s `dropToGround(level, 4.0, ..., false)`.
        const ogre::Vector3 place = character_to_ground(start);
        view_set_position(placed.node, place);
        script.move_unit(placed.id, tl::core::Vec3{place.x, place.y, place.z});
        return true;
    };
    world.add_item = [&](const tl::core::PopulatedUnit& made, const std::string& unit,
                         const tl::core::Vec3& at, const tl::core::PopulatedFacing& facing) {
        const ogre::Quaternion turn = turn_of(facing);
        if (!is_breakable_unit(unit)) {
            // `addItem`, on the ground (`CItem::snapToGround`).
            //
            const ogre::Vector3 ground = snap_item_to_ground(ogre::Vector3(at.x, at.y, at.z));
            const auto scale = made_scales.find(made.handle);
            if (!place_populated_trigger(unit, ground, turn, next_spawned_id(),
                                         scale != made_scales.end() ? scale->second : 1.0f)) {
                return false;
            }
            ++triggers;
            ++counts[unit];
            return true;
        }
        // `CLevel::addItem(item, at, true)`:
        // only a `CEquipment` is tossed, so a `CBreakable` stands where it was
        // put, on the ground (`setActiveInLevel`).
        const auto scale = made_scales.find(made.handle);
        if (!place_breakable(unit, ogre::Vector3(at.x, at.y, at.z), turn, next_spawned_id(),
                             nullptr, scale != made_scales.end() ? &scale->second : nullptr)) {
            return false;
        }
        ++breakables;
        ++counts[unit];
        return true;
    };
    world.discard = [&](const tl::core::PopulatedUnit& made) {
        made_characters.erase(made.handle);
        made_merchants.erase(made.handle);
        made_scales.erase(made.handle);
    };

    const int placed =
        tl::core::populate_phase(population_sections, sections, g_rand, spawn_class, wanted,
                                 phase.champion, phase.markers, phase.empty_counts, world);
    if (phase.markers) {
        for (size_t i = 0; i < markers.size() && i < quest_boss_markers.size(); ++i) {
            quest_boss_markers[i].taken = markers[i].taken;
        }
    }

    std::string summary;
    for (const auto& entry : counts) {
        summary += (summary.empty() ? "" : ", ") + std::to_string(entry.second) + " " + entry.first;
    }
    if (triggers > 0) {
        // The chests' own collision, into the soup and onto the map grid
        // (`addToAvoidanceMap`), and the routes around them.
        collision.build();
        register_door_avoidance();
        if (passability.ready()) nav.build(passability);
    }
    if (phase.props) {
        TL_LOG_INFO(
            "  props: %d of %u placed from '%s' over %zu section(s) (%d breakable(s), "
            "%d trigger prop(s)%s%s)",
            placed, wanted, spawn_class.c_str(), population_sections.size(), breakables, triggers,
            summary.empty() ? "" : ": ", summary.c_str());
    } else if (placed > 0) {
        TL_LOG_INFO("  %s: %d of %u placed from '%s'%s (%s)", phase.label, placed, wanted,
                    spawn_class.c_str(), phase.champion ? ", promoted to champion" : "",
                    summary.c_str());
    } else if (loaded) {
        TL_LOG_WARN("  %s: none placed from '%s'", phase.label, spawn_class.c_str());
    }
    return placed;
}

// A CFormationNode: setLayoutFile loads the layout
// hidden, its Monsters out of the level, until the player comes near (wake_formations()).
bool World::add_pending_formation(const std::string& reference, const ogre::Vector3& at,
                                  int monster_level) {
    std::string formation_path = resolve_archive_path(reference);
    if (formation_path.empty()) formation_path = resolve_archive_path(reference + ".cmp");
    if (formation_path.empty()) {
        TL_LOG_WARN("  formations: '%s' named but not found in the archive", reference.c_str());
        return false;
    }
    const tl::formats::CmpScene* const scene = layout_file(formation_path);
    if (scene == nullptr || !scene->ok) return false;
    bool spawners = false;
    std::vector<tl::core::LevelScript::UnitCreation> made;
    for (const tl::formats::CmpNode& node : scene->nodes) {
        if (node.type == tl::formats::cmp_type::kUnitSpawner) spawners = true;
        if (node.type != tl::formats::cmp_type::kMonster) continue;
        // `CMonsterDescriptor::CreateObject` as the layout loads: each monster
        // made now (`unitInit`'s rolls), kept out of the level until the formation wakes.
        const tl::formats::CmpField* who = node.field(tl::formats::cmp_field::monster::kMonster);
        const std::string unit = who != nullptr ? who->as_text() : std::string();
        index_unit(unit);
        made.push_back(script.roll_unit_creation(unit, true, monster_level));
    }
    pending_formations.push_back(
        {formation_path, at, monster_level, false, spawners, reference, std::move(made)});
    TL_LOG_INFO("  formation %s%s waits at (%.2f, %.2f, %.2f)", formation_path.c_str(),
                spawners ? " (unit spawners)" : "", static_cast<double>(at.x),
                static_cast<double>(at.y), static_cast<double>(at.z));
    return true;
}

// `CLevel::populate(true)`, the random
// population: formations, the six counts, then monsters, champions, NPCs,
// creeps, props and gold.
void World::populate_level(const tl::core::LevelTemplate& level_template, int level,
                           std::vector<std::pair<std::string, ogre::Vector3>>& placed_pieces) {
    tl::core::RunicRandom& g_rand = tl::core::game_random();
    tl::core::RunicRandom& volatile_rolls = tl::core::volatile_random();
    const tl::core::SpawnSlots& spawn_classes = level_template.chosen_spawn_classes;

    // The one section randomizer every phase picks from: `CRandomizer(0)`, a
    // choice a section, weight 100.
    tl::core::Randomizer sections(tl::core::RandomizerType::kNormal);
    for (size_t i = 0; i < population_sections.size(); ++i) {
        sections.add_choice(static_cast<int32_t>(i), 100);
    }

    // The pathable area is `CLevel +0x58`, the cells `calculatePassability`
    // left open before erosion (core/passability.h).
    const uint32_t pathable_cells = passability.passable_cells();

    // `populateFormations`, run only for a
    // template that names a formations file (`+0x5b0`); its count is drawn
    // inside, after the file randomizer is filled. The line is logged either way.
    uint32_t formations_wanted = 0;
    int formations_placed = 0;
    if (!spawn_classes[7].empty()) {
        std::vector<tl::core::Vec3> existing;
        for (const PendingFormation& formation : pending_formations) {
            existing.push_back({formation.at.x, formation.at.y, formation.at.z});
        }
        tl::core::FormationWorld world;
        world.volatile_rolls = &volatile_rolls;
        world.random_open_position = [this](const tl::core::Vec3& at, float radius) {
            return populate_open_position(at, radius);
        };
        world.in_no_spawn_region = [this](const tl::core::Vec3& at, float radius) {
            return tl::core::is_in_no_spawn_region(no_spawn_regions, at, radius);
        };
        world.add_formation = [&](const std::string& reference, const tl::core::Vec3& open) {
            // `CFormationNode::setPosition(at)`; its monsters go to the ground
            // as they join the level.
            const ogre::Vector3 at = dropped_to_ground(ogre::Vector3(open.x, open.y, open.z));
            if (!add_pending_formation(reference, at, level)) return;
            placed_pieces.emplace_back(pending_formations.back().path, at);
            ++formations_placed;
        };
        formations_wanted = tl::core::populate_formations(
            population_sections, sections, formation_choices(spawn_classes[7]),
            [&]() {
                return static_cast<uint32_t>(
                    tl::core::units_to_create(level_template, 7, pathable_cells, g_rand));
            },
            existing, g_rand, world);
    }
    TL_LOG_INFO("  Formations Created: %u (%d placed from '%s')", formations_wanted,
                formations_placed, spawn_classes[7].c_str());

    // The counts, in `populate`'s order: slots 0
    // (monsters), 2 (props), 1 (champions), 3 (NPCs), 4 (creeps) and 5 (gold).
    // `KSETTINGS_NETBOOK_MODE` 1 would zero the creeps; it is 0 here.
    const auto count = [&](size_t slot) {
        return static_cast<uint32_t>(std::max<int64_t>(
            0, tl::core::units_to_create(level_template, slot, pathable_cells, g_rand)));
    };
    const uint32_t monsters_wanted = count(0);
    const uint32_t props_wanted = count(2);
    const uint32_t champions_wanted = count(1);
    const uint32_t npcs_wanted = count(3);
    const uint32_t creeps_wanted = count(4);
    const uint32_t gold_wanted = count(5);
    TL_LOG_INFO(
        "  pathable area %.1f m^2: wants %u monster(s), %u prop(s), %u champion(s), "
        "%u NPC(s), %u creep(s), %u gold",
        static_cast<double>(static_cast<float>(pathable_cells) / 6.5f), monsters_wanted,
        props_wanted, champions_wanted, npcs_wanted, creeps_wanted, gold_wanted);

    // The phases (monsters, champions, NPCs, creeps
    // props). Only the monsters' loop lets ten empty rolls
    // pass before counting one; only the NPCs stand on the level's markers.
    PopulatePhase monsters{-2001, "monsters"};
    PopulatePhase champions{-2003, "champions"};
    champions.champion = true;
    champions.empty_counts = true;
    PopulatePhase npcs{-2004, "NPCs"};
    npcs.markers = true;
    npcs.empty_counts = true;
    PopulatePhase creeps{-2002, "creeps"};
    creeps.empty_counts = true;
    PopulatePhase props{-2005, "props"};
    props.empty_counts = true;
    props.props = true;
    // The loading screen is updated after each phase (`CGameUI::updateLoading`).
    populate_units(spawn_classes[0], monsters_wanted, level, sections, monsters);
    view_update_loading();
    populate_units(spawn_classes[1], champions_wanted, level, sections, champions);
    view_update_loading();
    populate_units(spawn_classes[3], npcs_wanted, level, sections, npcs);
    view_update_loading();
    populate_units(spawn_classes[4], creeps_wanted, level, sections, creeps);
    view_update_loading();
    // `setRandomizerToNormal(1.0)` ahead of the props.
    sections.set_randomizer_to_normal(1.0f);
    populate_units(spawn_classes[2], props_wanted, level, sections, props);
    view_update_loading();

    // Gold.
    if (gold_wanted > 0 && !population_sections.empty()) {
        tl::core::GoldWorld world;
        world.volatile_rolls = &volatile_rolls;
        world.random_open_item_position = [this](const tl::core::Vec3& at, float radius) {
            return tl::core::random_open_item_position(
                at, 0.0f, radius,
                [this](const tl::core::Vec3& p) {
                    return !passability.ready() || passability.map_passable(p);
                },
                tl::core::volatile_random());
        };
        world.in_no_spawn_region = [this](const tl::core::Vec3& at, float radius) {
            return tl::core::is_in_no_spawn_region(no_spawn_regions, at, radius);
        };
        world.add_gold = [this](int32_t amount, const tl::core::Vec3& at) {
            // `new CItemGold(rm, amount)`,
            // `addItem(gold, at, false)`.
            place_gold(tl::core::make_gold_pile(amount, tl::core::volatile_random(), gold_sounds()),
                       ogre::Vector3(at.x, at.y, at.z), nullptr, /*drop_sound=*/false);
        };
        const int piles =
            tl::core::populate_gold(population_sections, sections, gold_wanted, g_rand, world);
        TL_LOG_INFO("  gold: %d of %u pile(s) placed", piles, gold_wanted);
    }
    view_update_loading();

    if (!spawn_classes[6].empty()) {
        TL_LOG_INFO("  fish: '%s' named, not placed - this engine has no fishing spots",
                    spawn_classes[6].c_str());
    }
}

}  // namespace tl::core
