// The quest system: reading a quest file, rolling its reward, and the log the player carries
// it in. The reward check is `Vasman_Quest1`'s offer: `GP:80`, `XP:2400`, `Fame:360`.
#include "core/quest_book.h"
#include "core/random.h"
#include "core/text.h"
#include "core/unit_themes.h"
#include "core/unit_types.h"
#include "formats/adm.h"
#include "test_support.h"

#include <string>
#include <vector>

namespace {

using tl::core::QuestBook;
using tl::core::QuestDefinition;
using tl::core::QuestLog;
using tl::core::QuestObjective;
using tl::core::QuestReward;
using tl::core::QuestRewardSpec;
using tl::core::QuestSection;
using tl::core::QuestStatus;
using tl::core::StatBook;
using tl::core::StatGraph;
using tl::formats::AdmRecord;
using tl::formats::AdmSection;
using tl::formats::AdmType;

// -- building `.adm` sections by hand ---------------------------------------

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

AdmRecord bool_record(const std::string& key, bool value) {
    AdmRecord r;
    r.key = key;
    r.type = AdmType::Bool;
    r.integer = value ? 1u : 0u;
    return r;
}

AdmSection line_graph(const std::string& name, const std::vector<std::pair<float, float>>& pts) {
    AdmSection line;
    line.name = "LINE";
    line.records.push_back(text_record("NAME", name));
    line.records.push_back(bool_record("INFER_PASSED_END", true));
    for (const auto& point : pts) {
        AdmSection p;
        p.name = "POINT";
        AdmRecord x;
        x.key = "X";
        x.type = AdmType::Float;
        x.number = point.first;
        AdmRecord y;
        y.key = "Y";
        y.type = AdmType::Float;
        y.number = point.second;
        p.records.push_back(x);
        p.records.push_back(y);
        line.children.push_back(p);
    }
    return line;
}

/// The three curves a quest reward samples, at the values the shipped files
/// carry for levels 1 to 4.
StatBook reward_graphs() {
    StatBook book;
    book.add("GOLDDROP", StatGraph::from(line_graph(
                             "GoldDrop", {{1, 3.75f}, {2, 3.875f}, {3, 4.0f}, {4, 4.125f}})));
    book.add("EXPERIENCE_MONSTER",
             StatGraph::from(line_graph("Experience_Monster",
                                        {{1, 100.0f}, {2, 110.0f}, {3, 120.0f}, {4, 130.0f}})));
    book.add("FAME_CHAMPIONMONSTER",
             StatGraph::from(line_graph("Fame_ChampionMonster",
                                        {{1, 150.0f}, {2, 165.0f}, {3, 180.0f}, {4, 195.0f}})));
    return book;
}

}  // namespace

// ---------------------------------------------------------------------------
// The generator
// ---------------------------------------------------------------------------

TL_TEST(runic_random_reproduces_the_multiply_with_carry_step) {
    // `state = 0x29777b41 * (state & 0xffffffff) + (state >> 32)` from a seed of 1.
    tl::core::RunicRandom rng(1);
    TL_CHECK_EQ(rng.step(), 0x29777b41ull);

    const uint64_t low = 0x29777b41ull & 0xffffffffull;
    const uint64_t expected = 0x29777b41ull * low + (0x29777b41ull >> 32);
    TL_CHECK_EQ(rng.step(), expected);
}

TL_TEST(runic_random_between_leaves_the_stream_alone_when_the_ends_meet) {
    // `randomBetween` of an equal pair returns without a step, so a fixed gold range does not
    // move the fame roll.
    tl::core::RunicRandom rng(1234);
    TL_CHECK_EQ(rng.between(7.0f, 7.0f), 7.0f);
    TL_CHECK_EQ(rng.state(), 1234ull);

    const float rolled = rng.between(0.0f, 10.0f);
    TL_CHECK(rolled >= 0.0f && rolled <= 10.0f);
    TL_CHECK(rng.state() != 1234ull);
}

// ---------------------------------------------------------------------------
// The reward
// ---------------------------------------------------------------------------

TL_TEST(vasman_quest_one_pays_what_the_xbox_screenshot_says) {
    // Vasman's first quest: gold and XP at 2000%, fame at 200%.
    QuestRewardSpec spec;
    spec.gold_min_pct = spec.gold_max_pct = 2000.0f;
    spec.xp_min_pct = spec.xp_max_pct = 2000.0f;
    spec.fame_min_pct = spec.fame_max_pct = 200.0f;

    // The quest's own level is 2, so every curve is read at 3 - `level + 1` is
    // what `calculateRewards` passes to `CGraph::getValue`.
    const QuestReward reward = calculate_quest_reward(spec, 2, 0x1234u, reward_graphs());

    TL_CHECK_EQ(reward.gold, 80);   // ceil(4.0   * 20)
    TL_CHECK_EQ(reward.xp, 2400);   // ceil(120.0 * 20)
    TL_CHECK_EQ(reward.fame, 360);  // ceil(180.0 * 2)
}

TL_TEST(a_fixed_reward_does_not_depend_on_the_seed) {
    QuestRewardSpec spec;
    spec.gold_min_pct = spec.gold_max_pct = 2000.0f;
    spec.xp_min_pct = spec.xp_max_pct = 2000.0f;
    spec.fame_min_pct = spec.fame_max_pct = 200.0f;
    const StatBook graphs = reward_graphs();

    const QuestReward a = calculate_quest_reward(spec, 2, 1u, graphs);
    const QuestReward b = calculate_quest_reward(spec, 2, 999999u, graphs);
    TL_CHECK_EQ(a.gold, b.gold);
    TL_CHECK_EQ(a.xp, b.xp);
    TL_CHECK_EQ(a.fame, b.fame);
}

TL_TEST(a_ranged_reward_is_stable_for_one_seed_and_inside_its_range) {
    QuestRewardSpec spec;
    spec.gold_min_pct = 1000.0f;
    spec.gold_max_pct = 3000.0f;
    spec.xp_min_pct = 500.0f;
    spec.xp_max_pct = 800.0f;
    const StatBook graphs = reward_graphs();

    const QuestReward first = calculate_quest_reward(spec, 2, 4242u, graphs);
    const QuestReward again = calculate_quest_reward(spec, 2, 4242u, graphs);
    TL_CHECK_EQ(first.gold, again.gold);
    TL_CHECK_EQ(first.xp, again.xp);

    // 4.0 at level 3, times 10 to 30.
    TL_CHECK(first.gold >= 40 && first.gold <= 120);
    // 120.0 at level 3, times 5 to 8.
    TL_CHECK(first.xp >= 600 && first.xp <= 960);
}

TL_TEST(the_experience_curve_has_no_difficulty_variant) {
    // Every arm of `calculateRewards`'s switch reads `EXPERIENCE_MONSTER`: difficulty leaves
    // experience alone. Kept.
    QuestRewardSpec spec;
    spec.gold_min_pct = spec.gold_max_pct = 2000.0f;
    spec.xp_min_pct = spec.xp_max_pct = 2000.0f;
    const StatBook graphs = reward_graphs();

    const QuestReward normal = calculate_quest_reward(spec, 2, 7u, graphs, 1);
    const QuestReward hard = calculate_quest_reward(spec, 2, 7u, graphs, 2);
    TL_CHECK_EQ(normal.xp, hard.xp);
    TL_CHECK_EQ(normal.gold, 80);
    TL_CHECK_EQ(hard.gold, 0);
}

// ---------------------------------------------------------------------------
// Reading a quest file
// ---------------------------------------------------------------------------

namespace {

/// A stand-in for `media/Quests/Vasman/QUEST1.DAT.adm`, with the keys that file
/// actually carries.
AdmSection vasman_quest_one() {
    AdmSection quest;
    quest.name = "QUEST";
    quest.records.push_back(text_record("NAME", "Vasman_Quest1"));
    quest.records.push_back(text_record("DISPLAYNAME", "The |cFFFFBA00Gleaming Ember|u"));
    quest.records.push_back(text_record("DUNGEON", "main"));
    quest.records.push_back(bool_record("CANABANDON", true));
    quest.records.push_back(bool_record("SHOWQUESTCOMPLETE", true));

    AdmSection dialog;
    dialog.name = "DIALOG";
    for (const char* section : {"INTRO", "RETURN", "COMPLETE"}) {
        AdmSection entry;
        entry.name = section;
        entry.records.push_back(text_record("UNITNAME", "VASMAN"));
        entry.records.push_back(text_record("DIALOG", std::string("spoken in ") + section));
        entry.records.push_back(text_record("SOUND", "OldManGreet"));
        dialog.children.push_back(entry);
    }
    AdmSection details;
    details.name = "DETAILS";
    details.records.push_back(text_record("UNITNAME", "VASMAN"));
    details.records.push_back(
        text_record("DIALOG",
                    "Bring [ACQUIRE1COUNT] [ACQUIRE1] from floor [ACQUIRE1DEPTH]. "
                    "([ACQUIRE1COMPLETE])"));
    dialog.children.push_back(details);
    quest.children.push_back(dialog);

    AdmSection acquire;
    acquire.name = "ACQUIRE";
    acquire.records.push_back(int_record("MINCREATE", 0));
    acquire.records.push_back(int_record("MAXCREATE", 0));
    AdmSection unit;
    unit.name = "UNIT";
    unit.records.push_back(text_record("ITEM", "GleamingEmber"));
    unit.records.push_back(int_record("SPECIFICFLOOR", 0));
    unit.records.push_back(int_record("MINCOUNT", 1));
    unit.records.push_back(int_record("MAXCOUNT", 1));
    unit.records.push_back(bool_record("REMOVEFROMINVENTORY", true));
    acquire.children.push_back(unit);
    quest.children.push_back(acquire);

    AdmSection reward;
    reward.name = "REWARD";
    reward.records.push_back(int_record("GOLDMINPCT", 2000));
    reward.records.push_back(int_record("GOLDMAXPCT", 2000));
    reward.records.push_back(int_record("XPMINPCT", 2000));
    reward.records.push_back(int_record("XPMAXPCT", 2000));
    reward.records.push_back(int_record("FAMEMINPCT", 200));
    reward.records.push_back(int_record("FAMEMAXPCT", 200));
    reward.records.push_back(text_record("TREASURECLASS", "QUEST_REWARD_GEMGUY"));
    quest.children.push_back(reward);

    return quest;
}

}  // namespace

TL_TEST(a_quest_file_reads_into_a_definition) {
    QuestDefinition quest;
    TL_CHECK(parse_quest_definition(vasman_quest_one(), quest));

    TL_CHECK_EQ(quest.name, std::string("Vasman_Quest1"));
    TL_CHECK_EQ(quest.dungeon, std::string("main"));
    TL_CHECK(quest.can_abandon);
    TL_CHECK(!quest.force_accept);
    // `SHOWQUESTCOMPLETE` defaults to true and everything else to false.
    TL_CHECK(quest.show_quest_complete);
    TL_CHECK(!quest.repeatable);

    TL_CHECK_EQ(quest.acquire.size(), size_t{1});
    TL_CHECK_EQ(quest.acquire[0].item, std::string("GleamingEmber"));
    TL_CHECK_EQ(quest.acquire[0].min_count, 1);
    TL_CHECK_EQ(quest.acquire[0].max_count, 1);
    TL_CHECK_EQ(quest.acquire[0].specific_floor, 0);
    TL_CHECK(quest.defeat.empty());

    TL_CHECK_EQ(quest.reward_spec.gold_min_pct, 2000.0f);
    TL_CHECK_EQ(quest.reward_spec.treasure_class, std::string("QUEST_REWARD_GEMGUY"));

    TL_CHECK_EQ(quest.intro.size(), size_t{1});
    TL_CHECK_EQ(quest.intro[0].unit, std::string("VASMAN"));
    TL_CHECK(quest.has_details);
    TL_CHECK_EQ(quest.speakers().size(), size_t{1});
    TL_CHECK_EQ(quest.speakers()[0], std::string("VASMAN"));
}

// `CQuestUnitData::parseDataGroup`: `MINCOUNT` >= 1, `MAXCOUNT` >= it.
// `getQuestStrings`, `parseDialogTag`: empty lines dropped, the
// group's `UNITNAME` for a line naming nobody, only the first `[DETAILS]`.
TL_TEST(quest_lines_take_the_dialog_groups_speaker_and_drop_empty_ones) {
    AdmSection root;
    root.name = "QUEST";
    root.records.push_back(text_record("NAME", "Talky"));
    AdmSection dialog;
    dialog.name = "DIALOG";
    dialog.records.push_back(text_record("UNITNAME", "Bard"));
    AdmSection said;
    said.name = "INTRO";
    said.records.push_back(text_record("DIALOG", "hello"));
    AdmSection silent;
    silent.name = "INTRO";
    silent.records.push_back(text_record("UNITNAME", "Vasman"));
    AdmSection other;
    other.name = "RETURN";
    other.records.push_back(text_record("UNITNAME", "Vasman"));
    other.records.push_back(text_record("DIALOG", "back"));
    AdmSection first;
    first.name = "DETAILS";
    first.records.push_back(text_record("DIALOG", "first"));
    AdmSection second;
    second.name = "DETAILS";
    second.records.push_back(text_record("DIALOG", "second"));
    dialog.children = {said, silent, other, first, second};
    root.children.push_back(dialog);

    QuestDefinition parsed;
    TL_CHECK(parse_quest_definition(root, parsed));
    TL_CHECK_EQ(parsed.intro.size(), size_t{1});
    TL_CHECK_EQ(parsed.intro[0].unit, std::string("Bard"));
    TL_CHECK_EQ(parsed.ret[0].unit, std::string("Vasman"));
    TL_CHECK(parsed.has_details);
    TL_CHECK_EQ(parsed.details.texts[0], std::string("first"));
}

