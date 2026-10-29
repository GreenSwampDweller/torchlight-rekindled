// Procedural dungeons: a dungeon definition, CRandomizer and CLevelTemplateData, on the cave
// biome's numbers and made-up room pieces.
#include "core/dungeon.h"
#include "core/level_memory.h"
#include "core/logic_names.h"
#include "core/random_groups.h"
#include "core/randomizer.h"
#include "test_support.h"

#include <cctype>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

using tl::core::ChunkExit;
using tl::core::ChunkInstance;
using tl::core::DungeonDef;
using tl::core::DungeonStrata;
using tl::core::LevelLayout;
using tl::core::LevelTemplate;
using tl::core::LevelTemplateFiles;
using tl::core::Randomizer;
using tl::core::RandomizerType;
using tl::core::RunicRandom;
using tl::formats::AdmFile;
using tl::formats::AdmRecord;
using tl::formats::AdmSection;
using tl::formats::AdmType;

AdmRecord text_record(const std::string& key, const std::string& value) {
    AdmRecord r;
    r.key = key;
    r.type = AdmType::String;
    r.text = value;
    return r;
}

AdmRecord int_record(const std::string& key, uint32_t value) {
    AdmRecord r;
    r.key = key;
    r.type = AdmType::Integer;
    r.integer = value;
    return r;
}

AdmRecord float_record(const std::string& key, float value) {
    AdmRecord r;
    r.key = key;
    r.type = AdmType::Float;
    r.number = value;
    return r;
}

AdmRecord bool_record(const std::string& key, bool value) {
    AdmRecord r;
    r.key = key;
    r.type = AdmType::Bool;
    r.integer = value ? 1u : 0u;
    return r;
}

/// A `[STRATA0]` block, with the fields `TEST_CAVE.DAT.adm` authors.
AdmSection cave_strata() {
    AdmSection s;
    s.name = "STRATA0";
    s.records = {
        text_record("RULESET", "media/layouts/cave/rules.dat"),
        int_record("FLOORS", 25),
        text_record("MONSTERSPAWNCLASS", "CAVEFLOOR1"),
        float_record("MONSTERS_PER_METER_MIN", 0.015f),
        float_record("MONSTERS_PER_METER_MAX", 0.016f),
        text_record("CHAMPIONSPAWNCLASS", "MONSTERSETCHAMPION_GOTHIC"),
        float_record("CHAMPIONS_MIN", 4.0f),
        float_record("CHAMPIONS_MAX", 6.0f),
        text_record("PROPSPAWNCLASS", "CAVE_PROPS"),
        text_record("NPCSPAWNCLASS", "NPCS"),
        float_record("NPCS_MIN", 1.0f),
        float_record("NPCS_MAX", 1.0f),
        text_record("FISHSPAWNCLASS", "FISH_SPAWN_CAVE"),
    };
    return s;
}

/// `media/dungeons/TEST_CAVE.DAT.adm`, by hand.
AdmFile cave_dungeon_def() {
    AdmFile f;
    f.ok = true;
    f.root.name = "DUNGEON";
    f.root.records = {
        text_record("NAME", "Test_Cave"),
        text_record("PARENT_DUNGEON", "Town"),
        text_record("DISPLAYNAME", "Test_Cave"),
    };
    f.root.children = {cave_strata()};
    return f;
}

bool near(float a, float b, float tol = 1e-3f) { return std::fabs(a - b) <= tol; }

ChunkExit exit_at(float x, float y, float z) { return ChunkExit{x, y, z}; }

/// One `[CHUNKTYPE]`: NAME, the two flags, WIDTH/HEIGHT, MAX_APPEARANCE, then
/// its EXITs.
AdmSection chunk_type(const std::string& name, bool entrance, bool exit_flag,
                      const std::vector<ChunkExit>& exits) {
    AdmSection c;
    c.name = "CHUNKTYPE";
    c.records = {text_record("NAME", name)};
    if (entrance) c.records.push_back(bool_record("ENTRANCE_CHUNK", true));
    if (exit_flag) c.records.push_back(bool_record("EXIT_CHUNK", true));
    c.records.push_back(int_record("WIDTH", 1));
    c.records.push_back(int_record("HEIGHT", 1));
    c.records.push_back(int_record("MAX_APPEARANCE", 1));
    for (const ChunkExit& e : exits) {
        AdmSection exit_section;
        exit_section.name = "EXIT";
        exit_section.records = {float_record("X", e.x), float_record("Y", e.y),
                                float_record("Z", e.z)};
        c.children.push_back(exit_section);
    }
    return c;
}

/// The cave's `[LEVEL]` rules: fourteen `[CHUNKTYPE]`s, no dead ends.
AdmFile cave_rules() {
    AdmFile f;
    f.ok = true;
    f.root.name = "LEVEL";
    f.root.records = {
        text_record("NAME", "Cave"),
        float_record("TILEBASIS", 4.0f),
        float_record("CHUNKWIDTHBASIS", 25.0f),
        float_record("CHUNKHEIGHTBASIS", 25.0f),
        int_record("MINCHUNKS", 4),
        int_record("MAXCHUNKS", 4),
        bool_record("RANDOMIZED", true),
        bool_record("REQUIRESEXIT", true),
    };
    f.root.children = {
        chunk_type("1X1ENTRANCE_E", true, false, {exit_at(-48.0f, 0.0f, 48.0f)}),
        chunk_type("1X1ENTRANCE_N", true, false, {exit_at(0.0f, 0.0f, 96.0f)}),
        chunk_type("1X1ENTRANCE_S", true, false, {exit_at(0.0f, 0.0f, 0.0f)}),
        chunk_type("1X1ENTRANCE_W", true, false, {exit_at(48.0f, 0.0f, 48.0f)}),
        chunk_type("1X1EXIT_N", false, true, {exit_at(0.0f, 0.0f, 96.0f)}),
        chunk_type("1X1EXIT_E", false, true, {exit_at(-48.0f, 0.0f, 48.0f)}),
        chunk_type("1X1EXIT_W", false, true, {exit_at(48.0f, 0.0f, 48.0f)}),
        chunk_type("1X1EXIT_S", false, true, {exit_at(0.0f, 0.0f, 0.0f)}),
        chunk_type("1X1NS", false, false, {exit_at(0.0f, 0.0f, 96.0f), exit_at(0.0f, 0.0f, 0.0f)}),
        chunk_type("1X1NE", false, false,
                   {exit_at(0.0f, 0.0f, 96.0f), exit_at(-48.0f, 0.0f, 48.0f)}),
        chunk_type("1X1NW", false, false,
                   {exit_at(0.0f, 0.0f, 96.0f), exit_at(48.0f, 0.0f, 48.0f)}),
        chunk_type("1X1EW", false, false,
                   {exit_at(48.0f, 0.0f, 48.0f), exit_at(-48.0f, 0.0f, 48.0f)}),
        chunk_type("1X1SE", false, false,
                   {exit_at(-48.0f, 0.0f, 48.0f), exit_at(0.0f, 0.0f, 0.0f)}),
        chunk_type("1X1SW", false, false, {exit_at(48.0f, 0.0f, 48.0f), exit_at(0.0f, 0.0f, 0.0f)}),
    };
    return f;
}

