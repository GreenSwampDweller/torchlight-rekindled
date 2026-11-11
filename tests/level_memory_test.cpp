// The player's level memory (`CPlayer`'s remembered levels and dungeon
// trackers) and the save file records they are written as.
#include "core/level_memory.h"
#include "formats/save_file.h"
#include "test_support.h"

using tl::core::LevelMemoryRules;
using tl::core::QuestLevelClaim;
using tl::core::TravelRecord;

namespace {

tl::formats::LevelSaveState& store(TravelRecord& travel, const std::string& dungeon, int depth,
                                   bool town = false) {
    const int32_t salt = 7;
    tl::formats::LevelSaveState& state =
        tl::core::begin_store_level_saved_state(travel, dungeon, depth, town, false, 1, &salt);
    return state;
}

}  // namespace

TL_TEST(level_memory_keys_by_dungeon_and_depth) {
    TravelRecord travel;
    store(travel, "TOWN", 0, true);
    store(travel, "MAIN", 0);
    TL_CHECK(tl::core::has_level_saved_state(travel, "TOWN", 0));
    TL_CHECK(tl::core::has_level_saved_state(travel, "MAIN", 0));
    TL_CHECK(!tl::core::has_level_saved_state(travel, "MAIN", 1));
    // Exact comparison: the names are stored upper-cased by whoever builds them.
    TL_CHECK(!tl::core::has_level_saved_state(travel, "Main", 0));
    TL_CHECK(tl::core::has_dungeon_history(travel, "main"));
    TL_CHECK_EQ(tl::core::get_max_depth(travel, "MAIN"), 0);
    TL_CHECK_EQ(tl::core::get_max_depth(travel, "CRYPT"), -1);
}

TL_TEST(dungeon_rank_reads_the_tracker) {
    TravelRecord travel;
    tl::formats::DungeonTrackerSave tracker;
    tracker.name = "MAIN";
    tracker.player_level = 14;
    travel.trackers.push_back(tracker);

    // Found by the name as given: no upper-casing, so another case is not found.
    TL_CHECK_EQ(tl::core::get_dungeon_rank(travel, "MAIN"), 14);
    TL_CHECK_EQ(tl::core::get_dungeon_rank(travel, "main"), 0);
    TL_CHECK_EQ(tl::core::get_dungeon_rank(travel, "Main"), 0);

    // 0 when absent.
    TL_CHECK_EQ(tl::core::get_dungeon_rank(travel, "CRYPT"), 0);
    TL_CHECK_EQ(tl::core::get_dungeon_rank(travel, "town"), 0);
}

TL_TEST(level_memory_revisit_keeps_the_first_time) {
    TravelRecord travel;
    travel.play_time = 10.0f;
    store(travel, "MAIN", 2);
    travel.play_time = 500.0f;
    store(travel, "TOWN", 0, true);
    tl::formats::LevelSaveState& again = store(travel, "MAIN", 2);
    TL_CHECK_EQ(again.time, 10.0f);
    TL_CHECK_EQ(travel.levels.size(), size_t{2});
    TL_CHECK_EQ(travel.deepest_depth, 2);
}

TL_TEST(level_memory_dont_store_marks_minus_one) {
    TravelRecord travel;
    travel.play_time = 42.0f;
    const int32_t salt = 1;
    tl::formats::LevelSaveState& state =
        tl::core::begin_store_level_saved_state(travel, "RANDOMDUNGEON", 0, false, true, 3, &salt);
    TL_CHECK_EQ(state.time, -1.0f);
    // The next update drops it: expired by its -1 time, and nothing holds it.
    tl::core::update_stored_levels(travel, "TOWN", 0, {}, LevelMemoryRules{});
    TL_CHECK(!tl::core::has_level_saved_state(travel, "RANDOMDUNGEON", 0));
}

TL_TEST(level_memory_removal_moves_the_last_into_the_slot) {
    TravelRecord travel;
    store(travel, "A", 0);
    store(travel, "B", 0);
    store(travel, "C", 0);
    tl::core::remove_level_saved_state(travel, 0);
    TL_CHECK_EQ(travel.levels.size(), size_t{2});
    TL_CHECK_EQ(travel.levels[0].dungeon, std::string("C"));
    TL_CHECK_EQ(travel.levels[1].dungeon, std::string("B"));
    // Past the end still drops the last entry.
    tl::core::remove_level_saved_state(travel, 9);
    TL_CHECK_EQ(travel.levels.size(), size_t{1});
    TL_CHECK_EQ(travel.levels[0].dungeon, std::string("C"));
}