TL_TEST(an_objective_with_no_count_wants_one) {
    AdmSection quest = vasman_quest_one();
    for (AdmSection& child : quest.children) {
        if (child.name != "ACQUIRE") continue;
        std::vector<AdmRecord>& records = child.children[0].records;
        for (size_t i = records.size(); i-- > 0;) {
            if (records[i].key == "MINCOUNT" || records[i].key == "MAXCOUNT") {
                records.erase(records.begin() + long(i));
            }
        }
    }
    QuestDefinition parsed;
    TL_CHECK(parse_quest_definition(quest, parsed));
    TL_CHECK_EQ(parsed.acquire[0].min_count, 1);
    TL_CHECK_EQ(parsed.acquire[0].max_count, 1);

    // An authored 0 is raised the same way, and a `MAXCOUNT` below the minimum
    // is lifted to it.
    for (AdmSection& child : quest.children) {
        if (child.name != "ACQUIRE") continue;
        child.children[0].records.push_back(int_record("MINCOUNT", 3));
        child.children[0].records.push_back(int_record("MAXCOUNT", 2));
    }
    TL_CHECK(parse_quest_definition(quest, parsed));
    TL_CHECK_EQ(parsed.acquire[0].min_count, 3);
    TL_CHECK_EQ(parsed.acquire[0].max_count, 3);
}

TL_TEST(an_unauthored_specific_floor_is_minus_one_not_four_billion) {
    // `SPECIFICFLOOR` defaults to 0xffffffff, -1, recognised before it is narrowed.
    AdmSection quest = vasman_quest_one();
    for (AdmSection& child : quest.children) {
        if (child.name != "ACQUIRE") continue;
        std::vector<AdmRecord>& records = child.children[0].records;
        for (size_t i = 0; i < records.size(); ++i) {
            if (records[i].key == "SPECIFICFLOOR") records.erase(records.begin() + long(i));
        }
    }
    QuestDefinition parsed;
    TL_CHECK(parse_quest_definition(quest, parsed));
    TL_CHECK_EQ(parsed.acquire[0].specific_floor, -1);
}

// ---------------------------------------------------------------------------
// The log
// ---------------------------------------------------------------------------

namespace {

/// A minimal quest that occupies a tray: it has a journal entry and is not
/// `FORCEACCEPT`.
void place_nothing(const tl::core::QuestEntry&, const QuestObjective&,
                   const tl::core::QuestUnitTag&) {}

QuestDefinition journal_quest(const std::string& name) {
    QuestDefinition quest;
    quest.name = name;
    quest.display_name = name;
    quest.can_abandon = true;
    quest.has_details = true;
    quest.details.texts = {name + " details"};
    tl::core::DialogLine intro;
    intro.unit = "SEER";
    intro.texts = {"take this"};
    quest.intro.push_back(intro);
    return quest;
}

}  // namespace

TL_TEST(the_quest_dialog_refuses_a_seventh_journal_quest_and_give_quest_does_not) {
    // `CQuestDialogMenu::onClick` refuses at six held journal quests;
    // `CQuestManager::giveQuest` has no limit of its own.
    QuestBook book;
    for (int i = 0; i < 8; ++i) book.add(journal_quest("Q" + std::to_string(i)));

    QuestLog log;
    log.set_book(&book);
    const StatBook graphs = reward_graphs();

    for (int i = 0; i < tl::core::kQuestLogSize; ++i) {
        const QuestDefinition* quest = book.find("Q" + std::to_string(i));
        TL_CHECK(quest != nullptr);
        TL_CHECK(log.accept(*quest, 1, 1u, graphs));
    }
    TL_CHECK(log.full());
    TL_CHECK_EQ(log.journal_entries().size(), size_t{tl::core::kQuestLogSize});

    const QuestDefinition* seventh = book.find("Q6");
    TL_CHECK(log.log_full_for(*seventh));
    TL_CHECK(log.accept(*seventh, 1, 1u, graphs));
    TL_CHECK_EQ(log.journal_entries().size(), size_t{tl::core::kQuestLogSize + 1});
}

TL_TEST(the_held_quests_keep_the_managers_order_a_completion_moves_the_last_in) {
    // `CQuestManager::completeQuest` and `removeQuest`
    // take a quest off the held quests by putting the last in its place.
    QuestBook book;
    for (int i = 0; i < 4; ++i) book.add(journal_quest("Q" + std::to_string(i)));
    QuestLog log;
    log.set_book(&book);
    const StatBook graphs = reward_graphs();
    for (int i = 0; i < 4; ++i)
        TL_CHECK(log.accept(*book.find("Q" + std::to_string(i)), 1, 1u, graphs));
    TL_CHECK(log.complete("Q1"));
    std::vector<std::string> names;
    for (const tl::core::QuestEntry* e : log.held_entries()) names.push_back(e->name());
    TL_CHECK_EQ(names.size(), size_t{3});
    TL_CHECK(names.size() == 3 && names[0] == "Q0" && names[1] == "Q3" && names[2] == "Q2");
}

TL_TEST(a_forced_or_journal_less_quest_is_never_refused_for_room) {
    QuestBook book;
    for (int i = 0; i < tl::core::kQuestLogSize; ++i)
        book.add(journal_quest("Q" + std::to_string(i)));

    QuestDefinition forced = journal_quest("Cutscene");
    forced.force_accept = true;
    book.add(forced);

    QuestDefinition silent = journal_quest("Silent");
    silent.has_details = false;
    silent.details.texts.clear();
    book.add(silent);

    QuestLog log;
    log.set_book(&book);
    const StatBook graphs = reward_graphs();
    for (int i = 0; i < tl::core::kQuestLogSize; ++i) {
        TL_CHECK(log.accept(*book.find("Q" + std::to_string(i)), 1, 1u, graphs));
    }
    TL_CHECK(log.full());

    // The dialog takes both: the count is skipped for either.
    TL_CHECK(!log.log_full_for(*book.find("Cutscene")));
    TL_CHECK(!log.log_full_for(*book.find("Silent")));
    TL_CHECK(log.accept(*book.find("Cutscene"), 1, 1u, graphs));
    TL_CHECK(log.accept(*book.find("Silent"), 1, 1u, graphs));

    // `FORCEACCEPT` waives the check, not the tray: a quest with a journal entry shows; one
    // with no `[DETAILS]` does not.
    TL_CHECK(tl::core::QuestLog::shows_in_journal(*book.find("Cutscene")));
    TL_CHECK(!tl::core::QuestLog::needs_a_free_tray(*book.find("Cutscene")));
    TL_CHECK(!tl::core::QuestLog::shows_in_journal(*book.find("Silent")));
    TL_CHECK_EQ(log.journal_entries().size(), size_t{tl::core::kQuestLogSize + 1});
}

TL_TEST(accepting_a_quest_moves_the_gate_state_with_it) {
    QuestBook book;
    book.add(journal_quest("AidBrink"));
    QuestLog log;
    log.set_book(&book);

    TL_CHECK_EQ(static_cast<int>(log.state().status("AidBrink")),
                static_cast<int>(QuestStatus::Unknown));
    TL_CHECK(log.accept(*book.find("AidBrink"), 1, 1u, reward_graphs()));
    TL_CHECK(log.state().is_active("AidBrink"));

    QuestReward paid;
    TL_CHECK(log.complete("AidBrink", &paid));
    TL_CHECK(log.state().is_complete("AidBrink"));
    // Completing it takes the tray back.
    TL_CHECK(log.journal_entries().empty());
}

TL_TEST(only_an_abandonable_quest_can_be_abandoned) {
    QuestBook book;
    book.add(journal_quest("Free"));
    QuestDefinition stuck = journal_quest("Stuck");
    stuck.can_abandon = false;
    book.add(stuck);

    QuestLog log;
    log.set_book(&book);
    const StatBook graphs = reward_graphs();
    TL_CHECK(log.accept(*book.find("Free"), 1, 1u, graphs));
    TL_CHECK(log.accept(*book.find("Stuck"), 1, 1u, graphs));

    TL_CHECK(!log.abandon("Stuck"));
    TL_CHECK(log.abandon("Free"));
    TL_CHECK_EQ(static_cast<int>(log.state().status("Free")),
                static_cast<int>(QuestStatus::Unknown));
}

// What `CQuestManager` tells the quests' controllers (`QuestNotice`), under the quest's
// upper-cased `NAME`.
TL_TEST(quest_changes_are_told_to_the_controllers) {
    QuestBook book;
    QuestDefinition first = journal_quest("AidBrink");
    first.give_quests_on_complete.push_back("NextStep");
    book.add(first);
    book.add(journal_quest("NextStep"));
    book.add(journal_quest("Free"));
    QuestLog log;
    log.set_book(&book);
    const StatBook graphs = reward_graphs();

    TL_CHECK(log.accept(*book.find("AidBrink"), 1, 1u, graphs));
    std::vector<tl::core::QuestNotice> told = log.take_notices();
    TL_CHECK_EQ(told.size(), size_t{1});
    TL_CHECK(told[0].quest == "AIDBRINK");
    TL_CHECK(told[0].outputs == std::vector<uint32_t>{0x62});

    TL_CHECK(log.complete("AidBrink"));
    told = log.take_notices();
    TL_CHECK_EQ(told.size(), size_t{2});
    TL_CHECK(told[0].quest == "AIDBRINK" && told[0].outputs == std::vector<uint32_t>{0x64});
    TL_CHECK(told[1].quest == "NEXTSTEP" && told[1].outputs == std::vector<uint32_t>{0x62});

    TL_CHECK(log.accept(*book.find("Free"), 1, 1u, graphs));
    log.take_notices();
    TL_CHECK(log.abandon("Free"));
    told = log.take_notices();
    TL_CHECK_EQ(told.size(), size_t{1});
    TL_CHECK(told[0].quest == "FREE");
    TL_CHECK((told[0].outputs == std::vector<uint32_t>{0x63, 0x66}));
    TL_CHECK(log.take_notices().empty());
}

// `CQuestManager::completeQuest` does not test the state: a second close pays the
// gold, experience and fame again (`CQuestRewards::rewardPlayer`) but no items. Kept.
TL_TEST(a_complete_line_closed_twice_completes_the_quest_twice) {
    QuestBook book;
    book.add(journal_quest("AidBrink"));
    QuestLog log;
    log.set_book(&book);
    TL_CHECK(log.accept(*book.find("AidBrink"), 1, 1u, reward_graphs()));
    log.take_notices();

    TL_CHECK(log.complete_by_dialog(*book.find("AidBrink")));
    std::vector<tl::core::QuestCompletion> first = log.take_completions();
    TL_CHECK_EQ(first.size(), size_t{1});
    TL_CHECK(first[0].held && first[0].reward_items);

    // Any other way of completing it does nothing now ...
    TL_CHECK(!log.complete("AidBrink"));
    TL_CHECK(log.take_completions().empty());
    log.take_notices();

    // ... the dialogue's does it again.
    TL_CHECK(log.complete_by_dialog(*book.find("AidBrink")));
    std::vector<tl::core::QuestCompletion> second = log.take_completions();
    TL_CHECK_EQ(second.size(), size_t{1});
    TL_CHECK(second[0].held);
    TL_CHECK(!second[0].reward_items);
    TL_CHECK_EQ(second[0].reward.gold, first[0].reward.gold);
    TL_CHECK_EQ(second[0].reward.xp, first[0].reward.xp);
    TL_CHECK_EQ(second[0].reward.fame, first[0].reward.fame);
    const std::vector<tl::core::QuestNotice> told = log.take_notices();
    TL_CHECK_EQ(told.size(), size_t{1});
    TL_CHECK(told[0].quest == "AIDBRINK" && told[0].outputs == std::vector<uint32_t>{0x64});

    // Taking it again re-rolls the rewards, items and all (`reInitializeRewards`).
    TL_CHECK(log.accept(*book.find("AidBrink"), 1, 1u, reward_graphs()));
    TL_CHECK(log.complete_by_dialog(*book.find("AidBrink")));
    std::vector<tl::core::QuestCompletion> third = log.take_completions();
    TL_CHECK_EQ(third.size(), size_t{1});
    TL_CHECK(third[0].reward_items);
}

// `CQuest::questEventUpdate` event 3, `isComplete`: a quest
// naming another under `[COMPLETE_ON_QUEST_ACCEPT]` completes when that one is taken.
TL_TEST(complete_on_quest_accept_completes_the_quest_that_names_it) {
    QuestBook book;
    QuestDefinition waiting = journal_quest("Waiting");
    waiting.complete_on_quest_accept.push_back("LATER");
    book.add(waiting);
    book.add(journal_quest("Later"));

    QuestLog log;
    log.set_book(&book);
    TL_CHECK(log.accept(*book.find("Waiting"), 1, 1u, reward_graphs()));
    // Its one flag stands open, so it is not complete, acknowledged or not.
    log.find("Waiting")->dialog_interacted = true;
    TL_CHECK(!log.find("Waiting")->objectives_complete());
    TL_CHECK(log.take_finished().empty());

    TL_CHECK(log.accept(*book.find("Later"), 1, 1u, reward_graphs()));
    TL_CHECK(!log.state().is_complete("Later"));
    TL_CHECK(log.find("Waiting")->objectives_complete());
    const std::vector<const QuestDefinition*> finished = log.take_finished();
    TL_CHECK_EQ(finished.size(), size_t{1});
    TL_CHECK_EQ(finished[0]->name, std::string("Waiting"));
    // Complete to its objectives; it is still turned in to its giver.
    TL_CHECK(log.state().is_active("Waiting"));
}

// `completeQuest` sends event 4 before recording the
// completion, so the flag waits for the next event 4.
TL_TEST(complete_on_quest_complete_waits_for_the_next_completion) {
    QuestBook book;
    QuestDefinition waiting = journal_quest("Waiting");
    waiting.complete_on_quest_complete.push_back("FIRST");
    book.add(waiting);
    book.add(journal_quest("First"));
    book.add(journal_quest("Second"));

    QuestLog log;
    log.set_book(&book);
    TL_CHECK(log.accept(*book.find("Waiting"), 1, 1u, reward_graphs()));
    TL_CHECK(log.accept(*book.find("First"), 1, 1u, reward_graphs()));
    TL_CHECK(log.accept(*book.find("Second"), 1, 1u, reward_graphs()));
    TL_CHECK(log.complete("First"));
    TL_CHECK(!log.find("Waiting")->objectives_complete());
    TL_CHECK(log.complete("Second"));
    TL_CHECK(log.find("Waiting")->completed_named[0] != 0);
}