std::string upper(std::string text) {
    for (char& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return text;
}

/// Two pieces per folder, listed out of order, plus one under `/MERGE/` that
/// `load` must drop.
LevelTemplateFiles cave_files() {
    LevelTemplateFiles files;
    files.exists = [](const std::string&) { return true; };
    files.list_layouts = [](const std::string& folder) {
        const std::string up = upper(folder);
        return std::vector<std::string>{up + "B.LAYOUT", up + "MERGE/X.LAYOUT", up + "A.LAYOUT"};
    };
    return files;
}

LevelTemplate loaded_cave() {
    LevelTemplate t;
    RunicRandom load_rng(1u);
    TL_CHECK(t.load("media/layouts/cave/rules.dat", cave_rules(), cave_files(), load_rng));
    return t;
}

bool connected(const ChunkInstance& chunk, int32_t exit_index) {
    for (int32_t e : chunk.connected_exits) {
        if (e == exit_index) return true;
    }
    return false;
}

}  // namespace

TL_TEST(a_dungeon_definition_reads_its_strata) {
    const DungeonDef def = tl::core::parse_dungeon_def(cave_dungeon_def());
    TL_CHECK(def.ok);
    TL_CHECK_EQ(def.name, std::string("Test_Cave"));
    TL_CHECK_EQ(def.parent_dungeon, std::string("Town"));
    TL_CHECK_EQ(def.strata.size(), static_cast<size_t>(1));
    // TEST_CAVE.DAT.adm authors no MONSTER_LVL_MULT, so the default matches
    // the one dungeon that does author it (HATCHMINE.DAT.adm: 1.0).
    TL_CHECK(near(def.monster_level_mult, 1.0f));

    const tl::core::DungeonStrata& s = def.strata.front();
    TL_CHECK_EQ(s.ruleset, std::string("media/layouts/cave/rules.dat"));
    TL_CHECK_EQ(s.floors, 25);
    TL_CHECK_EQ(s.spawn_slot_names[0], std::string("CAVEFLOOR1"));
    TL_CHECK(s.spawn_slot_authored[0]);
    TL_CHECK(!s.spawn_slot_authored[5]);  // GOLD
    TL_CHECK(near(s.density_min[0], 0.015f));
    TL_CHECK(near(s.density_max[0], 0.016f));
    TL_CHECK(s.density_min_authored[0]);
    TL_CHECK(near(s.count_min[1], 4.0f));
    TL_CHECK(near(s.count_max[1], 6.0f));
    TL_CHECK(!s.count_min_authored[0]);
    TL_CHECK_EQ(s.spawn_slot_names[3], std::string("NPCS"));
    TL_CHECK_EQ(s.spawn_slot_names[6], std::string("FISH_SPAWN_CAVE"));
    // Unauthored here too, same as every TEST_* dungeon in the shipped
    // archive - only the hand-authored MAIN.DAT.adm authors these.
    TL_CHECK_EQ(s.monster_lvl_min, -1);
    TL_CHECK_EQ(s.monster_lvl_max, -1);
}

TL_TEST(rank_without_a_dungeon_is_the_depth_at_least_one) {
    TL_CHECK_EQ(tl::core::rank_of_monsters_on_floor(0, nullptr, nullptr, 0, true), 1);
    TL_CHECK_EQ(tl::core::rank_of_monsters_on_floor(1, nullptr, nullptr, 0, true), 1);
    TL_CHECK_EQ(tl::core::rank_of_monsters_on_floor(7, nullptr, nullptr, 0, true), 7);
    TL_CHECK_EQ(tl::core::rank_of_monsters_on_floor(-3, nullptr, nullptr, 0, true), 1);
}

TL_TEST(rank_scales_the_depth_by_the_multiplier) {
    DungeonDef def;
    def.monster_level_mult = 1.0f;
    // mult 1.0f: depth 0 -> (0+1)*1.0+0.5 = 1.5 -> 1
    TL_CHECK_EQ(tl::core::rank_of_monsters_on_floor(0, &def, nullptr, 0, true), 1);
    // depth 4 -> (4+1)*1.0+0.5 = 5.5 -> 5
    TL_CHECK_EQ(tl::core::rank_of_monsters_on_floor(4, &def, nullptr, 0, true), 5);
    // depth 24 -> (24+1)*1.0+0.5 = 25.5 -> 25
    TL_CHECK_EQ(tl::core::rank_of_monsters_on_floor(24, &def, nullptr, 0, true), 25);

    def.monster_level_mult = 0.5f;
    // depth 9 -> (9+1)*0.5+0.5 = 5.5 -> 5
    TL_CHECK_EQ(tl::core::rank_of_monsters_on_floor(9, &def, nullptr, 0, true), 5);

    def.monster_level_mult = 1.5f;
    // depth 2 -> (2+1)*1.5+0.5 = 5.0 -> 5
    TL_CHECK_EQ(tl::core::rank_of_monsters_on_floor(2, &def, nullptr, 0, true), 5);

    // Truncation of (float)base * mult + 0.5f, not std::lround: 2^23 + 1.5 rounds to even.
    def.monster_level_mult = 1.0f;
    TL_CHECK_EQ(tl::core::rank_of_monsters_on_floor(8388608, &def, nullptr, 0, true), 8388610);

    // Floor of 1 when mult produces <= 0:
    def.monster_level_mult = 0.0f;
    TL_CHECK_EQ(tl::core::rank_of_monsters_on_floor(0, &def, nullptr, 0, true), 1);
    TL_CHECK_EQ(tl::core::rank_of_monsters_on_floor(10, &def, nullptr, 0, true), 1);
}

