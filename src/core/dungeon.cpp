#include "core/dungeon.h"
#include "core/fused.h"
#include "core/level_memory.h"
#include "core/log.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <utility>

namespace tl::core {

namespace {

/// `STRINGS::StringUpper`. The data these paths come from is ASCII.
// PORT: ASCII case folding, the case-blind compare of names in the port's narrow strings.
std::string upper(std::string text) {
    for (char& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return text;
}

/// PowerPC's `fnmsubs`: `-(a * b - c)` rounded once, so `c - a * b` that comes
/// to exactly zero is -0 (the console steps a chunk back to the origin as -0).
// PORT: a name for it.
float fnmsubs(float a, float b, float c) { return fnmsub(a, b, c); }

/// The directory `CLevelTemplateData::load` prefixes folders with: the path
/// minus `FILESYSTEM::GetFileName`'s answer.
std::string directory_of(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

/// Which side of its chunk a doorway faces, from its offset `dz` along the
/// chunk's depth (measured from the middle) and `dx` across it. `load`,
/// `exitsMatch`, `matchExits` and `createRandomLayout` each inline this test
/// with strict comparisons in this order, so an exactly diagonal offset, or a
/// doorway on the centre point, faces no side.
enum class Facing { kNone, kPosZ, kNegZ, kPosX, kNegX };

// PORT: which side of a chunk an exit faces, from its offset (the generator's helper).
Facing facing_of(float dz, float dx) {
    if (0.0f < dz && std::fabs(dx) < dz) return Facing::kPosZ;
    if (dz < 0.0f && std::fabs(dx) < std::fabs(dz)) return Facing::kNegZ;
    if (0.0f < dx && std::fabs(dz) < dx) return Facing::kPosX;
    if (dx < 0.0f && std::fabs(dz) < std::fabs(dx)) return Facing::kNegX;
    return Facing::kNone;
}

// PORT: whether a chunk's exit is joined (the generator's helper).
bool is_connected(const ChunkInstance& chunk, int32_t exit_index) {
    for (int32_t joined : chunk.connected_exits) {
        if (joined == exit_index) return true;
    }
    return false;
}

// PORT: a placed chunk record made (the generator's helper).
ChunkInstance make_instance(uint32_t type_index, float x, float y, float z) {
    ChunkInstance instance;
    instance.type_index = type_index;
    instance.x = x;
    instance.y = y;
    instance.z = z;
    return instance;
}

// `createRandomLayout`'s draw passed over: a type with one doorway drawn first, then a type with
// no pieces, in that order.
bool drawn_type_passed_over(const std::vector<Randomizer>& piece_randomizers, uint32_t t,
                            bool first_with_one_doorway) {
    if (first_with_one_doorway) {
        TL_LOG_DEBUG("  layout: type %u drawn first with one doorway", t);
        return true;
    }
    if (tarray_at(piece_randomizers, t).choice_count() != 0) return false;
    TL_LOG_DEBUG("  layout: type %u drawn with no pieces", t);
    return true;
}

}  // namespace

// `CDungeon::loadDungeon(wchar_t const*)`.
DungeonDef parse_dungeon_def(const tl::formats::AdmFile& file) {
    DungeonDef def;
    if (!file.ok) return def;

    def.name = file.string_or("NAME", "");
    def.upper_name = upper(def.name);
    def.parent_dungeon = file.string_or("PARENT_DUNGEON", "");
    // `loadDungeon`: an absent `DISPLAYNAME` is empty, not
    // the `NAME`.
    def.display_name = file.string_or("DISPLAYNAME", "");
    def.is_volatile = file.bool_or("VOLATILE", true);
    def.monster_level_mult = file.float_or("MONSTER_LVL_MULT", 1.0f);
    def.player_level_match_min = static_cast<int>(file.integer_or("PLAYER_LVL_MATCH_MIN", 1));
    def.player_level_match_max = static_cast<int>(file.integer_or("PLAYER_LVL_MATCH_MAX", 1));
    def.player_level_match_offset = static_cast<int>(file.integer_or("PLAYER_LVL_MATCH_OFFSET", 0));
    def.bottomless = file.bool_or("BOTTOMLESS", false);

    // `CDataGroup::GetDataGroupByName(L"STRATA" + n)`: the strata in name
    // order, stopping at the first gap.
    for (uint32_t n = 0;; ++n) {
        const std::string wanted = "STRATA" + std::to_string(n);
        const tl::formats::AdmSection* found = nullptr;
        for (const tl::formats::AdmSection& child : file.root.children) {
            if (child.name == wanted) {
                found = &child;
                break;
            }
        }
        if (found == nullptr) break;
        const tl::formats::AdmSection& child = *found;

        DungeonStrata s;
        // `getStrataTemplate`'s default when `RULESET` is missing.
        s.ruleset = child.string_or("RULESET", "media/layouts/crypt/rules.dat");
        s.floors = std::max(1, static_cast<int>(child.integer_or("FLOORS", 1)));
        s.weight = static_cast<int>(child.integer_or("WEIGHT", 1));
        s.dungeon = child.string_or("DUNGEON", "");
        s.name_override = child.string_or("NAME_OVERRIDE", "");
        s.cinematic = child.string_or("CINEMATIC", "");
        s.no_random_quests = child.bool_or("NO_RANDOM_QUESTS", false);
        s.dont_store = child.bool_or("DONTSTORE", false);
        s.allow_portals = child.bool_or("ALLOW_PORTALS", true);
        s.allow_pet_return = child.bool_or("ALLOW_PET_RETURN", true);
        s.is_town = child.bool_or("IS_TOWN", false);
        // `CDungeon::loadDungeon`: the
        // stratum's two quest groups, every string value in each, in file
        // order.
        for (const tl::formats::AdmSection& group : child.children) {
            std::vector<std::string>* list = nullptr;
            if (group.name == "MUST_BE_COMPLETED") list = &s.must_be_completed;
            if (group.name == "MUST_BE_COMPLETED_OR_ACTIVE") list = &s.must_be_completed_or_active;
            if (list == nullptr) continue;
            for (const tl::formats::AdmRecord& record : group.records) {
                if (!record.text.empty()) list->push_back(record.text);
            }
        }
        // `g_LEVELLAYOUT_CREATION_NAMES` and `..._RANDOMIZED_NAMES`, read by
        // `CDungeon::getStrataTemplate`.
        static const char* const kSlotKeys[kSpawnSlotCount] = {
            "MONSTERSPAWNCLASS",      "CHAMPIONSPAWNCLASS",
            "PROPSPAWNCLASS",         "NPCSPAWNCLASS",
            "CREEPSPAWNCLASS",        "GOLD",
            "FISHSPAWNCLASS",         "FORMATIONS",
            "QUESTMONSTERSPAWNCLASS", "QUESTITEMSPAWNCLASS",
            "QUESTCHAMPIONSPAWNCLASS"};
        for (size_t slot = 0; slot < kSpawnSlotCount; ++slot) {
            const std::string key = kSlotKeys[slot];
            s.spawn_slot_authored[slot] = child.find(key) != nullptr;
            s.spawn_slot_names[slot] = child.string_or(key, "");
            s.spawn_slot_randomized_authored[slot] = child.find(key + "RANDOMIZED") != nullptr;
            s.spawn_slot_randomized[slot] = child.bool_or(key + "RANDOMIZED", false);
        }

        // One depth per floor, each a choice valued by its depth.
        const uint32_t stratum_index = static_cast<uint32_t>(def.strata.size());
        for (int f = 0; f < s.floors; ++f) {
            def.depth_weights.add_choice(static_cast<int32_t>(def.depth_strata.size()), s.weight);
            def.depth_strata.push_back(stratum_index);
        }

        // `g_LEVELLAYOUT_CREATION_PATHNODES` and `..._COUNTS`,
        // read per slot by `CDungeon::getStrataTemplate`. The tables stop before the
        // quest slots, and the counts table has no formations pair. `PPROPS_MAX` is
        // the binary's own spelling.
        static const char* const kDensityKeys[kSpawnSlotCount][2] = {
            {"MONSTERS_PER_METER_MIN", "MONSTERS_PER_METER_MAX"},
            {"CHAMPIONS_PER_METER_MIN", "CHAMPIONS_PER_METER_MAX"},
            {"PROPS_PER_METER_MIN", "PROPS_PER_METER_MAX"},
            {"NPCS_PER_METER_MIN", "NPCS_PER_METER_MAX"},
            {"CREEPS_PER_METER_MIN", "CREEPS_PER_METER_MAX"},
            {"GOLD_PER_METER_MIN", "GOLD_PER_METER_MAX"},
            {"FISH_PER_METER_MIN", "FISH_PER_METER_MAX"},
            {"FORMATIONS_PER_METER_MIN", "FORMATIONS_PER_METER_MAX"},
            {nullptr, nullptr},
            {nullptr, nullptr},
            {nullptr, nullptr}};
        static const char* const kCountKeys[kSpawnSlotCount][2] = {
            {"MONSTER_MIN", "MONSTER_MAX"},
            {"CHAMPIONS_MIN", "CHAMPIONS_MAX"},
            {"PROPS_MIN", "PPROPS_MAX"},
            {"NPCS_MIN", "NPCS_MAX"},
            {"CREEPS_MIN", "CREEPS_MAX"},
            {"GOLD_MIN", "GOLD_MAX"},
            {"FISH_MIN", "FISH_MAX"},
            {nullptr, nullptr},
            {nullptr, nullptr},
            {nullptr, nullptr},
            {nullptr, nullptr}};
        for (size_t slot = 0; slot < kSpawnSlotCount; ++slot) {
            if (kDensityKeys[slot][0] != nullptr) {
                s.density_min_authored[slot] = child.find(kDensityKeys[slot][0]) != nullptr;
                s.density_max_authored[slot] = child.find(kDensityKeys[slot][1]) != nullptr;
                s.density_min[slot] = child.float_or(kDensityKeys[slot][0], 0.0f);
                s.density_max[slot] = child.float_or(kDensityKeys[slot][1], 0.0f);
            }
            if (kCountKeys[slot][0] != nullptr) {
                s.count_min_authored[slot] = child.find(kCountKeys[slot][0]) != nullptr;
                s.count_max_authored[slot] = child.find(kCountKeys[slot][1]) != nullptr;
                s.count_min[slot] = child.float_or(kCountKeys[slot][0], 0.0f);
                s.count_max[slot] = child.float_or(kCountKeys[slot][1], 0.0f);
            }
        }

        s.monster_lvl_min = static_cast<int>(child.integer_or("MONSTER_LVL_MIN", -1));
        s.monster_lvl_max = static_cast<int>(child.integer_or("MONSTER_LVL_MAX", -1));

        def.strata.push_back(std::move(s));
    }

    def.ok = !def.name.empty() && !def.depth_strata.empty();
    return def;
}

// CGameClient::getRankOfMonstersOnFloor
int32_t rank_of_monsters_on_floor(int32_t depth, const DungeonDef* dungeon,
                                  const TravelRecord* player, int32_t player_level,
                                  bool use_player) {
    if (dungeon == nullptr) {
        return depth < 1 ? 1 : depth;
    }

    int32_t base = depth + 1;
    if (base < 1) {
        base = 1;
    }
    // `fmadds`: one rounding, then `fctiwz`.
    base =
        static_cast<int32_t>(std::fma(dungeon->monster_level_mult, static_cast<float>(base), 0.5f));
    if (base < 1) {
        base = 1;
    }

    if (!use_player || player == nullptr) {
        return std::max(1, base);
    }

    int32_t effective_level = 0;
    if (has_dungeon_history(*player, dungeon->name)) {
        effective_level = get_dungeon_rank(*player, dungeon->name);
        if (effective_level == 0) {
            effective_level = player_level;
        }
    } else {
        effective_level = player_level;
    }

    if (effective_level == 0) {
        return std::max(1, base);
    }

    const int32_t min_lvl = dungeon->player_level_match_min;
    const int32_t max_lvl = dungeon->player_level_match_max;
    if (min_lvl != 0 || max_lvl > 0) {
        int32_t clamped = effective_level;
        if (clamped < min_lvl) {
            clamped = min_lvl;
        }
        if (clamped > max_lvl) {
            clamped = max_lvl;
        }
        base = base + dungeon->player_level_match_offset - 1 + clamped;
    }

    return std::max(1, base);
}

// CGameClient::loadLevel, the block at
// and its tail.
int32_t level_monster_level(const LevelTemplate* level_template, int32_t rank, bool has_player,
                            int32_t player_level, RunicRandom& volatile_rng) {
    if (level_template == nullptr || level_template->monster_level_min < 1 ||
        level_template->monster_level_max < 1) {
        return std::max(1, rank);
    }

    const int32_t min_lvl = level_template->monster_level_min;
    const int32_t max_lvl = level_template->monster_level_max;

    int32_t level = 0;
    if (has_player) {
        if (player_level <= min_lvl) return min_lvl;
        if (max_lvl <= player_level) return max_lvl;
        level = volatile_rng.integer_between(player_level, max_lvl);
        if (level > 0) return level;
    }
    // Reached with no player, and (unreachably) after a draw of at most 0.
    level = volatile_rng.integer_between(min_lvl, max_lvl);
    if (level > 0) return level;
    // Unreachable: min_lvl >= 1 bounds every draw from below. The original
    // reads the player's level here even with no player.
    return std::max(1, player_level);
}

// ---------------------------------------------------------------------------
// CLevelTemplateData.

// `CLevelTemplateData::load(wchar_t const*)`.
bool LevelTemplate::load(const std::string& path, const tl::formats::AdmFile& file,
                         const LevelTemplateFiles& files, RunicRandom& rng) {
    if (path.empty() || !file.ok) return false;
    const std::string dir = directory_of(path);

    monster_level_min = static_cast<int32_t>(
        file.integer_or("MONSTER_LVL_MIN", static_cast<uint32_t>(monster_level_min)));
    monster_level_max = static_cast<int32_t>(
        file.integer_or("MONSTER_LVL_MAX", static_cast<uint32_t>(monster_level_max)));
    no_random_quests = file.bool_or("NO_RANDOM_QUESTS", false);
    tile_basis = file.float_or("TILEBASIS", tile_basis);
    chunk_width_basis = file.float_or("CHUNKWIDTHBASIS", chunk_width_basis);
    chunk_height_basis = file.float_or("CHUNKHEIGHTBASIS", chunk_height_basis);
    requires_exit = file.bool_or("REQUIRESEXIT", requires_exit);
    dont_store = file.bool_or("DONTSTORE", dont_store);
    randomized = file.bool_or("RANDOMIZED", randomized);
    has_pathing = file.bool_or("HASPATHING", has_pathing);
    has_automap = file.bool_or("HASAUTOMAP", has_automap);
    min_chunks =
        static_cast<int32_t>(file.integer_or("MINCHUNKS", static_cast<uint32_t>(min_chunks)));
    max_chunks =
        static_cast<int32_t>(file.integer_or("MAXCHUNKS", static_cast<uint32_t>(max_chunks)));
    populate = file.bool_or("POPULATE", populate);
    camera_mult = file.float_or("CAMERAMULT", 1.0f);
    active_range_mod = file.float_or("ACTIVERANGEMOD", 0.0f);
    level_name = file.string_or("LEVELNAME", level_name);
    cinematic = file.string_or("CINEMATIC", cinematic);
    level_particle = file.string_or("LEVELPARTICLE", level_particle);
    main_menu_rules = file.string_or("MAINMENURULES", main_menu_rules);
    show_floor_number = file.bool_or("SHOWFLOORNUMBER", show_floor_number);
    name = upper(file.string_or("NAME", ""));

    // `CDataGroup::GetDataGroupsMatchingName(L"CHUNKTYPE")`, taken as an exact
    // name match.
    size_t type_groups = 0;
    for (const tl::formats::AdmSection& group : file.root.children) {
        if (group.name != "CHUNKTYPE") continue;
        ++type_groups;

        ChunkType type;
        type.name = group.string_or("NAME", "");
        type.must_place = group.bool_or("MUST_PLACE", false);
        type.entrance = group.bool_or("ENTRANCE_CHUNK", false);
        type.exit = group.bool_or("EXIT_CHUNK", false);
        type.max_appearance = static_cast<int32_t>(group.integer_or("MAX_APPEARANCE", 10));
        // The test is meant to skip a folder that already ends in
        // a separator, but its second comparison can never succeed, so a '/'
        // is always appended.
        type.folder = dir + group.string_or("FOLDER", type.name) + "/";
        type.width = static_cast<int32_t>(group.integer_or("WIDTH", 1));
        type.height = static_cast<int32_t>(group.integer_or("HEIGHT", 1));
        if (type.must_place) must_place.push_back(static_cast<uint32_t>(chunk_types.size()));

        for (const tl::formats::AdmSection& exit_group : group.children) {
            if (exit_group.name != "EXIT") continue;
            ChunkExit exit;
            exit.x = exit_group.float_or("X", 0.0f);
            exit.y = exit_group.float_or("Y", 0.0f);
            exit.z = exit_group.float_or("Z", 0.0f);
            type.exits.push_back(exit);
        }

        // `GetDataGroupByName(L"INCLUSIVE_FILES")`: the first such group. Every
        // string value in it names a piece in the type's folder; the ones that
        // exist replace the folder listing.
        for (const tl::formats::AdmSection& inclusive : group.children) {
            if (inclusive.name != "INCLUSIVE_FILES") continue;
            for (const tl::formats::AdmRecord& record : inclusive.records) {
                if (record.type != tl::formats::AdmType::String &&
                    record.type != tl::formats::AdmType::Translate) {
                    continue;
                }
                const std::string piece = upper(type.folder + record.text + ".LAYOUT");
                if (files.exists && files.exists(piece)) type.inclusive_files.push_back(piece);
            }
            break;
        }

        chunk_types.push_back(std::move(type));
    }
    for (size_t i = 0; i < type_groups; ++i)
        piece_randomizers.emplace_back(RandomizerType::kNormal);

    // Each type's pieces, sorted, and the type into its pools at its first
    // piece. A type with no piece is in no pool, so it can never be placed.
    for (uint32_t t = 0; t < chunk_types.size(); ++t) {
        ChunkType& type = chunk_types[t];
        std::vector<std::string> listed;
        if (!type.inclusive_files.empty()) {
            listed = type.inclusive_files;
        } else if (files.list_layouts) {
            listed = files.list_layouts(type.folder);
        }
        std::vector<std::string> pieces;
        for (std::string& piece : listed) {
            if (piece.find("/MERGE/") == std::string::npos) pieces.push_back(std::move(piece));
        }
        std::sort(pieces.begin(), pieces.end());

        for (std::string& piece : pieces) {
            Chunk chunk;
            chunk.type_index = t;
            chunk.max_appearance = static_cast<uint32_t>(type.max_appearance);
            chunk.path = std::move(piece);
            tarray_at(piece_randomizers, t).add_choice(static_cast<int32_t>(chunks.size()), 1);
            chunks.push_back(std::move(chunk));

            if (type.registered) continue;
            type.registered = true;
            if (type.entrance) {
                entrance_types.add_choice(static_cast<int32_t>(t), 1);
                continue;
            }
            const bool single_exit = type.exits.size() == 1;
            Facing facing = Facing::kNone;
            if (single_exit) {
                const ChunkExit& e = type.exits[0];
                // `fnmsubs` of the half height.
                facing = facing_of(
                    fnmsubs(static_cast<float>(type.height) * tile_basis * chunk_height_basis, 0.5f,
                            e.z),
                    e.x);
            }
            if (!type.exit) {
                ordinary_types.add_choice(static_cast<int32_t>(t), 1);
                if (!single_exit) continue;
                switch (facing) {
                    case Facing::kPosZ:
                        dead_ends_facing_pos_z.add_choice(static_cast<int32_t>(t), 1);
                        break;
                    case Facing::kNegZ:
                        dead_ends_facing_neg_z.add_choice(static_cast<int32_t>(t), 1);
                        break;
                    case Facing::kPosX:
                        dead_ends_facing_pos_x.add_choice(static_cast<int32_t>(t), 1);
                        break;
                    case Facing::kNegX:
                        dead_ends_facing_neg_x.add_choice(static_cast<int32_t>(t), 1);
                        break;
                    case Facing::kNone: break;
                }
            } else if (single_exit) {
                switch (facing) {
                    case Facing::kPosZ:
                        exits_facing_pos_z.add_choice(static_cast<int32_t>(t), 1);
                        break;
                    case Facing::kNegZ:
                        exits_facing_neg_z.add_choice(static_cast<int32_t>(t), 1);
                        break;
                    case Facing::kPosX:
                        exits_facing_pos_x.add_choice(static_cast<int32_t>(t), 1);
                        break;
                    case Facing::kNegX:
                        exits_facing_neg_x.add_choice(static_cast<int32_t>(t), 1);
                        break;
                    case Facing::kNone: break;
                }
            }
        }
    }

    // The fixed layouts: each `[LAYOUT]` group's `CHUNK_RANDOM` entries, with
    // their room pieces rolled now, from whatever `rng` holds.
    layout_randomizer = Randomizer(RandomizerType::kNormal);
    has_layout_randomizer = true;
    if (!chunks.empty()) {
        for (const tl::formats::AdmSection& group : file.root.children) {
            if (group.name != "LAYOUT") continue;
            layout_randomizer.add_choice(static_cast<int32_t>(layouts.size()), 1);
            LevelLayout layout;
            std::vector<uint32_t> appearances(chunks.size(), 0);
            for (const tl::formats::AdmSection& entry : group.children) {
                if (entry.name != "CHUNK_RANDOM") continue;
                const std::string type_name = entry.string_or("TYPE", "");
                uint32_t type_index = 0xffffffffu;
                for (uint32_t i = 0; i < chunk_types.size(); ++i) {
                    if (chunk_types[i].name == type_name) {
                        type_index = i;
                        break;
                    }
                }
                ChunkInstance instance =
                    make_instance(type_index, entry.float_or("X", 0.0f), entry.float_or("Y", 0.0f),
                                  entry.float_or("Z", 0.0f));
                const uint32_t piece =
                    static_cast<uint32_t>(tarray_at(piece_randomizers, type_index).get_random(rng));
                appearances[piece] += 1;
                const uint32_t chunk = piece < chunks.size() ? piece : 0;
                if (chunks[chunk].max_appearance <= appearances[piece]) {
                    tarray_at(piece_randomizers, type_index)
                        .set_choice_odds(static_cast<int32_t>(piece), 0);
                }
                layout.instances.push_back(std::move(instance));
                layout.pieces.push_back(chunk);
            }
            layouts.push_back(std::move(layout));
            reset_odds();
        }
        fixed_layout_count = layouts.size();
    }
    return true;
}

// `CLevelTemplateData::resetOdds()`.
void LevelTemplate::reset_odds() {
    for (size_t i = 0; i < piece_randomizers.size(); ++i) {
        Randomizer& pieces = tarray_at(piece_randomizers, i);
        const uint32_t count = pieces.choice_count();
        for (uint32_t j = 0; j < count; ++j) {
            const int32_t chunk = pieces.value_at(j);
            pieces.set_choice_odds(chunk, tarray_at(chunks, static_cast<uint32_t>(chunk)).odds);
        }
    }
}

// `CLevelTemplateData::getRandomChunk(unsigned int)`.
uint32_t LevelTemplate::get_random_chunk(uint32_t type_index, RunicRandom& rng) {
    const uint32_t piece =
        static_cast<uint32_t>(tarray_at(piece_randomizers, type_index).get_random(rng));
    return piece < chunks.size() ? piece : 0;
}

// `CLevelTemplateData::intersectsChunks`. Axis-aligned boxes, a
// tenth of a unit shy on each far edge, and 2000 units tall. The depth is
// `CHUNKHEIGHTBASIS * WIDTH`, not `* HEIGHT`: the original reads the width
// field for both.
bool LevelTemplate::intersects_chunks(const ChunkInstance& chunk,
                                      const std::vector<ChunkInstance>& placed) const {
    const ChunkType& type = type_at(chunk.type_index);
    const float tile = tile_basis;
    const float span = chunk_width_basis * tile - 0.1f;
    for (const ChunkInstance& other : placed) {
        const ChunkType& other_type = type_at(other.type_index);
        if (span * -0.5f + other.x <=
                (static_cast<float>(type.width) - 1.0f) * span + span * 0.5f + chunk.x &&
            other.y + -1000.0f <= chunk.y + 1000.0f &&
            other.z + 0.0f <=
                (chunk_height_basis * static_cast<float>(type.width) * tile - 0.1f) + chunk.z &&
            chunk.x + span * -0.5f <=
                (static_cast<float>(other_type.width) - 1.0f) * span + span * 0.5f + other.x &&
            chunk.y + -1000.0f <= other.y + 1000.0f &&
            chunk.z + 0.0f <=
                (static_cast<float>(other_type.width) * chunk_height_basis * tile - 0.1f) +
                    other.z) {
            return true;
        }
    }
    return false;
}

// `CLevelTemplateData::exitsMatch`. For each doorway, a probe
// point one and a half tiles outside it must land on a doorway (within two
// tiles) of whichever placed chunk it falls inside.
//
// Kept as the binary has it: a probe that falls outside the *first*
// placed chunk - the entrance - accepts that doorway at once, without looking
// at the others. Only a probe inside the entrance, or one inside a later chunk
// after the entrance was hit and missed, is ever checked. The final pass in
// `createRandomLayout` is what rejects the walls this lets through.
bool LevelTemplate::exits_match(const ChunkInstance& chunk,
                                const std::vector<ChunkInstance>& placed) const {
    const ChunkType& type = type_at(chunk.type_index);
    const float tile = tile_basis;
    const float centre_z =
        static_cast<float>(type.height) * tile * chunk_height_basis * 0.5f + chunk.z;
    const int32_t exit_count = static_cast<int32_t>(type.exits.size());
    for (int32_t k = 0; k < exit_count; ++k) {
        const ChunkExit& exit = tarray_at(type.exits, static_cast<size_t>(k));
        float probe_z = chunk.z + exit.z;
        float probe_x = exit.x + chunk.x;
        const float probe_y = exit.y + chunk.y;
        switch (facing_of(probe_z - centre_z, probe_x - chunk.x)) {
            case Facing::kPosZ: probe_z = probe_z + 1.5f * tile; break;
            case Facing::kNegZ: probe_z = probe_z + -1.5f * tile; break;
            case Facing::kPosX: probe_x = probe_x + 1.5f * tile; break;
            case Facing::kNegX: probe_x = probe_x + -1.5f * tile; break;
            case Facing::kNone: break;
        }
        if (placed.empty()) continue;

        bool matched = false;
        for (size_t j = 0; j < placed.size() && !matched; ++j) {
            const ChunkInstance& other = placed[j];
            const ChunkType& other_type = type_at(other.type_index);
            const float span = chunk_width_basis * tile - 0.1f;
            const bool outside =
                probe_x < span * -0.5f + other.x ||
                span * 0.5f + (static_cast<float>(other_type.width) - 1.0f) * span + other.x <
                    probe_x ||
                probe_y < other.y + -1000.0f || other.y + 1000.0f < probe_y ||
                probe_z < other.z + 0.0f ||
                (static_cast<float>(other_type.width) * chunk_height_basis * tile - 0.1f) +
                        other.z <
                    probe_z;
            if (outside) {
                if (j == 0) matched = true;
                continue;
            }
            const int32_t other_exits = static_cast<int32_t>(other_type.exits.size());
            for (int32_t m = 0; m < other_exits; ++m) {
                const ChunkExit& door = tarray_at(other_type.exits, static_cast<size_t>(m));
                const float dx = (door.x + other.x) - probe_x;
                const float dy = (door.y + other.y) - probe_y;
                const float dz = (door.z + other.z) - probe_z;
                if (std::sqrt(dx * dx + dy * dy + dz * dz) < tile + tile) {
                    matched = true;
                    break;
                }
            }
        }
        if (!matched) return false;
    }
    return true;
}

// `CLevelTemplateData::matchExits`: every
// doorway whose probe lands within two tiles of a doorway of a placed chunk it
// falls inside is joined to it, on both sides, once per pair found.
void LevelTemplate::match_exits(ChunkInstance& chunk, std::vector<ChunkInstance>& placed) const {
    const ChunkType& type = type_at(chunk.type_index);
    const float tile = tile_basis;
    const int32_t exit_count = static_cast<int32_t>(type.exits.size());
    for (int32_t k = 0; k < exit_count; ++k) {
        const ChunkExit& exit = tarray_at(type.exits, static_cast<size_t>(k));
        // the doorway's point, its offset from
        // the chunk's middle (the half height fused on), and a step of a tile
        // and a half out through it, fused.
        float probe_x = exit.x + chunk.x;
        const float probe_y = chunk.y + exit.y;
        float probe_z = chunk.z + exit.z;
        const float centre_z =
            std::fma(static_cast<float>(type.height) * tile * chunk_height_basis, 0.5f, chunk.z);
        switch (facing_of(probe_z - centre_z, probe_x - chunk.x)) {
            case Facing::kPosZ: probe_z = std::fma(tile, 1.5f, probe_z); break;
            case Facing::kNegZ: probe_z = fnmsubs(tile, 1.5f, probe_z); break;
            case Facing::kPosX: probe_x = std::fma(tile, 1.5f, probe_x); break;
            case Facing::kNegX: probe_x = fnmsubs(tile, 1.5f, probe_x); break;
            case Facing::kNone: break;
        }

        for (ChunkInstance& other : placed) {
            const ChunkType& other_type = type_at(other.type_index);
            // the other chunk's box, made into an
            // `Ogre::AxisAlignedBox` and asked `intersects(point)`.
            // The depth is reckoned from the other chunk's
            // `WIDTH`, as the console does.
            const float span = std::fma(tile, chunk_width_basis, -0.1f);
            const float width = static_cast<float>(other_type.width);
            const float min_x = other.x + span * -0.5f;
            const float max_x = other.x + ((width - 1.0f) + 0.5f) * span;
            const float max_z = other.z + std::fma(width * tile, chunk_height_basis, -0.1f);
            // The test's own branches (`blt`/`bgt` out, the last `ble` in), so
            // a NaN fails only the last comparison.
            const bool inside = !(probe_x < min_x) && !(probe_x > max_x) &&
                                !(probe_y < other.y - 1000.0f) && !(probe_y > other.y + 1000.0f) &&
                                !(probe_z < other.z) && probe_z <= max_z;
            if (!inside) continue;
            const int32_t other_exits = static_cast<int32_t>(other_type.exits.size());
            for (int32_t m = 0; m < other_exits; ++m) {
                const ChunkExit& door = tarray_at(other_type.exits, static_cast<size_t>(m));
                // y squared, z and x fused on.
                const float dx = (door.x + other.x) - probe_x;
                const float dy = (other.y + door.y) - probe_y;
                const float dz = (other.z + door.z) - probe_z;
                if (std::sqrt(std::fma(dx, dx, std::fma(dz, dz, dy * dy))) < tile * 2.0f) {
                    other.connected_exits.push_back(m);
                    chunk.connected_exits.push_back(k);
                }
            }
        }
    }
}

// `CLevelTemplateData::createRandomLayout(int)`.
//
//
// One attempt: an entrance at the origin; ordinary chunks grown off unjoined
// doorways until `MINCHUNKS..MAXCHUNKS` of them stand; with `REQUIRESEXIT`, the
// first doorway an exit chunk can cap; then every doorway still open capped
// with a dead end; every `MUST_PLACE` type checked; and a room piece rolled per
// chunk. Any failure throws the attempt away and starts again, up to 1000
// attempts, and a layout is added only when an attempt succeeds. The same
// random draws happen in the same order as in the binary, including the ones a
// failing attempt still makes after it has failed.
int32_t LevelTemplate::create_random_layout(RunicRandom& rng, int32_t seed) {
    if (entrance_types.choice_count() == 0) return 0;
    if (seed != 0) rng.set_seed(seed);
    layout_created = true;

    const float tile = tile_basis;
    std::vector<ChunkInstance> instances;
    std::vector<uint32_t> pieces;

    for (int32_t attempt = 0;;) {
        instances.push_back(
            make_instance(static_cast<uint32_t>(entrance_types.get_random(rng)), 0.0f, 0.0f, 0.0f));
        std::vector<uint32_t> appearances(chunks.size(), 0);
        const int32_t target = rng.integer_between(min_chunks, max_chunks);

        // Ordinary chunks. `tries` counts only a type that was actually
        // tried: a single-doorway type drawn for the first chunk, or a type
        // with no pieces, is drawn again without counting.
        int32_t tries = 0;
        int32_t placed = 0;
        while (tries <= 999 && placed < target) {
            const uint32_t t = static_cast<uint32_t>(ordinary_types.get_random(rng));
            const ChunkType& type = type_at(t);
            const int32_t exit_count = static_cast<int32_t>(type.exits.size());
            if (drawn_type_passed_over(piece_randomizers, t, placed == 0 && exit_count == 1))
                continue;

            const int32_t existing = static_cast<int32_t>(instances.size());
            bool added = false;
            for (int32_t e = 0; e < exit_count && !added; ++e) {
                const ChunkExit& exit = tarray_at(type.exits, static_cast<size_t>(e));
                // Where the chunk's doorway sits in tiles from its edge; a
                // matching doorway on the other chunk shows its negation. The
                // console's arithmetic: one
                // reciprocal of the tile, and the edges fused (`fnmsubs`).
                const float per_tile = 1.0f / tile;
                const float door_x = fnmsubs((static_cast<float>(type.width) - 1.0f) * tile,
                                             chunk_width_basis, exit.x) *
                                     per_tile;
                const float door_z = fnmsubs(static_cast<float>(type.height) * 0.5f * tile,
                                             chunk_height_basis, std::fma(tile, 0.5f, exit.z)) *
                                     per_tile;
                const int32_t start = rng.integer_between(0, existing - 1);

                int32_t index = start;
                for (int32_t j = 0; j < existing && !added; ++j, ++index) {
                    const int32_t wrapped = index < existing ? index : index - existing;
                    const ChunkInstance& other = instances[static_cast<size_t>(wrapped)];
                    const ChunkType& other_type = type_at(other.type_index);
                    const int32_t other_exits = static_cast<int32_t>(other_type.exits.size());
                    for (int32_t m = 0; m < other_exits; ++m) {
                        if (is_connected(other, m)) continue;
                        const ChunkExit& door = tarray_at(other_type.exits, static_cast<size_t>(m));
                        float nx = other.x;
                        const float ny = other.y;
                        float nz = other.z;
                        // the same reckoning of
                        // the other doorway (its height's half by the basis,
                        // then by the tile), and the step fused onto the
                        // other chunk's position - so a step back to 0 is -0.
                        const float other_per_tile = 1.0f / tile;
                        const float have_x =
                            fnmsubs((static_cast<float>(other_type.width) - 1.0f) * tile,
                                    chunk_width_basis, door.x) *
                            other_per_tile;
                        const float have_z = fnmsubs(static_cast<float>(other_type.height) * 0.5f *
                                                         chunk_height_basis,
                                                     tile, std::fma(tile, 0.5f, door.z)) *
                                             other_per_tile;
                        if (have_x != 0.0f && have_x == -door_x) {
                            nx = 0.0f < have_x ? std::fma(tile, chunk_width_basis, other.x)
                                               : fnmsubs(tile, chunk_width_basis, other.x);
                        } else {
                            if (!(have_z != 0.0f && have_z == -door_z)) continue;
                            nz = 0.0f < have_z ? std::fma(chunk_height_basis, tile, other.z)
                                               : fnmsubs(chunk_height_basis, tile, other.z);
                        }
                        ChunkInstance candidate = make_instance(t, nx, ny, nz);
                        if (!intersects_chunks(candidate, instances) &&
                            exits_match(candidate, instances)) {
                            match_exits(candidate, instances);
                            instances.push_back(std::move(candidate));
                            ++placed;
                            added = true;
                        }
                        // One candidate per placed chunk, kept or not.
                        break;
                    }
                }
            }
            ++tries;
        }

        bool failed = tries > 999;
        bool reset_needed = false;

        // An exit chunk against the first open doorway one can cap.
        if (!failed && requires_exit) {
            bool capped = false;
            for (size_t i = 0; i < instances.size() && !capped; ++i) {
                const ChunkType& type = type_at(instances[i].type_index);
                const int32_t exit_count = static_cast<int32_t>(type.exits.size());
                for (int32_t k = 0; k < exit_count && !capped; ++k) {
                    if (is_connected(instances[i], k)) continue;
                    const ChunkExit& exit = tarray_at(type.exits, static_cast<size_t>(k));
                    const float x = instances[i].x;
                    const float y = instances[i].y;
                    const float z = instances[i].z;
                    // (the dead ends' twin):
                    // the half height by the basis, then by the
                    // tile, fused onto the chunk's z.
                    const float dz = (z + exit.z) - std::fma(static_cast<float>(type.height) *
                                                                 chunk_height_basis * tile,
                                                             0.5f, z);
                    const float dx = (exit.x + x) - x;
                    // A negative step is sized by the entrance's type, a
                    // positive one by the new chunk's.
                    const ChunkType& entrance = type_at(instances[0].type_index);
                    ChunkInstance candidate;
                    switch (facing_of(dz, dx)) {
                        case Facing::kPosZ: {
                            if (exits_facing_neg_z.choice_count() == 0) continue;
                            const uint32_t t =
                                static_cast<uint32_t>(exits_facing_neg_z.get_random(rng));
                            candidate = make_instance(
                                t, x, y,
                                std::fma(static_cast<float>(type_at(t).height) * chunk_height_basis,
                                         tile, z));
                            break;
                        }
                        case Facing::kNegZ: {
                            if (exits_facing_pos_z.choice_count() == 0) continue;
                            const uint32_t t =
                                static_cast<uint32_t>(exits_facing_pos_z.get_random(rng));
                            candidate = make_instance(
                                t, x, y,
                                fnmsubs(static_cast<float>(entrance.height) * chunk_height_basis,
                                        tile, z));
                            break;
                        }
                        case Facing::kPosX: {
                            if (exits_facing_neg_x.choice_count() == 0) continue;
                            const uint32_t t =
                                static_cast<uint32_t>(exits_facing_neg_x.get_random(rng));
                            candidate = make_instance(
                                t,
                                std::fma(static_cast<float>(type_at(t).width) * chunk_width_basis,
                                         tile, x),
                                y, z);
                            break;
                        }
                        case Facing::kNegX: {
                            if (exits_facing_pos_x.choice_count() == 0) continue;
                            const uint32_t t =
                                static_cast<uint32_t>(exits_facing_pos_x.get_random(rng));
                            candidate = make_instance(
                                t,
                                fnmsubs(static_cast<float>(entrance.width) * chunk_width_basis,
                                        tile, x),
                                y, z);
                            break;
                        }
                        case Facing::kNone: continue;
                    }
                    if (!intersects_chunks(candidate, instances) &&
                        exits_match(candidate, instances)) {
                        match_exits(candidate, instances);
                        instances.push_back(std::move(candidate));
                        capped = true;
                    }
                }
            }
            if (!capped) failed = true;
        }

        if (!failed) {
            // Every doorway still open gets a dead end. A doorway no dead end
            // can cap marks the attempt failed and moves on to the next chunk;
            // the pass itself keeps going, and so do its draws.
            for (size_t i = 0; i < instances.size(); ++i) {
                const ChunkType& type = type_at(instances[i].type_index);
                const int32_t exit_count = static_cast<int32_t>(type.exits.size());
                for (int32_t k = 0; k < exit_count; ++k) {
                    if (is_connected(instances[i], k)) continue;
                    const ChunkExit& exit = tarray_at(type.exits, static_cast<size_t>(k));
                    const float x = instances[i].x;
                    const float y = instances[i].y;
                    const float z = instances[i].z;
                    // (the dead ends' twin):
                    // the half height by the basis, then by the
                    // tile, fused onto the chunk's z.
                    const float dz = (z + exit.z) - std::fma(static_cast<float>(type.height) *
                                                                 chunk_height_basis * tile,
                                                             0.5f, z);
                    const float dx = (exit.x + x) - x;
                    const ChunkType& entrance = type_at(instances[0].type_index);
                    Randomizer* pool = nullptr;
                    Facing facing = facing_of(dz, dx);
                    switch (facing) {
                        case Facing::kPosZ: pool = &dead_ends_facing_neg_z; break;
                        case Facing::kNegZ: pool = &dead_ends_facing_pos_z; break;
                        case Facing::kPosX: pool = &dead_ends_facing_neg_x; break;
                        case Facing::kNegX: pool = &dead_ends_facing_pos_x; break;
                        case Facing::kNone: break;
                    }
                    if (pool == nullptr) continue;
                    if (pool->choice_count() == 0) {
                        failed = true;
                        break;
                    }
                    const uint32_t t = static_cast<uint32_t>(pool->get_random(rng));
                    ChunkInstance candidate;
                    switch (facing) {
                        case Facing::kPosZ:
                            candidate = make_instance(
                                t, x, y,
                                std::fma(static_cast<float>(type_at(t).height) * chunk_height_basis,
                                         tile, z));
                            break;
                        case Facing::kNegZ:
                            candidate = make_instance(
                                t, x, y,
                                fnmsubs(static_cast<float>(entrance.height) * chunk_height_basis,
                                        tile, z));
                            break;
                        case Facing::kPosX:
                            candidate = make_instance(
                                t,
                                std::fma(static_cast<float>(type_at(t).width) * chunk_width_basis,
                                         tile, x),
                                y, z);
                            break;
                        case Facing::kNegX:
                        case Facing::kNone:
                            candidate = make_instance(
                                t,
                                fnmsubs(static_cast<float>(entrance.width) * chunk_width_basis,
                                        tile, x),
                                y, z);
                            break;
                    }
                    if (!intersects_chunks(candidate, instances) &&
                        exits_match(candidate, instances)) {
                        match_exits(candidate, instances);
                        instances.push_back(std::move(candidate));
                        continue;
                    }
                    failed = true;
                    break;
                }
            }

            for (uint32_t wanted : must_place) {
                bool found = false;
                for (const ChunkInstance& instance : instances) {
                    const uint32_t resolved =
                        instance.type_index < chunk_types.size() ? instance.type_index : 0;
                    if (resolved == wanted) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    failed = true;
                    break;
                }
            }

            // A room piece per chunk, even for an attempt already failed.
            for (const ChunkInstance& instance : instances) {
                Randomizer& pool = tarray_at(piece_randomizers, instance.type_index);
                if (!pool.has_valid_choices()) {
                    failed = true;
                    break;
                }
                const uint32_t piece = static_cast<uint32_t>(pool.get_random(rng));
                appearances[piece] += 1;
                const uint32_t chunk = piece < chunks.size() ? piece : 0;
                if (chunks[chunk].max_appearance <= appearances[piece]) {
                    tarray_at(piece_randomizers, instance.type_index)
                        .set_choice_odds(static_cast<int32_t>(piece), 0);
                }
                pieces.push_back(chunk);
            }
            reset_needed = true;
        }

        if (reset_needed) reset_odds();
        if (!failed) {
            layout_randomizer.add_choice(static_cast<int32_t>(layouts.size()), 1);
            LevelLayout layout;
            layout.seed = seed;
            for (size_t i = 0; i < pieces.size(); ++i) {
                layout.instances.push_back(instances[i]);
                layout.pieces.push_back(pieces[i]);
            }
            layouts.push_back(std::move(layout));
            return seed;
        }

        instances.clear();
        pieces.clear();
        if (++attempt == 1000) return seed;
    }
}

// `CLevelTemplateData::getRandomLayout()`.
const LevelLayout* LevelTemplate::get_random_layout(RunicRandom& rng) {
    if (!has_layout_randomizer || layout_randomizer.choice_count() == 0) return nullptr;
    const uint32_t index = static_cast<uint32_t>(layout_randomizer.get_random(rng));
    return &tarray_at(layouts, index);
}

// `CLevelTemplateData::deleteLayouts()`: the
// fixed layouts are taken out of the list, everything left is deleted, and the
// fixed ones go back in their original order with odds of 1 each.
void LevelTemplate::delete_layouts() {
    layouts.resize(fixed_layout_count);
    if (!has_layout_randomizer) {
        layout_randomizer = Randomizer(RandomizerType::kNormal);
        has_layout_randomizer = true;
    } else {
        layout_randomizer.clear();
    }
    layout_created = false;
    for (size_t i = 0; i < fixed_layout_count; ++i) {
        layout_randomizer.add_choice(static_cast<int32_t>(i), 1);
    }
}

// `CLevel::loadRoomLayout(CLevelTemplateData*, ...)`,
// the layout stage. The ten calls are
// unconditional: a failed generation simply adds nothing
// to the pool.
const LevelLayout* choose_room_layout(LevelTemplate& level_template, RunicRandom& rng,
                                      int32_t level_seed) {
    rng.set_seed(level_seed);
    if (!level_template.layout_created && level_template.randomized) {
        for (int i = 0; i != 10; ++i) level_template.create_random_layout(rng, 0);
    }
    rng.set_seed(level_seed);
    return level_template.get_random_layout(rng);
}

// `CLevelTemplateData::chooseUnitSpawners(CResourceManager*)`.
// Only entries whose `CSpawnClassData` holds
// a referenced class (+0x58) take part; the pick is drawn even when none does,
// and then reads entry 0's class.
void choose_unit_spawners(LevelTemplate& level_template, const SpawnClassLookup& lookup,
                          RunicRandom& rng) {
    for (size_t slot = 0; slot < kSpawnSlotCount; ++slot) {
        const std::string& name = level_template.spawn_classes[slot];
        std::string& chosen = level_template.chosen_spawn_classes[slot];
        std::vector<SpawnClassRef> entries;
        if (!level_template.spawn_classes_randomized[slot] || !lookup || !lookup(name, entries)) {
            chosen = name;
            continue;
        }
        Randomizer picks(RandomizerType::kNormal);
        for (size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].spawn_class.empty()) continue;
            const int32_t weight = entries[i].weight;
            picks.add_choice(static_cast<int32_t>(i), weight < 1 ? 100 : weight);
        }
        const uint32_t index = static_cast<uint32_t>(picks.get_random(rng));
        chosen = entries.empty() ? std::string() : tarray_at(entries, index).spawn_class;
    }
}

// `CLevelTemplateData::getNumberOfUnitsToCreate`:
// the ends picked by `min`/`max` (a count
// authored the wrong way round still works) and truncated; the density branch
// multiplies by the float 1 / 6.5 (the Linux build divides) before `ceilf`.
int64_t units_to_create(const LevelTemplate& level_template, size_t slot, uint32_t pathable_cells,
                        RunicRandom& rng) {
    if (slot >= kSpawnSlotCount) return 0;
    const float count_min = level_template.count_min[slot];
    const float count_max = level_template.count_max[slot];
    if (0.0f < count_min || 0.0f < count_max) {
        const float lo = count_max < count_min ? count_max : count_min;
        const float hi = count_max > count_min ? count_max : count_min;
        return rng.integer_between(static_cast<int>(lo), static_cast<int>(hi));
    }
    const float density_min = level_template.density_min[slot];
    const float density_max = level_template.density_max[slot];
    if (!(0.0f < density_min) || !(0.0f < density_max)) return 0;
    const float area = static_cast<float>(pathable_cells) * 0.15384616f;
    const float lo = (density_max < density_min ? density_max : density_min) * area;
    const float hi = (density_max > density_min ? density_max : density_min) * area;
    const float wanted = rng.between(lo, hi);
    // `ceilf`, for the non-negative values this can produce.
    int64_t whole = static_cast<int64_t>(wanted);
    if (static_cast<float>(whole) < wanted) ++whole;
    return whole;
}

// ---------------------------------------------------------------------------
// CDungeon.

Dungeon::Dungeon(DungeonDef definition) : def(std::move(definition)) {
    templates.resize(def.depth_strata.size());
}

// `CDungeon::getStrataTemplate(unsigned int)`.
// The template is constructed (and so
// loaded, drawing any `[LAYOUT]` pieces from `rng`) before the stratum's keys
// are laid onto it.
LevelTemplate* Dungeon::get_strata_template(uint32_t depth, const DungeonFiles& files,
                                            RunicRandom& rng, const DungeonStrata** source) {
    if (depth >= templates.size()) return nullptr;
    const DungeonStrata& strata = def.strata[def.depth_strata[depth]];
    if (source != nullptr) *source = &strata;
    std::unique_ptr<LevelTemplate>& slot = templates[depth];
    if (slot) return slot.get();

    auto built = std::make_unique<LevelTemplate>();
    const tl::formats::AdmFile rules =
        files.read_ruleset ? files.read_ruleset(strata.ruleset) : tl::formats::AdmFile{};
    built->load(strata.ruleset, rules, files.layouts, rng);

    built->cinematic = strata.cinematic;
    if (!strata.name_override.empty()) built->level_name = strata.name_override;
    if (0 < strata.monster_lvl_max && 0 < strata.monster_lvl_min) {
        built->monster_level_min = strata.monster_lvl_min;
        built->monster_level_max = strata.monster_lvl_max;
    }
    if (strata.no_random_quests) built->no_random_quests = true;
    if (strata.dont_store) built->dont_store = true;
    built->allow_portals = strata.allow_portals;
    built->allow_pet_return = strata.allow_pet_return;
    built->is_town = strata.is_town;
    built->redirect_dungeon = strata.dungeon;
    // The spawn slots: the stratum's class and
    // `RANDOMIZED` flag where authored, the template's otherwise.
    for (size_t slot = 0; slot < kSpawnSlotCount; ++slot) {
        if (strata.spawn_slot_authored[slot])
            built->spawn_classes[slot] = strata.spawn_slot_names[slot];
        if (strata.spawn_slot_randomized_authored[slot]) {
            built->spawn_classes_randomized[slot] = strata.spawn_slot_randomized[slot];
        }
        if (strata.density_min_authored[slot]) built->density_min[slot] = strata.density_min[slot];
        if (strata.density_max_authored[slot]) built->density_max[slot] = strata.density_max[slot];
        if (strata.count_min_authored[slot]) built->count_min[slot] = strata.count_min[slot];
        if (strata.count_max_authored[slot]) built->count_max[slot] = strata.count_max[slot];
    }

    slot = std::move(built);
    return slot.get();
}

// `CDungeon::getRandomLevelTemplateData()`.
LevelTemplate* Dungeon::get_random_level_template(const DungeonFiles& files, RunicRandom& rng,
                                                  const DungeonStrata** source) {
    const uint32_t depth = static_cast<uint32_t>(def.depth_weights.get_random(rng));
    return get_strata_template(depth < templates.size() ? depth : 0, files, rng, source);
}

// `CDungeon::getLevelTemplateDataForDepth(CGameClient*, unsigned int)`.
// A dungeon with no depths asks for depth
// 0xffffffff and gets null.
LevelTemplate* Dungeon::get_level_template_for_depth(uint32_t depth, const DungeonFiles& files,
                                                     RunicRandom& rng,
                                                     const DungeonStrata** source) {
    const uint32_t count = static_cast<uint32_t>(templates.size());
    LevelTemplate* found =
        get_strata_template(depth < count ? depth : count - 1, files, rng, source);
    if (found == nullptr || found->redirect_dungeon.empty()) return found;
    Dungeon* other =
        files.dungeon_by_name ? files.dungeon_by_name(upper(found->redirect_dungeon)) : nullptr;
    if (other == nullptr) return found;
    return other->get_random_level_template(files, rng, source);
}

// `CDungeonManager::reload()`. `clear` first;
// the file list is `getFileList(L"MEDIA/DUNGEONS/", list, L"*.dat", true, true,
// false, false)`, which the caller has read into `definitions` in its order.
void DungeonManager::reload(std::vector<DungeonDef> definitions, RunicRandom& g_rand,
                            RunicRandom& volatile_rng) {
    // `CDungeonManager::clear()`: every
    // dungeon deleted, the list emptied.
    dungeons.clear();
    g_rand.set_seed(0);
    for (DungeonDef& definition : definitions) {
        dungeons.push_back(std::make_unique<Dungeon>(std::move(definition)));
        // after the dungeon joins the list.
        dungeons.back()->salt = volatile_rng.integer_between(0, 0x7fffffff);
    }
    loaded = true;
}

// `CDungeonManager::getDungeonByName(std::wstring)`.
//
Dungeon* DungeonManager::dungeon_by_name(const std::string& name) const {
    const std::string wanted = upper(name);
    for (const std::unique_ptr<Dungeon>& dungeon : dungeons) {
        if (dungeon->def.upper_name == wanted) return dungeon.get();
    }
    return nullptr;
}

// PORT: the process's `CDungeonManager`.
DungeonManager& dungeon_manager() {
    static DungeonManager manager;
    return manager;
}

// PORT: the process's `CGameClient` seed fields.
GameSeed& game_seed() {
    static GameSeed seed;
    return seed;
}

// `CGameClient::CGameClient`, `setGameState`.
int32_t roll_game_seed(int32_t level_seed_setting, RunicRandom& volatile_rng) {
    if (level_seed_setting != 0) return level_seed_setting;
    return volatile_rng.integer_between(0, 0x7fffffff);
}

// `CGameClient::resetGameSeed(int)`.
int32_t reset_game_seed(const GameSeed& seed, int32_t depth, RunicRandom& g_rand) {
    const int32_t salt = seed.current_dungeon != nullptr ? seed.current_dungeon->salt : 0;
    if (depth == -1) depth = seed.saved_depth;
    const int32_t value =
        static_cast<int32_t>(static_cast<uint32_t>(salt) + static_cast<uint32_t>(seed.seed) +
                             static_cast<uint32_t>(depth));
    g_rand.set_seed(value);
    return value;
}

// `CLevel::loadRoomLayout(CLevelTemplateData*, ...)`:
// `LEVEL_SEED` overrides `+0x228` (the game seed `CLevel::CLevel` took), then
// `+0x228 + +0x1a4 + salt`.
int32_t level_seed(int32_t level_seed_setting, const GameSeed& seed, int32_t depth) {
    const int32_t base = level_seed_setting != 0 ? level_seed_setting : seed.seed;
    const int32_t salt = seed.current_dungeon != nullptr ? seed.current_dungeon->salt : 0;
    return static_cast<int32_t>(static_cast<uint32_t>(base) + static_cast<uint32_t>(depth) +
                                static_cast<uint32_t>(salt));
}

// `CGameClient::loadLevel`: `LEVELNAME`, or `LEVELNAME - Floor N`
// with N the depth plus one.
std::string level_display_name(const std::string& level_name, bool show_floor_number,
                               int32_t depth) {
    if (!show_floor_number) return level_name;
    // `LEVELNAME + L" - " + g_Floor + L" " + GetValueAsWString(depth + 1)`.
    return level_name + " - " + level_floor_caption(true, depth);
}

// `CGameClient::loadLevel`: the small banner under the level's
// name on arrival, "Floor N" when the template shows floor numbers.
std::string level_floor_caption(bool show_floor_number, int32_t depth) {
    if (!show_floor_number) return std::string();
    // `g_Floor + L" " + GetValueAsWString(CLevel +0x1a4 + 1)`.
    return "Floor " + std::to_string(depth + 1);
}

}  // namespace tl::core