TL_TEST(give_quests_on_complete_hands_the_next_one_over) {
    QuestBook book;
    QuestDefinition first = journal_quest("Part1");
    first.give_quests_on_complete.push_back("Part2");
    book.add(first);
    book.add(journal_quest("Part2"));

    QuestLog log;
    log.set_book(&book);
    TL_CHECK(log.accept(*book.find("Part1"), 1, 1u, reward_graphs()));
    TL_CHECK(log.complete("Part1"));
    TL_CHECK(log.state().is_active("Part2"));
}

TL_TEST(objectives_are_marked_off_by_kills_and_pickups) {
    QuestDefinition quest = journal_quest("KillThree");
    QuestObjective wolves;
    wolves.monster = "Ratlin";
    wolves.min_count = 3;
    wolves.max_count = 3;
    quest.defeat.push_back(wolves);

    QuestObjective ember;
    ember.item = "GleamingEmber";
    ember.min_count = 1;
    ember.max_count = 1;
    ember.kind = 1;  // an `[ACQUIRE]` unit
    quest.acquire.push_back(ember);

    QuestBook book;
    book.add(quest);
    QuestLog log;
    log.set_book(&book);
    TL_CHECK(log.accept(*book.find("KillThree"), 2, 5u, reward_graphs()));
    // Nothing counts until the level has placed the objectives. The quest names
    // no dungeon, so it creates nothing and any unit of the kind counts.
    log.populate_level("", 0, place_nothing);

    const tl::core::QuestEntry* entry = log.find("KillThree");
    TL_CHECK(entry != nullptr);
    TL_CHECK_EQ(entry->defeat[0].wanted, 3);
    TL_CHECK(!entry->objectives_complete());

    log.unit_defeated("ratlin");  // matched without case, like every other name
    log.unit_defeated("Ratlin");
    TL_CHECK(!log.find("KillThree")->objectives_complete());
    log.unit_defeated("Ratlin");
    TL_CHECK(log.find("KillThree")->defeat[0].complete());

    TL_CHECK(!log.find("KillThree")->objectives_complete());
    TL_CHECK_EQ(log.item_picked_up("GleamingEmber").size(), 1u);  // this pickup finished it
    TL_CHECK(log.find("KillThree")->objectives_complete());
}

TL_TEST(an_objective_with_no_authored_count_needs_exactly_one) {
    // A `[DEFEAT]` with no counts: `unitDefeated` increments before it tests, so one kill
    // settles it and none does not.
    QuestDefinition quest = journal_quest("SlayIt");
    QuestObjective boss;
    boss.monster = "Varkolyn Mage";
    boss.unique_monster_name = "Varkaseer";
    quest.defeat.push_back(boss);

    QuestBook book;
    book.add(quest);
    QuestLog log;
    log.set_book(&book);
    TL_CHECK(log.accept(*book.find("SlayIt"), 2, 1u, reward_graphs()));
    log.populate_level("", 0, place_nothing);

    TL_CHECK_EQ(log.find("SlayIt")->defeat[0].wanted, 0);
    TL_CHECK(!log.find("SlayIt")->objectives_complete());

    // A journal line prints the unit's own name (`getUnitName`), not
    // `UNIQUE_MONSTER_NAME`.
    TL_CHECK_EQ(log.find("SlayIt")->defeat[0].unit_name(), std::string("Varkolyn Mage"));

    log.unit_defeated("Varkolyn Mage");
    TL_CHECK(log.find("SlayIt")->objectives_complete());
}

TL_TEST(a_quest_controller_quest_is_never_complete_by_itself) {
    // A `QUESTCONTROLLERCOMPLETES` quest with no objectives shows no "Quest Complete" line.
    QuestDefinition quest = journal_quest("AidBrink");
    quest.quest_controller_completes = true;
    quest.force_accept = true;

    QuestBook book;
    book.add(quest);
    QuestLog log;
    log.set_book(&book);
    TL_CHECK(log.accept(*book.find("AidBrink"), 1, 1u, reward_graphs()));

    tl::core::QuestEntry* entry = log.find("AidBrink");
    TL_CHECK(!entry->objectives_complete());
    entry->dialog_interacted = true;
    TL_CHECK(!entry->objectives_complete());
}

TL_TEST(a_quest_that_asks_for_nothing_is_done_when_its_offer_is_acknowledged) {
    QuestDefinition quest = journal_quest("JustTalk");
    QuestBook book;
    book.add(quest);
    QuestLog log;
    log.set_book(&book);
    TL_CHECK(log.accept(*book.find("JustTalk"), 1, 1u, reward_graphs()));

    tl::core::QuestEntry* entry = log.find("JustTalk");
    TL_CHECK(!entry->has_objectives());
    TL_CHECK(!entry->objectives_complete());
    entry->dialog_interacted = true;
    TL_CHECK(entry->objectives_complete());
}

TL_TEST(the_section_a_giver_speaks_from_follows_the_quest_state) {
    QuestDefinition quest = journal_quest("Errand");
    tl::core::DialogLine ret;
    ret.unit = "SEER";
    ret.texts = {"not yet"};
    quest.ret.push_back(ret);
    tl::core::DialogLine done;
    done.unit = "SEER";
    done.texts = {"well done"};
    quest.complete.push_back(done);
    QuestObjective ember;
    ember.item = "GleamingEmber";
    ember.min_count = 1;
    ember.max_count = 1;
    ember.kind = 1;
    quest.acquire.push_back(ember);

    QuestBook book;
    book.add(quest);
    QuestLog log;
    log.set_book(&book);
    const QuestDefinition* def = book.find("Errand");

    // `CQuest::getQuestDialog`, by the quest's state.
    const auto section_now = [&] {
        QuestSection section = QuestSection::Passive;
        TL_CHECK(log.dialog_for(*def, "SEER", &section) != nullptr);
        return static_cast<int>(section);
    };
    TL_CHECK_EQ(section_now(), static_cast<int>(QuestSection::Intro));
    TL_CHECK(log.accept(*def, 1, 1u, reward_graphs()));
    // Taken, the giver still speaks the `[INTRO]` line until it has been
    // shown (`+0x76`); then the `[RETURN]` one.
    TL_CHECK_EQ(section_now(), static_cast<int>(QuestSection::Intro));
    log.find("Errand")->dialog_interacted = true;
    TL_CHECK_EQ(section_now(), static_cast<int>(QuestSection::Return));
    log.populate_level("", 0, place_nothing);
    log.item_picked_up("GleamingEmber");
    TL_CHECK_EQ(section_now(), static_cast<int>(QuestSection::Complete));
}

TL_TEST(a_quest_already_taken_from_an_npc_beats_anything_else_they_have) {
    // `getQuestForNPC` returns the player's own quest from this giver before it
    // rolls among the candidates.
    QuestBook book;
    book.add(journal_quest("AAA_First"));
    book.add(journal_quest("ZZZ_Second"));
    QuestLog log;
    log.set_book(&book);
    tl::core::RunicRandom roll(7);

    const QuestDefinition* offered = log.quest_for_npc("SEER", {}, roll);
    TL_CHECK(offered != nullptr);
    TL_CHECK(log.accept(*offered, 1, 1u, reward_graphs()));
    log.find(offered->name)->dialog_interacted = true;
    for (int i = 0; i < 8; ++i) {
        TL_CHECK_EQ(log.quest_for_npc("SEER", {}, roll)->name, offered->name);
    }
}

TL_TEST(a_quest_of_passive_lines_speaks_once_it_is_given) {
    // `CQuest::getQuestDialog` offers a passive line only for an accepted quest.
    QuestDefinition quest;
    quest.name = "RandomMiners";
    quest.quest_controller_completes = true;
    tl::core::DialogLine line;
    line.unit = "MINERTOWN1";
    line.texts = {"this load of Ember may be the last"};
    line.sound = "MINERGREET";
    quest.passive.push_back(line);

    QuestBook book;
    book.add(quest);
    QuestLog log;
    log.set_book(&book);
    tl::core::RunicRandom roll(1);
    const QuestDefinition* def = book.find("RandomMiners");

    TL_CHECK(log.dialog_for(*def, "MinerTown1") == nullptr);
    // `getQuestIsValidForNPC`: with no line for the unit only a
    // `REPEATABLE` quest (`+0x20b`) is valid - not one a controller completes.
    TL_CHECK(!log.valid_for_npc(*def, "MinerTown1"));
    {
        QuestDefinition again = quest;
        again.name = "RandomAgain";
        again.repeatable = true;
        QuestBook other;
        other.add(again);
        QuestLog fresh;
        fresh.set_book(&other);
        TL_CHECK(fresh.valid_for_npc(*other.find("RandomAgain"), "MinerTown1"));
    }

    TL_CHECK(log.accept(*def, 1, 1u, reward_graphs()));
    tl::core::QuestSection section = tl::core::QuestSection::Intro;
    const QuestDefinition* from = nullptr;
    const tl::core::DialogLine* said = log.dialog_for_npc("MinerTown1", {}, roll, &from, &section);
    TL_CHECK(said != nullptr);
    TL_CHECK_EQ(said->sound, std::string("MINERGREET"));
    TL_CHECK_EQ(static_cast<int>(section), static_cast<int>(tl::core::QuestSection::Passive));
    TL_CHECK(log.dialog_for_npc("MinerTown2", {}, roll, &from, &section) == nullptr);
}

TL_TEST(an_answered_intro_gives_way_to_the_return_line) {
    QuestDefinition quest = journal_quest("Errand");
    tl::core::DialogLine ret;
    ret.unit = "SEER";
    ret.texts = {"not yet"};
    quest.ret.push_back(ret);
    QuestBook book;
    book.add(quest);
    QuestLog log;
    log.set_book(&book);
    const QuestDefinition* def = book.find("Errand");

    tl::core::QuestSection section = tl::core::QuestSection::Return;
    TL_CHECK_EQ(log.dialog_for(*def, "seer", &section)->texts[0], std::string("take this"));
    TL_CHECK(log.accept(*def, 1, 1u, reward_graphs()));
    TL_CHECK_EQ(log.dialog_for(*def, "seer", &section)->texts[0], std::string("take this"));
    log.find("Errand")->dialog_interacted = true;
    TL_CHECK_EQ(log.dialog_for(*def, "seer", &section)->texts[0], std::string("not yet"));
    TL_CHECK_EQ(static_cast<int>(section), static_cast<int>(tl::core::QuestSection::Return));
}

/// A player for the requirements test: on a level, with `MAIN`'s deepest floor
/// and the template `NAME` each depth answers with.
tl::core::QuestPlayer requirement_player(uint32_t level, int32_t deepest,
                                         const std::string& ruleset) {
    tl::core::QuestPlayer player;
    player.present = true;
    player.on_level = true;
    player.level = level;
    player.max_depth = [deepest](const std::string& dungeon) {
        return dungeon == "MAIN" ? deepest : -1;
    };
    player.template_name = [ruleset](const std::string& dungeon, uint32_t, std::string& name) {
        if (dungeon != "MAIN") return false;
        name = ruleset;
        return true;
    };
    return player;
}

TL_TEST(requirements_hold_a_quest_back) {
    QuestDefinition gated = journal_quest("Later");
    gated.requirements.present = true;
    gated.requirements.min_player_level = 5;
    gated.requirements.max_player_level = 0xffffffffu;
    gated.requirements.min_dungeon_depth = -1;
    gated.requirements.quests_complete.push_back("EARLIER");

    QuestBook book;
    book.add(gated);
    QuestLog log;
    log.set_book(&book);
    uint32_t level = 1;
    log.set_player_source([&] { return requirement_player(level, 0, "TOWN"); });

    TL_CHECK(!log.requirements_met(*book.find("Later")));
    level = 5;
    TL_CHECK(!log.requirements_met(*book.find("Later")));
    log.state().complete("Earlier");
    TL_CHECK(log.requirements_met(*book.find("Later")));
    // No player: nothing with a `[REQUIREMENTS]` group is offered.
    log.set_player_source(nullptr);
    TL_CHECK(!log.requirements_met(*book.find("Later")));
}

// `parseRequirementTag`: an unauthored number is 0xffffffff, names are
// upper-cased, a list takes every string value whatever its key.
TL_TEST(requirements_parse_as_the_console_reads_them) {
    AdmSection quest;
    quest.name = "QUEST";
    quest.records.push_back(text_record("NAME", "EmberHunterCaves"));
    AdmSection need;
    need.name = "REQUIREMENTS";
    need.records.push_back(text_record("RULESET", "Cave"));
    need.records.push_back(int_record("MINDUNGEONDEPTH", 3));
    AdmSection done;
    done.name = "QUESTSCOMPLETE";
    done.records.push_back(text_record("QUEST", "Vasman_Quest1"));
    done.records.push_back(text_record("OTHER", "Vasman_Quest2"));
    done.records.push_back(int_record("COUNT", 2));
    need.children.push_back(done);
    quest.children.push_back(need);

    QuestDefinition def;
    TL_CHECK(tl::core::parse_quest_definition(quest, def));
    const tl::core::QuestRequirementSpec& r = def.requirements;
    TL_CHECK(r.present);
    TL_CHECK_EQ(r.min_player_level, 0xffffffffu);
    TL_CHECK_EQ(r.max_player_level, 0xffffffffu);
    TL_CHECK_EQ(r.min_dungeon_depth, 3);
    TL_CHECK_EQ(r.max_dungeon_depth, -1);
    TL_CHECK_EQ(r.ruleset, std::string("CAVE"));
    TL_CHECK_EQ(r.quests_complete.size(), size_t{2});
    TL_CHECK_EQ(r.quests_complete[1], std::string("VASMAN_QUEST2"));

    // No group: the constructor's numbers, and the quest passes untested.
    QuestDefinition open_quest;
    AdmSection bare;
    bare.name = "QUEST";
    bare.records.push_back(text_record("NAME", "Open"));
    TL_CHECK(tl::core::parse_quest_definition(bare, open_quest));
    TL_CHECK(!open_quest.requirements.present);
    TL_CHECK_EQ(open_quest.requirements.max_player_level, 999999u);
    QuestBook book;
    book.add(open_quest);
    QuestLog log;
    log.set_book(&book);
    TL_CHECK(log.requirements_met(*book.find("Open")));
}