TL_TEST(rank_adds_the_player_term_clamped) {
    tl::core::TravelRecord travel;  // empty, no history
    DungeonDef def;
    def.name = "MINE";
    def.monster_level_mult = 1.0f;
    def.player_level_match_min = 10;
    def.player_level_match_max = 20;
    def.player_level_match_offset = 5;

    // base 5 + offset 5 - 1 + the player level clamped to the minimum 10.
    TL_CHECK_EQ(tl::core::rank_of_monsters_on_floor(4, &def, &travel, 5, true), 19);

    // player level inside range (15): clamped = 15.
    // sum = 5 + 5 - 1 + 15 = 24.
    TL_CHECK_EQ(tl::core::rank_of_monsters_on_floor(4, &def, &travel, 15, true), 24);

    // player level above max (25 > 20): clamped to max (20).
    // sum = 5 + 5 - 1 + 20 = 29.
    TL_CHECK_EQ(tl::core::rank_of_monsters_on_floor(4, &def, &travel, 25, true), 29);

    // min == 0 && max <= 0 skips the player term entirely.
    def.player_level_match_min = 0;
    def.player_level_match_max = 0;
    TL_CHECK_EQ(tl::core::rank_of_monsters_on_floor(4, &def, &travel, 15, true), 5);
    def.player_level_match_max = -2;
    TL_CHECK_EQ(tl::core::rank_of_monsters_on_floor(4, &def, &travel, 15, true), 5);

    // Clamp order: max applied after min (maxss min, minss max).
    // When min > max, max takes precedence.
    def.player_level_match_min = 20;
    def.player_level_match_max = 10;
    // base = 5, offset = 5, clamped = min(max(15, 20), 10) = 10.
    // sum = 5 + 5 - 1 + 10 = 19.
    TL_CHECK_EQ(tl::core::rank_of_monsters_on_floor(4, &def, &travel, 15, true), 19);
}

TL_TEST(rank_uses_the_tracker_once_the_dungeon_has_history) {
    tl::core::TravelRecord travel;
    tl::formats::LevelSaveState state;
    state.dungeon = "CAVE";
    state.depth = 0;
    travel.levels.push_back(state);

    tl::formats::DungeonTrackerSave tracker;
    tracker.name = "CAVE";
    tracker.player_level = 12;
    travel.trackers.push_back(tracker);

    DungeonDef def;
    def.name = "CAVE";
    def.monster_level_mult = 1.0f;
    def.player_level_match_min = 1;
    def.player_level_match_max = 30;
    def.player_level_match_offset = 0;

    // depth 0 -> base = 1. Current player level is 20, but tracker has 12.
    // Effective level is tracker's 12. sum = 1 + 0 - 1 + 12 = 12.
    TL_CHECK_EQ(tl::core::rank_of_monsters_on_floor(0, &def, &travel, 20, true), 12);

    // If tracker level is 0, falls back to current player level (20).
    travel.trackers[0].player_level = 0;
    TL_CHECK_EQ(tl::core::rank_of_monsters_on_floor(0, &def, &travel, 20, true), 20);
}

TL_TEST(rank_skips_the_player_when_asked) {
    tl::core::TravelRecord travel;
    DungeonDef def;
    def.monster_level_mult = 1.0f;
    def.player_level_match_min = 10;
    def.player_level_match_max = 20;
    def.player_level_match_offset = 5;

    // depth 4 -> base = 5. With use_player = false, player term is skipped.
    TL_CHECK_EQ(tl::core::rank_of_monsters_on_floor(4, &def, &travel, 15, false), 5);
}

TL_TEST(level_is_the_rank_without_a_template_range) {
    tl::core::RunicRandom rng(999u);
    const uint64_t initial_state = rng.state();

    // null template: returns max(1, rank), draws nothing
    TL_CHECK_EQ(tl::core::level_monster_level(nullptr, 5, true, 10, rng), 5);
    TL_CHECK_EQ(tl::core::level_monster_level(nullptr, 0, true, 10, rng), 1);
    TL_CHECK_EQ(rng.state(), initial_state);

    // template with min or max below 1: returns max(1, rank), draws nothing
    LevelTemplate tmpl;
    tmpl.monster_level_min = -1;
    tmpl.monster_level_max = -1;
    TL_CHECK_EQ(tl::core::level_monster_level(&tmpl, 8, true, 10, rng), 8);
    TL_CHECK_EQ(rng.state(), initial_state);

    tmpl.monster_level_min = 0;
    tmpl.monster_level_max = 10;
    TL_CHECK_EQ(tl::core::level_monster_level(&tmpl, 8, true, 10, rng), 8);
    TL_CHECK_EQ(rng.state(), initial_state);

    tmpl.monster_level_min = 5;
    tmpl.monster_level_max = 0;
    TL_CHECK_EQ(tl::core::level_monster_level(&tmpl, 8, true, 10, rng), 8);
    TL_CHECK_EQ(rng.state(), initial_state);
}

TL_TEST(level_draws_inside_the_template_range) {
    LevelTemplate tmpl;
    tmpl.monster_level_min = 5;
    tmpl.monster_level_max = 12;

    // Case 1: No player -> draws integer_between(min, max)
    {
        tl::core::RunicRandom rng(12345u);
        tl::core::RunicRandom expected_rng(12345u);
        const int32_t expected = expected_rng.integer_between(5, 12);
        const int32_t got = tl::core::level_monster_level(&tmpl, 1, false, 0, rng);
        TL_CHECK_EQ(got, expected);
        TL_CHECK_EQ(rng.state(), expected_rng.state());
    }

    // Case 2: Player present, player_level <= min -> returns min, draws nothing
    {
        tl::core::RunicRandom rng(54321u);
        const uint64_t state_before = rng.state();
        TL_CHECK_EQ(tl::core::level_monster_level(&tmpl, 1, true, 3, rng), 5);
        TL_CHECK_EQ(rng.state(), state_before);
        TL_CHECK_EQ(tl::core::level_monster_level(&tmpl, 1, true, 5, rng), 5);
        TL_CHECK_EQ(rng.state(), state_before);
    }

    // Case 3: Player present, player_level >= max -> returns max, draws nothing
    {
        tl::core::RunicRandom rng(54321u);
        const uint64_t state_before = rng.state();
        TL_CHECK_EQ(tl::core::level_monster_level(&tmpl, 1, true, 12, rng), 12);
        TL_CHECK_EQ(rng.state(), state_before);
        TL_CHECK_EQ(tl::core::level_monster_level(&tmpl, 1, true, 20, rng), 12);
        TL_CHECK_EQ(rng.state(), state_before);
    }

    // Case 4: Player present, min < player_level < max -> draws integer_between(player_level, max)
    {
        tl::core::RunicRandom rng(67890u);
        tl::core::RunicRandom expected_rng(67890u);
        const int32_t expected = expected_rng.integer_between(8, 12);
        const int32_t got = tl::core::level_monster_level(&tmpl, 1, true, 8, rng);
        TL_CHECK_EQ(got, expected);
        TL_CHECK_EQ(rng.state(), expected_rng.state());
    }
}

// ---------------------------------------------------------------------------
// UTILITIES::setSeed and CRandomizer.

TL_TEST(set_seed_stores_the_seed_zero_extended) {
    RunicRandom rng(0u);
    rng.set_seed(-1);
    TL_CHECK_EQ(rng.state(), static_cast<uint64_t>(0xffffffffull));
}