TL_TEST(level_memory_evicts_the_oldest_over_the_limit) {
    TravelRecord travel;
    LevelMemoryRules rules;
    rules.max_level_memory = 3;
    travel.play_time = 0.0f;
    store(travel, "TOWN", 0, true);  // a town is never evicted
    travel.play_time = 1.0f;
    store(travel, "MAIN", 0);
    travel.play_time = 2.0f;
    store(travel, "MAIN", 1);
    travel.play_time = 3.0f;
    store(travel, "MAIN", 2);
    tl::core::update_stored_levels(travel, "MAIN", 2, {}, rules);
    TL_CHECK_EQ(travel.levels.size(), size_t{3});
    TL_CHECK(tl::core::has_level_saved_state(travel, "TOWN", 0));
    TL_CHECK(!tl::core::has_level_saved_state(travel, "MAIN", 0));
    TL_CHECK(tl::core::has_level_saved_state(travel, "MAIN", 1));
}

TL_TEST(level_memory_expires_after_the_respawn_time) {
    TravelRecord travel;
    LevelMemoryRules rules;
    travel.play_time = 0.0f;
    store(travel, "MAIN", 0);
    store(travel, "TOWN", 0, true);
    travel.play_time = 1800.0f;
    tl::core::update_stored_levels(travel, "TOWN", 0, {}, rules);
    TL_CHECK(tl::core::has_level_saved_state(travel, "MAIN", 0));  // 1800 <= 1800 + 0
    travel.play_time = 1800.5f;
    tl::core::update_stored_levels(travel, "TOWN", 0, {}, rules);
    TL_CHECK(!tl::core::has_level_saved_state(travel, "MAIN", 0));
    TL_CHECK(tl::core::has_level_saved_state(travel, "TOWN", 0));
}

TL_TEST(level_memory_expiry_spares_held_levels) {
    TravelRecord travel;
    LevelMemoryRules rules;
    store(travel, "MAIN", 0);
    store(travel, "MAIN", 3).holds_quest_item = 1;
    store(travel, "MAIN", 4);
    store(travel, "MAIN", 5);
    travel.last_dungeon = "MAIN";
    travel.last_dungeon_depth = 4;
    travel.play_time = 99999.0f;
    std::vector<QuestLevelClaim> quests(1);
    quests[0].has_details = true;
    quests[0].dungeon = "main";
    quests[0].depth = 5;
    tl::core::update_stored_levels(travel, "MAIN", 0, quests, rules);
    TL_CHECK(tl::core::has_level_saved_state(travel, "MAIN", 0));  // the level being stored
    TL_CHECK(tl::core::has_level_saved_state(travel, "MAIN", 3));  // a quest item
    TL_CHECK(tl::core::has_level_saved_state(travel, "MAIN", 4));  // the last dungeon
    TL_CHECK(tl::core::has_level_saved_state(travel, "MAIN", 5));  // quest-bound
}

TL_TEST(dungeon_tracking_records_salt_and_level) {
    TravelRecord travel;
    int32_t salt = 100;
    tl::core::update_dungeon_tracking(travel, "Main", 0, 1, &salt);
    TL_CHECK_EQ(travel.trackers.size(), size_t{1});
    TL_CHECK_EQ(travel.trackers[0].name, std::string("MAIN"));
    TL_CHECK_EQ(travel.trackers[0].salt, 100);
    tl::core::update_dungeon_tracking(travel, "MAIN", 3, 5, &salt);
    TL_CHECK_EQ(travel.trackers[0].deepest, 3);
    TL_CHECK_EQ(travel.trackers[0].player_level, 1);  // same salt: level kept
    salt = 200;
    tl::core::update_dungeon_tracking(travel, "MAIN", 1, 6, &salt);
    TL_CHECK_EQ(travel.trackers[0].deepest, 3);
    TL_CHECK_EQ(travel.trackers[0].player_level, 6);
    TL_CHECK_EQ(travel.trackers[0].salt, 200);
}