// `questRequirmentsHaveBeenMet`: the next floor's template must carry the
// `RULESET`; the depth range is signed, so a minimum alone shuts the quest. Kept.
TL_TEST(requirements_test_the_next_floors_ruleset_and_depth) {
    QuestDefinition caves = journal_quest("EmberHunterCaves");
    caves.requirements.present = true;
    caves.requirements.min_player_level = 0xffffffffu;
    caves.requirements.min_dungeon_depth = -1;
    caves.requirements.ruleset = "CAVE";
    QuestDefinition deep = journal_quest("Deep");
    deep.requirements.present = true;
    deep.requirements.min_player_level = 0xffffffffu;
    deep.requirements.min_dungeon_depth = 3;
    deep.requirements.max_dungeon_depth = 5;
    QuestDefinition no_max = journal_quest("NoMax");
    no_max.requirements.present = true;
    no_max.requirements.min_player_level = 0xffffffffu;
    no_max.requirements.min_dungeon_depth = 0;
    no_max.requirements.max_dungeon_depth = -1;

    QuestBook book;
    book.add(caves);
    book.add(deep);
    book.add(no_max);
    QuestLog log;
    log.set_book(&book);
    int32_t deepest = -1;
    std::string ruleset = "MINE";
    log.set_player_source([&] { return requirement_player(1, deepest, ruleset); });

    TL_CHECK(!log.requirements_met(*book.find("EmberHunterCaves")));
    ruleset = "CAVE";
    TL_CHECK(log.requirements_met(*book.find("EmberHunterCaves")));
    // Never below: the next floor is 0.
    TL_CHECK(!log.requirements_met(*book.find("Deep")));
    deepest = 2;  // the next floor is 3
    TL_CHECK(log.requirements_met(*book.find("Deep")));
    deepest = 5;
    TL_CHECK(!log.requirements_met(*book.find("Deep")));
    TL_CHECK(!log.requirements_met(*book.find("NoMax")));
}

// `parseRewardTag`: the n-th `[UNITREWARD]` is looked up in
// the n-th resource list, and its flag is `IDENTIFY`.
TL_TEST(unit_rewards_keep_their_groups_index) {
    AdmSection quest;
    quest.name = "QUEST";
    quest.records.push_back(text_record("NAME", "Paid"));
    AdmSection reward;
    reward.name = "REWARD";
    for (const char* unit : {"Sword", "Bat"}) {
        AdmSection one;
        one.name = "UNITREWARD";
        one.records.push_back(text_record("UNITNAME", unit));
        one.records.push_back(bool_record("IDENTIFY", false));
        reward.children.push_back(one);
    }
    quest.children.push_back(reward);
    QuestDefinition def;
    TL_CHECK(tl::core::parse_quest_definition(quest, def));
    TL_CHECK_EQ(def.reward_spec.unit_rewards.size(), size_t{2});
    TL_CHECK_EQ(def.reward_spec.unit_rewards[1].resource_group, 1u);
    TL_CHECK(!def.reward_spec.unit_rewards[0].identify);
}

// ---------------------------------------------------------------------------
// The journal entry's `[TAG]`s
// ---------------------------------------------------------------------------

TL_TEST(a_journal_entry_substitutes_its_tags) {
    QuestDefinition quest;
    TL_CHECK(parse_quest_definition(vasman_quest_one(), quest));
    QuestBook book;
    book.add(quest);

    QuestLog log;
    log.set_book(&book);
    TL_CHECK(log.accept(*book.find("Vasman_Quest1"), 2, 1u, reward_graphs()));

    const tl::core::QuestEntry* entry = log.find("Vasman_Quest1");
    // The tags count from 1 and a floor is printed from 1 (`getAcquireTags`).
    //
    TL_CHECK_EQ(entry->details(), std::string("Bring 1 GleamingEmber from floor 1. (Incomplete)"));

    tl::core::QuestUnitTag placed;
    log.populate_level("MAIN", 0,
                       [&](const tl::core::QuestEntry&, const QuestObjective&,
                           const tl::core::QuestUnitTag& tag) { placed = tag; });
    log.item_picked_up("GleamingEmber", placed);
    TL_CHECK_EQ(log.find("Vasman_Quest1")->details(),
                std::string("Bring 1 GleamingEmber from floor 1. (Complete)"));
}

TL_TEST(the_longer_tag_wins_over_its_own_prefix) {
    // `[ACQUIRE1]` is a prefix of `[ACQUIRE1COUNT]`, so the order the
    // substitutions run in is load-bearing.
    tl::core::QuestEntry entry;
    QuestObjective ember;
    ember.item = "Ember";
    ember.wanted = 4;
    entry.acquire.push_back(ember);
    entry.depth = 2;

    TL_CHECK_EQ(replace_quest_tags("[ACQUIRE1COUNT] x [ACQUIRE1] at [DEPTH]", entry),
                std::string("4 x Ember at 3"));
    // The first objective is 1: a `0` tag is nobody's.
    TL_CHECK_EQ(replace_quest_tags("[ACQUIRE0]", entry), std::string("[ACQUIRE0]"));
}

// `CQuest::replaceStringTags`: counts so far, the floor plus `RELATIVEFLOOR`,
// `COMPLETE` per kind, and a DEFEAT's children as `[DEFEAT<i>ACQUIRE<j>]`.
TL_TEST(quest_tags_follow_replace_string_tags) {
    tl::core::QuestEntry entry;
    entry.depth = 4;

    QuestObjective boss;
    boss.monster = "Ratlin";
    boss.display_name = "Ratlin {the} Foreman";
    boss.wanted = 10;
    boss.have = 3;
    boss.relative_floor = 2;
    boss.done = true;
    QuestObjective relic;
    relic.item = "Relic";
    relic.wanted = 1;
    relic.specific_floor = 9;
    boss.children.push_back(relic);
    entry.defeat.push_back(boss);

    QuestObjective ember;
    ember.item = "Ember";
    ember.wanted = 5;
    ember.have = 2;
    ember.done = true;
    QuestObjective shard;
    shard.item = "Shard";
    ember.children.push_back(shard);
    entry.acquire.push_back(ember);
    QuestObjective gem;
    gem.unit_type = "GEM";
    gem.display_name = "Gem {x}";
    gem.have = 4;
    gem.relative_floor = -9;
    entry.acquire.push_back(gem);

    TL_CHECK_EQ(replace_quest_tags("[DEFEATCOMPLETECOUNT]/[DEFEAT1COUNT]", entry),
                std::string("3/10"));
    TL_CHECK_EQ(replace_quest_tags("[ACQUIRECOMPLETECOUNT]", entry), std::string("6"));
    // 4 + 2, shown from 1; a sum under 0 keeps the quest's floor.
    TL_CHECK_EQ(replace_quest_tags("[DEFEAT1DEPTH] [ACQUIRE2DEPTH] [DEPTH]", entry),
                std::string("7 5 5"));
    // The braces go from a unit's name, not from a unit type's.
    TL_CHECK_EQ(replace_quest_tags("[DEFEAT1]|[ACQUIRE2]", entry),
                std::string("Ratlin  Foreman|Gem {x}"));
    // The boss is settled though its relic is not; the ember is not until its
    // shard is.
    TL_CHECK_EQ(replace_quest_tags("[DEFEAT1COMPLETE] [ACQUIRE1COMPLETE]", entry),
                std::string("Complete Incomplete"));
    TL_CHECK_EQ(replace_quest_tags("[DEFEAT1ACQUIRE1] on [DEFEAT1ACQUIRE1DEPTH]: "
                                   "[DEFEAT1ACQUIRE1COMPLETE]",
                                   entry),
                std::string("Relic on 10: Incomplete"));
}

// `CQuest::reinitializeQuest`: one past the deepest floor, further past templates
// refusing random quests, up to ten tries.
TL_TEST(a_quests_floor_is_one_past_the_deepest_the_player_has_been) {
    tl::core::QuestDungeon dungeon;
    dungeon.found = true;
    dungeon.depth_count = 35;
    // Never been there (-1): the first floor.
    dungeon.max_depth = -1;
    TL_CHECK_EQ(tl::core::roll_quest_floor(false, dungeon).depth, 0);
    dungeon.max_depth = 4;
    TL_CHECK_EQ(tl::core::roll_quest_floor(false, dungeon).depth, 5);
    TL_CHECK(!tl::core::roll_quest_floor(false, dungeon).exhausted);

    // Floors 5 and 6 refuse random quests: the third try stands.
    std::vector<int> asked;
    dungeon.no_random_quests = [&](int floor) {
        asked.push_back(floor);
        return floor < 7;
    };
    TL_CHECK_EQ(tl::core::roll_quest_floor(false, dungeon).depth, 7);
    TL_CHECK_EQ(asked.size(), 3u);
    TL_CHECK_EQ(asked[0], 5);

    // Every floor refuses: ten tries, and the last one tried stands.
    asked.clear();
    dungeon.no_random_quests = [&](int floor) {
        asked.push_back(floor);
        return true;
    };
    TL_CHECK_EQ(tl::core::roll_quest_floor(false, dungeon).depth, 14);
    TL_CHECK_EQ(asked.size(), 10u);

    // A repeatable quest runs out at the dungeon's depth count; not in a bottomless dungeon.
    dungeon.no_random_quests = nullptr;
    dungeon.max_depth = 33;
    TL_CHECK(!tl::core::roll_quest_floor(true, dungeon).exhausted);
    TL_CHECK_EQ(tl::core::roll_quest_floor(true, dungeon).depth, 34);
    dungeon.max_depth = 34;
    TL_CHECK(tl::core::roll_quest_floor(true, dungeon).exhausted);
    TL_CHECK_EQ(tl::core::roll_quest_floor(true, dungeon).depth, 35);
    TL_CHECK(!tl::core::roll_quest_floor(false, dungeon).exhausted);
    dungeon.bottomless = true;
    TL_CHECK(!tl::core::roll_quest_floor(true, dungeon).exhausted);
}

// `initializeQuestWithNPC`: seeded on the volatile stream and rolled a floor the
// accepted copy keeps.
TL_TEST(an_npcs_quest_is_seeded_and_bound_to_a_floor) {
    QuestDefinition quest = journal_quest("Fetch");
    quest.dungeon = "Main";
    quest.repeatable = true;
    QuestBook book;
    book.add(quest);
    QuestLog log;
    log.set_book(&book);
    int deepest = 2;
    log.set_dungeon_source([&](const std::string& name) {
        tl::core::QuestDungeon dungeon;
        dungeon.found = name == "MAIN";
        dungeon.max_depth = deepest;
        dungeon.depth_count = 5;
        return dungeon;
    });
    const QuestDefinition* def = book.find("Fetch");
    TL_CHECK(log.offered(*def) == nullptr);

    tl::core::RunicRandom roll(11);
    tl::core::RunicRandom mirror(11);
    TL_CHECK(log.quest_for_npc("SEER", {}, roll) == def);
    (void)mirror.integer_between(0, 0);  // the pick among one candidate
    const QuestLog::Offered* rolled = log.offered(*def);
    TL_CHECK(rolled != nullptr);
    TL_CHECK_EQ(rolled->seed, mirror.integer_between(0, 0x7fffffff));
    TL_CHECK_EQ(rolled->depth, 3);
    TL_CHECK(!rolled->exhausted);
    TL_CHECK_EQ(roll.state(), mirror.state());

    TL_CHECK(log.accept(*def, 1, static_cast<uint32_t>(rolled->seed), reward_graphs()));
    TL_CHECK_EQ(log.find("Fetch")->depth, 3);
    TL_CHECK_EQ(replace_quest_tags("[DEPTH]", *log.find("Fetch")), std::string("4"));

    // Another quest, taken up once the player has reached the last floor.
    QuestDefinition last = journal_quest("Last");
    last.dungeon = "Main";
    last.repeatable = true;
    QuestBook other;
    other.add(last);
    QuestLog late;
    late.set_book(&other);
    late.set_dungeon_source([&](const std::string&) {
        tl::core::QuestDungeon dungeon;
        dungeon.found = true;
        dungeon.max_depth = 4;
        dungeon.depth_count = 5;
        return dungeon;
    });
    const QuestDefinition* gone = other.find("Last");
    TL_CHECK(late.valid_for_npc(*gone, "SEER"));
    TL_CHECK(late.quest_for_npc("SEER", {}, roll) == gone);
    TL_CHECK(late.offered(*gone)->exhausted);
    TL_CHECK(!late.valid_for_npc(*gone, "SEER"));
}

// `CQuestUnitData::getDungeonFloorActiveOn`.
TL_TEST(an_objectives_floor_is_relative_then_specific_then_the_quests) {
    QuestObjective o;
    TL_CHECK_EQ(tl::core::objective_floor(o, 4), 4);
    o.specific_floor = 7;
    TL_CHECK_EQ(tl::core::objective_floor(o, 4), 7);
    o.relative_floor = 3;
    TL_CHECK_EQ(tl::core::objective_floor(o, 4), 7);
    o.relative_floor = -4;
    TL_CHECK_EQ(tl::core::objective_floor(o, 4), 0);
    o.relative_floor = -5;
    TL_CHECK_EQ(tl::core::objective_floor(o, 4), 4);
}

// `CQuestUnitData::getDeepestFloor`: a negative sum is 1, an unauthored
// `SPECIFICFLOOR` 0, and a child can be deeper.
TL_TEST(an_objectives_deepest_floor_counts_its_children) {
    QuestObjective o;
    TL_CHECK_EQ(tl::core::deepest_floor(o, 4), 0);
    o.specific_floor = 2;
    TL_CHECK_EQ(tl::core::deepest_floor(o, 4), 2);
    o.relative_floor = -5;
    TL_CHECK_EQ(tl::core::deepest_floor(o, 4), 1);
    o.relative_floor = -4;
    TL_CHECK_EQ(tl::core::deepest_floor(o, 4), 0);
    QuestObjective child;
    child.relative_floor = 3;
    o.children.push_back(child);
    TL_CHECK_EQ(tl::core::deepest_floor(o, 4), 7);

    // `getDungeonMaxFloor`: the quest's floor, or a deeper unit.
    QuestObjective shallow;
    shallow.specific_floor = 1;
    TL_CHECK_EQ(tl::core::dungeon_max_floor({shallow}, {}, 5), 5);
    TL_CHECK_EQ(tl::core::dungeon_max_floor({shallow}, {o}, 4), 7);
}