TL_TEST(a_randomizer_pick_is_one_roll_against_the_running_sum) {
    for (uint32_t seed = 1; seed <= 50; ++seed) {
        Randomizer r;
        r.add_choice(10, 1);
        r.add_choice(20, 3);
        RunicRandom picked(seed);
        RunicRandom expected(seed);
        int32_t index = -2;
        const int32_t value = r.get_random(picked, &index);
        const float roll = expected.between(0.0f, 1.0f);
        const bool first = roll < 0.25f;
        TL_CHECK_EQ(value, first ? 10 : 20);
        TL_CHECK_EQ(index, first ? 0 : 1);
        // One randomBetween, whatever was picked.
        TL_CHECK_EQ(picked.state(), expected.state());
    }
}

TL_TEST(an_empty_randomizer_still_rolls_and_returns_zero) {
    Randomizer r;
    RunicRandom picked(99u);
    RunicRandom expected(99u);
    int32_t index = 5;
    TL_CHECK_EQ(r.get_random(picked, &index), 0);
    TL_CHECK_EQ(index, -1);
    expected.between(0.0f, 1.0f);
    TL_CHECK_EQ(picked.state(), expected.state());
    TL_CHECK(!r.has_valid_choices());
}

TL_TEST(a_decaying_randomizer_shrinks_the_odds_it_picks) {
    Randomizer r(RandomizerType::kDecay);
    r.decay = 0.5f;
    r.add_choice(7, 8);
    RunicRandom rng(3u);
    TL_CHECK_EQ(r.get_random(rng), 7);
    TL_CHECK_EQ(r.get_odds(0), 4);
    TL_CHECK_EQ(r.get_random(rng), 7);
    TL_CHECK_EQ(r.get_odds(0), 2);
}

TL_TEST(removing_a_choice_moves_the_last_one_into_its_place) {
    Randomizer r;
    r.add_choice(1, 1);
    r.add_choice(2, 2);
    r.add_choice(3, 3);
    r.remove_choice(1);
    TL_CHECK_EQ(r.choice_count(), 2u);
    TL_CHECK_EQ(r.value_at(0), 3);
    TL_CHECK_EQ(r.get_odds(0), 3);
    TL_CHECK_EQ(r.value_at(1), 2);
}

TL_TEST(zero_odds_leave_no_valid_choice) {
    Randomizer r;
    r.add_choice(5, 1);
    TL_CHECK(r.has_valid_choices());
    r.set_choice_odds(5, 0);
    TL_CHECK(!r.has_valid_choices());
    r.set_randomizer_to_normal(2.0f);
    TL_CHECK(r.has_valid_choices());
    TL_CHECK_EQ(r.get_odds(0), 2);
}

// ---------------------------------------------------------------------------
// CLevelTemplateData.

TL_TEST(a_level_template_sorts_its_pieces_and_types) {
    const LevelTemplate t = loaded_cave();
    TL_CHECK_EQ(t.chunk_types.size(), static_cast<size_t>(14));
    TL_CHECK_EQ(t.min_chunks, 4);
    TL_CHECK_EQ(t.max_chunks, 4);
    TL_CHECK_EQ(t.name, std::string("CAVE"));

    // The folder is the rules directory plus the type name plus '/'.
    TL_CHECK_EQ(t.chunk_types[0].folder, std::string("media/layouts/cave/1X1ENTRANCE_E/"));

    // Two pieces per type, sorted, the /MERGE/ one dropped.
    TL_CHECK_EQ(t.chunks.size(), static_cast<size_t>(28));
    TL_CHECK_EQ(t.chunks[0].path, std::string("MEDIA/LAYOUTS/CAVE/1X1ENTRANCE_E/A.LAYOUT"));
    TL_CHECK_EQ(t.chunks[1].path, std::string("MEDIA/LAYOUTS/CAVE/1X1ENTRANCE_E/B.LAYOUT"));
    TL_CHECK_EQ(t.chunks[1].type_index, 0u);
    TL_CHECK_EQ(t.piece_randomizers.size(), static_cast<size_t>(14));
    TL_CHECK_EQ(t.piece_randomizers[8].value_at(0), 16);

    TL_CHECK_EQ(t.entrance_types.choice_count(), 4u);
    TL_CHECK_EQ(t.ordinary_types.choice_count(), 6u);
    TL_CHECK_EQ(t.dead_ends_facing_pos_z.choice_count(), 0u);

    // An exit chunk goes into the pool for the side its one doorway faces:
    // N (z 96, above the middle at 50) is +z, E (x -48) is -x, and so on.
    TL_CHECK_EQ(t.exits_facing_pos_z.value_at(0), 4);
    TL_CHECK_EQ(t.exits_facing_neg_x.value_at(0), 5);
    TL_CHECK_EQ(t.exits_facing_pos_x.value_at(0), 6);
    TL_CHECK_EQ(t.exits_facing_neg_z.value_at(0), 7);
}

TL_TEST(an_unauthored_template_keeps_the_constructor_defaults) {
    AdmFile f;
    f.ok = true;
    f.root.name = "LEVEL";
    f.root.children = {chunk_type("ROOM", true, false, {exit_at(0.0f, 0.0f, 0.0f)})};
    f.root.children[0].records.pop_back();  // no MAX_APPEARANCE
    LevelTemplate t;
    RunicRandom rng(1u);
    TL_CHECK(t.load("media/layouts/x/rules.dat", f, cave_files(), rng));
    TL_CHECK_EQ(t.min_chunks, 2);
    TL_CHECK_EQ(t.max_chunks, 3);
    TL_CHECK(near(t.tile_basis, 4.0f));
    TL_CHECK_EQ(t.chunk_types[0].max_appearance, 10);
    TL_CHECK_EQ(t.chunks[0].max_appearance, 10u);
}

TL_TEST(a_generated_cave_layout_closes_every_doorway) {
    for (int32_t seed = 1; seed <= 20; ++seed) {
        LevelTemplate t = loaded_cave();
        RunicRandom rng(0u);
        TL_CHECK_EQ(t.create_random_layout(rng, seed), seed);
        TL_CHECK(t.layout_created);
        TL_CHECK_EQ(t.layouts.size(), static_cast<size_t>(1));
        if (t.layouts.empty()) continue;
        const LevelLayout& layout = t.layouts[0];
        TL_CHECK_EQ(layout.seed, seed);
        TL_CHECK_EQ(layout.instances.size(), layout.pieces.size());

        // An entrance at the origin, four ordinary chunks, one exit.
        TL_CHECK_EQ(layout.instances.size(), static_cast<size_t>(6));
        TL_CHECK(t.type_at(layout.instances[0].type_index).entrance);
        TL_CHECK(near(layout.instances[0].x, 0.0f) && near(layout.instances[0].z, 0.0f));
        int exits = 0;
        std::set<std::pair<int, int>> cells;
        for (size_t i = 0; i < layout.instances.size(); ++i) {
            const ChunkInstance& chunk = layout.instances[i];
            const tl::core::ChunkType& type = t.type_at(chunk.type_index);
            if (type.exit) ++exits;
            // No cave doorway can be capped by a dead end, so a layout that
            // exists has every doorway joined.
            for (int32_t k = 0; k < static_cast<int32_t>(type.exits.size()); ++k) {
                TL_CHECK(connected(chunk, k));
            }
            // Chunks step a whole chunk (25 tiles of 4) at a time.
            TL_CHECK(near(std::fmod(std::fabs(chunk.x), 100.0f), 0.0f));
            TL_CHECK(near(std::fmod(std::fabs(chunk.z), 100.0f), 0.0f));
            TL_CHECK(cells.insert({static_cast<int>(chunk.x), static_cast<int>(chunk.z)}).second);
            TL_CHECK_EQ(t.chunks[layout.pieces[i]].type_index, chunk.type_index);
        }
        TL_CHECK_EQ(exits, 1);
    }
}