TL_TEST(logic_node_state_flag_order_quirk) {
    tl::formats::LogicNodeState state;
    state.descriptor = "Player Sphere Trigger";
    state.flags = {1, 1, 0, 0, 1};
    state.object_id = 42;
    tl::formats::ByteWriter out;
    tl::formats::write_logic_state(out, state);
    tl::formats::ByteReader in(out.bytes().data(), out.size());
    tl::formats::LogicNodeState back;
    TL_CHECK(tl::formats::read_logic_state(in, back));
    TL_CHECK_EQ(back.descriptor, state.descriptor);
    TL_CHECK_EQ(back.object_id, int64_t{42});
    // +0x12 comes back as the saved +0x13, and +0x13 as the saved +0x10.
    TL_CHECK_EQ(int(back.flags[2]), 0);
    TL_CHECK_EQ(int(back.flags[3]), 1);
    TL_CHECK_EQ(int(back.flags[4]), 1);
}

TL_TEST(character_file_round_trips) {
    tl::formats::CharacterFile file;
    file.class_name = "Alchemist";
    file.difficulty = 2;
    file.player.name = "Hero";
    file.player.level = 7;
    file.player.hp = 123.5f;
    tl::formats::ItemSaveState potion;
    potion.unit_guid = 12345;
    potion.name = "Health Potion";
    file.player.inventory.push_back(potion);
    file.current_dungeon = "TOWN";
    tl::formats::LevelSaveState level;
    level.dungeon = "MAIN";
    level.depth = 1;
    level.time = 33.0f;
    level.automap_width = 2;
    level.automap_height = 1;
    level.automap = {1.0f, 0.0f};
    tl::formats::CharacterSaveState rat;
    rat.unit_guid = 777;
    rat.position = {1.0f, 2.0f, 3.0f};
    level.characters.push_back(rat);
    tl::formats::FormationSaveState formation;
    formation.pending = 1;
    formation.layout = "media/formations/x.layout";
    level.formations.push_back(formation);
    file.levels.push_back(level);
    tl::formats::DungeonTrackerSave tracker;
    tracker.name = "MAIN";
    tracker.salt = 99;
    file.trackers.push_back(tracker);
    file.quests.completed = {"INTROTOGAMEPT1"};
    tl::formats::QuestSave quest;
    quest.name = "INTROTOGAMEPT2";
    quest.dialogs[0] = {{1, 0}};
    quest.objectives[0].resize(1);
    quest.objectives[0][0].resize(1);
    file.quests.active.push_back(quest);

    const std::vector<uint8_t> bytes = tl::formats::write_character_file(file);
    tl::formats::CharacterFile back;
    TL_CHECK(tl::formats::read_character_file(bytes.data(), bytes.size(), back));
    TL_CHECK_EQ(back.class_name, std::string("Alchemist"));
    TL_CHECK_EQ(back.player.level, 7);
    TL_CHECK_EQ(back.player.hp, 123.5f);
    TL_CHECK_EQ(back.player.inventory.size(), size_t{1});
    TL_CHECK_EQ(back.player.inventory[0].name, std::string("Health Potion"));
    TL_CHECK_EQ(back.levels.size(), size_t{1});
    TL_CHECK_EQ(back.levels[0].characters[0].unit_guid, int64_t{777});
    TL_CHECK_EQ(back.levels[0].automap.size(), size_t{2});
    TL_CHECK_EQ(back.levels[0].formations[0].layout, formation.layout);
    TL_CHECK_EQ(back.trackers[0].salt, 99);
    TL_CHECK_EQ(back.quests.completed[0], std::string("INTROTOGAMEPT1"));
    TL_CHECK_EQ(back.quests.active[0].name, std::string("INTROTOGAMEPT2"));
    TL_CHECK_EQ(int(back.quests.active[0].dialogs[0][0][0]), 1);

    // A file whose last four bytes are not its size is refused.
    std::vector<uint8_t> broken = bytes;
    broken.push_back(0);
    TL_CHECK(!tl::formats::read_character_file(broken.data(), broken.size(), back));
}