// `CQuestUnitData::getIsRandom` and `getQuestHasRandomPieces`.
//
TL_TEST(a_count_range_or_a_spawn_class_makes_a_unit_random) {
    QuestObjective fixed;
    fixed.min_count = fixed.max_count = 2;
    TL_CHECK(!fixed.is_random());
    QuestObjective range = fixed;
    range.max_count = 3;
    TL_CHECK(range.is_random());
    QuestObjective rolled = fixed;
    rolled.spawn_class = "LEVEL_QUEST_ITEMS";
    TL_CHECK(rolled.is_random());
    QuestObjective parent = fixed;
    parent.children.push_back(range);
    TL_CHECK(parent.is_random());
    TL_CHECK(!tl::core::quest_has_random_pieces({fixed}, {fixed}));
    TL_CHECK(tl::core::quest_has_random_pieces({fixed}, {parent}));
}

// `calculateUnitsFromTag`, `reinitializeQuest`: the first
// `MINCREATE` (at least 1) to all `[DEFEAT]` groups.
TL_TEST(a_quest_takes_between_mincreate_and_all_of_its_defeat_groups) {
    AdmSection root;
    root.name = "QUEST";
    root.records.push_back(text_record("NAME", "Hunt"));
    AdmSection defeat;
    defeat.name = "DEFEAT";
    for (const char* monster : {"First", "Second", "Third"}) {
        AdmSection unit;
        unit.name = "UNIT";
        unit.records.push_back(text_record("MONSTER", monster));
        defeat.children.push_back(unit);
    }
    root.children.push_back(defeat);
    QuestDefinition parsed;
    TL_CHECK(parse_quest_definition(root, parsed));
    TL_CHECK_EQ(parsed.defeat_create, 1u);   // no `MINCREATE`: 1, not all three
    TL_CHECK_EQ(parsed.acquire_create, 0u);  // no `[ACQUIRE]`
    root.children[0].records.push_back(int_record("MINCREATE", 2));
    root.children[0].records.push_back(int_record("MAXCREATE", 0));
    TL_CHECK(parse_quest_definition(root, parsed));
    TL_CHECK_EQ(parsed.defeat_create, 2u);

    tl::core::RunicRandom g_rand(5);
    tl::core::RunicRandom mirror(5);
    const uint32_t taken = tl::core::take_defeat_groups(parsed, g_rand);
    TL_CHECK_EQ(taken, static_cast<uint32_t>(mirror.integer_between(2, 3)));
    TL_CHECK_EQ(g_rand.state(), mirror.state());

    QuestBook book;
    book.add(parsed);
    QuestLog log;
    log.set_book(&book);
    tl::core::game_random().set_seed(5);
    TL_CHECK(log.accept(*book.find("Hunt"), 1, 9u, reward_graphs()));
    const tl::core::QuestEntry* entry = log.find("Hunt");
    TL_CHECK_EQ(entry->defeat.size(), size_t{taken});
    TL_CHECK_EQ(entry->defeat[0].monster, std::string("First"));
    TL_CHECK_EQ(entry->defeat[1].monster, std::string("Second"));
}

// A `MINCREATE` past the groups reads element 0 beyond the end (`TArrayList`):
// the first group again. Kept.
TL_TEST(a_mincreate_past_the_groups_repeats_the_first) {
    QuestDefinition quest = journal_quest("Many");
    QuestObjective boss;
    boss.monster = "Boss";
    boss.min_count = boss.max_count = 1;
    quest.defeat.push_back(boss);
    quest.defeat_create = 3;
    QuestBook book;
    book.add(quest);
    QuestLog log;
    log.set_book(&book);
    TL_CHECK(log.accept(*book.find("Many"), 1, 9u, reward_graphs()));
    const tl::core::QuestEntry* entry = log.find("Many");
    TL_CHECK_EQ(entry->defeat.size(), size_t{1});
    TL_CHECK_EQ(entry->defeat_repeats, 2u);
    TL_CHECK_EQ(entry->defeat_group_count(), size_t{3});
    TL_CHECK_EQ(replace_quest_tags("[DEFEAT3]", *entry), std::string("Boss"));
}

// `initializeQuestWithNPC`: a quest pays at the rank of its last roll's
// deepest floor, read before the offer's roll.
TL_TEST(an_offered_quest_is_worth_the_rank_of_its_last_floor) {
    QuestDefinition quest = journal_quest("Deep");
    quest.dungeon = "Main";
    tl::core::DialogLine oracle = quest.intro[0];
    oracle.unit = "ORACLE";
    quest.intro.push_back(oracle);
    QuestObjective below;
    below.relative_floor = 2;
    quest.acquire.push_back(below);
    QuestBook book;
    book.add(quest);
    QuestLog log;
    log.set_book(&book);
    int deepest = 0;
    std::vector<int> ranked;
    log.set_dungeon_source([&](const std::string&) {
        tl::core::QuestDungeon dungeon;
        dungeon.found = true;
        dungeon.max_depth = deepest;
        dungeon.depth_count = 20;
        dungeon.rank_on_floor = [&](int floor) {
            ranked.push_back(floor);
            return floor * 10;
        };
        return dungeon;
    });
    const QuestDefinition* def = book.find("Deep");
    TL_CHECK_EQ(log.offer_level(*def, 99), 99);  // never offered: the caller's

    tl::core::RunicRandom roll(3);
    TL_CHECK(log.quest_for_npc("SEER", {}, roll) == def);
    // The first offer reads the roll it was given first (floor 1, the unit two
    // below it): 3. The offer's own roll then binds it to floor 1 again.
    TL_CHECK_EQ(ranked.size(), size_t{1});
    TL_CHECK_EQ(ranked[0], 3);
    TL_CHECK_EQ(log.offer_level(*def, 99), 30);
    TL_CHECK_EQ(log.offered(*def)->depth, 1);

    // Another giver once the player has been to floor 6: the level is still
    // the old roll's (floor 1 + 2), and the new roll binds it to floor 7.
    deepest = 6;
    TL_CHECK(log.quest_for_npc("ORACLE", {}, roll) == def);
    TL_CHECK_EQ(ranked.size(), size_t{2});
    TL_CHECK_EQ(ranked[1], 3);
    TL_CHECK_EQ(log.offered(*def)->depth, 7);
    // And back to the first: now floor 7 + 2.
    TL_CHECK(log.quest_for_npc("SEER", {}, roll) == def);
    TL_CHECK_EQ(ranked.size(), size_t{3});
    TL_CHECK_EQ(log.offer_level(*def, 99), 90);
}

// ---------------------------------------------------------------------------
// Completion, quest units and their credit
// ---------------------------------------------------------------------------

TL_TEST(completing_a_quest_the_player_never_took_still_gives_what_it_gives) {
    // `CQuestController::setQuestComplete` -> `CQuestManager::completeQuest` on a quest with no
    // log entry still runs its `[GIVE_QUESTS_ON_COMPLETE]`.
    QuestDefinition intro;
    intro.name = "IntroToGamePT1";
    intro.give_quests_on_complete.push_back("RANDOMMINERS");
    QuestDefinition miners;
    miners.name = "RandomMiners";

    QuestBook book;
    book.add(intro);
    book.add(miners);
    QuestLog log;
    log.set_book(&book);

    TL_CHECK(log.complete_quest(*book.find("IntroToGamePT1")));
    TL_CHECK(log.state().is_complete("IntroToGamePT1"));
    TL_CHECK(log.state().is_active("RandomMiners"));
    TL_CHECK(!log.complete_quest(*book.find("IntroToGamePT1")));
}

// `CQuestUnitData::populate`: Vasman's Ember is placed on `MAIN`
// depth 0 only, once, one unit per wanted, and only while the quest is active.
TL_TEST(a_quest_places_its_unit_on_its_own_floor_once) {
    QuestDefinition quest;
    TL_CHECK(parse_quest_definition(vasman_quest_one(), quest));
    TL_CHECK_EQ(quest.acquire.at(0).kind, 1);
    QuestBook book;
    book.add(quest);
    QuestLog log;
    log.set_book(&book);

    int placed = 0;
    auto count = [&](const tl::core::QuestEntry&, const QuestObjective& objective,
                     const tl::core::QuestUnitTag& tag) {
        ++placed;
        TL_CHECK_EQ(objective.item, std::string("GleamingEmber"));
        TL_CHECK_EQ(tag.quest, std::string("Vasman_Quest1"));
        TL_CHECK_EQ(tag.objective, objective.id);
    };
    log.populate_level("MAIN", 0, count);  // not taken yet
    TL_CHECK_EQ(placed, 0);

    TL_CHECK(log.accept(*book.find("Vasman_Quest1"), 2, 1u, reward_graphs()));
    log.populate_level("MAIN", 1, count);  // floor 2
    log.populate_level("TOWN", 0, count);
    TL_CHECK_EQ(placed, 0);
    log.populate_level("MAIN", 0, count);
    TL_CHECK_EQ(placed, 1);
    TL_CHECK(log.find("Vasman_Quest1")->acquire.at(0).populated);
    log.populate_level("MAIN", 0, count);  // `+0xb9` holds
    TL_CHECK_EQ(placed, 1);
}

// PORT: an objective whose unit could not be made is unmarked and made on the next load.
TL_TEST(an_unplaced_quest_unit_is_made_on_the_next_load) {
    QuestDefinition quest;
    TL_CHECK(parse_quest_definition(vasman_quest_one(), quest));
    QuestBook book;
    book.add(quest);
    QuestLog log;
    log.set_book(&book);
    TL_CHECK(log.accept(*book.find("Vasman_Quest1"), 2, 1u, reward_graphs()));

    int tried = 0;
    tl::core::QuestUnitTag last;
    auto fail = [&](const tl::core::QuestEntry&, const QuestObjective&,
                    const tl::core::QuestUnitTag& tag) {
        ++tried;
        last = tag;
    };
    log.populate_level("MAIN", 0, fail);
    TL_CHECK_EQ(tried, 1);
    TL_CHECK(log.unplace(last));
    TL_CHECK(!log.find("Vasman_Quest1")->acquire.at(0).populated);
    TL_CHECK(!log.unplace(last));  // nothing placed to take back
    log.populate_level("MAIN", 0, fail);
    TL_CHECK_EQ(tried, 2);
    TL_CHECK(log.find("Vasman_Quest1")->acquire.at(0).populated);
    TL_CHECK(!log.unplace(tl::core::QuestUnitTag{"NoSuchQuest", last.objective}));
}

// `unitPickedUp`: an objective that creates its unit counts only the one it tagged.
TL_TEST(a_created_objective_counts_only_its_own_tagged_unit) {
    QuestDefinition quest;
    TL_CHECK(parse_quest_definition(vasman_quest_one(), quest));
    QuestBook book;
    book.add(quest);
    QuestLog log;
    log.set_book(&book);
    TL_CHECK(log.accept(*book.find("Vasman_Quest1"), 2, 1u, reward_graphs()));

    // Before it is placed nothing counts, tagged or not.
    TL_CHECK(log.item_picked_up("GleamingEmber").empty());

    tl::core::QuestUnitTag placed;
    log.populate_level("MAIN", 0,
                       [&](const tl::core::QuestEntry&, const QuestObjective&,
                           const tl::core::QuestUnitTag& tag) { placed = tag; });
    TL_CHECK(log.item_picked_up("GleamingEmber").empty());
    TL_CHECK(log.item_picked_up("GleamingEmber", {"SomeOtherQuest", placed.objective}).empty());
    TL_CHECK(log.item_picked_up("GleamingEmber", {"Vasman_Quest1", placed.objective + 1}).empty());
    TL_CHECK(!log.find("Vasman_Quest1")->objectives_complete());

    const auto finished = log.item_picked_up("GleamingEmber", placed);
    TL_CHECK_EQ(finished.size(), 1u);
    TL_CHECK(log.find("Vasman_Quest1")->objectives_complete());
}

// `RELATIVEFLOOR` adds to the quest's floor, `SPECIFICFLOOR` replaces it.
TL_TEST(the_quest_floor_gate_follows_relative_then_specific_floor) {
    QuestObjective relative;
    relative.relative_floor = 2;
    TL_CHECK(tl::core::objective_on_level(relative, "MAIN", 3, "MAIN", 5));
    TL_CHECK(!tl::core::objective_on_level(relative, "MAIN", 3, "MAIN", 3));

    QuestObjective specific;
    specific.specific_floor = 4;
    TL_CHECK(tl::core::objective_on_level(specific, "MAIN", 0, "MAIN", 4));
    TL_CHECK(!tl::core::objective_on_level(specific, "MAIN", 0, "CRYPT", 4));

    QuestObjective anywhere;
    TL_CHECK(tl::core::objective_on_level(anywhere, "", 0, "CRYPT", 7));
    TL_CHECK(!anywhere.create);
    QuestObjective in_dungeon;
    in_dungeon.create = false;
    TL_CHECK(tl::core::objective_on_level(in_dungeon, "MAIN", 0, "MAIN", 6));
    TL_CHECK(!tl::core::objective_on_level(in_dungeon, "MAIN", 0, "CRYPT", 6));
}