TL_TEST(the_same_level_seed_chooses_the_same_layout) {
    LevelTemplate a = loaded_cave();
    LevelTemplate b = loaded_cave();
    RunicRandom rng_a(0u);
    RunicRandom rng_b(0u);
    const LevelLayout* la = tl::core::choose_room_layout(a, rng_a, 12345);
    const LevelLayout* lb = tl::core::choose_room_layout(b, rng_b, 12345);
    TL_CHECK(la != nullptr && lb != nullptr);
    if (la == nullptr || lb == nullptr) return;
    TL_CHECK_EQ(rng_a.state(), rng_b.state());
    TL_CHECK_EQ(la->instances.size(), lb->instances.size());
    for (size_t i = 0; i < la->instances.size() && i < lb->instances.size(); ++i) {
        TL_CHECK_EQ(la->instances[i].type_index, lb->instances[i].type_index);
        TL_CHECK_EQ(la->pieces[i], lb->pieces[i]);
        TL_CHECK(near(la->instances[i].x, lb->instances[i].x));
        TL_CHECK(near(la->instances[i].z, lb->instances[i].z));
    }
}

TL_TEST(different_level_seeds_usually_choose_different_layouts) {
    auto signature = [](int32_t seed) {
        LevelTemplate t = loaded_cave();
        RunicRandom rng(0u);
        const LevelLayout* layout = tl::core::choose_room_layout(t, rng, seed);
        std::vector<uint32_t> out;
        if (layout == nullptr) return out;
        for (size_t i = 0; i < layout->instances.size(); ++i) {
            out.push_back(layout->instances[i].type_index);
            out.push_back(layout->pieces[i]);
        }
        return out;
    };
    const std::vector<uint32_t> reference = signature(1);
    bool found_a_difference = false;
    for (int32_t seed = 2; seed <= 20; ++seed) {
        if (signature(seed) != reference) found_a_difference = true;
    }
    TL_CHECK(found_a_difference);
}

/// `CLevel::loadRoomLayout` generates ten layouts once; `deleteLayouts` is what
/// empties the pool again.
TL_TEST(ten_layouts_are_generated_and_kept_until_deleted) {
    LevelTemplate t = loaded_cave();
    RunicRandom rng(0u);
    TL_CHECK(tl::core::choose_room_layout(t, rng, 777) != nullptr);
    TL_CHECK_EQ(t.layouts.size(), static_cast<size_t>(10));
    TL_CHECK_EQ(t.layout_randomizer.choice_count(), 10u);
    TL_CHECK(tl::core::choose_room_layout(t, rng, 778) != nullptr);
    TL_CHECK_EQ(t.layouts.size(), static_cast<size_t>(10));

    t.delete_layouts();
    TL_CHECK(t.layouts.empty());
    TL_CHECK(!t.layout_created);
    TL_CHECK_EQ(t.layout_randomizer.choice_count(), 0u);
    TL_CHECK(t.get_random_layout(rng) == nullptr);
}

/// `exitsMatch` stops at the first placed chunk the probe misses, so the answer
/// depends on the placed list's order. Kept.
TL_TEST(exits_match_only_checks_later_chunks_once_the_first_is_hit) {
    const LevelTemplate t = loaded_cave();
    auto instance = [](uint32_t type, float x, float z) {
        ChunkInstance c;
        c.type_index = type;
        c.x = x;
        c.z = z;
        return c;
    };
    const ChunkInstance entrance = instance(1, 0.0f, 0.0f);     // 1X1ENTRANCE_N
    const ChunkInstance corridor = instance(11, 100.0f, 0.0f);  // 1X1EW, doors at z 48
    // 1X1NS one chunk further on: its south doorway probes (100, 94), inside
    // the corridor, where the corridor has no doorway.
    const ChunkInstance candidate = instance(8, 100.0f, 100.0f);

    TL_CHECK(t.exits_match(candidate, {entrance, corridor}));
    TL_CHECK(!t.exits_match(candidate, {corridor, entrance}));
}

// ---------------------------------------------------------------------------
// CDungeon.

namespace {

AdmSection strata_section(const std::string& name, std::vector<AdmRecord> records) {
    AdmSection s;
    s.name = name;
    s.records = std::move(records);
    return s;
}

AdmFile dungeon_file(const std::string& name, std::vector<AdmSection> strata) {
    AdmFile f;
    f.ok = true;
    f.root.name = "DUNGEON";
    f.root.records = {text_record("NAME", name)};
    f.root.children = std::move(strata);
    return f;
}

/// Every ruleset reads as the cave's; the registry answers `DUNGEON` names.
tl::core::DungeonFiles dungeon_files(std::map<std::string, tl::core::Dungeon*>& registry) {
    tl::core::DungeonFiles files;
    files.layouts = cave_files();
    files.read_ruleset = [](const std::string&) { return cave_rules(); };
    files.dungeon_by_name = [&registry](const std::string& upper_name) -> tl::core::Dungeon* {
        const auto it = registry.find(upper_name);
        return it == registry.end() ? nullptr : it->second;
    };
    return files;
}

}  // namespace

TL_TEST(a_dungeon_expands_each_stratum_into_its_floors) {
    const DungeonDef def = tl::core::parse_dungeon_def(dungeon_file(
        "Deep", {strata_section("STRATA0", {text_record("RULESET", "media/layouts/cave/rules.dat"),
                                            int_record("FLOORS", 2)}),
                 strata_section("STRATA1", {int_record("WEIGHT", 5)}),
                 // After a gap: never read.
                 strata_section("STRATA3", {int_record("FLOORS", 9)})}));
    TL_CHECK(def.ok);
    TL_CHECK_EQ(def.upper_name, std::string("DEEP"));
    TL_CHECK_EQ(def.strata.size(), static_cast<size_t>(2));
    TL_CHECK(def.depth_strata == (std::vector<uint32_t>{0, 0, 1}));
    TL_CHECK_EQ(def.depth_weights.choice_count(), 3u);
    TL_CHECK_EQ(def.depth_weights.value_at(2), 2);
    TL_CHECK_EQ(def.depth_weights.get_odds(2), 5);
    // No RULESET: getStrataTemplate's default.
    TL_CHECK_EQ(def.strata[1].ruleset, std::string("media/layouts/crypt/rules.dat"));
}

TL_TEST(each_depth_builds_its_own_template_once) {
    std::map<std::string, tl::core::Dungeon*> registry;
    const tl::core::DungeonFiles files = dungeon_files(registry);
    tl::core::Dungeon dungeon(tl::core::parse_dungeon_def(
        dungeon_file("Deep", {strata_section("STRATA0", {int_record("FLOORS", 2)}),
                              strata_section("STRATA1", {text_record("RULESET", "b")})})));
    RunicRandom rng(1u);
    const tl::core::DungeonStrata* source = nullptr;
    LevelTemplate* first = dungeon.get_level_template_for_depth(0, files, rng, &source);
    TL_CHECK(first != nullptr);
    TL_CHECK(source == &dungeon.def.strata[0]);
    LevelTemplate* second = dungeon.get_level_template_for_depth(1, files, rng);
    TL_CHECK(second != nullptr && second != first);
    TL_CHECK(dungeon.get_level_template_for_depth(0, files, rng) == first);
    // Past the last depth: the last depth's template.
    LevelTemplate* last = dungeon.get_level_template_for_depth(2, files, rng);
    TL_CHECK(dungeon.get_level_template_for_depth(40, files, rng, &source) == last);
    TL_CHECK(source == &dungeon.def.strata[1]);
    TL_CHECK(dungeon.get_strata_template(40, files, rng) == nullptr);
}

TL_TEST(a_stratum_lays_its_keys_onto_its_template) {
    std::map<std::string, tl::core::Dungeon*> registry;
    const tl::core::DungeonFiles files = dungeon_files(registry);
    tl::core::Dungeon dungeon(tl::core::parse_dungeon_def(dungeon_file(
        "Deep",
        {strata_section("STRATA0",
                        {text_record("NAME_OVERRIDE", "Hall"), int_record("MONSTER_LVL_MIN", 3),
                         int_record("MONSTER_LVL_MAX", 5), bool_record("IS_TOWN", true),
                         bool_record("ALLOW_PORTALS", false)}),
         // A zero minimum leaves the template's range alone.
         strata_section("STRATA1",
                        {int_record("MONSTER_LVL_MIN", 0), int_record("MONSTER_LVL_MAX", 5)})})));
    RunicRandom rng(1u);
    const LevelTemplate* hall = dungeon.get_strata_template(0, files, rng);
    TL_CHECK_EQ(hall->level_name, std::string("Hall"));
    TL_CHECK_EQ(hall->monster_level_min, 3);
    TL_CHECK_EQ(hall->monster_level_max, 5);
    TL_CHECK(hall->is_town);
    TL_CHECK(!hall->allow_portals);
    TL_CHECK(hall->allow_pet_return);
    const LevelTemplate* plain = dungeon.get_strata_template(1, files, rng);
    TL_CHECK_EQ(plain->level_name, std::string("Dungeon"));
    TL_CHECK_EQ(plain->monster_level_min, -1);
}

TL_TEST(a_stratum_naming_a_dungeon_takes_a_weighted_floor_of_it) {
    for (uint32_t seed = 1; seed <= 30; ++seed) {
        std::map<std::string, tl::core::Dungeon*> registry;
        const tl::core::DungeonFiles files = dungeon_files(registry);
        tl::core::Dungeon pool(tl::core::parse_dungeon_def(dungeon_file(
            "Pool",
            {strata_section("STRATA0", {text_record("RULESET", "a")}),
             strata_section("STRATA1", {text_record("RULESET", "b"), int_record("WEIGHT", 3)})})));
        registry["POOL"] = &pool;
        tl::core::Dungeon entry(tl::core::parse_dungeon_def(
            dungeon_file("Entry", {strata_section("STRATA0", {text_record("DUNGEON", "pool")})})));

        RunicRandom rng(seed);
        RunicRandom expected(seed);
        const tl::core::DungeonStrata* source = nullptr;
        const LevelTemplate* found = entry.get_level_template_for_depth(0, files, rng, &source);
        TL_CHECK(found != nullptr && source != nullptr);
        if (source == nullptr) continue;
        // No template here has a [LAYOUT], so the pick is the only draw.
        const bool first = expected.between(0.0f, 1.0f) < 0.25f;
        TL_CHECK_EQ(source->ruleset, std::string(first ? "a" : "b"));
        TL_CHECK_EQ(rng.state(), expected.state());
    }
}

// ---------------------------------------------------------------------------
// CLevelTemplateData::chooseUnitSpawners.

TL_TEST(a_stratum_lays_its_spawn_slots_onto_the_template) {
    std::map<std::string, tl::core::Dungeon*> registry;
    const tl::core::DungeonFiles files = dungeon_files(registry);
    tl::core::Dungeon dungeon(tl::core::parse_dungeon_def(dungeon_file(
        "Deep", {strata_section("STRATA0", {text_record("MONSTERSPAWNCLASS", "POOL"),
                                            bool_record("MONSTERSPAWNCLASSRANDOMIZED", true),
                                            text_record("NPCSPAWNCLASS", "")})})));
    RunicRandom rng(1u);
    const LevelTemplate* t = dungeon.get_strata_template(0, files, rng);
    TL_CHECK_EQ(t->spawn_classes[0], std::string("POOL"));
    TL_CHECK(t->spawn_classes_randomized[0]);
    // Authored empty wins over the default; unauthored keeps it.
    TL_CHECK_EQ(t->spawn_classes[3], std::string(""));
    TL_CHECK_EQ(t->spawn_classes[1], std::string("MONSTERSETCHAMPION"));
    TL_CHECK(!t->spawn_classes_randomized[1]);
}

TL_TEST(a_randomized_spawn_slot_picks_a_referenced_class_by_weight) {
    const tl::core::SpawnClassLookup lookup = [](const std::string& name,
                                                 std::vector<tl::core::SpawnClassRef>& entries) {
        if (name != "POOL") return false;
        entries = {{"", 500}, {"DUO_A", 50}, {"DUO_B", -1}};  // a unit entry, 50, unauthored (100)
        return true;
    };
    for (uint32_t seed = 1; seed <= 40; ++seed) {
        LevelTemplate t;
        t.spawn_classes[0] = "POOL";
        t.spawn_classes_randomized[0] = true;
        t.spawn_classes[4] = "POOL";  // not randomized: kept, no draw
        RunicRandom rng(seed);
        RunicRandom expected(seed);
        tl::core::choose_unit_spawners(t, lookup, rng);
        const bool first = expected.between(0.0f, 1.0f) < 50.0f / 150.0f;
        TL_CHECK_EQ(t.chosen_spawn_classes[0], std::string(first ? "DUO_A" : "DUO_B"));
        TL_CHECK_EQ(t.chosen_spawn_classes[4], std::string("POOL"));
        TL_CHECK_EQ(t.chosen_spawn_classes[1], std::string("MONSTERSETCHAMPION"));
        TL_CHECK_EQ(rng.state(), expected.state());
    }
}