// `CQuest::giveRewardForQuest` -> `cleanUp(true)`.
TL_TEST(completing_vasmans_quest_takes_the_ember_back) {
    QuestDefinition quest = journal_quest("Vasman_Quest1");
    quest.guid = -7385;
    QuestObjective ember;
    ember.item = "GleamingEmber";
    ember.min_count = 1;
    ember.max_count = 1;
    ember.kind = 1;  // `[ACQUIRE]`; REMOVEFROMINVENTORY and REMOVE_ONLY_QUESTITEMS default true
    quest.acquire.push_back(ember);
    QuestObjective kept = ember;
    kept.item = "Keepsake";
    kept.remove_from_inventory = false;
    quest.acquire.push_back(kept);
    QuestObjective any_copy = ember;
    any_copy.item = "Trinket";
    any_copy.remove_only_questitems = false;  // looks up quest 0xffffffff, finds nothing
    quest.acquire.push_back(any_copy);
    QuestObjective boss = ember;
    boss.item.clear();
    boss.monster = "Ratlin";
    quest.defeat.push_back(boss);

    QuestBook book;
    book.add(quest);
    QuestLog log;
    log.set_book(&book);
    TL_CHECK(log.accept(*book.find("Vasman_Quest1"), 1, 1u, reward_graphs()));
    TL_CHECK(log.take_item_removals().empty());
    TL_CHECK(log.complete("Vasman_Quest1"));

    const std::vector<tl::core::QuestItemRemoval> removals = log.take_item_removals();
    TL_CHECK_EQ(removals.size(), 3u);
    TL_CHECK_EQ(removals[0].quest_guid, int64_t{-7385});
    TL_CHECK(removals[0].unit == "GleamingEmber");
    TL_CHECK(removals[0].whole_stack);
    TL_CHECK_EQ(removals[0].isa, 0u);
    TL_CHECK(!removals[0].level_items);
    TL_CHECK(removals[1].unit.empty());
    TL_CHECK(!removals[1].whole_stack);
    TL_CHECK_EQ(removals[1].isa, 103u);
    TL_CHECK(!removals[1].level_items);
    // `CLevel::makeQuestUnitsUncool`: the quest's `QuestItem`s on the floor.
    TL_CHECK_EQ(removals[2].quest_guid, int64_t{-7385});
    TL_CHECK(removals[2].level_items);
    TL_CHECK_EQ(removals[2].isa, 103u);
    TL_CHECK(log.take_item_removals().empty());
}

namespace {

tl::core::UnitThemeBook marker_themes() {
    std::vector<tl::formats::AdmFile> files;
    for (const char* name : {"Quest Complete", "Quest Incomplete", "Quest Giving", "Quest Passive",
                             "Merchant", "Gambler", "WaitingTheme"}) {
        tl::formats::AdmFile file;
        file.ok = true;
        file.root.name = "THEME";
        AdmRecord rec;
        rec.key = "NAME";
        rec.type = AdmType::String;
        rec.text = name;
        file.root.records.push_back(rec);
        files.push_back(file);
    }
    tl::core::UnitThemeBook book;
    book.reload(files);
    return book;
}

bool holds(const tl::core::UnitThemeSet& themes, const tl::core::UnitThemeBook& book,
           const std::string& name) {
    return themes.has_unit_theme(name, &book);
}

AdmSection hierarchy_type(const std::string& name, uint32_t id,
                          const std::vector<uint32_t>& ancestors) {
    AdmSection section;
    section.name = name;
    AdmRecord key;
    key.key = "ID";
    key.integer = id;
    section.records.push_back(key);
    for (size_t i = 0; i < ancestors.size(); ++i) {
        AdmRecord child;
        child.key = i == 0 ? "child" : "child" + std::to_string(i);
        child.integer = ancestors[i];
        section.records.push_back(child);
    }
    return section;
}

}  // namespace

TL_TEST(the_quest_marker_follows_the_quest_from_offer_to_completion) {
    // `CQuestManager::calculateNPCIcon`: `QUEST GIVING`, `QUEST INCOMPLETE`,
    // `QUEST COMPLETE`, each replacing the last.
    QuestDefinition errand = journal_quest("Errand");
    tl::core::DialogLine done;
    done.unit = "SEER";
    done.texts = {"well done"};
    errand.complete.push_back(done);
    QuestBook book;
    book.add(errand);
    QuestLog log;
    log.set_book(&book);
    const tl::core::UnitThemeBook themes_book = marker_themes();
    tl::core::UnitThemeSet themes;
    tl::core::RunicRandom roll(3);

    TL_CHECK(log.calculate_npc_icon("SEER", {}, roll, false, true, themes, &themes_book));
    TL_CHECK(holds(themes, themes_book, "QUEST GIVING"));

    TL_CHECK(log.accept(*book.find("Errand"), 1, 1u, reward_graphs()));
    TL_CHECK(log.calculate_npc_icon("SEER", {}, roll, false, true, themes, &themes_book));
    TL_CHECK(!holds(themes, themes_book, "QUEST GIVING"));
    TL_CHECK(holds(themes, themes_book, "QUEST INCOMPLETE"));

    // Nothing asked for but the offer acknowledged: `isComplete`.
    log.find("Errand")->dialog_interacted = true;
    TL_CHECK(log.calculate_npc_icon("SEER", {}, roll, false, true, themes, &themes_book));
    TL_CHECK(!holds(themes, themes_book, "QUEST INCOMPLETE"));
    TL_CHECK(holds(themes, themes_book, "QUEST COMPLETE"));

    // A cinematic takes every marker off and says it has answered.
    TL_CHECK(log.calculate_npc_icon("SEER", {}, roll, true, true, themes, &themes_book));
    TL_CHECK(themes.own_themes().empty());
}

TL_TEST(the_quest_marker_honours_the_line_icon_flag_and_override) {
    QuestDefinition quiet = journal_quest("Quiet");
    quiet.intro[0].icon_above_head = false;
    QuestDefinition themed = journal_quest("Themed");
    themed.intro[0].unit = "BRINK";
    themed.intro[0].theme_override = "WAITINGTHEME";
    QuestBook book;
    book.add(quiet);
    book.add(themed);
    QuestLog log;
    log.set_book(&book);
    const tl::core::UnitThemeBook themes_book = marker_themes();
    tl::core::RunicRandom roll(3);

    tl::core::UnitThemeSet seer;
    TL_CHECK(log.calculate_npc_icon("SEER", {}, roll, false, true, seer, &themes_book));
    TL_CHECK(seer.own_themes().empty());

    tl::core::UnitThemeSet brink;
    TL_CHECK(log.calculate_npc_icon("BRINK", {}, roll, false, true, brink, &themes_book));
    TL_CHECK(holds(brink, themes_book, "WAITINGTHEME"));
    TL_CHECK(!holds(brink, themes_book, "QUEST GIVING"));
}

TL_TEST(a_passive_line_always_shows_the_passive_marker) {
    QuestDefinition quest;
    quest.name = "RandomMiners";
    quest.quest_controller_completes = true;
    tl::core::DialogLine line;
    line.unit = "MINERTOWN1";
    line.texts = {"this load of Ember may be the last"};
    quest.passive.push_back(line);
    QuestBook book;
    book.add(quest);
    QuestLog log;
    log.set_book(&book);
    TL_CHECK(log.accept(*book.find("RandomMiners"), 1, 1u, reward_graphs()));
    const tl::core::UnitThemeBook themes_book = marker_themes();
    tl::core::UnitThemeSet themes;
    tl::core::RunicRandom roll(3);

    TL_CHECK(log.calculate_npc_icon("MinerTown1", {}, roll, false, true, themes, &themes_book));
    TL_CHECK(holds(themes, themes_book, "QUEST PASSIVE"));
    TL_CHECK(!holds(themes, themes_book, "QUEST INCOMPLETE"));
}

TL_TEST(a_unit_with_no_quest_marker_gets_its_vendor_theme) {
    // `CLevel::updateNPCIcons`: `GAMBLER` is tested before
    // `MERCHANT`, so a gambler (who is also a merchant) shows the gambler's.
    AdmSection root;
    root.name = "HIERACHY";
    AdmSection all;
    all.name = "UNITTYPES";
    all.children.push_back(hierarchy_type("any", 0, {}));
    all.children.push_back(hierarchy_type("MERCHANT", 41, {0}));
    all.children.push_back(hierarchy_type("GAMBLER", 127, {41, 0}));
    root.children.push_back(all);
    tl::core::UnitTypeHierarchy types;
    types.parse(root);

    QuestBook book;
    QuestLog log;
    log.set_book(&book);
    const tl::core::UnitThemeBook themes_book = marker_themes();
    tl::core::RunicRandom roll(3);

    tl::core::UnitThemeSet gambler;
    tl::core::update_npc_icon(log, "Gambler", {}, 127, types, roll, false, true, false, gambler,
                              &themes_book);
    TL_CHECK(holds(gambler, themes_book, "GAMBLER"));
    TL_CHECK(!holds(gambler, themes_book, "MERCHANT"));

    tl::core::UnitThemeSet merchant;
    tl::core::update_npc_icon(log, "Vendor", {}, 41, types, roll, false, true, false, merchant,
                              &themes_book);
    TL_CHECK(holds(merchant, themes_book, "MERCHANT"));

    // Hidden, or on its way out: left alone.
    tl::core::UnitThemeSet hidden;
    tl::core::update_npc_icon(log, "Vendor", {}, 41, types, roll, false, false, false, hidden,
                              &themes_book);
    tl::core::update_npc_icon(log, "Vendor", {}, 41, types, roll, false, true, true, hidden,
                              &themes_book);
    TL_CHECK(hidden.own_themes().empty());
}

// `CRandomNames::load`, `generateName(bool)`: set, prefix, suffix,
// centre, title, on the game generator in that order.
TL_TEST(random_names_are_built_in_the_binarys_draw_order) {
    using tl::formats::AdmRecord;
    using tl::formats::AdmSection;
    const auto text = [](const char* key, const char* value) {
        AdmRecord r;
        r.key = key;
        r.type = tl::formats::AdmType::String;
        r.text = value;
        return r;
    };
    const auto number = [](const char* key, uint32_t value) {
        AdmRecord r;
        r.key = key;
        r.type = tl::formats::AdmType::Integer;
        r.integer = value;
        return r;
    };
    AdmSection root;
    root.name = "NAMES";
    AdmSection plain;
    plain.name = "NAME";
    plain.records = {number("WEIGHT", 10),  text("PREFIX", "Head"),    text("PREFIX", "Face"),
                     text("PREFIX", "Eye"), text("SUFFIX", "crusher"), text("SUFFIX", "biter")};
    AdmSection split;
    split.name = "NAME";
    split.records = {number("WEIGHT", 20),  number("CENTERCHANCE", 40), text("PREFIX", "Gar"),
                     text("PREFIX", "Zul"), text("SUFFIX", "dok"),      text("SUFFIX", "mesh")};
    AdmSection title;
    title.name = "TITLE";
    title.records = {number("CHANCE", 80), text("SUFFIX", "the Red"), text("SUFFIX", "the Black")};
    root.children = {plain, split, title};

    tl::core::RandomNames names;
    TL_CHECK(names.empty());
    names.load(root);
    TL_CHECK_EQ(names.sets().size(), 2u);
    TL_CHECK_EQ(names.sets()[0].prefixes.size(), 3u);
    TL_CHECK_EQ(names.sets()[0].center_chance, 0);
    TL_CHECK_EQ(names.sets()[1].center, std::string("'"));  // the default
    TL_CHECK_EQ(names.sets()[1].center_chance, 40);
    TL_CHECK_EQ(names.title_chance(), 80);
    TL_CHECK_EQ(names.titles().size(), 2u);

    for (uint32_t seed = 1; seed <= 60; ++seed) {
        tl::core::RunicRandom rng(seed);
        tl::core::RunicRandom mirror(seed);
        const std::string name = names.generate_name(true, rng);

        // `CRandomizer::getRandom`: one roll in [0, 1) against 10 and 20.
        const float roll = mirror.between(0.0f, 1.0f);
        const auto& set = names.sets()[roll < 10.0f / 30.0f ? 0 : 1];
        const int prefix = mirror.integer_between(0, static_cast<int>(set.prefixes.size()) - 1);
        const int suffix = mirror.integer_between(0, static_cast<int>(set.suffixes.size()) - 1);
        std::string expected = set.prefixes[static_cast<size_t>(prefix)];
        if (0 < set.center_chance && mirror.integer_between(0, 100) <= set.center_chance) {
            expected += set.center;
        }
        expected += set.suffixes[static_cast<size_t>(suffix)];
        if (mirror.integer_between(0, 100) <= 80) {
            expected += " " + names.titles()[static_cast<size_t>(mirror.integer_between(0, 1))];
        }
        TL_CHECK_EQ(name, expected);
        TL_CHECK_EQ(rng.state(), mirror.state());

        // Without a title nothing more is drawn after the name.
        tl::core::RunicRandom bare(seed);
        const std::string untitled = names.generate_name(false, bare);
        TL_CHECK(name.compare(0, untitled.size(), untitled) == 0);
    }
}

// `CQuestUnitData::reinitialize(true)`: each objective's count is the first draw
// off the quest's seed, and so is a champion's name (`getBaseUnitDataGroup`).
TL_TEST(each_objective_rolls_off_the_quests_seed) {
    QuestDefinition quest = journal_quest("Hunt");
    QuestObjective boss;
    boss.monster = "Varkolyn Mage";
    boss.min_count = 2;
    boss.max_count = 9;
    boss.make_champion = true;
    QuestObjective relic;
    relic.item = "Relic";
    relic.min_count = 2;
    relic.max_count = 9;
    relic.make_champion = true;  // an `ITEM` leaves before the name
    boss.children.push_back(relic);
    quest.defeat.push_back(boss);
    QuestObjective ember;
    ember.item = "Ember";
    ember.min_count = 2;
    ember.max_count = 9;
    quest.acquire.push_back(ember);

    tl::formats::AdmSection root;
    tl::formats::AdmSection set;
    set.name = "NAME";
    for (const char* prefix : {"Head", "Face", "Eye", "Bone"}) {
        tl::formats::AdmRecord r;
        r.key = "PREFIX";
        r.text = prefix;
        set.records.push_back(r);
    }
    for (const char* suffix : {"crusher", "biter", "ripper"}) {
        tl::formats::AdmRecord r;
        r.key = "SUFFIX";
        r.text = suffix;
        set.records.push_back(r);
    }
    root.children.push_back(set);
    tl::core::RandomNames names;
    names.load(root);

    QuestBook book;
    book.add(quest);
    QuestLog log;
    log.set_book(&book);
    log.set_random_names(&names);
    const int32_t seed = 424242;
    TL_CHECK(log.accept(*book.find("Hunt"), 1, static_cast<uint32_t>(seed), reward_graphs()));
    const tl::core::QuestEntry* entry = log.find("Hunt");

    tl::core::RunicRandom first(0u);
    first.set_seed(seed);
    const int count = first.integer_between(2, 9);
    TL_CHECK_EQ(entry->defeat[0].wanted, count);
    TL_CHECK_EQ(entry->defeat[0].children[0].wanted, count);
    TL_CHECK_EQ(entry->acquire[0].wanted, count);

    tl::core::RunicRandom named(0u);
    named.set_seed(seed);
    const std::string name = names.generate_name(true, named);
    TL_CHECK(!name.empty());
    TL_CHECK_EQ(entry->defeat[0].champion_name, name);
    TL_CHECK_EQ(entry->defeat[0].unit_name(), name);
    TL_CHECK(entry->defeat[0].children[0].champion_name.empty());
    TL_CHECK_EQ(replace_quest_tags("Slay [DEFEAT1]", *entry), "Slay " + name);
}