TL_TEST(a_randomized_slot_naming_no_class_keeps_its_name_and_draws_nothing) {
    LevelTemplate t;
    t.spawn_classes[0] = "MISSING";
    t.spawn_classes_randomized[0] = true;
    RunicRandom rng(7u);
    const uint64_t before = rng.state();
    tl::core::choose_unit_spawners(
        t, [](const std::string&, std::vector<tl::core::SpawnClassRef>&) { return false; }, rng);
    TL_CHECK_EQ(t.chosen_spawn_classes[0], std::string("MISSING"));
    TL_CHECK_EQ(rng.state(), before);
}

// ---------------------------------------------------------------------------
// CLevelTemplateData::getNumberOfUnitsToCreate.

TL_TEST(a_stratum_lays_population_numbers_onto_the_template) {
    std::map<std::string, tl::core::Dungeon*> registry;
    const tl::core::DungeonFiles files = dungeon_files(registry);
    tl::core::Dungeon dungeon(tl::core::parse_dungeon_def(dungeon_file(
        "Deep", {strata_section(
                    "STRATA0", {float_record("MONSTER_MIN", 3.0f), float_record("PPROPS_MAX", 9.0f),
                                float_record("PROPS_MAX", 99.0f),
                                float_record("GOLD_PER_METER_MAX", 0.5f)})})));
    RunicRandom rng(1u);
    const LevelTemplate* t = dungeon.get_strata_template(0, files, rng);
    TL_CHECK(near(t->count_min[0], 3.0f));
    TL_CHECK(near(t->count_max[2], 9.0f));  // the binary reads PPROPS_MAX, not PROPS_MAX
    TL_CHECK(near(t->count_min[2], 0.0f));
    TL_CHECK(near(t->density_max[5], 0.5f));
    TL_CHECK(near(t->density_min[5], 0.0012f));  // unauthored: the constructor's
    TL_CHECK(near(t->density_min[0], 0.01f));
}

TL_TEST(unit_counts_take_the_flat_count_either_way_round) {
    LevelTemplate t;
    t.count_min[1] = 6.0f;
    t.count_max[1] = 4.0f;
    RunicRandom rng(11u);
    RunicRandom expected(11u);
    const int64_t n = tl::core::units_to_create(t, 1, 5000u, rng);
    TL_CHECK_EQ(n, static_cast<int64_t>(expected.integer_between(4, 6)));
    TL_CHECK_EQ(rng.state(), expected.state());
}

TL_TEST(unit_counts_scale_the_density_by_pathable_area) {
    LevelTemplate t;               // monsters: the constructor's 0.01 .. 0.0125 per square metre
    const uint32_t cells = 6500u;  // 1,000 square metres
    for (uint32_t seed = 1; seed <= 20; ++seed) {
        RunicRandom rng(seed);
        RunicRandom expected(seed);
        const int64_t n = tl::core::units_to_create(t, 0, cells, rng);
        const float area = static_cast<float>(cells) * 0.15384616f;
        const float wanted = expected.between(0.01f * area, 0.0125f * area);
        int64_t whole = static_cast<int64_t>(wanted);
        if (static_cast<float>(whole) < wanted) ++whole;
        TL_CHECK_EQ(n, whole);
        TL_CHECK(n >= 10 && n <= 13);
        TL_CHECK_EQ(rng.state(), expected.state());
    }
}

TL_TEST(unit_counts_are_zero_without_both_densities_and_draw_nothing) {
    LevelTemplate t;
    t.density_min[5] = 0.0f;  // gold: one end missing
    RunicRandom rng(9u);
    const uint64_t before = rng.state();
    TL_CHECK_EQ(tl::core::units_to_create(t, 3, 6500u, rng),
                static_cast<int64_t>(0));  // NPCs 0 / 0
    TL_CHECK_EQ(tl::core::units_to_create(t, 5, 6500u, rng), static_cast<int64_t>(0));
    TL_CHECK_EQ(rng.state(), before);
}

// ---------------------------------------------------------------------------
// Random groups inside a layout.

namespace {

using tl::core::LayoutGroup;

LayoutGroup layout_group(int64_t id, int64_t parent, uint32_t type = 0, uint32_t picks = 1,
                         uint32_t weight = 1) {
    LayoutGroup g;
    g.id = id;
    g.parent_id = parent;
    g.random_type = type;
    g.number_of_picks = picks;
    g.random_weight = weight;
    return g;
}

}  // namespace

TL_TEST(a_random_group_type_is_read_by_name) {
    TL_CHECK_EQ(tl::core::random_group_type_from_string("ALL"), 0u);
    TL_CHECK_EQ(tl::core::random_group_type_from_string("Weight"), 1u);
    TL_CHECK_EQ(tl::core::random_group_type_from_string("Random Chance"), 2u);
    TL_CHECK_EQ(tl::core::random_group_type_from_string("weight"), 0u);
}

TL_TEST(an_all_group_keeps_every_child_and_draws_nothing) {
    const std::vector<LayoutGroup> groups = {layout_group(1, -1, 0), layout_group(2, 1),
                                             layout_group(3, 1)};
    RunicRandom rng(5u);
    const uint64_t before = rng.state();
    TL_CHECK(tl::core::roll_random_groups(groups, rng).empty());
    TL_CHECK_EQ(rng.state(), before);
}

TL_TEST(a_weight_group_draws_its_picks_by_weight) {
    for (uint32_t seed = 1; seed <= 40; ++seed) {
        // Children weighted 1 and 3, one pick; a grandchild under each.
        const std::vector<LayoutGroup> groups = {
            layout_group(1, -1, 1, 1), layout_group(2, 1, 0, 1, 1), layout_group(3, 1, 0, 1, 3),
            layout_group(4, 2), layout_group(5, 3)};
        RunicRandom rng(seed);
        RunicRandom expected(seed);
        const std::set<int64_t> removed = tl::core::roll_random_groups(groups, rng);
        const bool first = expected.between(0.0f, 1.0f) < 0.25f;
        TL_CHECK_EQ(rng.state(), expected.state());
        TL_CHECK(removed == (first ? std::set<int64_t>{3, 5} : std::set<int64_t>{2, 4}));
    }
}