namespace {

/// A world whose chosen spawn classes, class table and roll are scripted, and
/// which records what `spawnChildrenUnits` asked for.
struct ScriptedQuestWorld {
    std::vector<std::string> chosen = std::vector<std::string>(11);
    std::vector<std::string> classes;
    std::string rolled;
    std::vector<int32_t> seed_depths;
    std::string rolled_class;
    int32_t rolled_level = 0, rolled_bonus = 0, rolled_rarity = 0;
    std::vector<float> radii;
    struct Spawned {
        std::string unit;
        tl::core::QuestUnitTag tag;
        tl::core::Vec3 at;
    };
    std::vector<Spawned> spawned;

    tl::core::QuestUnitWorld world() {
        tl::core::QuestUnitWorld w;
        w.choose_spawners = [this](const tl::core::QuestEntry&, int32_t seed_depth,
                                   std::vector<std::string>& out) {
            seed_depths.push_back(seed_depth);
            out = chosen;
            return true;
        };
        w.has_spawn_class = [this](const std::string& name) {
            for (const std::string& c : classes) {
                if (c == name) return true;
            }
            return false;
        };
        w.roll_spawn_class = [this](const std::string& name, int32_t level, int32_t bonus,
                                    int32_t rarity) {
            rolled_class = name;
            rolled_level = level;
            rolled_bonus = bonus;
            rolled_rarity = rarity;
            return rolled;
        };
        w.random_open_position = [this](const tl::core::Vec3& at, float radius) {
            radii.push_back(radius);
            return at;
        };
        w.spawn_unit = [this](const tl::core::QuestEntry&, const QuestObjective& o,
                              const tl::core::QuestUnitTag& tag, const tl::core::Vec3& at) {
            spawned.push_back({o.resolved, tag, at});
        };
        return w;
    }
};

/// `RANDOMQUEST01`'s shape: a champion that names nothing, carrying a stone.
QuestDefinition champion_with_stone(const std::string& name) {
    QuestDefinition quest = journal_quest(name);
    quest.dungeon = "MAIN";
    QuestObjective champion;
    champion.make_champion = true;
    champion.min_count = 1;
    champion.max_count = 1;
    QuestObjective stone;
    stone.item = "Rare Stone";
    stone.kind = 1;
    stone.create = false;
    stone.min_count = 1;
    stone.max_count = 1;
    champion.children.push_back(stone);
    quest.defeat.push_back(champion);
    return quest;
}

}  // namespace

TL_TEST(a_champion_that_names_nothing_is_rolled_from_the_floors_champion_class) {
    // A champion with no key is `LEVEL_QUEST_CHAMPIONS`; slot 10 saying
    // `MONSTERSETCHAMPION` hands over to slot 1 (`getSpawnClassUnitDataLookingFor`).
    ScriptedQuestWorld scripted;
    scripted.chosen[1] = "CRYPT_CHAMPIONS";
    scripted.chosen[10] = "MONSTERSETCHAMPION";
    scripted.classes = {"CRYPT_CHAMPIONS"};
    scripted.rolled = "Skeleton Warrior";

    QuestBook book;
    book.add(champion_with_stone("Hunt"));
    QuestLog log;
    log.set_book(&book);
    log.set_unit_world(scripted.world());
    TL_CHECK(log.accept(*book.find("Hunt"), 7, 100u, reward_graphs(), /*depth=*/3));

    const tl::core::QuestEntry* entry = log.find("Hunt");
    TL_CHECK(entry != nullptr);
    const QuestObjective& champion = entry->defeat[0];
    TL_CHECK_EQ(champion.spawn_class, std::string("LEVEL_QUEST_CHAMPIONS"));
    TL_CHECK_EQ(champion.predefined_class, 2);
    TL_CHECK_EQ(champion.resolved, std::string("Skeleton Warrior"));
    TL_CHECK_EQ(scripted.rolled_class, std::string("CRYPT_CHAMPIONS"));
    // The seed is the floor plus the quest's seed; the level the quest's own,
    // moved by the objective's floor against the quest's (both 3 here).
    TL_CHECK_EQ(scripted.seed_depths.back(), 103);
    TL_CHECK_EQ(scripted.rolled_level, 7);
    TL_CHECK_EQ(scripted.rolled_bonus, 0);
    TL_CHECK_EQ(scripted.rolled_rarity, -1);
    // The stone names an item: no roll.
    TL_CHECK_EQ(champion.children[0].resolved, std::string("Rare Stone"));
}

TL_TEST(the_quest_class_slots_fall_back_as_the_original_reads_them) {
    // a slot of `NONE` or nothing is the default class; the
    // items slot (9) has no default and rolls with level bonus -1, rarity 1.
    ScriptedQuestWorld scripted;
    scripted.chosen[8] = "NONE";
    scripted.classes = {"MONSTERSET", "QUEST_LOOT"};
    scripted.rolled = "Thing";
    QuestDefinition quest = journal_quest("Q");
    QuestObjective monsters;
    monsters.min_count = 1;
    monsters.max_count = 1;
    quest.defeat.push_back(monsters);
    QuestObjective loot;
    loot.kind = 1;
    loot.min_count = 1;
    loot.max_count = 1;
    quest.acquire.push_back(loot);
    QuestBook book;
    book.add(quest);
    QuestLog log;
    log.set_book(&book);
    log.set_unit_world(scripted.world());

    // The items slot empty: nothing rolled, nothing resolved.
    TL_CHECK(log.accept(*book.find("Q"), 1, 5u, reward_graphs()));
    TL_CHECK(log.find("Q")->acquire[0].resolved.empty());
    TL_CHECK_EQ(log.find("Q")->acquire[0].spawn_class, std::string("LEVEL_QUEST_ITEMS"));
    TL_CHECK_EQ(log.find("Q")->defeat[0].resolved, std::string("Thing"));
    TL_CHECK_EQ(scripted.rolled_class, std::string("MONSTERSET"));

    // With the slot filled, the item roll's own arguments.
    scripted.chosen[9] = "QUEST_LOOT";
    log.reset(tl::core::QuestState{});
    scripted.rolled_class.clear();
    QuestBook items_only;
    QuestDefinition fetch = journal_quest("F");
    fetch.acquire.push_back(loot);
    items_only.add(fetch);
    log.set_book(&items_only);
    TL_CHECK(log.accept(*items_only.find("F"), 1, 5u, reward_graphs()));
    TL_CHECK_EQ(log.find("F")->acquire[0].resolved, std::string("Thing"));
    TL_CHECK_EQ(scripted.rolled_class, std::string("QUEST_LOOT"));
    TL_CHECK_EQ(scripted.rolled_bonus, -1);
    TL_CHECK_EQ(scripted.rolled_rarity, 1);
}

TL_TEST(a_defeated_champion_drops_its_stone_and_the_stone_finishes_the_quest) {
    // `unitDefeated` runs `spawnChildrenUnits`; the pickup reaches the
    // child through the finished champion (`unitPickedUp`).
    ScriptedQuestWorld scripted;
    scripted.chosen[1] = "CHAMPS";
    scripted.chosen[10] = "MONSTERSETCHAMPION";
    scripted.classes = {"CHAMPS"};
    scripted.rolled = "Skeleton Warrior";
    QuestBook book;
    book.add(champion_with_stone("Hunt"));
    QuestLog log;
    log.set_book(&book);
    log.set_unit_world(scripted.world());
    TL_CHECK(log.accept(*book.find("Hunt"), 7, 100u, reward_graphs(), 0));
    std::vector<tl::core::QuestUnitTag> placed;
    log.populate_level("MAIN", 0,
                       [&](const tl::core::QuestEntry&, const QuestObjective&,
                           const tl::core::QuestUnitTag& tag) { placed.push_back(tag); });
    TL_CHECK_EQ(placed.size(), 1u);
    TL_CHECK(!log.find("Hunt")->defeat[0].children[0].populated);

    // An ordinary skeleton of the same kind is not the quest's.
    TL_CHECK(log.unit_defeated("Skeleton Warrior").empty());
    TL_CHECK(scripted.spawned.empty());

    tl::core::QuestEventUnit body;
    body.name = "Skeleton Warrior";
    body.tag = placed[0];
    body.position = {10.0f, 2.0f, -4.0f};
    TL_CHECK(log.unit_defeated(body).empty());  // the stone is still wanted
    TL_CHECK_EQ(scripted.spawned.size(), 1u);
    TL_CHECK_EQ(scripted.spawned[0].unit, std::string("Rare Stone"));
    TL_CHECK_EQ(scripted.spawned[0].tag.quest, std::string("Hunt"));
    const int32_t stone_id = log.find("Hunt")->defeat[0].children[0].id;
    TL_CHECK_EQ(scripted.spawned[0].tag.objective, stone_id);
    TL_CHECK(scripted.spawned[0].at.y == 3.5f);
    TL_CHECK(scripted.radii.size() == 1u && scripted.radii[0] == 1.0f);
    TL_CHECK(log.find("Hunt")->defeat[0].children[0].populated);

    // A stone some other quest tagged does nothing; this one finishes it.
    TL_CHECK(log.item_picked_up("Rare Stone", {"Other", stone_id}).empty());
    const auto finished = log.item_picked_up("Rare Stone", scripted.spawned[0].tag);
    TL_CHECK_EQ(finished.size(), 1u);
    TL_CHECK(log.find("Hunt")->objectives_complete());
}

TL_TEST(an_interaction_counts_only_for_the_player) {
    // `CQuest::questEventUpdate` case 2 wants the manager's player;
    // `unitInteracted` needs no placement and no tag.
    QuestDefinition quest = journal_quest("Pull");
    QuestObjective lever;
    lever.item = "Lever";
    lever.min_count = 1;
    lever.max_count = 1;
    quest.defeat.push_back(lever);
    QuestBook book;
    book.add(quest);
    QuestLog log;
    log.set_book(&book);
    TL_CHECK(log.accept(*book.find("Pull"), 1, 5u, reward_graphs()));
    tl::core::QuestEventUnit prop;
    prop.name = "Lever";
    TL_CHECK(log.unit_interacted(prop, /*by_player=*/false).empty());
    TL_CHECK(!log.find("Pull")->objectives_complete());
    TL_CHECK_EQ(log.unit_interacted(prop, /*by_player=*/true).size(), 1u);
    TL_CHECK(log.find("Pull")->objectives_complete());
}

TL_TEST(a_dropped_quest_item_undoes_its_objective_and_the_banner_comes_once) {
    // `unitDropped`: the objective's own tagged item takes one off
    // and clears done; `+0x209` keeps the second completion quiet.
    QuestDefinition quest = journal_quest("Fetch");
    quest.dungeon = "MAIN";
    QuestObjective ember;
    ember.item = "GleamingEmber";
    ember.kind = 1;
    ember.min_count = 1;
    ember.max_count = 1;
    quest.acquire.push_back(ember);
    QuestBook book;
    book.add(quest);
    QuestLog log;
    log.set_book(&book);
    TL_CHECK(log.accept(*book.find("Fetch"), 1, 5u, reward_graphs()));
    tl::core::QuestUnitTag placed;
    log.populate_level("MAIN", 0,
                       [&](const tl::core::QuestEntry&, const QuestObjective&,
                           const tl::core::QuestUnitTag& tag) { placed = tag; });
    TL_CHECK_EQ(log.item_picked_up("GleamingEmber", placed).size(), 1u);

    tl::core::QuestEventUnit gone;
    gone.name = "GleamingEmber";
    gone.tag = placed;
    TL_CHECK(log.item_dropped(gone).empty());
    TL_CHECK(!log.find("Fetch")->objectives_complete());
    TL_CHECK_EQ(log.find("Fetch")->acquire[0].have, 0);

    TL_CHECK(log.item_picked_up("GleamingEmber", placed).empty());  // no second banner
    TL_CHECK(log.find("Fetch")->objectives_complete());
}

TL_TEST(a_pickup_an_uncreated_objective_counts_stamps_the_item) {
    // `unitPickedUp`: `CREATE:False` counts any item of its unit; a drop does not
    // take it off again.
    QuestDefinition quest = journal_quest("Glyphs");
    QuestObjective glyph;
    glyph.item = "glyph1";
    glyph.kind = 1;
    glyph.create = false;
    glyph.min_count = 1;
    glyph.max_count = 1;
    quest.acquire.push_back(glyph);
    QuestBook book;
    book.add(quest);
    QuestLog log;
    log.set_book(&book);
    TL_CHECK(log.accept(*book.find("Glyphs"), 1, 5u, reward_graphs()));
    log.populate_level("", 0, place_nothing);

    tl::core::QuestEventUnit item;
    item.name = "glyph1";
    TL_CHECK_EQ(log.item_picked_up(item).size(), 1u);
    TL_CHECK_EQ(item.tag.quest, std::string("Glyphs"));
    TL_CHECK_EQ(item.tag.objective, -1);
    TL_CHECK(log.item_dropped(item).empty());
    TL_CHECK(log.find("Glyphs")->objectives_complete());
}