TL_TEST(a_weight_group_with_nothing_to_draw_still_draws_once) {
    // Weight 0 keeps a child outright; the pick loop still runs once.
    const std::vector<LayoutGroup> groups = {layout_group(1, -1, 1, 1), layout_group(2, 1, 0, 1, 0),
                                             layout_group(3, 1, 0, 1, 0)};
    RunicRandom rng(9u);
    RunicRandom expected(9u);
    TL_CHECK(tl::core::roll_random_groups(groups, rng).empty());
    expected.between(0.0f, 1.0f);
    TL_CHECK_EQ(rng.state(), expected.state());
}

TL_TEST(a_random_chance_group_rolls_each_child_until_it_has_its_picks) {
    for (uint32_t seed = 1; seed <= 40; ++seed) {
        // 30% child, a child outside 1..99 (kept with no roll), a 70% child;
        // one pick.
        const std::vector<LayoutGroup> groups = {
            layout_group(1, -1, 2, 1), layout_group(2, 1, 0, 1, 30), layout_group(3, 1, 0, 1, 150),
            layout_group(4, 1, 0, 1, 70)};
        RunicRandom rng(seed);
        RunicRandom expected(seed);
        const std::set<int64_t> removed = tl::core::roll_random_groups(groups, rng);
        std::set<int64_t> want;
        if (30.0f < expected.between(1.0f, 100.0f)) {
            // Not kept: the 150 is kept without a roll, which does not count
            // against NUMBER, and the 70 rolls.
            want.insert(2);
            if (70.0f < expected.between(1.0f, 100.0f)) want.insert(4);
        } else {
            // Kept, and that was the one pick: the rest are never looked at.
            want.insert(3);
            want.insert(4);
        }
        TL_CHECK(removed == want);
        TL_CHECK_EQ(rng.state(), expected.state());
    }
}

TL_TEST(a_group_that_cannot_be_created_takes_its_subtree_with_it) {
    std::vector<LayoutGroup> groups = {layout_group(1, -1, 1, 1), layout_group(2, 1),
                                       layout_group(3, 2, 1, 1), layout_group(4, 3),
                                       layout_group(5, 3)};
    groups[1].can_be_created = false;  // group 2: removed before it can roll
    RunicRandom rng(3u);
    const std::set<int64_t> removed = tl::core::roll_random_groups(groups, rng);
    TL_CHECK(removed.count(2) == 1 && removed.count(3) == 1);
    TL_CHECK(removed.count(4) + removed.count(5) == 2);
    // Group 1's roll (one child) and group 3's (two children, whose ancestor
    // is gone but which still rolls) both drew.
    RunicRandom expected(3u);
    expected.between(0.0f, 1.0f);
    expected.between(0.0f, 1.0f);
    TL_CHECK_EQ(rng.state(), expected.state());
}

// `CDungeonManager::reload` and `getDungeonByName`.
TL_TEST(the_dungeon_manager_salts_each_dungeon_and_finds_the_first_by_name) {
    std::vector<DungeonDef> definitions(3);
    const char* names[3] = {"Town", "Main", "Town"};  // `TOWN_ALT.DAT` shares the name
    for (size_t i = 0; i < 3; ++i) {
        definitions[i].name = names[i];
        definitions[i].upper_name = tl::core::upper_ascii(names[i]);
    }
    tl::core::DungeonManager manager;
    RunicRandom g_rand(5u);
    RunicRandom volatile_rng(77u);
    RunicRandom expected(77u);
    manager.reload(std::move(definitions), g_rand, volatile_rng);
    TL_CHECK(manager.loaded);
    TL_CHECK_EQ(manager.dungeons.size(), static_cast<size_t>(3));
    // One volatile roll a dungeon, in list order.
    for (const auto& dungeon : manager.dungeons) {
        TL_CHECK_EQ(dungeon->salt, expected.integer_between(0, 0x7fffffff));
    }
    TL_CHECK(manager.dungeon_by_name("town") == manager.dungeons[0].get());
    TL_CHECK(manager.dungeon_by_name("Main") == manager.dungeons[1].get());
    TL_CHECK(manager.dungeon_by_name("nowhere") == nullptr);
}

// `CGameClient::resetGameSeed` and the level seed of
// `CLevel::loadRoomLayout`.
TL_TEST(a_level_is_seeded_from_the_game_seed_the_salt_and_the_depth) {
    DungeonDef definition;
    tl::core::Dungeon dungeon(definition);
    dungeon.salt = 1000;
    tl::core::GameSeed seed;
    seed.seed = 50;
    seed.saved_depth = 7;
    seed.current_dungeon = &dungeon;

    RunicRandom g_rand(1u);
    RunicRandom reference(1u);
    TL_CHECK_EQ(tl::core::reset_game_seed(seed, 3, g_rand), 1053);
    reference.set_seed(1053);
    TL_CHECK_EQ(g_rand.state(), reference.state());
    // -1 is the depth the character was saved at.
    TL_CHECK_EQ(tl::core::reset_game_seed(seed, -1, g_rand), 1057);
    // In no dungeon there is no salt.
    seed.current_dungeon = nullptr;
    TL_CHECK_EQ(tl::core::reset_game_seed(seed, 2, g_rand), 52);
    seed.current_dungeon = &dungeon;

    TL_CHECK_EQ(tl::core::level_seed(0, seed, 2), 1052);
    // `LEVEL_SEED` stands in for the game seed, not for the salt or the depth.
    TL_CHECK_EQ(tl::core::level_seed(9, seed, 2), 1011);
    // 32-bit sums wrap.
    seed.seed = 0x7fffffff;
    dungeon.salt = 0x7fffffff;
    TL_CHECK_EQ(tl::core::level_seed(0, seed, 2), 0);
}

// `CGameClient::CGameClient`, `setGameState`.
TL_TEST(a_new_game_seed_is_the_level_seed_setting_or_a_volatile_roll) {
    RunicRandom volatile_rng(9u);
    RunicRandom reference(9u);
    TL_CHECK_EQ(tl::core::roll_game_seed(1234, volatile_rng), 1234);
    TL_CHECK_EQ(volatile_rng.state(), reference.state());
    TL_CHECK_EQ(tl::core::roll_game_seed(0, volatile_rng),
                reference.integer_between(0, 0x7fffffff));
}

// `UTILITIES::setSeed`: zero-extended, and 0 is `GetTickCount()`.
TL_TEST(a_seed_is_stored_zero_extended_and_zero_takes_the_clock) {
    RunicRandom rng(1u);
    rng.set_seed(-1);
    TL_CHECK_EQ(rng.state(), static_cast<uint64_t>(0xffffffffu));
    rng.set_seed(0);
    TL_CHECK(rng.state() <= 0xffffffffull);
}