TL_TEST(a_units_children_are_the_units_of_its_own_acquire_group) {
    // `calculateUnitsFromTag`: a `[UNIT]`'s children are its `[ACQUIRE]` group's.
    AdmSection quest;
    quest.name = "QUEST";
    quest.records.push_back(text_record("NAME", "GarDefeatAcquire"));
    quest.records.push_back(text_record("DUNGEON", "RandomDungeon"));
    AdmSection carried;
    carried.name = "UNIT";
    carried.records.push_back(text_record("SPAWNCLASS", "RANDOM_QUEST_REWARD"));
    carried.records.push_back(bool_record("CREATE", false));
    AdmSection carried_group;
    carried_group.name = "ACQUIRE";
    carried_group.children.push_back(carried);
    AdmSection champion;
    champion.name = "UNIT";
    champion.records.push_back(bool_record("MAKECHAMPION", true));
    champion.children.push_back(carried_group);
    AdmSection defeat;
    defeat.name = "DEFEAT";
    defeat.children.push_back(champion);
    quest.children.push_back(defeat);

    QuestDefinition parsed;
    TL_CHECK(parse_quest_definition(quest, parsed));
    TL_CHECK_EQ(parsed.defeat.size(), 1u);
    TL_CHECK_EQ(parsed.defeat[0].kind, 0);
    TL_CHECK_EQ(parsed.defeat[0].children.size(), 1u);
    TL_CHECK_EQ(parsed.defeat[0].children[0].kind, 1);
    TL_CHECK_EQ(parsed.defeat[0].children[0].spawn_class, std::string("RANDOM_QUEST_REWARD"));
    TL_CHECK(!parsed.defeat[0].children[0].create);
    TL_CHECK(parsed.acquire.empty());
}

TL_TEST(every_objective_is_offered_an_event) {
    // `CQuest::questEventUpdate`, `calculateUnitsFromTag`: two objectives
    // wanting the same item both count one pickup.
    QuestDefinition quest = journal_quest("Twice");
    QuestObjective first;
    first.item = "glyph1";
    first.kind = 1;
    first.create = false;
    first.min_count = 1;
    first.max_count = 1;
    quest.acquire.push_back(first);
    quest.acquire.push_back(first);
    QuestBook book;
    book.add(quest);
    QuestLog log;
    log.set_book(&book);
    TL_CHECK(log.accept(*book.find("Twice"), 1, 5u, reward_graphs()));
    log.populate_level("", 0, place_nothing);

    TL_CHECK_EQ(log.item_picked_up("glyph1").size(), 1u);
    TL_CHECK_EQ(log.find("Twice")->acquire[0].have, 1);
    TL_CHECK_EQ(log.find("Twice")->acquire[1].have, 1);
}

TL_TEST(no_quest_unit_is_rolled_while_the_player_has_no_level) {
    // `getSpawnClassUnitDataLookingFor`: no player, no roll (a save's read); the
    // count is still drawn.
    ScriptedQuestWorld scripted;
    scripted.chosen[1] = "CHAMPS";
    scripted.chosen[10] = "MONSTERSETCHAMPION";
    scripted.classes = {"CHAMPS"};
    scripted.rolled = "Skeleton Warrior";
    tl::core::QuestUnitWorld world = scripted.world();
    world.player_on_level = [] { return false; };
    QuestBook book;
    book.add(champion_with_stone("Hunt"));
    QuestLog log;
    log.set_book(&book);
    log.set_unit_world(world);
    TL_CHECK(log.accept(*book.find("Hunt"), 7, 100u, reward_graphs(), 0));
    TL_CHECK(log.find("Hunt")->defeat[0].resolved.empty());
    TL_CHECK(scripted.seed_depths.empty());
    TL_CHECK(scripted.rolled_class.empty());
    TL_CHECK_EQ(log.find("Hunt")->defeat[0].wanted, 1);
    // The class it would roll is settled all the same (`+0x78`, `+0xb0`).
    TL_CHECK_EQ(log.find("Hunt")->defeat[0].spawn_class, std::string("LEVEL_QUEST_CHAMPIONS"));
    // The stone names its item and needs no roll.
    TL_CHECK_EQ(log.find("Hunt")->defeat[0].children[0].resolved, std::string("Rare Stone"));
}

TL_TEST(a_counted_kill_and_a_found_item_put_up_their_progress_lines) {
    // "<name> - <have>/<wanted> Defeated" and "... Found"
    // under the quest's name.
    QuestDefinition quest = journal_quest("Cull");
    quest.display_name = "The Cull";
    QuestObjective rats;
    rats.monster = "Ratlin";
    rats.create = false;
    rats.min_count = 2;
    rats.max_count = 2;
    quest.defeat.push_back(rats);
    QuestObjective glyph;
    glyph.item = "glyph1";
    glyph.kind = 1;
    glyph.create = false;
    glyph.min_count = 1;
    glyph.max_count = 1;
    quest.acquire.push_back(glyph);
    QuestBook book;
    book.add(quest);
    QuestLog log;
    log.set_book(&book);
    TL_CHECK(log.accept(*book.find("Cull"), 1, 5u, reward_graphs()));
    log.populate_level("", 0, place_nothing);

    tl::core::QuestEventUnit rat;
    rat.name = "Ratlin";
    rat.shown_name = "Ratlin Scout";
    rat.isa = [](uint32_t type) { return type == tl::core::unit_type::kCharacter; };
    TL_CHECK(log.unit_defeated(rat).empty());
    std::vector<tl::core::QuestProgressText> lines = log.take_progress_texts();
    TL_CHECK_EQ(lines.size(), 1u);
    TL_CHECK_EQ(lines[0].big, std::string("The Cull"));
    TL_CHECK_EQ(lines[0].small, std::string("Ratlin Scout - 1/2 Defeated"));
    TL_CHECK(log.take_progress_texts().empty());

    // A unit that is no character (a breakable of the same name) counts, quietly.
    tl::core::QuestEventUnit crate;
    crate.name = "Ratlin";
    crate.shown_name = "Ratlin";
    TL_CHECK(log.unit_defeated(crate).empty());
    TL_CHECK_EQ(log.find("Cull")->defeat[0].have, 2);
    TL_CHECK(log.take_progress_texts().empty());

    tl::core::QuestEventUnit item;
    item.name = "glyph1";
    item.shown_name = "Glyph of Turning";
    item.isa = [](uint32_t type) { return type == tl::core::unit_type::kItem; };
    TL_CHECK_EQ(log.item_picked_up(item).size(), 1u);
    lines = log.take_progress_texts();
    TL_CHECK_EQ(lines.size(), 1u);
    TL_CHECK_EQ(lines[0].small, std::string("Glyph of Turning - 1/1 Found"));
}

TL_TEST(a_saved_unit_whose_count_was_reached_loads_done) {
    // DIVERGENCE (2026-10-11): a saved objective whose count was reached reads as done
    // (`CQuestUnitData::save`, `load`).
    TL_CHECK(tl::core::quest_unit_done_from_save(true, 1, 1));
    TL_CHECK(tl::core::quest_unit_done_from_save(false, 1, 1));  // the original's record
    TL_CHECK(tl::core::quest_unit_done_from_save(false, 3, 3));
    TL_CHECK(!tl::core::quest_unit_done_from_save(false, 3, 2));
    TL_CHECK(!tl::core::quest_unit_done_from_save(false, 1, 0));
    // A count of 0 is a unit nothing has counted, whatever it wanted.
    TL_CHECK(!tl::core::quest_unit_done_from_save(false, 0, 0));
}

// `CQuestDialogItem::parseDialogItemGroup`: `COUNT` at least 1,
// `NAME` upper-cased, `GOLD` its own case; `giveOrRemoveItems` runs once a line.
TL_TEST(dialog_items_parse_and_hand_over_once) {
    AdmSection entry;
    entry.name = "COMPLETE";
    entry.records.push_back(text_record("DIALOG", "here"));
    AdmSection give;
    give.name = "GIVEITEM";
    give.records.push_back(text_record("NAME", "Gold"));
    give.records.push_back(int_record("COUNT", 250));
    entry.children.push_back(give);
    AdmSection take;
    take.name = "TAKEITEM";
    take.records.push_back(text_record("NAME", "Gleaming Ember"));
    take.records.push_back(int_record("COUNT", 0));
    entry.children.push_back(take);

    const tl::core::DialogLine line = tl::core::parse_dialog_line(entry);
    TL_CHECK_EQ(line.give_items.size(), size_t{1});
    TL_CHECK(line.give_items[0].gold);
    TL_CHECK_EQ(line.give_items[0].count, 250);
    TL_CHECK_EQ(line.take_items.size(), size_t{1});
    TL_CHECK_EQ(line.take_items[0].name, std::string("GLEAMING EMBER"));
    TL_CHECK_EQ(line.take_items[0].count, 1);
    TL_CHECK(!line.take_items[0].gold);

    QuestDefinition quest = journal_quest("Errand");
    quest.complete.push_back(line);
    QuestBook book;
    book.add(quest);
    QuestLog log;
    log.set_book(&book);
    const QuestDefinition* def = book.find("Errand");
    TL_CHECK(!log.items_handed(*def, def->complete[0]));
    log.set_items_handed(*def, def->complete[0]);
    TL_CHECK(log.items_handed(*def, def->complete[0]));
    QuestLog later;
    later.set_book(&book);
    later.restore(log.carry());
    TL_CHECK(later.items_handed(*def, def->complete[0]));
}

// `CQuestManager::resetQuestManager(false)`.
TL_TEST(the_quest_managers_reset_rolls_every_quest_in_its_maps_order) {
    // The map compares the names as written: "Zeta" before "alpha".
    QuestBook book;
    QuestDefinition lower = journal_quest("alpha");
    lower.dungeon = "Main";
    book.add(lower);
    book.add(journal_quest("Zeta"));
    const std::vector<const QuestDefinition*> order = book.in_manager_order();
    TL_CHECK_EQ(order.size(), 2u);
    TL_CHECK_EQ(order[0]->name, std::string("Zeta"));
    TL_CHECK_EQ(order[1]->name, std::string("alpha"));

    QuestLog log;
    log.set_book(&book);
    log.set_dungeon_source([&](const std::string& name) {
        tl::core::QuestDungeon dungeon;
        dungeon.found = name == "MAIN";
        dungeon.max_depth = 2;
        dungeon.depth_count = 9;
        return dungeon;
    });
    log.state().complete("Zeta");
    log.reset_quest_manager(true);
    // Both rolled; with no player the floor is the first try's, 1.
    TL_CHECK(log.offered(*book.find("Zeta")) != nullptr);
    TL_CHECK_EQ(log.offered(*book.find("alpha"))->depth, 1);
    TL_CHECK(!log.state().is_complete("Zeta"));
    // With one, its deepest floor and one more.
    log.reset_quest_manager(false);
    TL_CHECK_EQ(log.offered(*book.find("alpha"))->depth, 3);
}

// `CQuestUnitData::populate`: `SPAWNFROMUNITTYPE` tags a
// unit of that type already there, drawn on `g_Rand`.
TL_TEST(a_spawn_from_unittype_objective_tags_a_unit_already_there) {
    QuestDefinition quest;
    TL_CHECK(parse_quest_definition(vasman_quest_one(), quest));
    TL_CHECK(!quest.acquire.empty());
    quest.acquire[0].spawn_from_unittype = "MONSTER";
    QuestBook book;
    book.add(quest);

    QuestLog log;
    log.set_book(&book);
    std::vector<int64_t> tagged;
    tl::core::QuestUnitTag stamped;
    tl::core::QuestUnitWorld world;
    world.type_id = [](const std::string& name) {
        return name == "MONSTER" ? tl::core::unit_type::kMonster : tl::core::unit_type::kNone;
    };
    uint32_t asked = 0;
    world.units_of_type = [&](uint32_t type) {
        asked = type;
        std::vector<tl::core::QuestUnitWorld::LevelUnit> units(4);
        for (size_t i = 0; i < units.size(); ++i) units[i].handle = static_cast<int64_t>(i + 1);
        // Only the third already belongs to a quest.
        units[2].tag = tl::core::QuestUnitTag{"Another", 5};
        return units;
    };
    world.tag_unit = [&](const tl::core::QuestUnitWorld::LevelUnit& unit,
                         const tl::core::QuestUnitTag& tag) {
        tagged.push_back(unit.handle);
        stamped = tag;
    };
    log.set_unit_world(world);
    TL_CHECK(log.accept(*book.find("Vasman_Quest1"), 2, 1u, reward_graphs()));

    tl::core::game_random().set_seed(1234);
    tl::core::RunicRandom expected = tl::core::game_random();
    // The draws `populate` makes: one per unit taken out, the last moved into
    // its place, until the one with a GUID comes out.
    std::vector<int64_t> left{1, 2, 3, 4};
    for (;;) {
        const int pick = expected.integer_between(0, static_cast<int>(left.size()) - 1);
        const int64_t unit = left[static_cast<size_t>(pick)];
        left[static_cast<size_t>(pick)] = left.back();
        left.pop_back();
        if (unit == 3) break;
    }

    int created = 0;
    log.populate_level("MAIN", 0,
                       [&](const tl::core::QuestEntry&, const QuestObjective&,
                           const tl::core::QuestUnitTag&) { ++created; });
    TL_CHECK_EQ(created, 0);
    TL_CHECK_EQ(asked, tl::core::unit_type::kMonster);
    TL_CHECK_EQ(tagged.size(), size_t{1});
    TL_CHECK_EQ(tagged.front(), int64_t{3});
    TL_CHECK_EQ(stamped.quest, std::string("Vasman_Quest1"));
    TL_CHECK_EQ(tl::core::game_random().integer_between(0, 1 << 30),
                expected.integer_between(0, 1 << 30));
}

// The same, with `UNITTYPE`: the objective's id (`+0x44`) is set, and
// `populate` stops there - nothing made, nothing tagged, nothing drawn.
TL_TEST(a_unittype_objective_places_nothing) {
    QuestDefinition quest;
    TL_CHECK(parse_quest_definition(vasman_quest_one(), quest));
    quest.acquire[0].unit_type_id = tl::core::unit_type::kMonster;
    QuestBook book;
    book.add(quest);
    QuestLog log;
    log.set_book(&book);
    TL_CHECK(log.accept(*book.find("Vasman_Quest1"), 2, 1u, reward_graphs()));
    tl::core::QuestEntry* entry = const_cast<tl::core::QuestEntry*>(log.find("Vasman_Quest1"));
    entry->acquire[0].unit_type_id = tl::core::unit_type::kMonster;
    int created = 0;
    log.populate_level("MAIN", 0,
                       [&](const tl::core::QuestEntry&, const QuestObjective&,
                           const tl::core::QuestUnitTag&) { ++created; });
    TL_CHECK_EQ(created, 0);
    TL_CHECK(entry->acquire[0].populated);
}
