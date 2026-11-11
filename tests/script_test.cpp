// Quest state and the level-script machine, on scenes assembled by hand.
#include "core/script.h"
#include "core/damage_shape.h"
#include "core/quest.h"
#include "core/randomizer.h"
#include "core/skill_cast.h"
#include "core/stats.h"
#include "core/unit_types.h"
#include "formats/adm.h"
#include "test_seeds.h"
#include "test_support.h"

#include <cmath>
#include <cstring>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace {

using tl::core::LevelScript;
using tl::core::QuestState;
using tl::core::QuestStatus;
using tl::core::ScriptAction;
using tl::core::ScriptEvent;
using tl::formats::CmpField;
using tl::formats::CmpNode;
using tl::formats::CmpScene;

/// A string field: UTF-16LE with a NUL, padded out to whole dwords, which is
/// how a string is stored inside a node's field stream.
CmpField text_field(const std::string& text) {
    const size_t units = text.size() + 1;
    const size_t dwords = (units * 2 + 3) / 4;
    std::vector<uint8_t> payload(dwords * 4, 0);
    for (size_t i = 0; i < text.size(); ++i) {
        payload[2 * i] = static_cast<uint8_t>(text[i]);
    }
    CmpField f;
    f.words.resize(dwords);
    for (size_t i = 0; i < dwords; ++i) {
        f.words[i] = static_cast<uint32_t>(payload[4 * i]) |
                     (static_cast<uint32_t>(payload[4 * i + 1]) << 8) |
                     (static_cast<uint32_t>(payload[4 * i + 2]) << 16) |
                     (static_cast<uint32_t>(payload[4 * i + 3]) << 24);
    }
    return f;
}

CmpField uint_field(uint32_t value) {
    CmpField f;
    f.words.push_back(value);
    return f;
}

CmpField float_field(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, 4);
    return uint_field(bits);
}

CmpField vec3_field(float x, float y, float z) {
    CmpField f;
    for (float v : {x, y, z}) {
        uint32_t bits = 0;
        std::memcpy(&bits, &v, 4);
        f.words.push_back(bits);
    }
    return f;
}

/// Pad a node's field list out to `count` so a later index lands where the
/// schema says. The compiled form always writes every field of a descriptor.
void pad_to(CmpNode& node, size_t count) {
    while (node.fields.size() < count) node.fields.push_back(uint_field(0));
}

CmpNode& add_node(CmpScene& scene, uint32_t type, int64_t id, const std::string& name) {
    CmpNode node;
    node.type = type;
    node.id = id;
    node.name = name;
    scene.nodes.push_back(std::move(node));
    return scene.nodes.back();
}

/// A group naming one quest in one gate field.
CmpNode quest_group(size_t gate_index, const std::string& quest) {
    CmpNode node;
    node.type = tl::formats::cmp_type::kGroup;
    pad_to(node, gate_index);
    node.fields.push_back(text_field(quest));
    return node;
}

}  // namespace

// ------------------------------------------------------------------ quests

TL_TEST(quest_state_starts_empty_and_is_case_insensitive) {
    QuestState quests;
    TL_CHECK(quests.status("INTROTOGAMEPT1") == QuestStatus::Unknown);

    quests.complete("IntroToGamePt1");
    TL_CHECK(quests.is_complete("INTROTOGAMEPT1"));
    TL_CHECK(!quests.is_active("INTROTOGAMEPT1"));

    // An unauthored gate compiles to the empty string, and it must never become
    // a quest - it would match every other unauthored gate.
    quests.complete("");
    TL_CHECK_EQ(quests.size(), size_t{1});
}

TL_TEST(quest_specification_defaults_to_complete_and_rejects_junk) {
    QuestState quests;
    TL_CHECK(quests.apply_specification("DEFEATFIRSTBOSS"));
    TL_CHECK(quests.is_complete("DEFEATFIRSTBOSS"));

    TL_CHECK(quests.apply_specification("CRYPTPT1:active"));
    TL_CHECK(quests.is_active("CRYPTPT1"));

    TL_CHECK(quests.apply_specification("LAVAPT1:COMPLETE"));
    TL_CHECK(quests.is_complete("LAVAPT1"));

    TL_CHECK(!quests.apply_specification("SOMETHING:halfway"));
    TL_CHECK(quests.status("SOMETHING") == QuestStatus::Unknown);
}

// With no quest state the three positive gates are shut and the two negative ones open.
TL_TEST(quest_gates_resolve_against_an_empty_state_the_way_a_new_game_does) {
    namespace group = tl::formats::cmp_field::group;
    QuestState empty;

    TL_CHECK(!tl::core::group_is_open(quest_group(group::kComplete, "DEFEATFIRSTBOSS"), empty));
    TL_CHECK(!tl::core::group_is_open(quest_group(group::kActive, "CRYPTPT1"), empty));
    TL_CHECK(!tl::core::group_is_open(quest_group(group::kActiveOrComplete, "CAVEPT1"), empty));

    TL_CHECK(tl::core::group_is_open(quest_group(group::kNotComplete, "INTROTOGAMEPT1"), empty));
    TL_CHECK(
        tl::core::group_is_open(quest_group(group::kNotActiveOrComplete, "ENTEREDRUINS"), empty));

    // A group naming nothing is always open, and so is anything that is not a
    // group at all.
    CmpNode plain;
    plain.type = tl::formats::cmp_type::kGroup;
    TL_CHECK(tl::core::group_is_open(plain, empty));
    CmpNode monster;
    monster.type = tl::formats::cmp_type::kMonster;
    TL_CHECK(tl::core::group_is_open(monster, empty));
}

// Which quest a gate is waiting on, a different question from whether it is open.
TL_TEST(a_group_says_which_quest_its_gate_names) {
    namespace group = tl::formats::cmp_field::group;

    const CmpNode npcs = quest_group(group::kComplete, "INTROTOGAMEPT1");
    TL_CHECK(tl::core::group_gates_on(npcs, "INTROTOGAMEPT1"));
    // Matched without case, like every other quest name here.
    TL_CHECK(tl::core::group_gates_on(npcs, "introtogamept1"));
    TL_CHECK(!tl::core::group_gates_on(npcs, "INTROTOGAMEPT2"));

    // Any of the five fields counts, including the two negative ones - a group
    // held *open* by a quest is also waiting on it.
    TL_CHECK(tl::core::group_gates_on(quest_group(group::kNotComplete, "INTROTOGAMEPT1"),
                                      "INTROTOGAMEPT1"));
    TL_CHECK(tl::core::group_gates_on(quest_group(group::kNotActiveOrComplete, "ENTEREDRUINS"),
                                      "ENTEREDRUINS"));

    // A group naming nothing, an empty name, and a node that is not a group.
    CmpNode plain;
    plain.type = tl::formats::cmp_type::kGroup;
    TL_CHECK(!tl::core::group_gates_on(plain, "INTROTOGAMEPT1"));
    TL_CHECK(!tl::core::group_gates_on(npcs, ""));
    CmpNode monster;
    monster.type = tl::formats::cmp_type::kMonster;
    TL_CHECK(!tl::core::group_gates_on(monster, "INTROTOGAMEPT1"));
}

TL_TEST(quest_gates_open_and_shut_as_the_story_moves) {
    namespace group = tl::formats::cmp_field::group;
    QuestState quests;
    quests.complete("INTROTOGAMEPT1");

    // Completing the first quest opens one group and shuts the other.
    TL_CHECK(tl::core::group_is_open(quest_group(group::kComplete, "INTROTOGAMEPT1"), quests));
    TL_CHECK(!tl::core::group_is_open(quest_group(group::kNotComplete, "INTROTOGAMEPT1"), quests));

    // A complete quest is not an active one.
    TL_CHECK(!tl::core::group_is_open(quest_group(group::kActive, "INTROTOGAMEPT1"), quests));
    TL_CHECK(
        tl::core::group_is_open(quest_group(group::kActiveOrComplete, "INTROTOGAMEPT1"), quests));
}

// Every authored gate has to pass. The town's `Ruins` group names two.
TL_TEST(a_group_with_two_gates_needs_both) {
    namespace group = tl::formats::cmp_field::group;
    CmpNode node;
    node.type = tl::formats::cmp_type::kGroup;
    pad_to(node, group::kActiveOrComplete);
    node.fields.push_back(text_field("CRYPTSYLENTERS"));  // ACTIVE OR COMPLETE
    node.fields.push_back(text_field(""));                // COMPLETE
    node.fields.push_back(text_field("ENTEREDCAVE"));     // NOT COMPLETE

    QuestState quests;
    TL_CHECK(!tl::core::group_is_open(node, quests));  // first gate shut

    quests.activate("CRYPTSYLENTERS");
    TL_CHECK(tl::core::group_is_open(node, quests));  // both pass

    quests.complete("ENTEREDCAVE");
    TL_CHECK(!tl::core::group_is_open(node, quests));  // second gate now shut
}

// ------------------------------------------------------------ the machine

namespace {

/// A sphere trigger wired to a quest controller, which is the shape the town's
/// `TellBrinkToTalk` has: walk into a radius, a quest moves on.
CmpScene trigger_scene(float radius) {
    CmpScene scene;

    CmpNode& trigger =
        add_node(scene, tl::formats::cmp_type::kPlayerSphereTrigger, 1, "TellBrinkToTalk");
    trigger.fields.push_back(vec3_field(10.0f, 0.0f, 0.0f));  // 0 POSITION
    trigger.fields.push_back(uint_field(1));                  // 1 ENABLED
    trigger.fields.push_back(float_field(radius));            // 2 RADIUS

    CmpNode& quest = add_node(scene, tl::formats::cmp_type::kQuestController, 2, "SeerAsksForHelp");
    pad_to(quest, tl::formats::cmp_field::quest_controller::kQuest);
    quest.fields.push_back(text_field("INTROTOGAMEPT1"));

    CmpNode& group = add_node(scene, tl::formats::cmp_type::kLogicGroup, 3, "CutsceneLogic");
    group.fields.push_back(uint_field(0));

    // `Force Accept`: an `Interact` naming no unit in the level does nothing
    // (`CQuestController::interactWithUnit`).
    tl::formats::CmpLogicGroup wiring;
    wiring.node_id = 3;
    wiring.objects.push_back({0, 1, 0.0f, 0.0f, {{1, "Triggered First Time", "Force Accept"}}});
    wiring.objects.push_back({1, 2, 0.0f, 0.0f, {}});
    scene.logic_groups.push_back(std::move(wiring));
    return scene;
}

}  // namespace

// A Warper's `Activate Warper` (CWarperDescriptor::InputLogicEvent input 0x25) asks for its
// dungeon, level delta and depth.
TL_TEST(a_warper_activated_by_a_trigger_asks_for_its_dungeon) {
    namespace f = tl::formats::cmp_field::warper;
    CmpScene scene;

    CmpNode& trigger = add_node(scene, tl::formats::cmp_type::kPlayerSphereTrigger, 1, "Entrance");
    trigger.fields.push_back(vec3_field(10.0f, 0.0f, 0.0f));  // 0 POSITION
    trigger.fields.push_back(uint_field(1));                  // 1 ENABLED
    trigger.fields.push_back(float_field(3.0f));              // 2 RADIUS

    CmpNode& warper = add_node(scene, tl::formats::cmp_type::kWarper, 2, "WarperMain");
    pad_to(warper, f::kEnabled);
    warper.fields.push_back(uint_field(1));       // 5 ENABLED
    warper.fields.push_back(uint_field(0));       // 6 LEVEL DELTA
    warper.fields.push_back(uint_field(2));       // 7 LEVEL ABSOLUTE
    warper.fields.push_back(text_field("Main"));  // 8 DUNGEON NAME
    warper.fields.push_back(text_field(""));      // 9 WARP NAME
    warper.fields.push_back(uint_field(0));       // 10 WAYPOINT

    CmpNode& group = add_node(scene, tl::formats::cmp_type::kLogicGroup, 3, "Logic");
    group.fields.push_back(uint_field(0));
    tl::formats::CmpLogicGroup wiring;
    wiring.node_id = 3;
    wiring.objects.push_back({0, 1, 0.0f, 0.0f, {{1, "Triggered", "Activate Warper"}}});
    wiring.objects.push_back({1, 2, 0.0f, 0.0f, {}});
    scene.logic_groups.push_back(std::move(wiring));

    LevelScript script;
    script.build(scene, {});
    QuestState quests;
    std::vector<ScriptEvent> events;
    script.update(1.0f / 60.0f, {0.0f, 0.0f, 0.0f}, quests, events);
    TL_CHECK(events.empty());

    script.update(1.0f / 60.0f, {10.0f, 0.0f, 1.0f}, quests, events);
    const ScriptEvent* warp = nullptr;
    for (const ScriptEvent& e : events) {
        if (e.action == ScriptAction::Warp) warp = &e;
    }
    TL_CHECK(warp != nullptr);
    if (warp != nullptr) {
        TL_CHECK_EQ(warp->text, std::string("Main"));
        TL_CHECK_EQ(warp->level_delta, 0);
        TL_CHECK_EQ(warp->level_absolute, 2);
        TL_CHECK(!warp->waypoint);
    }
}

TL_TEST(a_sphere_trigger_fires_when_the_player_walks_into_it) {
    CmpScene scene = trigger_scene(3.0f);
    LevelScript script;
    script.build(scene, {});
    TL_CHECK_EQ(script.trigger_count(), size_t{1});
    TL_CHECK_EQ(script.wire_count(), size_t{1});

    QuestState quests;
    std::vector<ScriptEvent> events;

    // Well outside: nothing happens, however long it runs.
    script.update(1.0f / 60.0f, {0.0f, 0.0f, 0.0f}, quests, events);
    TL_CHECK(events.empty());
    TL_CHECK(quests.status("INTROTOGAMEPT1") == QuestStatus::Unknown);

    // Inside the radius.
    script.update(1.0f / 60.0f, {10.0f, 0.0f, 1.0f}, quests, events);
    TL_CHECK(!events.empty());
    TL_CHECK(quests.is_active("INTROTOGAMEPT1"));
}

// `Triggered First Time` is the output most of the archive's triggers wire, and
// it means what it says: walking back in fires `Triggered` again and this never.
TL_TEST(triggered_first_time_fires_once_however_often_the_player_returns) {
    CmpScene scene = trigger_scene(3.0f);
    LevelScript script;
    script.build(scene, {});

    QuestState quests;
    std::vector<ScriptEvent> events;
    const tl::core::Vec3 inside{10.0f, 0.0f, 0.0f};
    const tl::core::Vec3 outside{0.0f, 0.0f, 0.0f};

    script.update(0.016f, inside, quests, events);
    const size_t after_first = events.size();
    TL_CHECK(after_first > 0);

    // Staying inside does not re-fire; a trigger is an edge, not a state.
    script.update(0.016f, inside, quests, events);
    TL_CHECK_EQ(events.size(), after_first);

    // Out and back in again: `Triggered` fires, `Triggered First Time` does not,
    // and nothing here is wired to `Triggered`.
    script.update(0.016f, outside, quests, events);
    script.update(0.016f, inside, quests, events);
    TL_CHECK_EQ(events.size(), after_first);
}

TL_TEST(a_disabled_trigger_does_not_fire) {
    CmpScene scene = trigger_scene(3.0f);
    // ENABLED false, which 43 of the archive's sphere triggers author.
    scene.nodes[0].fields[1] = uint_field(0);

    LevelScript script;
    script.build(scene, {});
    QuestState quests;
    std::vector<ScriptEvent> events;
    script.update(0.016f, {10.0f, 0.0f, 0.0f}, quests, events);
    TL_CHECK(events.empty());
    TL_CHECK(quests.status("INTROTOGAMEPT1") == QuestStatus::Unknown);
}

// A box trigger's DIMENSIONS is the box's *full* extent, so a 4x4x4 box reaches
// 2 units from its centre and not 4.
TL_TEST(a_box_trigger_uses_dimensions_as_a_full_extent) {
    CmpScene scene;
    CmpNode& trigger =
        add_node(scene, tl::formats::cmp_type::kPlayerBoxTrigger, 1, "WaterTrigger_1");
    trigger.fields.push_back(vec3_field(0.0f, 0.0f, 0.0f));
    trigger.fields.push_back(uint_field(1));
    trigger.fields.push_back(vec3_field(4.0f, 4.0f, 4.0f));

    CmpNode& quest = add_node(scene, tl::formats::cmp_type::kQuestController, 2, "Q");
    pad_to(quest, tl::formats::cmp_field::quest_controller::kQuest);
    quest.fields.push_back(text_field("SOAKED"));

    CmpNode& group = add_node(scene, tl::formats::cmp_type::kLogicGroup, 3, "Logic");
    group.fields.push_back(uint_field(0));
    tl::formats::CmpLogicGroup wiring;
    wiring.node_id = 3;
    wiring.objects.push_back({0, 1, 0.0f, 0.0f, {{1, "Triggered First Time", "Force Accept"}}});
    wiring.objects.push_back({1, 2, 0.0f, 0.0f, {}});
    scene.logic_groups.push_back(std::move(wiring));

    LevelScript script;
    script.build(scene, {});
    QuestState quests;
    std::vector<ScriptEvent> events;

    script.update(0.016f, {2.5f, 0.0f, 0.0f}, quests, events);  // outside the half-extent
    TL_CHECK(quests.status("SOAKED") == QuestStatus::Unknown);

    script.update(0.016f, {1.5f, 0.0f, 0.0f}, quests, events);  // inside
    TL_CHECK(quests.is_active("SOAKED"));
}

// The town's cutscene hangs off a counter: a trigger kills four monsters, each
// death adds one, and the camera move waits for all four.
TL_TEST(a_counter_fires_only_when_it_reaches_its_authored_value) {
    CmpScene scene;

    CmpNode& trigger = add_node(scene, tl::formats::cmp_type::kPlayerSphereTrigger, 4, "Sphere");
    trigger.fields.push_back(vec3_field(0.0f, 0.0f, 0.0f));
    trigger.fields.push_back(uint_field(1));
    trigger.fields.push_back(float_field(3.0f));

    CmpNode& counter = add_node(scene, tl::formats::cmp_type::kCounter, 1, "Counter3");
    counter.fields.push_back(uint_field(1));  // 0 ENABLED
    counter.fields.push_back(uint_field(3));  // 1 EQUALS VALUE
    counter.fields.push_back(uint_field(0));  // 2 STARTING VALUE
    counter.fields.push_back(text_field(""));

    CmpNode& quest = add_node(scene, tl::formats::cmp_type::kQuestController, 2, "Done");
    pad_to(quest, tl::formats::cmp_field::quest_controller::kQuest);
    quest.fields.push_back(text_field("COUNTED"));

    CmpNode& group = add_node(scene, tl::formats::cmp_type::kLogicGroup, 3, "Logic");
    group.fields.push_back(uint_field(0));

    tl::formats::CmpLogicGroup wiring;
    wiring.node_id = 3;
    // `Triggered` rather than `Triggered First Time`: walking back in fires it
    // again, which is what drives the counter here.
    wiring.objects.push_back({0, 4, 0.0f, 0.0f, {{1, "Triggered", "Add"}}});
    wiring.objects.push_back({1, 1, 0.0f, 0.0f, {{2, "Activated", "Force Complete"}}});
    wiring.objects.push_back({2, 2, 0.0f, 0.0f, {}});
    scene.logic_groups.push_back(std::move(wiring));

    LevelScript script;
    script.build(scene, {});
    QuestState quests;
    std::vector<ScriptEvent> events;

    const tl::core::Vec3 inside{0.0f, 0.0f, 0.0f};
    const tl::core::Vec3 outside{100.0f, 0.0f, 0.0f};

    script.update(0.016f, inside, quests, events);
    TL_CHECK(quests.status("COUNTED") == QuestStatus::Unknown);
    script.update(0.016f, outside, quests, events);
    script.update(0.016f, inside, quests, events);
    TL_CHECK(quests.status("COUNTED") == QuestStatus::Unknown);
    script.update(0.016f, outside, quests, events);
    script.update(0.016f, inside, quests, events);
    TL_CHECK(quests.is_complete("COUNTED"));
}

// A counter's `LOGIC` is read by name (`gCOUNTER_TYPE_NAMES`); an unknown name is
// `Activate only once`.
TL_TEST(counter_logic_is_read_by_name) {
    const std::pair<const char*, int> cases[] = {
        {"Activate and reset", 2}, {"Activate only once", 1}, {"activate and reset", 1}};
    for (const auto& c : cases) {
        CmpScene scene;
        CmpNode& trigger =
            add_node(scene, tl::formats::cmp_type::kPlayerSphereTrigger, 4, "Sphere");
        trigger.fields.push_back(vec3_field(0.0f, 0.0f, 0.0f));
        trigger.fields.push_back(uint_field(1));
        trigger.fields.push_back(float_field(3.0f));
        CmpNode& counter = add_node(scene, tl::formats::cmp_type::kCounter, 1, "Counter");
        counter.fields.push_back(uint_field(1));        // 0 ENABLED
        counter.fields.push_back(uint_field(2));        // 1 EQUALS VALUE
        counter.fields.push_back(uint_field(0));        // 2 STARTING VALUE
        counter.fields.push_back(text_field(c.first));  // 3 LOGIC
        add_node(scene, tl::formats::cmp_type::kMonster, 101, "Target");
        CmpNode& group = add_node(scene, tl::formats::cmp_type::kLogicGroup, 3, "Logic");
        group.fields.push_back(uint_field(0));
        tl::formats::CmpLogicGroup wiring;
        wiring.node_id = 3;
        wiring.objects.push_back({0, 4, 0.0f, 0.0f, {{1, "Triggered", "Add"}}});
        wiring.objects.push_back({1, 1, 0.0f, 0.0f, {{2, "Activated", "Show"}}});
        wiring.objects.push_back({2, 101, 0.0f, 0.0f, {}});
        scene.logic_groups.push_back(std::move(wiring));

        LevelScript script;
        script.build(scene, {});
        QuestState quests;
        std::vector<ScriptEvent> events;
        const tl::core::Vec3 inside{0.0f, 0.0f, 0.0f};
        const tl::core::Vec3 outside{100.0f, 0.0f, 0.0f};
        for (int i = 0; i < 4; ++i) {
            script.update(0.016f, inside, quests, events);
            script.update(0.016f, outside, quests, events);
        }
        int shown = 0;
        for (const ScriptEvent& e : events) {
            if (e.node_id == 101) ++shown;
        }
        TL_CHECK_EQ(shown, c.second);
    }
}

// `Level Activated` is what opens a level, and it happens once. A second call
// fires nothing - the level has already been activated.
TL_TEST(level_activated_fires_once) {
    CmpScene scene;
    CmpNode& group = add_node(scene, tl::formats::cmp_type::kLogicGroup, 1, "Logic");
    group.fields.push_back(uint_field(0));

    CmpNode& quest = add_node(scene, tl::formats::cmp_type::kQuestController, 2, "Q");
    pad_to(quest, tl::formats::cmp_field::quest_controller::kQuest);
    quest.fields.push_back(text_field("OPENED"));

    tl::formats::CmpLogicGroup wiring;
    wiring.node_id = 1;
    wiring.objects.push_back({0, 1, 0.0f, 0.0f, {{1, "Level Activated", "Force Complete"}}});
    wiring.objects.push_back({1, 2, 0.0f, 0.0f, {}});
    scene.logic_groups.push_back(std::move(wiring));

    LevelScript script;
    script.build(scene, {});
    QuestState quests;
    std::vector<ScriptEvent> events;

    script.level_activated(quests, events);
    const size_t after_first = events.size();
    TL_CHECK(after_first > 0);
    TL_CHECK(quests.is_complete("OPENED"));

    script.level_activated(quests, events);
    TL_CHECK_EQ(events.size(), after_first);
}

// A timer counts only while it is enabled, and the town authors ENABLED false on
// the two that gate its cutscene - they wait to be switched on.
TL_TEST(a_timer_runs_only_once_it_is_enabled) {
    CmpScene scene;

    CmpNode& timer = add_node(scene, tl::formats::cmp_type::kTimer, 1, "PauseBeforeCameraMove");
    timer.fields.push_back(uint_field(0));      // 0 ENABLED - off, as authored
    timer.fields.push_back(float_field(0.5f));  // 1 TIME
    pad_to(timer, 5);

    CmpNode& quest = add_node(scene, tl::formats::cmp_type::kQuestController, 2, "Fired");
    pad_to(quest, tl::formats::cmp_field::quest_controller::kQuest);
    quest.fields.push_back(text_field("TIMEDOUT"));

    CmpNode& group = add_node(scene, tl::formats::cmp_type::kLogicGroup, 3, "Logic");
    group.fields.push_back(uint_field(0));

    tl::formats::CmpLogicGroup wiring;
    wiring.node_id = 3;
    wiring.objects.push_back({0, 3, 0.0f, 0.0f, {{1, "Level Activated", "Enable"}}});
    wiring.objects.push_back({1, 1, 0.0f, 0.0f, {{2, "Activated", "Force Complete"}}});
    wiring.objects.push_back({2, 2, 0.0f, 0.0f, {}});
    scene.logic_groups.push_back(std::move(wiring));

    LevelScript script;
    script.build(scene, {});
    QuestState quests;
    std::vector<ScriptEvent> events;

    // Half a second of running before it is switched on changes nothing.
    for (int i = 0; i < 60; ++i) script.update(1.0f / 60.0f, {}, quests, events);
    TL_CHECK(quests.status("TIMEDOUT") == QuestStatus::Unknown);

    script.level_activated(quests, events);
    for (int i = 0; i < 20; ++i) script.update(1.0f / 60.0f, {}, quests, events);
    TL_CHECK(quests.status("TIMEDOUT") == QuestStatus::Unknown);  // 0.33 s, not yet
    for (int i = 0; i < 20; ++i) script.update(1.0f / 60.0f, {}, quests, events);
    TL_CHECK(quests.is_complete("TIMEDOUT"));  // 0.67 s, elapsed
}

// A timeline drives a target's position over its DURATION and fires `Stopped`
// when it runs out - which is what the town wires its whole cutscene cleanup to.
TL_TEST(a_timeline_moves_its_target_and_reports_when_it_stops) {
    CmpScene scene;

    CmpNode& timeline = add_node(scene, tl::formats::cmp_type::kTimeline, 1, "Timeline2");
    timeline.fields.push_back(float_field(10.0f));  // 0 DURATION
    pad_to(timeline, 4);
    timeline.fields.push_back(uint_field(1));  // 4 START ON LOAD
    pad_to(timeline, 8);

    add_node(scene, tl::formats::cmp_type::kMonster, 2, "Roach");

    CmpNode& quest = add_node(scene, tl::formats::cmp_type::kQuestController, 3, "IntroComplete");
    pad_to(quest, tl::formats::cmp_field::quest_controller::kQuest);
    quest.fields.push_back(text_field("SHOWINTRO"));

    CmpNode& group = add_node(scene, tl::formats::cmp_type::kLogicGroup, 4, "Logic Group1");
    group.fields.push_back(uint_field(0));

    tl::formats::CmpTimeline keys;
    keys.node_id = 1;
    tl::formats::CmpTimelineTrack track;
    track.tag = 0;
    track.is_event = false;
    auto key = [](float t, float x, float z) {
        tl::formats::CmpTimelineKey k;
        k.time = t;
        for (float v : {x, 0.0f, z}) {
            uint32_t bits = 0;
            std::memcpy(&bits, &v, 4);
            k.words.push_back(bits);
        }
        return k;
    };
    track.keys.push_back(key(0.0f, 0.0f, 0.0f));
    track.keys.push_back(key(1.0f, 100.0f, 200.0f));
    keys.targets.push_back({2, {track}});
    scene.timelines.push_back(std::move(keys));

    tl::formats::CmpLogicGroup wiring;
    wiring.node_id = 4;
    wiring.objects.push_back({0, 1, 0.0f, 0.0f, {{1, "Stopped", "Force Complete"}}});
    wiring.objects.push_back({1, 3, 0.0f, 0.0f, {}});
    scene.logic_groups.push_back(std::move(wiring));

    LevelScript script;
    script.build(scene, {});
    TL_CHECK_EQ(script.timeline_count(), size_t{1});
    TL_CHECK(script.busy());  // START ON LOAD

    QuestState quests;
    std::vector<ScriptEvent> events;

    // Half way: the target should be half way along.
    for (int i = 0; i < 300; ++i) script.update(1.0f / 60.0f, {}, quests, events);
    bool saw_move = false;
    for (const ScriptEvent& e : events) {
        if (e.action != ScriptAction::MoveNode) continue;
        saw_move = true;
    }
    TL_CHECK(saw_move);
    TL_CHECK(events.back().position.x > 45.0f && events.back().position.x < 55.0f);
    TL_CHECK(quests.status("SHOWINTRO") == QuestStatus::Unknown);

    // And past the end it stops, which is what the cleanup hangs off.
    for (int i = 0; i < 330; ++i) script.update(1.0f / 60.0f, {}, quests, events);
    TL_CHECK(!script.busy());
    TL_CHECK(quests.is_complete("SHOWINTRO"));
}

// A timeline whose position track is authored as Spline moves its target
// according to Ogre::SimpleSpline Catmull-Rom interpolation.
TL_TEST(a_timeline_moves_its_target_via_spline_interpolation) {
    CmpScene scene;

    CmpNode& timeline = add_node(scene, tl::formats::cmp_type::kTimeline, 1, "TimelineSpline");
    timeline.fields.push_back(float_field(10.0f));  // 0 DURATION
    pad_to(timeline, 4);
    timeline.fields.push_back(uint_field(1));  // 4 START ON LOAD
    pad_to(timeline, 8);

    add_node(scene, tl::formats::cmp_type::kMonster, 2, "Roach");

    tl::formats::CmpTimeline keys;
    keys.node_id = 1;
    tl::formats::CmpTimelineTrack track;
    track.tag = 0;
    track.is_event = false;
    auto key = [](float t, float x, float z) {
        tl::formats::CmpTimelineKey k;
        k.time = t;
        k.interpolation = static_cast<uint32_t>(tl::core::TimelineInterpolation::Spline);
        for (float v : {x, 0.0f, z}) {
            uint32_t bits = 0;
            std::memcpy(&bits, &v, sizeof(float));
            k.words.push_back(bits);
        }
        return k;
    };
    // 4 control points with uneven key times.
    track.keys.push_back(key(0.0f, 0.0f, 0.0f));
    track.keys.push_back(key(0.2f, 10.0f, 0.0f));
    track.keys.push_back(key(0.4f, 40.0f, 0.0f));
    track.keys.push_back(key(1.0f, 60.0f, 0.0f));
    keys.targets.push_back({2, {track}});
    scene.timelines.push_back(std::move(keys));

    LevelScript script;
    script.build(scene, {});
    TL_CHECK_EQ(script.timeline_count(), size_t{1});
    TL_CHECK(script.busy());  // START ON LOAD

    QuestState quests;
    std::vector<ScriptEvent> events;

    // Advance exactly to 5.0 seconds (percent = 0.5):
    // First step brings tl_time from -0.01 (CTimeline's, not a percent) to 0.0f:
    script.update(0.01f, {}, quests, events);
    events.clear();
    // Next step advances 5.0s to 50% of the 10.0s timeline:
    script.update(5.0f, {}, quests, events);

    // Hand-computed Catmull-Rom spline value at s=0.5 between P1 and P2 is 24.375.
    bool saw_move = false;
    float last_x = 0.0f;
    for (const ScriptEvent& e : events) {
        if (e.action != ScriptAction::MoveNode || e.node_id != 2) continue;
        saw_move = true;
        last_x = e.position.x;
    }
    TL_CHECK(saw_move);
    TL_CHECK(std::abs(last_x - 24.375f) < 0.01f);
}

// A wire into a subtree the quest gates held back is dropped: the node was never
// placed, so driving it would act on nothing.
TL_TEST(wires_into_a_gated_subtree_are_dropped) {
    CmpScene scene = trigger_scene(3.0f);
    LevelScript script;
    script.build(scene, {2});  // the quest controller is gated away

    TL_CHECK_EQ(script.wire_count(), size_t{0});
    TL_CHECK_EQ(script.trigger_count(), size_t{1});

    QuestState quests;
    std::vector<ScriptEvent> events;
    script.update(0.016f, {10.0f, 0.0f, 0.0f}, quests, events);
    TL_CHECK(quests.status("INTROTOGAMEPT1") == QuestStatus::Unknown);
}

// The camera eases along a spline through (0,0), (0.8,0.9), (1,1) whose tangents overshoot,
// so a 1.5 s pan's `Camera Stopped` fires after about 1.03 s.
TL_TEST(a_camera_pan_stops_when_its_spline_passes_one) {
    CmpScene scene;
    namespace cam = tl::formats::cmp_field::camera_controller;

    CmpNode& camera = add_node(scene, tl::formats::cmp_type::kCameraController, 1, "Camera");
    camera.fields.push_back(vec3_field(0.0f, 0.0f, 0.0f));  // 0 POSITION
    camera.fields.push_back(float_field(1.5f));             // 1 PAN TIME
    camera.fields.push_back(float_field(0.9f));             // 2 EASE IN DISTANCE PCT
    camera.fields.push_back(float_field(0.8f));             // 3 EASE IN PCT
    camera.fields.push_back(float_field(0.0f));             // 4 TIME
    camera.fields.push_back(uint_field(0));                 // 5 RESETS AT END
    pad_to(camera, cam::kUnit);
    camera.fields.push_back(text_field("Roach"));

    CmpNode& roach = add_node(scene, tl::formats::cmp_type::kMonster, 2, "Roach");
    pad_to(roach, tl::formats::cmp_field::monster::kMonster);
    roach.fields.push_back(text_field("Roach"));

    CmpNode& quest = add_node(scene, tl::formats::cmp_type::kQuestController, 3, "Stopped");
    pad_to(quest, tl::formats::cmp_field::quest_controller::kQuest);
    quest.fields.push_back(text_field("PANNED"));

    CmpNode& group = add_node(scene, tl::formats::cmp_type::kLogicGroup, 4, "Logic");
    group.fields.push_back(uint_field(0));

    tl::formats::CmpLogicGroup wiring;
    wiring.node_id = 4;
    wiring.objects.push_back({0, 4, 0.0f, 0.0f, {{1, "Level Activated", "Start Camera"}}});
    wiring.objects.push_back({1, 1, 0.0f, 0.0f, {{2, "Camera Stopped", "Force Complete"}}});
    wiring.objects.push_back({2, 3, 0.0f, 0.0f, {}});
    scene.logic_groups.push_back(std::move(wiring));

    LevelScript script;
    script.build(scene, {});
    QuestState quests;
    std::vector<ScriptEvent> events;
    script.level_activated(quests, events);

    for (int i = 0; i < 100; ++i) script.update(0.01f, {}, quests, events);  // 1.00 s
    TL_CHECK(!quests.is_complete("PANNED"));
    for (int i = 0; i < 5; ++i) script.update(0.01f, {}, quests, events);  // 1.05 s
    TL_CHECK(quests.is_complete("PANNED"));
}

TL_TEST(a_cyclic_graph_stops_rather_than_recursing_away) {
    CmpScene scene;
    add_node(scene, tl::formats::cmp_type::kLogicGroup, 1, "A").fields.push_back(uint_field(0));
    add_node(scene, tl::formats::cmp_type::kLogicGroup, 2, "B").fields.push_back(uint_field(0));

    tl::formats::CmpLogicGroup wiring;
    wiring.node_id = 1;
    wiring.objects.push_back({0, 1, 0.0f, 0.0f, {{1, "Start", "Start"}}});
    wiring.objects.push_back({1, 2, 0.0f, 0.0f, {{0, "Start", "Start"}}});
    scene.logic_groups.push_back(std::move(wiring));

    LevelScript script;
    script.build(scene, {});
    QuestState quests;
    std::vector<ScriptEvent> events;
    script.level_activated(quests, events);  // returns, rather than hanging
    TL_CHECK(true);
}

// What a level asked for and did not get is reported, not dropped. A level that
// half-runs looks exactly like one that is running.
TL_TEST(an_unimplemented_wire_is_reported) {
    CmpScene scene;

    CmpNode& group = add_node(scene, tl::formats::cmp_type::kLogicGroup, 1, "Logic");
    group.fields.push_back(uint_field(0));
    add_node(scene, tl::formats::cmp_type::kUnitSpawner, 2, "Spiders");

    tl::formats::CmpLogicGroup wiring;
    wiring.node_id = 1;
    // `Play` is not one of `CUnitSpawnerDescriptor`'s inputs; `Spawn Units` is,
    // and reaches the spawner the machine holds for the node.
    wiring.objects.push_back(
        {0,
         1,
         0.0f,
         0.0f,
         {{1, "Level Activated", "Play"}, {1, "Level Activated", "Spawn Units"}}});
    wiring.objects.push_back({1, 2, 0.0f, 0.0f, {}});
    scene.logic_groups.push_back(std::move(wiring));

    LevelScript script;
    script.build(scene, {});
    QuestState quests;
    std::vector<ScriptEvent> events;
    script.level_activated(quests, events);

    const std::vector<std::string> gaps = script.unimplemented();
    TL_CHECK_EQ(gaps.size(), size_t{1});
    TL_CHECK_EQ(gaps.front(), std::string("input 'Play' on Unit Spawner"));
    size_t inputs = 0;
    for (const ScriptEvent& e : events) {
        if (e.action != ScriptAction::SpawnerInput) continue;
        ++inputs;
        TL_CHECK(e.spawner == script.unit_spawner(2));
        TL_CHECK_EQ(e.input, uint32_t{tl::core::logic::kSpawnUnits});
    }
    // Twice: the wire, and `SPAWN ON CREATE`, true from the constructor when
    // the node carries no field 16.
    TL_CHECK_EQ(inputs, size_t{2});
}

// Units stand on the object grid: `CCharacter::update` lifts a unit off while it
// moves; `CBaseUnit::addToAvoidanceMap` puts back `COLLISION_RADIUS` + 0.4.
TL_TEST(a_standing_unit_occupies_the_object_grid) {
    tl::core::CollisionWorld world;
    const float h = 10.0f;
    world.add_triangle({-h, 0.0f, -h}, {-h, 0.0f, h}, {h, 0.0f, h});
    world.add_triangle({-h, 0.0f, -h}, {h, 0.0f, h}, {h, 0.0f, -h});
    world.build();
    tl::core::PassabilityMap grid;
    grid.bake(world, nullptr);
    tl::core::NavGrid nav;
    nav.build(grid);

    tl::core::UnitBook units;
    tl::core::UnitStats ratlin;
    ratlin.radius = 0.3f;
    ratlin.movement.walking_speed = 0.0f;
    units.add("ratlin", ratlin);
    tl::core::UnitStats corpse;
    corpse.collideable = false;
    corpse.movement.walking_speed = 0.0f;
    units.add("corpse", corpse);

    CmpScene empty_scene;
    LevelScript script;
    script.set_units(&units);
    script.set_navigation(&nav);
    script.set_collision(&world);
    script.set_passability(&grid);
    script.build(empty_scene, {});
    script.add_spawned_unit(-1001, "ratlin", tl::core::Vec3{2.0f, 0.0f, 2.0f}, false);
    script.add_spawned_unit(-1002, "corpse", tl::core::Vec3{-2.0f, 0.0f, -2.0f}, false);
    QuestState quests;
    std::vector<ScriptEvent> events;
    script.update(1.0f / 60.0f, tl::core::Vec3{0.0f, 0.0f, 0.0f}, quests, events);

    TL_CHECK(!grid.position_passable(tl::core::Vec3{2.0f, 0.0f, 2.0f}));
    TL_CHECK(grid.map_passable(tl::core::Vec3{2.0f, 0.0f, 2.0f}));
    TL_CHECK(grid.position_passable(tl::core::Vec3{-2.0f, 0.0f, -2.0f}));
    // Well outside the circle of 0.7.
    TL_CHECK(grid.position_passable(tl::core::Vec3{3.2f, 0.0f, 2.0f}));
}

// `FLEE EFFECT` (`CCharacter::applyEffectOnUnit` case 0x4f) turns a monster to flee once; a
// second only renews the held copy.
TL_TEST(a_flee_effect_turns_a_monster_to_flee_once) {
    tl::core::CollisionWorld world;
    world.add_triangle({-10.0f, 0.0f, -10.0f}, {-10.0f, 0.0f, 10.0f}, {10.0f, 0.0f, 10.0f});
    world.add_triangle({-10.0f, 0.0f, -10.0f}, {10.0f, 0.0f, 10.0f}, {10.0f, 0.0f, -10.0f});
    world.build();
    tl::core::PassabilityMap grid;
    grid.bake(world, nullptr);
    tl::core::NavGrid nav;
    nav.build(grid);

    tl::core::UnitBook units;
    tl::core::UnitStats ratlin;
    ratlin.radius = 0.3f;
    ratlin.movement.walking_speed = 1.4f;
    ratlin.movement.running_speed = 5.0f;
    units.add("Ratlin", ratlin);

    CmpScene empty_scene;
    LevelScript script;
    script.set_units(&units);
    script.set_navigation(&nav);
    script.set_collision(&world);
    script.set_passability(&grid);
    script.build(empty_scene, {});
    script.add_spawned_unit(-1001, "Ratlin", tl::core::Vec3{2.0f, 0.0f, 2.0f}, false);
    QuestState quests;
    std::vector<ScriptEvent> events;
    script.update(1.0f / 60.0f, tl::core::Vec3{9.0f, 0.0f, 9.0f}, quests, events);
    TL_CHECK_EQ(script.living_unit(-1001), std::string("Ratlin"));
    const tl::core::Vec3 source{0.0f, 0.0f, 0.0f};
    // The resistance roll is `integer_between(0, 100)` against 0, so a draw of
    // 0 resists even a 100% effect, as in the original: try until one lands.
    tl::core::volatile_random().set_seed(0x1234);
    const auto sounds = [&events] {
        int n = 0;
        for (const ScriptEvent& e : events) {
            if (e.action == tl::core::ScriptAction::UnitSample &&
                e.input == static_cast<uint32_t>(tl::core::SoundSlot::Flee)) {
                ++n;
            }
        }
        return n;
    };
    events.clear();
    bool turned = false;
    for (int i = 0; i < 16 && !turned; ++i) {
        turned = script.apply_flee_effect(-1001, 100.0f, 0.5f, &source, quests, events);
    }
    TL_CHECK(turned);
    TL_CHECK_EQ(sounds(), 1);
    // Fleeing already: the effect is copied again, with no second sound.
    for (int i = 0; i < 16; ++i) {
        if (script.apply_flee_effect(-1001, 100.0f, 0.5f, &source, quests, events)) break;
    }
    TL_CHECK_EQ(sounds(), 1);
    TL_CHECK(!script.apply_flee_effect(-9999, 100.0f, 0.5f, &source, quests, events));
}

// A room's units are the level's: one list of characters, positioned where the room put
// them (`getPosition(true)`).
TL_TEST(a_rooms_unit_stands_in_the_world_and_shares_the_levels_list) {
    tl::core::CollisionWorld world;
    world.add_triangle({-10.0f, 0.0f, -20.0f}, {-10.0f, 0.0f, 20.0f}, {50.0f, 0.0f, 20.0f});
    world.add_triangle({-10.0f, 0.0f, -20.0f}, {50.0f, 0.0f, 20.0f}, {50.0f, 0.0f, -20.0f});
    world.build();
    tl::core::PassabilityMap grid;
    grid.bake(world, nullptr);
    tl::core::NavGrid nav;
    nav.build(grid);

    tl::core::UnitBook units;
    tl::core::UnitStats ratlin;
    ratlin.radius = 0.3f;
    ratlin.movement.walking_speed = 2.0f;
    units.add("ratlin", ratlin);
    tl::core::UnitStats miner;
    miner.radius = 0.3f;
    miner.alignment = tl::core::UnitStats::Alignment::Friendly;
    miner.movement.walking_speed = 1.8f;
    units.add("miner", miner);

    // The room: one miner at (1, 0, 0) in its file, the file turned a quarter
    // about +Y and placed at (20, 0, 0) - its +X is the world's -Z.
    namespace m = tl::formats::cmp_field::monster;
    CmpScene room_scene;
    CmpNode& unit = add_node(room_scene, tl::formats::cmp_type::kMonster, 5, "Miner");
    pad_to(unit, 16);
    unit.fields[m::kPosition] = vec3_field(1.0f, 0.0f, 0.0f);
    unit.fields[m::kForward] = vec3_field(0.0f, 0.0f, 1.0f);
    unit.fields[m::kMonster] = text_field("miner");
    unit.fields[6] = uint_field(1);   // ENABLED
    unit.fields[12] = uint_field(1);  // VISIBLE
    unit.fields[m::kTargetable] = uint_field(1);

    std::vector<LevelScript*> list;
    LevelScript level;
    LevelScript room;
    list = {&level, &room};
    for (LevelScript* machine : list) {
        machine->set_units(&units);
        machine->set_navigation(&nav);
        machine->set_collision(&world);
        machine->set_passability(&grid);
        machine->set_level_machines(&list);
    }
    tl::core::LayoutFrame frame;
    frame.origin = {20.0f, 0.0f, 0.0f};
    frame.x = {0.0f, 0.0f, -1.0f};
    frame.z = {1.0f, 0.0f, 0.0f};
    room.set_frame(frame);
    CmpScene empty_scene;
    tl::test::seed_generators();  // the level load, not the room
    level.build(empty_scene, {});
    room.build(room_scene, {});

    tl::core::Vec3 at;
    TL_CHECK(room.unit_position(5, at));
    TL_CHECK(std::fabs(at.x - 20.0f) < 1e-4f);
    TL_CHECK(std::fabs(at.z + 1.0f) < 1e-4f);

    // A ratlin of the level's own, three away: inside its motion radius.
    level.add_spawned_unit(-1001, "ratlin", tl::core::Vec3{21.0f, 0.0f, 2.0f}, false);

    // The player near enough to keep both on the updated list, too far to be seen.
    const tl::core::Vec3 player{20.0f, 0.0f, 12.0f};
    QuestState quests;
    bool ratlin_hunts_miner = false;
    bool miner_hunts_ratlin = false;
    for (int frame_index = 0; frame_index < 60 * 3; ++frame_index) {
        std::vector<ScriptEvent> events;
        level.update(1.0f / 60.0f, player, quests, events);
        room.update(1.0f / 60.0f, player, quests, events);
        for (const ScriptEvent& e : events) {
            if (e.action != ScriptAction::Hunt || !e.on) continue;
            if (e.node_id == -1001 && e.target_machine == &room && e.target_node == 5) {
                ratlin_hunts_miner = true;
            }
            if (e.node_id == 5 && e.target_machine == &level && e.target_node == -1001) {
                miner_hunts_ratlin = true;
            }
        }
    }
    TL_CHECK(ratlin_hunts_miner);
    TL_CHECK(miner_hunts_ratlin);
}

// A pet keeps up: `CCharacter::addPet` -> `setMaster`; `CMonster::idleAI` teleports it back
// (`teleportToMaster(20)`) past 25.
TL_TEST(a_player_pet_left_far_behind_is_brought_back_to_its_master) {
    tl::core::CollisionWorld world;
    world.add_triangle({-50.0f, 0.0f, -50.0f}, {-50.0f, 0.0f, 50.0f}, {50.0f, 0.0f, 50.0f});
    world.add_triangle({-50.0f, 0.0f, -50.0f}, {50.0f, 0.0f, 50.0f}, {50.0f, 0.0f, -50.0f});
    world.build();
    tl::core::PassabilityMap grid;
    grid.bake(world, nullptr);
    tl::core::NavGrid nav;
    nav.build(grid);

    tl::core::UnitBook units;
    tl::core::UnitStats merc;
    merc.radius = 0.3f;
    merc.alignment = tl::core::UnitStats::Alignment::Friendly;
    merc.movement.walking_speed = 1.9f;
    merc.movement.running_speed = 6.5f;
    units.add("merc", merc);

    LevelScript level;
    level.set_units(&units);
    level.set_navigation(&nav);
    level.set_collision(&world);
    level.set_passability(&grid);
    tl::test::seed_generators();
    level.build(CmpScene{}, {});
    level.add_spawned_unit(-2001, "merc", tl::core::Vec3{-30.0f, 0.0f, 0.0f}, false);

    QuestState quests;
    std::vector<ScriptEvent> events;
    TL_CHECK(level.make_player_pet(-2001, quests, events));
    TL_CHECK_EQ(level.player_pets().size(), 1u);

    const tl::core::Vec3 player{20.0f, 0.0f, 0.0f};
    for (int frame_index = 0; frame_index < 60; ++frame_index) {
        events.clear();
        level.update(1.0f / 60.0f, player, quests, events);
    }
    tl::core::Vec3 at;
    TL_CHECK(level.unit_position(-2001, at));
    const float dx = at.x - player.x;
    const float dz = at.z - player.z;
    // Twenty from the master, scattered at most 8 each way, and nowhere near 50.
    TL_CHECK(std::sqrt(dx * dx + dz * dz) < 25.0f + 1e-3f);
}

// -------------------------------------------------- volatile script rolls

// Timer with TIME: 1, MAXTIME: 3: after reset, timer_left matches an identically-seeded
// RunicRandom stepped with between(1, 3), and the volatile stream advances by two steps.
TL_TEST(timer_reset_draws_from_volatile_stream_when_maxtime_greater_than_time) {
    constexpr int32_t kSeed = 0x12345678;
    tl::core::volatile_random().set_seed(kSeed);

    tl::core::RunicRandom reference(0);
    reference.set_seed(kSeed);
    const float expected_time = reference.between(1.0f, 3.0f);
    const uint64_t expected_state = reference.state();

    CmpScene scene;
    CmpNode& timer = add_node(scene, tl::formats::cmp_type::kTimer, 1, "Timer");
    timer.fields.push_back(uint_field(1));      // 0 ENABLED
    timer.fields.push_back(float_field(1.0f));  // 1 TIME
    timer.fields.push_back(float_field(3.0f));  // 2 MAXTIME
    pad_to(timer, 5);

    LevelScript script;
    script.build(scene, {});

    // Set known non-zero seed after build and reset the timer:
    tl::core::volatile_random().set_seed(kSeed);
    script.timer_reset(1);

    TL_CHECK_EQ(script.timer_left(1), expected_time);
    TL_CHECK_EQ(tl::core::volatile_random().state(), expected_state);

    // Verify stream advanced by exactly two multiply-with-carry steps:
    tl::core::RunicRandom two_steps(0);
    two_steps.set_seed(kSeed);
    two_steps.step();
    two_steps.step();
    TL_CHECK_EQ(tl::core::volatile_random().state(), two_steps.state());
}

// Timer with MAXTIME absent (-1) or <= TIME: after reset, timer_left == TIME and
// the volatile stream takes zero steps (state unchanged).
TL_TEST(timer_reset_takes_no_draw_when_maxtime_absent_or_not_greater_than_time) {
    constexpr int32_t kSeed = 0x12345678;
    tl::core::volatile_random().set_seed(kSeed);

    CmpScene scene;
    // Timer 1: MAXTIME absent (-1)
    CmpNode& t_absent = add_node(scene, tl::formats::cmp_type::kTimer, 1, "AbsentMax");
    t_absent.fields.push_back(uint_field(1));      // 0 ENABLED
    t_absent.fields.push_back(float_field(2.0f));  // 1 TIME
    pad_to(t_absent, 5);

    // Timer 2: MAXTIME == TIME (3.0f)
    CmpNode& t_equal = add_node(scene, tl::formats::cmp_type::kTimer, 2, "EqualMax");
    t_equal.fields.push_back(uint_field(1));      // 0 ENABLED
    t_equal.fields.push_back(float_field(3.0f));  // 1 TIME
    t_equal.fields.push_back(float_field(3.0f));  // 2 MAXTIME
    pad_to(t_equal, 5);

    // Timer 3: MAXTIME < TIME (1.0f < 4.0f)
    CmpNode& t_less = add_node(scene, tl::formats::cmp_type::kTimer, 3, "LessMax");
    t_less.fields.push_back(uint_field(1));      // 0 ENABLED
    t_less.fields.push_back(float_field(4.0f));  // 1 TIME
    t_less.fields.push_back(float_field(1.0f));  // 2 MAXTIME
    pad_to(t_less, 5);

    LevelScript script;
    script.build(scene, {});

    tl::core::volatile_random().set_seed(kSeed);
    const uint64_t state_before = tl::core::volatile_random().state();

    // Absent: timer_left == TIME, zero draws
    script.timer_reset(1);
    TL_CHECK_EQ(script.timer_left(1), 2.0f);
    TL_CHECK_EQ(tl::core::volatile_random().state(), state_before);

    // Equal: timer_left == TIME, zero draws
    script.timer_reset(2);
    TL_CHECK_EQ(script.timer_left(2), 3.0f);
    TL_CHECK_EQ(tl::core::volatile_random().state(), state_before);

    // Less: timer_left == TIME, zero draws
    script.timer_reset(3);
    TL_CHECK_EQ(script.timer_left(3), 4.0f);
    TL_CHECK_EQ(tl::core::volatile_random().state(), state_before);
}

// Random Choice mode 1 ("RANDOM"): fixed seed, hand-computed outputs based on
// integer_between(1, 100) <= weight. Weights <= 0 take no draw.
TL_TEST(random_choice_mode_random_draws_from_volatile_stream_per_positive_weight) {
    constexpr int32_t kSeed = 0x12345678;
    tl::core::volatile_random().set_seed(kSeed);

    CmpScene scene;
    // Node 1: Mode 1 (RANDOM), weights [50, 0, 70, 0, 30]
    CmpNode& choice = add_node(scene, 39, 1, "ChoiceRandom");
    choice.fields.push_back(uint_field(1));         // 0 ENABLED
    choice.fields.push_back(text_field("RANDOM"));  // 1 TYPE
    choice.fields.push_back(uint_field(1));         // 2 COUNT
    choice.fields.push_back(uint_field(50));        // 3 ONE (50)
    choice.fields.push_back(uint_field(0));         // 4 TWO (0: no draw)
    choice.fields.push_back(uint_field(70));        // 5 THREE (70)
    choice.fields.push_back(uint_field(0));         // 6 FOUR (0: no draw)
    choice.fields.push_back(uint_field(30));        // 7 FIVE (30)

    for (int64_t id = 101; id <= 105; ++id) {
        add_node(scene, tl::formats::cmp_type::kMonster, id, "Target");
    }

    CmpNode& group = add_node(scene, tl::formats::cmp_type::kLogicGroup, 10, "Logic");
    group.fields.push_back(uint_field(0));
    tl::formats::CmpLogicGroup wiring;
    wiring.node_id = 10;
    wiring.objects.push_back({0, 10, 0.0f, 0.0f, {}});
    wiring.objects.push_back({1,
                              1,
                              0.0f,
                              0.0f,
                              {
                                  {2, "One", "Show"},
                                  {3, "Two", "Show"},
                                  {4, "Three", "Show"},
                                  {5, "Four", "Show"},
                                  {6, "Five", "Show"},
                              }});
    wiring.objects.push_back({2, 101, 0.0f, 0.0f, {}});
    wiring.objects.push_back({3, 102, 0.0f, 0.0f, {}});
    wiring.objects.push_back({4, 103, 0.0f, 0.0f, {}});
    wiring.objects.push_back({5, 104, 0.0f, 0.0f, {}});
    wiring.objects.push_back({6, 105, 0.0f, 0.0f, {}});
    scene.logic_groups.push_back(std::move(wiring));

    LevelScript script;
    script.build(scene, {});

    // Hand-step prediction with a reference generator:
    tl::core::RunicRandom ref(0);
    ref.set_seed(kSeed);
    const int r0 = ref.integer_between(1, 100);  // 45 <= 50 -> HIT One (target 101)
    // Choice 1 weight is 0 -> no draw
    const int r2 = ref.integer_between(1, 100);  // 34 <= 70 -> HIT Three (target 103)
    // Choice 3 weight is 0 -> no draw
    const int r4 = ref.integer_between(1, 100);  // 58 > 30 -> MISS Five
    TL_CHECK_EQ(r0, 45);
    TL_CHECK_EQ(r2, 34);
    TL_CHECK_EQ(r4, 58);

    tl::core::volatile_random().set_seed(kSeed);
    QuestState quests;
    std::vector<ScriptEvent> events;
    script.roll_random_choice(1, quests, events);

    // Exactly 2 events broadcast: 101 (One) and 103 (Three)
    TL_CHECK_EQ(events.size(), size_t{2});
    TL_CHECK_EQ(events[0].node_id, 101);
    TL_CHECK_EQ(events[1].node_id, 103);

    // Stream advanced by exactly 3 draws (choices 0, 2, 4)
    TL_CHECK_EQ(tl::core::volatile_random().state(), ref.state());
}

// Random Choice mode 0 ("WEIGHT"): fixed seed, outputs predicted by a reference
// Randomizer on g_Rand (`game_random()`).
TL_TEST(random_choice_mode_weight_draws_from_game_random_with_replacement) {
    constexpr int32_t kSeed = 100;
    tl::core::game_random().set_seed(kSeed);

    CmpScene scene;
    // Weights: [40, 30, 30, 0, 0], COUNT: 3, TYPE: 0 (WEIGHT)
    CmpNode& choice = add_node(scene, 39, 1, "ChoiceWeight");
    choice.fields.push_back(uint_field(1));         // 0 ENABLED
    choice.fields.push_back(text_field("WEIGHT"));  // 1 TYPE
    choice.fields.push_back(uint_field(3));         // 2 COUNT
    choice.fields.push_back(uint_field(40));        // 3 ONE (40)
    choice.fields.push_back(uint_field(30));        // 4 TWO (30)
    choice.fields.push_back(uint_field(30));        // 5 THREE (30)
    choice.fields.push_back(uint_field(0));         // 6 FOUR (0)
    choice.fields.push_back(uint_field(0));         // 7 FIVE (0)

    for (int64_t id = 101; id <= 105; ++id) {
        add_node(scene, tl::formats::cmp_type::kMonster, id, "Target");
    }

    CmpNode& group = add_node(scene, tl::formats::cmp_type::kLogicGroup, 10, "Logic");
    group.fields.push_back(uint_field(0));
    tl::formats::CmpLogicGroup wiring;
    wiring.node_id = 10;
    wiring.objects.push_back({0, 10, 0.0f, 0.0f, {}});
    wiring.objects.push_back({1,
                              1,
                              0.0f,
                              0.0f,
                              {
                                  {2, "One", "Show"},
                                  {3, "Two", "Show"},
                                  {4, "Three", "Show"},
                                  {5, "Four", "Show"},
                                  {6, "Five", "Show"},
                              }});
    wiring.objects.push_back({2, 101, 0.0f, 0.0f, {}});
    wiring.objects.push_back({3, 102, 0.0f, 0.0f, {}});
    wiring.objects.push_back({4, 103, 0.0f, 0.0f, {}});
    wiring.objects.push_back({5, 104, 0.0f, 0.0f, {}});
    wiring.objects.push_back({6, 105, 0.0f, 0.0f, {}});
    scene.logic_groups.push_back(std::move(wiring));

    LevelScript script;
    script.build(scene, {});

    // Reference Randomizer with the same seed:
    tl::core::Randomizer ref_rand(tl::core::RandomizerType::kNormal);
    ref_rand.add_choice(0, 40);
    ref_rand.add_choice(1, 30);
    ref_rand.add_choice(2, 30);
    tl::core::RunicRandom ref_rng(0);
    ref_rng.set_seed(kSeed);
    const int32_t p0 = ref_rand.get_random(ref_rng);  // choice 2 -> Three (103)
    const int32_t p1 = ref_rand.get_random(ref_rng);  // choice 0 -> One (101)
    const int32_t p2 = ref_rand.get_random(ref_rng);  // choice 2 -> Three (103)
    TL_CHECK_EQ(p0, 2);
    TL_CHECK_EQ(p1, 0);
    TL_CHECK_EQ(p2, 2);

    tl::core::game_random().set_seed(kSeed);
    tl::core::volatile_random().set_seed(0x7e57);
    const uint64_t volatile_before = tl::core::volatile_random().state();
    QuestState quests;
    std::vector<ScriptEvent> events;
    script.roll_random_choice(1, quests, events);
    // The weighted mode takes nothing from the volatile stream.
    TL_CHECK_EQ(tl::core::volatile_random().state(), volatile_before);

    TL_CHECK_EQ(events.size(), size_t{3});
    TL_CHECK_EQ(events[0].node_id, 103);
    TL_CHECK_EQ(events[1].node_id, 101);
    TL_CHECK_EQ(events[2].node_id, 103);

    // g_Rand matches the reference generator after 3 draws (`CRandomizer::getRandom`
    // calls `randomBetween`, not the volatile twin):
    TL_CHECK_EQ(tl::core::game_random().state(), ref_rng.state());
}

// Random Choice with COUNT > positive weights (repeats picks), and with all weights 0
// (zero draws, zero outputs).
TL_TEST(random_choice_count_repeats_picks_and_all_zero_weights_draw_nothing) {
    constexpr int32_t kSeed = 0x12345678;
    tl::core::game_random().set_seed(kSeed);

    CmpScene scene;
    // Node 1: Only Choice 1 has positive weight (10), COUNT = 3
    CmpNode& choice_single = add_node(scene, 39, 1, "ChoiceSingle");
    choice_single.fields.push_back(uint_field(1));         // 0 ENABLED
    choice_single.fields.push_back(text_field("WEIGHT"));  // 1 TYPE
    choice_single.fields.push_back(uint_field(3));         // 2 COUNT = 3
    choice_single.fields.push_back(uint_field(0));         // 3 ONE (0)
    choice_single.fields.push_back(uint_field(10));        // 4 TWO (10)
    choice_single.fields.push_back(uint_field(0));         // 5 THREE (0)
    choice_single.fields.push_back(uint_field(0));         // 6 FOUR (0)
    choice_single.fields.push_back(uint_field(0));         // 7 FIVE (0)

    // Node 2: All weights 0, COUNT = 3 (Mode 0: WEIGHT)
    CmpNode& choice_zero_mode0 = add_node(scene, 39, 2, "ChoiceZero0");
    choice_zero_mode0.fields.push_back(uint_field(1));         // 0 ENABLED
    choice_zero_mode0.fields.push_back(text_field("WEIGHT"));  // 1 TYPE
    choice_zero_mode0.fields.push_back(uint_field(3));         // 2 COUNT = 3
    for (int i = 0; i < 5; ++i) choice_zero_mode0.fields.push_back(uint_field(0));

    // Node 3: All weights 0, COUNT = 3 (Mode 1: RANDOM)
    CmpNode& choice_zero_mode1 = add_node(scene, 39, 3, "ChoiceZero1");
    choice_zero_mode1.fields.push_back(uint_field(1));         // 0 ENABLED
    choice_zero_mode1.fields.push_back(text_field("RANDOM"));  // 1 TYPE
    choice_zero_mode1.fields.push_back(uint_field(3));         // 2 COUNT = 3
    for (int i = 0; i < 5; ++i) choice_zero_mode1.fields.push_back(uint_field(0));

    for (int64_t id = 101; id <= 105; ++id) {
        add_node(scene, tl::formats::cmp_type::kMonster, id, "Target");
    }

    CmpNode& group = add_node(scene, tl::formats::cmp_type::kLogicGroup, 10, "Logic");
    group.fields.push_back(uint_field(0));
    tl::formats::CmpLogicGroup wiring;
    wiring.node_id = 10;
    wiring.objects.push_back({0, 10, 0.0f, 0.0f, {}});
    wiring.objects.push_back({1, 1, 0.0f, 0.0f, {{2, "Two", "Show"}}});
    wiring.objects.push_back({2, 102, 0.0f, 0.0f, {}});
    scene.logic_groups.push_back(std::move(wiring));

    LevelScript script;
    script.build(scene, {});

    tl::core::game_random().set_seed(kSeed);
    QuestState quests;

    // Part A: COUNT > positive weights (3 draws for 1 choice) repeats picks
    {
        std::vector<ScriptEvent> events;
        script.roll_random_choice(1, quests, events);
        TL_CHECK_EQ(events.size(), size_t{3});
        for (const auto& e : events) {
            TL_CHECK_EQ(e.node_id, 102);
        }
    }

    // Part B: All weights 0 in Mode 0 takes 0 draws, broadcasts 0
    {
        const uint64_t state_before = tl::core::game_random().state();
        std::vector<ScriptEvent> events;
        script.roll_random_choice(2, quests, events);
        TL_CHECK(events.empty());
        TL_CHECK_EQ(tl::core::game_random().state(), state_before);
    }

    // Part C: All weights 0 in Mode 1 takes 0 draws, broadcasts 0
    {
        const uint64_t state_before = tl::core::volatile_random().state();
        std::vector<ScriptEvent> events;
        script.roll_random_choice(3, quests, events);
        TL_CHECK(events.empty());
        TL_CHECK_EQ(tl::core::volatile_random().state(), state_before);
    }
}

// Random Choice with ENABLED: false takes zero draws and broadcasts nothing.
TL_TEST(random_choice_disabled_takes_zero_draws_and_broadcasts_nothing) {
    constexpr int32_t kSeed = 0x12345678;
    tl::core::volatile_random().set_seed(kSeed);

    CmpScene scene;
    CmpNode& choice = add_node(scene, 39, 1, "ChoiceDisabled");
    choice.fields.push_back(uint_field(0));         // 0 ENABLED = false
    choice.fields.push_back(text_field("WEIGHT"));  // 1 TYPE
    choice.fields.push_back(uint_field(2));         // 2 COUNT = 2
    for (int i = 0; i < 5; ++i) choice.fields.push_back(uint_field(10));

    LevelScript script;
    script.build(scene, {});

    tl::core::volatile_random().set_seed(kSeed);
    const uint64_t state_before = tl::core::volatile_random().state();

    QuestState quests;
    std::vector<ScriptEvent> events;
    script.roll_random_choice(1, quests, events);

    TL_CHECK(events.empty());
    TL_CHECK_EQ(tl::core::volatile_random().state(), state_before);
}

// Two consecutive Rolls on one node draw on from the stream: no per-node reseed.
TL_TEST(random_choice_consecutive_rolls_advance_volatile_stream_without_reseed) {
    constexpr int32_t kSeed = 0x12345678;
    tl::core::volatile_random().set_seed(kSeed);

    CmpScene scene;
    // Mode 1 (RANDOM), weights [50, 40, 0, 0, 0]
    CmpNode& choice = add_node(scene, 39, 1, "ChoiceConsecutive");
    choice.fields.push_back(uint_field(1));         // 0 ENABLED
    choice.fields.push_back(text_field("RANDOM"));  // 1 TYPE
    choice.fields.push_back(uint_field(1));         // 2 COUNT
    choice.fields.push_back(uint_field(50));        // 3 ONE (50)
    choice.fields.push_back(uint_field(40));        // 4 TWO (40)
    choice.fields.push_back(uint_field(0));         // 5 THREE (0)
    choice.fields.push_back(uint_field(0));         // 6 FOUR (0)
    choice.fields.push_back(uint_field(0));         // 7 FIVE (0)

    for (int64_t id = 101; id <= 102; ++id) {
        add_node(scene, tl::formats::cmp_type::kMonster, id, "Target");
    }

    CmpNode& group = add_node(scene, tl::formats::cmp_type::kLogicGroup, 10, "Logic");
    group.fields.push_back(uint_field(0));
    tl::formats::CmpLogicGroup wiring;
    wiring.node_id = 10;
    wiring.objects.push_back({0, 10, 0.0f, 0.0f, {}});
    wiring.objects.push_back({1,
                              1,
                              0.0f,
                              0.0f,
                              {
                                  {2, "One", "Show"},
                                  {3, "Two", "Show"},
                              }});
    wiring.objects.push_back({2, 101, 0.0f, 0.0f, {}});
    wiring.objects.push_back({3, 102, 0.0f, 0.0f, {}});
    scene.logic_groups.push_back(std::move(wiring));

    LevelScript script;
    script.build(scene, {});

    // Hand-stepped prediction with a reference generator:
    tl::core::RunicRandom ref(0);
    ref.set_seed(kSeed);
    // Roll 1:
    const int r1_c0 = ref.integer_between(1, 100);  // 45 <= 50 -> HIT One (101)
    const int r1_c1 = ref.integer_between(1, 100);  // 34 <= 40 -> HIT Two (102)
    TL_CHECK_EQ(r1_c0, 45);
    TL_CHECK_EQ(r1_c1, 34);
    const uint64_t state_after_roll1 = ref.state();

    // Roll 2 (continues sequentially without reseeding):
    const int r2_c0 = ref.integer_between(1, 100);  // 58 > 50 -> MISS One
    const int r2_c1 = ref.integer_between(1, 100);  // 46 > 40 -> MISS Two
    TL_CHECK_EQ(r2_c0, 58);
    TL_CHECK_EQ(r2_c1, 46);
    const uint64_t state_after_roll2 = ref.state();

    tl::core::volatile_random().set_seed(kSeed);
    QuestState quests;

    // First roll:
    std::vector<ScriptEvent> events1;
    script.roll_random_choice(1, quests, events1);
    TL_CHECK_EQ(events1.size(), size_t{2});
    TL_CHECK_EQ(events1[0].node_id, 101);
    TL_CHECK_EQ(events1[1].node_id, 102);
    TL_CHECK_EQ(tl::core::volatile_random().state(), state_after_roll1);

    // Second roll: draws sequentially from the ongoing stream:
    std::vector<ScriptEvent> events2;
    script.roll_random_choice(1, quests, events2);
    // If it had reseeded, events2 would have repeated the hits; since it continued, no hits:
    TL_CHECK(events2.empty());
    TL_CHECK_EQ(tl::core::volatile_random().state(), state_after_roll2);
}

// ------------------------------------------------------------------ timelines

namespace {

tl::formats::CmpTimelineKey make_tl_key(float t, std::initializer_list<float> vals) {
    tl::formats::CmpTimelineKey k;
    k.time = t;
    k.interpolation = tl::formats::cmp_interpolation::kLinear;
    for (float v : vals) {
        uint32_t bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        k.words.push_back(bits);
    }
    return k;
}

std::vector<ScriptEvent> events_of(const std::vector<ScriptEvent>& events, ScriptAction action) {
    std::vector<ScriptEvent> out;
    for (const ScriptEvent& e : events) {
        if (e.action == action) out.push_back(e);
    }
    return out;
}

}  // namespace

// `Set_setForward` -> `CPositionableObject::setForward`: the key
// replaces the forward column as it comes, not normalised.
TL_TEST(timeline_driving_generic_model_forward_turns_it) {
    CmpScene scene;
    CmpNode& model = add_node(scene, tl::formats::cmp_type::kModel, 101, "Model");
    pad_to(model, 20);
    model.fields[tl::formats::cmp_field::model::kRight] = vec3_field(0.0f, 0.0f, -1.0f);
    model.fields[tl::formats::cmp_field::model::kUp] = vec3_field(0.0f, 1.0f, 0.0f);
    model.fields[tl::formats::cmp_field::model::kForward] = vec3_field(1.0f, 0.0f, 0.0f);

    CmpNode& tl_node = add_node(scene, tl::formats::cmp_type::kTimeline, 10, "Timeline");
    pad_to(tl_node, 10);
    tl_node.fields[tl::formats::cmp_field::timeline::kDuration] = float_field(1.0f);
    tl_node.fields[tl::formats::cmp_field::timeline::kStartOnLoad] = uint_field(1);

    tl::formats::CmpTimeline tl_data;
    tl_data.node_id = 10;
    tl::formats::CmpTimelineTarget target;
    target.object_id = 101;
    tl::formats::CmpTimelineTrack track;
    track.tag = 2;  // Generic Model tag 2 is FORWARD (3 floats)
    track.is_event = false;
    track.keys.push_back(make_tl_key(0.0f, {0.0f, 0.0f, 1.0f}));
    track.keys.push_back(make_tl_key(1.0f, {1.0f, 0.0f, 0.0f}));
    target.tracks.push_back(track);
    tl_data.targets.push_back(target);
    scene.timelines.push_back(tl_data);

    LevelScript script;
    script.build(scene, {});
    QuestState quests;
    std::vector<ScriptEvent> events;
    script.level_activated(quests, events);
    events.clear();

    script.update(0.5f, {0, 0, 0}, quests, events);

    bool moved = false;
    bool turned = false;
    ScriptEvent orient;
    for (const auto& e : events) {
        if (e.action == ScriptAction::MoveNode && e.node_id == 101) moved = true;
        if (e.action == ScriptAction::OrientNode && e.node_id == 101) {
            turned = true;
            orient = e;
        }
    }
    TL_CHECK(!moved);
    TL_CHECK(turned);
    // A timeline starts at -0.01 of its length (`Reset_To_Beginning`), so half
    // a second in it stands at 0.49.
    TL_CHECK(std::abs(orient.axis_z.x - 0.49f) < 1e-4f);
    TL_CHECK(std::abs(orient.axis_z.y) < 1e-4f);
    TL_CHECK(std::abs(orient.axis_z.z - 0.51f) < 1e-4f);
    // The placed right and up, untouched.
    TL_CHECK(std::abs(orient.axis_x.z + 1.0f) < 1e-6f);
    TL_CHECK(std::abs(orient.axis_y.y - 1.0f) < 1e-6f);
    for (const auto& gap : script.unimplemented())
        TL_CHECK(gap.find("FORWARD") == std::string::npos);
}

// A Monster's `FORWARD` is its facing (the yaw of `+0xb4`), which `MoveUnit`
// draws it by.
TL_TEST(timeline_driving_monster_forward_turns_its_facing) {
    CmpScene scene;
    CmpNode& monster = add_node(scene, tl::formats::cmp_type::kMonster, 201, "Monster");
    pad_to(monster, 20);
    monster.fields[tl::formats::cmp_field::monster::kMonster] = text_field("TEST_MONSTER");
    monster.fields[tl::formats::cmp_field::monster::kForward] = vec3_field(0.0f, 0.0f, 1.0f);

    CmpNode& tl_node = add_node(scene, tl::formats::cmp_type::kTimeline, 10, "Timeline");
    pad_to(tl_node, 10);
    tl_node.fields[tl::formats::cmp_field::timeline::kDuration] = float_field(1.0f);
    tl_node.fields[tl::formats::cmp_field::timeline::kStartOnLoad] = uint_field(1);

    tl::formats::CmpTimeline tl_data;
    tl_data.node_id = 10;
    tl::formats::CmpTimelineTarget target;
    target.object_id = 201;
    tl::formats::CmpTimelineTrack track;
    track.tag = tl::formats::cmp_field::monster::kForward;
    track.is_event = false;
    track.keys.push_back(make_tl_key(0.0f, {0.0f, 0.0f, 1.0f}));
    track.keys.push_back(make_tl_key(1.0f, {1.0f, 0.0f, 0.0f}));
    target.tracks.push_back(track);
    tl_data.targets.push_back(target);
    scene.timelines.push_back(tl_data);

    LevelScript script;
    script.build(scene, {});
    QuestState quests;
    std::vector<ScriptEvent> events;
    script.level_activated(quests, events);
    events.clear();

    float facing = -1.0f;
    TL_CHECK(script.unit_facing(201, facing));
    TL_CHECK(std::abs(facing) < 1e-6f);

    script.update(0.5f, {0, 0, 0}, quests, events);

    bool oriented = false;
    for (const auto& e : events) {
        if (e.action == ScriptAction::OrientNode) oriented = true;
    }
    TL_CHECK(!oriented);
    // 0.49 of the way (`Reset_To_Beginning` starts at -0.01): the forward
    // column (0.49, 0, 0.51), unnormalised, whose yaw is the facing.
    TL_CHECK(script.unit_facing(201, facing));
    TL_CHECK(std::abs(facing - std::atan2(0.49f, 0.51f)) < 1e-4f);
}

// `Set_setEnabled` on a Property Node is `CPropertyNode::setEnabled`:
// the blocker and its `Disabled` output.
TL_TEST(timeline_driving_property_node_enabled_switches_its_blocker) {
    namespace pn = tl::formats::cmp_field::property_node;
    CmpScene scene;
    CmpNode& marker = add_node(scene, tl::formats::cmp_type::kMarker, 5, "PathNodeBLOCKER");
    pad_to(marker, pn::kEnabled + 1);
    marker.fields[pn::kType] = text_field("Path Node Occupation Box");
    marker.fields[pn::kEnabled] = uint_field(1);

    CmpNode& quest = add_node(scene, tl::formats::cmp_type::kQuestController, 2, "Done");
    pad_to(quest, tl::formats::cmp_field::quest_controller::kQuest);
    quest.fields.push_back(text_field("OPENED"));

    CmpNode& group = add_node(scene, tl::formats::cmp_type::kLogicGroup, 3, "Logic");
    group.fields.push_back(uint_field(0));
    tl::formats::CmpLogicGroup wiring;
    wiring.node_id = 3;
    wiring.objects.push_back({0, 5, 0.0f, 0.0f, {{1, "Disabled", "Force Complete"}}});
    wiring.objects.push_back({1, 2, 0.0f, 0.0f, {}});
    scene.logic_groups.push_back(std::move(wiring));

    CmpNode& tl_node = add_node(scene, tl::formats::cmp_type::kTimeline, 10, "Timeline");
    pad_to(tl_node, 10);
    tl_node.fields[tl::formats::cmp_field::timeline::kDuration] = float_field(1.0f);
    tl_node.fields[tl::formats::cmp_field::timeline::kStartOnLoad] = uint_field(1);

    tl::formats::CmpTimeline tl_data;
    tl_data.node_id = 10;
    tl::formats::CmpTimelineTarget target;
    target.object_id = 5;
    tl::formats::CmpTimelineTrack track;
    track.tag = pn::kEnabled;
    track.is_event = false;
    tl::formats::CmpTimelineKey on;
    on.time = 0.0f;
    on.words = {1u};
    tl::formats::CmpTimelineKey off;
    off.time = 0.5f;
    off.words = {0u};
    track.keys = {on, off};
    target.tracks.push_back(track);
    tl_data.targets.push_back(target);
    scene.timelines.push_back(tl_data);

    LevelScript script;
    script.build(scene, {});
    QuestState quests;
    std::vector<ScriptEvent> events;
    script.level_activated(quests, events);
    events.clear();

    script.update(0.25f, {0, 0, 0}, quests, events);
    TL_CHECK(!quests.is_complete("OPENED"));
    events.clear();
    script.update(0.5f, {0, 0, 0}, quests, events);
    bool blocker_off = false;
    for (const auto& e : events) {
        if (e.action == ScriptAction::SetEnabled && e.node_id == 5 && !e.on) blocker_off = true;
    }
    TL_CHECK(blocker_off);
    TL_CHECK(quests.is_complete("OPENED"));
    for (const auto& gap : script.unimplemented())
        TL_CHECK(gap.find("ENABLED") == std::string::npos);
}

// `CAffectorDescriptor::Set_setEnabled` is `CAffectorWrapper::setEnabled`.
//
TL_TEST(timeline_driving_affector_enable_switches_the_affector) {
    CmpScene scene;
    add_node(scene, tl::formats::cmp_type::kLinearForce, 7, "Suction");

    CmpNode& tl_node = add_node(scene, tl::formats::cmp_type::kTimeline, 10, "Changes");
    pad_to(tl_node, 10);
    tl_node.fields[tl::formats::cmp_field::timeline::kDuration] = float_field(1.0f);
    tl_node.fields[tl::formats::cmp_field::timeline::kStartOnLoad] = uint_field(1);

    tl::formats::CmpTimeline tl_data;
    tl_data.node_id = 10;
    tl::formats::CmpTimelineTarget target;
    target.object_id = 7;
    tl::formats::CmpTimelineTrack track;
    track.tag = 1;  // an affector's ENABLE
    track.is_event = false;
    tl::formats::CmpTimelineKey off;
    off.time = 0.0f;
    off.words = {0u};
    tl::formats::CmpTimelineKey on;
    on.time = 0.75f;
    on.words = {1u};
    track.keys = {off, on};
    target.tracks.push_back(track);
    tl_data.targets.push_back(target);
    scene.timelines.push_back(tl_data);

    LevelScript script;
    script.build(scene, {});
    QuestState quests;
    std::vector<ScriptEvent> events;
    script.level_activated(quests, events);
    events.clear();

    script.update(0.25f, {0, 0, 0}, quests, events);
    const std::vector<ScriptEvent> early = events_of(events, ScriptAction::AffectorEnabled);
    TL_CHECK(!early.empty());
    for (const ScriptEvent& e : early) {
        TL_CHECK(e.node_id == 7);
        TL_CHECK(!e.on);
    }
    events.clear();
    script.update(0.6f, {0, 0, 0}, quests, events);
    const std::vector<ScriptEvent> late = events_of(events, ScriptAction::AffectorEnabled);
    TL_CHECK(!late.empty());
    TL_CHECK(late.back().node_id == 7);
    TL_CHECK(late.back().on);
    for (const auto& gap : script.unimplemented())
        TL_CHECK(gap.find("ENABLE") == std::string::npos);
}

// `CCameraShakeDescriptor::InputLogicEvent`: `Activate` (0xf)
// starts the node's shake about its layout's position (Quake's, Stomp's...).
TL_TEST(timeline_activate_on_a_camera_shake_starts_it_at_the_layout) {
    CmpScene scene;
    add_node(scene, tl::formats::cmp_type::kCameraShake, 7, "Camera Shake1");

    CmpNode& tl_node = add_node(scene, tl::formats::cmp_type::kTimeline, 10, "Timeline1");
    pad_to(tl_node, 10);
    tl_node.fields[tl::formats::cmp_field::timeline::kDuration] = float_field(1.0f);
    tl_node.fields[tl::formats::cmp_field::timeline::kStartOnLoad] = uint_field(1);

    tl::formats::CmpTimeline tl_data;
    tl_data.node_id = 10;
    tl::formats::CmpTimelineTarget target;
    target.object_id = 7;
    tl::formats::CmpTimelineTrack activate;
    activate.tag = 0;  // `Activate`
    activate.is_event = true;
    tl::formats::CmpTimelineKey at;
    at.time = 0.1f;
    activate.keys.push_back(at);
    target.tracks.push_back(activate);
    tl_data.targets.push_back(target);
    scene.timelines.push_back(tl_data);

    LevelScript script;
    script.build(scene, {});
    tl::core::LayoutFrame frame;
    frame.origin = {3.0f, 0.5f, -2.0f};
    script.set_frame(frame);
    QuestState quests;
    std::vector<ScriptEvent> events;
    script.level_activated(quests, events);
    script.update(0.25f, {0, 0, 0}, quests, events);
    const std::vector<ScriptEvent> shakes = events_of(events, ScriptAction::CameraShake);
    TL_CHECK(shakes.size() == 1);
    TL_CHECK(shakes.front().node_id == 7);
    TL_CHECK(shakes.front().node == &scene.nodes.front());
    TL_CHECK_EQ(shakes.front().position.x, 3.0f);
    TL_CHECK_EQ(shakes.front().position.z, -2.0f);
    for (const auto& gap : script.unimplemented())
        TL_CHECK(gap.find("Activate") == std::string::npos);
}

// `CParticleTechWrapperDescriptor::InputLogicEvent`: `Start
// Particle` is the wrapper's `start`, `Stop Particle` its `stop`.
TL_TEST(timeline_event_on_a_particle_node_starts_and_stops_its_system) {
    CmpScene scene;
    add_node(scene, tl::formats::cmp_type::kParticle, 9, "TellSystem");

    CmpNode& tl_node = add_node(scene, tl::formats::cmp_type::kTimeline, 10, "Changes");
    pad_to(tl_node, 10);
    tl_node.fields[tl::formats::cmp_field::timeline::kDuration] = float_field(1.0f);
    tl_node.fields[tl::formats::cmp_field::timeline::kStartOnLoad] = uint_field(1);

    tl::formats::CmpTimeline tl_data;
    tl_data.node_id = 10;
    tl::formats::CmpTimelineTarget target;
    target.object_id = 9;
    tl::formats::CmpTimelineTrack start;
    start.tag = 0;  // `Start Particle`
    start.is_event = true;
    tl::formats::CmpTimelineKey at_start;
    at_start.time = 0.0f;
    start.keys.push_back(at_start);
    tl::formats::CmpTimelineTrack stop;
    stop.tag = 1;  // `Stop Particle`
    stop.is_event = true;
    tl::formats::CmpTimelineKey at_half;
    at_half.time = 0.5f;
    stop.keys.push_back(at_half);
    target.tracks = {start, stop};
    tl_data.targets.push_back(target);
    scene.timelines.push_back(tl_data);

    LevelScript script;
    script.build(scene, {});
    QuestState quests;
    std::vector<ScriptEvent> events;
    script.level_activated(quests, events);
    script.update(0.25f, {0, 0, 0}, quests, events);
    const std::vector<ScriptEvent> started = events_of(events, ScriptAction::ParticleNode);
    TL_CHECK(started.size() == 1);
    TL_CHECK(started.front().node_id == 9);
    TL_CHECK(started.front().on);
    // Nothing is shown or hidden: an enabled wrapper is only started.
    TL_CHECK(events_of(events, ScriptAction::SetVisible).empty());

    events.clear();
    script.update(0.5f, {0, 0, 0}, quests, events);
    const std::vector<ScriptEvent> stopped = events_of(events, ScriptAction::ParticleNode);
    TL_CHECK(stopped.size() == 1);
    TL_CHECK(!stopped.front().on);
}

// A disabled wrapper is activated first (`setActivated(true)`), then started again.
TL_TEST(start_particle_on_a_disabled_particle_node_activates_it) {
    CmpScene scene;
    CmpNode& particle = add_node(scene, tl::formats::cmp_type::kParticle, 9, "Hidden");
    pad_to(particle, tl::formats::cmp_field::particle::kVisible + 1);
    particle.fields[tl::formats::cmp_field::particle::kVisible] = uint_field(0);

    CmpNode& tl_node = add_node(scene, tl::formats::cmp_type::kTimeline, 10, "Changes");
    pad_to(tl_node, 10);
    tl_node.fields[tl::formats::cmp_field::timeline::kDuration] = float_field(1.0f);
    tl_node.fields[tl::formats::cmp_field::timeline::kStartOnLoad] = uint_field(1);

    tl::formats::CmpTimeline tl_data;
    tl_data.node_id = 10;
    tl::formats::CmpTimelineTarget target;
    target.object_id = 9;
    tl::formats::CmpTimelineTrack start;
    start.tag = 0;
    start.is_event = true;
    tl::formats::CmpTimelineKey at_start;
    at_start.time = 0.0f;
    start.keys.push_back(at_start);
    target.tracks.push_back(start);
    tl_data.targets.push_back(target);
    scene.timelines.push_back(tl_data);

    LevelScript script;
    script.build(scene, {});
    QuestState quests;
    std::vector<ScriptEvent> events;
    script.level_activated(quests, events);
    script.update(0.25f, {0, 0, 0}, quests, events);
    const std::vector<ScriptEvent> shown = events_of(events, ScriptAction::SetVisible);
    TL_CHECK(shown.size() == 1);
    TL_CHECK(shown.front().on);
    const std::vector<ScriptEvent> started = events_of(events, ScriptAction::ParticleNode);
    TL_CHECK(started.size() == 2);
    for (const ScriptEvent& e : started) TL_CHECK(e.on);
}

TL_TEST(timeline_driving_generic_model_position_moves_target) {
    CmpScene scene;
    CmpNode& model = add_node(scene, tl::formats::cmp_type::kModel, 101, "Model");
    pad_to(model, 20);

    CmpNode& tl_node = add_node(scene, tl::formats::cmp_type::kTimeline, 10, "Timeline");
    pad_to(tl_node, 10);
    tl_node.fields[tl::formats::cmp_field::timeline::kDuration] = float_field(1.0f);
    tl_node.fields[tl::formats::cmp_field::timeline::kStartOnLoad] = uint_field(1);

    tl::formats::CmpTimeline tl_data;
    tl_data.node_id = 10;
    tl::formats::CmpTimelineTarget target;
    target.object_id = 101;
    tl::formats::CmpTimelineTrack track;
    track.tag = 1;  // Generic Model tag 1 is POSITION
    track.is_event = false;
    track.keys.push_back(make_tl_key(0.0f, {0.0f, 0.0f, 0.0f}));
    track.keys.push_back(make_tl_key(1.0f, {10.0f, 20.0f, 30.0f}));
    target.tracks.push_back(track);
    tl_data.targets.push_back(target);
    scene.timelines.push_back(tl_data);

    LevelScript script;
    script.build(scene, {});
    QuestState quests;
    std::vector<ScriptEvent> events;
    script.level_activated(quests, events);
    events.clear();

    script.update(0.5f, {0, 0, 0}, quests, events);

    // Verify target IS moved (generates MoveNode event at {5, 10, 15})
    bool moved = false;
    tl::core::Vec3 pos;
    for (const auto& e : events) {
        if (e.action == ScriptAction::MoveNode && e.node_id == 101) {
            moved = true;
            pos = e.position;
        }
    }
    TL_CHECK(moved);
    TL_CHECK(std::abs(pos.x - 5.0f) < 0.5f);
    TL_CHECK(std::abs(pos.y - 10.0f) < 0.5f);
    TL_CHECK(std::abs(pos.z - 15.0f) < 0.5f);
}

TL_TEST(timeline_driving_monster_position_moves_target) {
    CmpScene scene;
    CmpNode& monster = add_node(scene, tl::formats::cmp_type::kMonster, 201, "Monster");
    pad_to(monster, 20);
    monster.fields[tl::formats::cmp_field::monster::kMonster] = text_field("TEST_MONSTER");

    CmpNode& tl_node = add_node(scene, tl::formats::cmp_type::kTimeline, 10, "Timeline");
    pad_to(tl_node, 10);
    tl_node.fields[tl::formats::cmp_field::timeline::kDuration] = float_field(1.0f);
    tl_node.fields[tl::formats::cmp_field::timeline::kStartOnLoad] = uint_field(1);

    tl::formats::CmpTimeline tl_data;
    tl_data.node_id = 10;
    tl::formats::CmpTimelineTarget target;
    target.object_id = 201;
    tl::formats::CmpTimelineTrack track;
    track.tag = 0;  // Monster tag 0 is POSITION
    track.is_event = false;
    track.keys.push_back(make_tl_key(0.0f, {0.0f, 0.0f, 0.0f}));
    track.keys.push_back(make_tl_key(1.0f, {2.0f, 4.0f, 6.0f}));
    target.tracks.push_back(track);
    tl_data.targets.push_back(target);
    scene.timelines.push_back(tl_data);

    LevelScript script;
    script.build(scene, {});
    QuestState quests;
    std::vector<ScriptEvent> events;
    script.level_activated(quests, events);
    events.clear();

    script.update(0.5f, {0, 0, 0}, quests, events);

    // Verify target IS moved (generates MoveNode event at {1, 2, 3})
    bool moved = false;
    tl::core::Vec3 pos;
    for (const auto& e : events) {
        if (e.action == ScriptAction::MoveNode && e.node_id == 201) {
            moved = true;
            pos = e.position;
        }
    }
    TL_CHECK(moved);
    TL_CHECK(std::abs(pos.x - 1.0f) < 0.2f);
    TL_CHECK(std::abs(pos.y - 2.0f) < 0.2f);
    TL_CHECK(std::abs(pos.z - 3.0f) < 0.2f);
}

// ------------------------------------------------------------------ damage shapes

namespace {

using tl::core::DamageShape;
using tl::core::DamageShapeHit;
using tl::core::DamageShapeListener;
using tl::core::DamageShapeSwitch;
using tl::core::DamageShapeUnit;
using tl::core::DamageShapeVictim;
using tl::core::ShapeFrame;
using tl::core::SpawnerShape;
using tl::core::Vec3;

namespace dsf = tl::formats::cmp_field::damage_shape;

/// A dynamic attribute as the archive compiles a fixed one: kind 0, then the value.
CmpField attribute_field(float value) {
    CmpField f;
    f.words.push_back(0u);
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    f.words.push_back(bits);
    return f;
}

tl::core::DynamicAttribute fixed(float value) {
    tl::core::DynamicAttribute a;
    a.value = value;
    return a;
}

/// The few types of `UNITTYPES.HIE` a script test needs: `CBaseUnit::ISA`
/// answers false with no hierarchy at all, which the game never runs without.
const tl::core::UnitTypeHierarchy& basic_unit_types() {
    static const tl::core::UnitTypeHierarchy types = [] {
        auto make_type = [](const std::string& name, uint32_t id,
                            const std::vector<uint32_t>& ancestors) {
            tl::formats::AdmSection s;
            s.name = name;
            tl::formats::AdmRecord rid;
            rid.key = "ID";
            rid.type = tl::formats::AdmType::Integer;
            rid.integer = id;
            s.records.push_back(rid);
            for (size_t i = 0; i < ancestors.size(); ++i) {
                tl::formats::AdmRecord r;
                r.key = i == 0 ? "child" : "child" + std::to_string(i);
                r.type = tl::formats::AdmType::Integer;
                r.integer = ancestors[i];
                s.records.push_back(r);
            }
            return s;
        };
        tl::formats::AdmSection root;
        tl::formats::AdmSection list;
        list.name = "UNITTYPES";
        list.children.push_back(make_type("any", 0, {}));
        list.children.push_back(make_type("Character", 25, {0}));
        list.children.push_back(make_type("Monster", 27, {25, 0}));
        list.children.push_back(make_type("Player", 28, {25, 0}));
        root.children.push_back(list);
        tl::core::UnitTypeHierarchy h;
        h.parse(root);
        return h;
    }();
    return types;
}

/// `DAMAGE_MONSTER` flat at 40, monsters' health flat at 100.
tl::core::StatBook damage_shape_graphs() {
    tl::core::StatBook book;
    auto flat = [](const char* name, float y) {
        tl::core::StatGraph g(tl::core::GraphType::Linear, name);
        g.add_point(0.0f, y);
        g.add_point(100.0f, y);
        g.process_points();
        return g;
    };
    book.add("DAMAGE_MONSTER", flat("DAMAGE_MONSTER", 40.0f));
    book.add("HEALTH_MONSTER_BYLEVEL", flat("HEALTH_MONSTER_BYLEVEL", 100.0f));
    book.add("HEALTH_MONSTER_BYLEVEL_HARD", flat("HEALTH_MONSTER_BYLEVEL_HARD", 100.0f));
    return book;
}

/// A Damage Shape node with every field the archive compiles: a sphere of
/// `radius` at `at`, off, one loop, no timer, `MIN` = `MAX` = 50, not DOT.
CmpNode& add_damage_shape(CmpScene& scene, int64_t id, const std::string& name, const Vec3& at,
                          float radius) {
    CmpNode& node = add_node(scene, tl::formats::cmp_type::kDamageShape, id, name);
    pad_to(node, 24);
    node.fields[dsf::kPosition] = vec3_field(at.x, at.y, at.z);
    node.fields[dsf::kShape] = text_field("Sphere");
    node.fields[dsf::kReleaseOrder] = text_field("Clockwise");
    node.fields[dsf::kDirection] = text_field("Forward");
    node.fields[dsf::kBoxSize] = vec3_field(10.0f, 1.0f, 10.0f);
    node.fields[dsf::kUseMaxOnly] = uint_field(0);
    node.fields[dsf::kMinRadius] = attribute_field(0.0f);
    node.fields[dsf::kMaxRadius] = attribute_field(radius);
    node.fields[dsf::kAngle] = attribute_field(360.0f);
    node.fields[dsf::kAngleOffset] = attribute_field(0.0f);
    node.fields[dsf::kDamageType] = text_field("Physical");
    node.fields[dsf::kUnitType] = text_field("ANY");
    node.fields[dsf::kAlignment] = text_field("ALL");
    node.fields[dsf::kUpdateInEditor] = uint_field(1);
    node.fields[dsf::kEnabled] = uint_field(0);
    node.fields[dsf::kDot] = uint_field(0);
    node.fields[dsf::kTargetCorpse] = uint_field(0);
    node.fields[dsf::kMaxUnitsToHit] = uint_field(0);
    node.fields[dsf::kMin] = float_field(50.0f);
    node.fields[dsf::kMax] = float_field(50.0f);
    node.fields[dsf::kLoops] = uint_field(0);
    node.fields[dsf::kLoopCount] = uint_field(1);
    node.fields[dsf::kTimer] = float_field(0.0f);
    node.fields[dsf::kPauseTimer] = float_field(0.0f);
    return node;
}

void add_target(CmpScene& scene, int64_t id, const Vec3& at) {
    CmpNode& monster = add_node(scene, tl::formats::cmp_type::kMonster, id, "Target");
    pad_to(monster, 5);
    monster.fields[tl::formats::cmp_field::monster::kPosition] = vec3_field(at.x, at.y, at.z);
    monster.fields[tl::formats::cmp_field::monster::kMonster] = text_field("Target");
}

float health_of(const LevelScript& script, int64_t id) {
    float hp = 0.0f;
    float max = 0.0f;
    TL_CHECK(script.unit_health(id, hp, max));
    return hp;
}

/// Records what it is asked and answers as told.
struct RecordingListener : DamageShapeListener {
    bool allow = true;
    bool take_over = false;
    int asked = 0;
    int acted = 0;
    std::vector<int64_t>* order = nullptr;
    int64_t tag = 0;
    bool can_affect(const DamageShape&, const DamageShapeUnit&) override {
        ++asked;
        if (order != nullptr) order->push_back(tag);
        return allow;
    }
    bool affect(const DamageShape&, const DamageShapeUnit&) override {
        ++acted;
        return take_over;
    }
};

DamageShapeVictim victim_at(int64_t node) {
    DamageShapeVictim v;
    v.unit.node = node;
    v.alignment = 2;
    return v;
}

}  // namespace

TL_TEST(shape_sphere_ignores_min_radius_and_measures_height) {
    // `getPositionIsInShapeAtPercent` case 2: the full 3-D distance
    // against the max radius plus the body's; the min radius plays no part.
    SpawnerShape shape;
    shape.shape = 2;
    shape.min_radius = fixed(1.5f);
    shape.max_radius = fixed(2.0f);
    ShapeFrame frame;
    TL_CHECK(tl::core::shape_contains_position(shape, frame, 1.0f, {0.5f, 0.0f, 0.0f}, 0.0f));
    TL_CHECK(tl::core::shape_contains_position(shape, frame, 1.0f, {2.0f, 0.0f, 0.0f}, 0.0f));
    // 2.5 straight up: out bare, in with a 0.6 body (2 + 0.6 >= 2.5).
    TL_CHECK(!tl::core::shape_contains_position(shape, frame, 1.0f, {0.0f, 2.5f, 0.0f}, 0.0f));
    TL_CHECK(tl::core::shape_contains_position(shape, frame, 1.0f, {0.0f, 2.5f, 0.0f}, 0.6f));
    // `+0x134` scales the max radius by 1 plus it.
    shape.max_radius_scale = 0.5f;
    TL_CHECK(tl::core::shape_contains_position(shape, frame, 1.0f, {2.9f, 0.0f, 0.0f}, 0.0f));
}

TL_TEST(shape_angle_fan_is_centred_on_the_offset) {
    // A 90 degree fan: `getAngleOffsetAtPercent` gives -45, so the
    // fan runs -45..45 from the facing (+Z here).
    SpawnerShape shape;
    shape.shape = 0;
    shape.max_radius = fixed(10.0f);
    shape.angle = fixed(90.0f);
    ShapeFrame frame;
    TL_CHECK_EQ(tl::core::shape_angle_offset(shape, 1.0f), -45.0f);
    const float r = 5.0f;
    auto at_heading = [r](float degrees) {
        const float rad = degrees * 0.017453292f;
        return Vec3{std::sin(rad) * r, 0.0f, std::cos(rad) * r};
    };
    TL_CHECK(tl::core::shape_contains_position(shape, frame, 1.0f, at_heading(30.0f), 0.0f));
    TL_CHECK(tl::core::shape_contains_position(shape, frame, 1.0f, at_heading(-30.0f), 0.0f));
    TL_CHECK(!tl::core::shape_contains_position(shape, frame, 1.0f, at_heading(60.0f), 0.0f));
    TL_CHECK(!tl::core::shape_contains_position(shape, frame, 1.0f, at_heading(180.0f), 0.0f));
    // Standing exactly on the centre is inside on the console
    // (has no rule for it): heading 0, the facing's own.
    TL_CHECK(tl::core::shape_contains_position(shape, frame, 1.0f, {0.0f, 0.0f, 0.0f}, 1.0f));
    // Past the radius, and inside the min radius less the body.
    TL_CHECK(!tl::core::shape_contains_position(shape, frame, 1.0f, {0.0f, 0.0f, 10.5f}, 0.0f));
    shape.min_radius = fixed(3.0f);
    TL_CHECK(!tl::core::shape_contains_position(shape, frame, 1.0f, {0.0f, 0.0f, 2.0f}, 0.5f));
    TL_CHECK(tl::core::shape_contains_position(shape, frame, 1.0f, {0.0f, 0.0f, 2.0f}, 1.0f));
    // An offset of 90 turns the fan to 45..135: 60 is in.
    shape.min_radius = fixed(0.0f);
    shape.angle_offset = fixed(90.0f);
    TL_CHECK(tl::core::shape_contains_position(shape, frame, 1.0f, at_heading(60.0f), 0.0f));
}

TL_TEST(shape_angle_edge_test_measures_headings_from_the_facing) {
    // the body's two sides are measured from the shape's facing.
    SpawnerShape shape;
    shape.shape = 0;
    shape.max_radius = fixed(10.0f);
    shape.angle = fixed(90.0f);
    ShapeFrame frame;
    frame.forward = {0.0f, 0.0f, -1.0f};
    frame.right = {-1.0f, 0.0f, 0.0f};
    TL_CHECK(tl::core::shape_contains_position(shape, frame, 1.0f, {0.0f, 0.0f, -5.0f}, 0.0f));
    // Due +Z, behind the fan: out, its sides too.
    TL_CHECK(!tl::core::shape_contains_position(shape, frame, 1.0f, {0.0f, 0.0f, 5.0f}, 0.0f));
    // Due +X: 90 - 180 = -90 relative: out.
    TL_CHECK(!tl::core::shape_contains_position(shape, frame, 1.0f, {5.0f, 0.0f, 0.0f}, 0.0f));
    // Just past the fan's edge (heading 180 + 50 in the world), a body wide
    // enough to reach back over the edge: in by its side.
    const float rad = 230.0f * 0.017453292f;
    const Vec3 at{std::sin(rad) * 5.0f, 0.0f, std::cos(rad) * 5.0f};
    TL_CHECK(!tl::core::shape_contains_position(shape, frame, 1.0f, at, 0.0f));
    TL_CHECK(tl::core::shape_contains_position(shape, frame, 1.0f, at, 1.0f));
}

TL_TEST(shape_box_ignores_height_and_overlaps_by_radius) {
    // `positionWithRadiusIntersectsBox`: half widths 2 (x) and 3 (z).
    SpawnerShape shape;
    shape.shape = 4;
    shape.box_size = {4.0f, 2.0f, 6.0f};
    ShapeFrame frame;
    frame.center = {10.0f, 0.0f, 10.0f};
    TL_CHECK(tl::core::shape_contains_position(shape, frame, 1.0f, {11.0f, 50.0f, 12.0f}, 0.0f));
    TL_CHECK(!tl::core::shape_contains_position(shape, frame, 1.0f, {12.5f, 0.0f, 10.0f}, 0.0f));
    TL_CHECK(tl::core::shape_contains_position(shape, frame, 1.0f, {12.5f, 0.0f, 10.0f}, 0.6f));
    TL_CHECK(!tl::core::shape_contains_position(shape, frame, 1.0f, {10.0f, 0.0f, 13.5f}, 0.4f));
    // Turned a quarter: the long side now runs along x.
    frame.forward = {1.0f, 0.0f, 0.0f};
    frame.right = {0.0f, 0.0f, -1.0f};
    TL_CHECK(tl::core::shape_contains_position(shape, frame, 1.0f, {12.5f, 0.0f, 10.0f}, 0.0f));
}

TL_TEST(shape_line_runs_along_its_angle) {
    // `calculateLineSequment`: angle 0 points down the local x axis;
    // min = max collapses the start to the centre.
    SpawnerShape shape;
    shape.shape = 1;
    shape.min_radius = fixed(4.0f);
    shape.max_radius = fixed(4.0f);
    shape.angle = fixed(0.0f);
    ShapeFrame frame;
    Vec3 start;
    Vec3 end;
    tl::core::shape_line_segment(shape, frame, 1.0f, start, end);
    TL_CHECK_EQ(start.x, 0.0f);
    TL_CHECK_EQ(end.x, 4.0f);
    TL_CHECK(tl::core::shape_contains_position(shape, frame, 1.0f, {2.0f, 0.0f, 0.3f}, 0.5f));
    TL_CHECK(!tl::core::shape_contains_position(shape, frame, 1.0f, {2.0f, 0.0f, 1.0f}, 0.5f));
    TL_CHECK(!tl::core::shape_contains_position(shape, frame, 1.0f, {5.0f, 0.0f, 0.0f}, 0.5f));
}

TL_TEST(damage_shape_reads_the_compiled_node) {
    CmpScene scene;
    CmpNode& node = add_damage_shape(scene, 1, "Trap", {1.0f, 2.0f, 3.0f}, 4.0f);
    node.fields[dsf::kDamageType] = text_field("Fire");
    node.fields[dsf::kAlignment] = text_field("EVIL");
    node.fields[dsf::kLoopCount] = uint_field(static_cast<uint32_t>(-1));
    node.fields[dsf::kTimer] = float_field(0.75f);
    const DamageShape s = tl::core::read_damage_shape(node, nullptr);
    TL_CHECK_EQ(s.shape.shape, 2);
    TL_CHECK_EQ(s.shape.max_radius.at(1.0f), 4.0f);
    TL_CHECK_EQ(s.damage_type, 2);
    TL_CHECK_EQ(s.alignment, 2);
    TL_CHECK_EQ(s.loop_count, -1);
    TL_CHECK_EQ(s.loops_left, 0);  // `Set_setNumLoops`: never below 0
    TL_CHECK_EQ(s.timer, 0.75f);
    TL_CHECK_EQ(s.time_left, 0.75f);
    TL_CHECK(!s.enabled);
    // The word tables compare case and all: an unknown word is 0.
    node.fields[dsf::kDamageType] = text_field("FIRE");
    node.fields[dsf::kAlignment] = text_field("Evil");
    const DamageShape t = tl::core::read_damage_shape(node, nullptr);
    TL_CHECK_EQ(t.damage_type, 0);
    TL_CHECK_EQ(t.alignment, 0);
}

TL_TEST(damage_shape_amount_min_max_draws_once_on_volatile) {
    // v = 40 x (2 / 100) = 0.8; MIN differs, so
    // v = randomBetweenVolatile(0, 0.8 - 1/100) + 1/100 - one draw.
    constexpr int32_t kSeed = 0x2468ace;
    DamageShape s;
    s.dot = false;
    s.min_percent = 1.0f;
    s.max_percent = 2.0f;
    tl::core::RunicRandom rng(0);
    rng.set_seed(kSeed);
    tl::core::RunicRandom ref(0);
    ref.set_seed(kSeed);
    const float expected =
        ref.between(0.0f, 40.0f * (2.0f / 100.0f) - 1.0f / 100.0f) + 1.0f / 100.0f;
    const float amount = tl::core::damage_shape_amount(s, 40.0f, 0.1f, rng);
    TL_CHECK_EQ(amount, expected);
    TL_CHECK(0.01f <= amount && amount <= 0.8f);
    TL_CHECK_EQ(rng.state(), ref.state());

    // MIN == MAX: 40 x 0.5, and the generator is not touched.
    s.min_percent = 50.0f;
    s.max_percent = 50.0f;
    const uint64_t before = rng.state();
    TL_CHECK_EQ(tl::core::damage_shape_amount(s, 40.0f, 0.1f, rng), 20.0f);
    TL_CHECK_EQ(rng.state(), before);
}

TL_TEST(damage_shape_dot_scales_by_dt) {
    DamageShape s;
    s.dot = true;
    s.min_percent = 50.0f;
    s.max_percent = 50.0f;
    tl::core::RunicRandom rng(0);
    TL_CHECK_EQ(tl::core::damage_shape_amount(s, 40.0f, 0.25f, rng), 5.0f);
    s.dot = false;
    TL_CHECK_EQ(tl::core::damage_shape_amount(s, 40.0f, 0.25f, rng), 20.0f);
}

TL_TEST(damage_shape_hits_each_character_once_unless_dot) {
    const tl::core::StatBook graphs = damage_shape_graphs();
    const tl::core::StatGraph* graph = graphs.find("DAMAGE_MONSTER");
    tl::core::RunicRandom rng(0);
    DamageShape s;
    s.dot = false;
    s.min_percent = s.max_percent = 50.0f;
    DamageShapeHit hit =
        tl::core::damage_shape_do_damage_to_character(s, victim_at(7), graph, 1, 0.1f, rng);
    TL_CHECK(hit.deal);
    TL_CHECK(hit.record);
    TL_CHECK_EQ(hit.amount, 20.0f);
    s.hit.push_back(victim_at(7).unit);  // what the level does once it has landed
    hit = tl::core::damage_shape_do_damage_to_character(s, victim_at(7), graph, 1, 0.1f, rng);
    TL_CHECK(!hit.deal);
    TL_CHECK(
        tl::core::damage_shape_do_damage_to_character(s, victim_at(8), graph, 1, 0.1f, rng).deal);

    s.dot = true;
    s.hit.clear();
    for (int i = 0; i < 2; ++i) {
        hit = tl::core::damage_shape_do_damage_to_character(s, victim_at(7), graph, 1, 0.1f, rng);
        TL_CHECK(hit.deal);
        TL_CHECK(!hit.record);
        TL_CHECK_EQ(hit.amount, 2.0f);
    }
    // No curve, no damage and nothing recorded.
    s.dot = false;
    hit = tl::core::damage_shape_do_damage_to_character(s, victim_at(9), nullptr, 1, 0.1f, rng);
    TL_CHECK(!hit.deal);
    TL_CHECK(!hit.record);
}

TL_TEST(damage_shape_max_units_to_hit_caps_the_list) {
    const tl::core::StatBook graphs = damage_shape_graphs();
    tl::core::RunicRandom rng(0);
    DamageShape s;
    s.dot = false;
    s.max_units_to_hit = 1;
    TL_CHECK(tl::core::damage_shape_do_damage_to_character(
                 s, victim_at(1), graphs.find("DAMAGE_MONSTER"), 1, 0.1f, rng)
                 .deal);
    s.hit.push_back(victim_at(1).unit);
    TL_CHECK(!tl::core::damage_shape_do_damage_to_character(
                  s, victim_at(2), graphs.find("DAMAGE_MONSTER"), 1, 0.1f, rng)
                  .deal);
    // Below 1 is no cap.
    s.max_units_to_hit = 0;
    TL_CHECK(tl::core::damage_shape_do_damage_to_character(
                 s, victim_at(2), graphs.find("DAMAGE_MONSTER"), 1, 0.1f, rng)
                 .deal);
}

TL_TEST(damage_shape_alignment_three_is_anyone) {
    const tl::core::StatBook graphs = damage_shape_graphs();
    const tl::core::StatGraph* graph = graphs.find("DAMAGE_MONSTER");
    tl::core::RunicRandom rng(0);
    DamageShape s;
    s.dot = true;
    for (int32_t alignment = 0; alignment < 3; ++alignment) {
        DamageShapeVictim v = victim_at(1);
        v.alignment = alignment;
        s.alignment = 3;
        TL_CHECK(tl::core::damage_shape_do_damage_to_character(s, v, graph, 1, 0.1f, rng).deal);
        s.alignment = 2;
        TL_CHECK_EQ(tl::core::damage_shape_do_damage_to_character(s, v, graph, 1, 0.1f, rng).deal,
                    alignment == 2);
    }
}

TL_TEST(damage_shape_unit_type_by_isa) {
    // `UNITTYPE` through the hierarchy: `any` (0) takes everything, `MONSTER`
    // only what ISAs it.
    tl::formats::AdmSection root;
    root.name = "HIERACHY";
    tl::formats::AdmSection types;
    types.name = "UNITTYPES";
    auto make_type = [](const std::string& name, uint32_t id,
                        const std::vector<uint32_t>& ancestors) {
        tl::formats::AdmSection s;
        s.name = name;
        tl::formats::AdmRecord rid;
        rid.key = "ID";
        rid.type = tl::formats::AdmType::Integer;
        rid.integer = id;
        s.records.push_back(rid);
        for (size_t i = 0; i < ancestors.size(); ++i) {
            tl::formats::AdmRecord r;
            r.key = i == 0 ? "child" : "child" + std::to_string(i);
            r.type = tl::formats::AdmType::Integer;
            r.integer = ancestors[i];
            s.records.push_back(r);
        }
        return s;
    };
    types.children.push_back(make_type("any", 0, {}));
    types.children.push_back(make_type("Character", 25, {0}));
    types.children.push_back(make_type("Monster", 27, {25, 0}));
    types.children.push_back(make_type("Player", 28, {25, 0}));
    types.children.push_back(make_type("Grunt", 60, {27, 25, 0}));
    root.children.push_back(types);
    tl::core::UnitTypeHierarchy hierarchy;
    hierarchy.parse(root);

    tl::core::UnitBook units;
    tl::core::UnitStats grunt;
    grunt.unittype = "GRUNT";
    units.add("Grunt", grunt);

    const tl::core::StatBook graphs = damage_shape_graphs();
    for (const char* only : {"ANY", "MONSTER", "PLAYER", "NO SUCH TYPE"}) {
        CmpScene scene;
        CmpNode& node = add_damage_shape(scene, 1, "Trap", {0.0f, 0.0f, 0.0f}, 3.0f);
        node.fields[dsf::kUnitType] = text_field(only);
        CmpNode& monster = add_node(scene, tl::formats::cmp_type::kMonster, 2, "Grunt");
        pad_to(monster, 5);
        monster.fields[tl::formats::cmp_field::monster::kPosition] = vec3_field(1.0f, 0.0f, 0.0f);
        monster.fields[tl::formats::cmp_field::monster::kMonster] = text_field("Grunt");
        LevelScript script;
        script.set_units(&units);
        script.set_stat_graphs(&graphs);
        script.set_unit_types(&hierarchy);
        script.build(scene, {});
        QuestState quests;
        std::vector<ScriptEvent> events;
        script.level_activated(quests, events);
        const float before = health_of(script, 2);
        script.send_damage_shape_input(1, 0x02, quests, events);
        script.update(0.1f, {10.0f, 0.0f, 0.0f}, quests, events);
        const bool hit = health_of(script, 2) < before;
        // An unknown name is load index 0, `any`.
        TL_CHECK_EQ(hit, std::string(only) != "PLAYER");
    }
}

/// `doDamageToItem`: a breakable in an enabled shape is interacted with once,
/// unless a listener says no.
TL_TEST(damage_shape_reaches_an_item_once) {
    DamageShape s;
    s.enabled = true;
    s.dot = false;
    s.shape.shape = 2;
    s.shape.max_radius = fixed(2.0f);
    ShapeFrame frame;
    tl::core::DamageShapeItem barrel;
    barrel.id = 77;
    barrel.at = {1.0f, 0.0f, 0.0f};
    barrel.radius = 0.3f;
    tl::core::DamageShapeItem far = barrel;
    far.id = 78;
    far.at = {9.0f, 0.0f, 0.0f};

    TL_CHECK(!tl::core::damage_shape_do_damage_to_item(s, frame, far, true));
    TL_CHECK(!tl::core::damage_shape_do_damage_to_item(s, frame, barrel, false));
    TL_CHECK(tl::core::damage_shape_do_damage_to_item(s, frame, barrel, true));
    TL_CHECK(!tl::core::damage_shape_do_damage_to_item(s, frame, barrel, true));
    // Switched on again, the hit list starts over.
    tl::core::damage_shape_set_enabled(s, true);
    s.enabled = true;
    TL_CHECK(tl::core::damage_shape_do_damage_to_item(s, frame, barrel, true));

    struct Refuses final : tl::core::DamageShapeListener {
        int asked = 0;
        bool can_affect(const DamageShape&, const tl::core::DamageShapeUnit&) override {
            return true;
        }
        bool affect(const DamageShape&, const tl::core::DamageShapeUnit&) override { return false; }
        bool affect_item(const DamageShape&, const tl::core::DamageShapeItem&) override {
            ++asked;
            return false;
        }
    } refuses;
    DamageShape guarded = s;
    guarded.hit.clear();
    guarded.dot = true;
    guarded.listeners.push_back(&refuses);
    TL_CHECK(!tl::core::damage_shape_do_damage_to_item(guarded, frame, barrel, true));
    TL_CHECK_EQ(refuses.asked, 1);
    // A `DOT` shape keeps no list: the item is offered again every frame.
    TL_CHECK(!tl::core::damage_shape_do_damage_to_item(guarded, frame, barrel, true));
    TL_CHECK_EQ(refuses.asked, 2);
    s.enabled = false;
    TL_CHECK(!tl::core::damage_shape_do_damage_to_item(s, frame, barrel, true));
}

TL_TEST(damage_shape_target_corpse_hits_only_the_dead) {
    const tl::core::StatBook graphs = damage_shape_graphs();
    const tl::core::StatGraph* graph = graphs.find("DAMAGE_MONSTER");
    tl::core::RunicRandom rng(0);
    DamageShape s;
    s.enabled = true;
    s.dot = true;
    s.shape.shape = 2;
    s.shape.max_radius = fixed(2.0f);
    ShapeFrame frame;
    DamageShapeVictim living = victim_at(1);
    DamageShapeVictim dead = victim_at(2);
    dead.alive = false;
    TL_CHECK(tl::core::damage_shape_reaches(s, frame, false, true, true, {1.0f, 0.0f, 0.0f}, 0.3f));
    TL_CHECK(
        !tl::core::damage_shape_reaches(s, frame, false, true, false, {1.0f, 0.0f, 0.0f}, 0.3f));
    TL_CHECK(tl::core::damage_shape_do_damage_to_character(s, living, graph, 1, 0.1f, rng).deal);
    TL_CHECK(!tl::core::damage_shape_do_damage_to_character(s, dead, graph, 1, 0.1f, rng).deal);
    s.target_corpse = true;
    TL_CHECK(
        !tl::core::damage_shape_reaches(s, frame, false, true, true, {1.0f, 0.0f, 0.0f}, 0.3f));
    TL_CHECK(
        tl::core::damage_shape_reaches(s, frame, false, true, false, {1.0f, 0.0f, 0.0f}, 0.3f));
    TL_CHECK(!tl::core::damage_shape_do_damage_to_character(s, living, graph, 1, 0.1f, rng).deal);
    TL_CHECK(tl::core::damage_shape_do_damage_to_character(s, dead, graph, 1, 0.1f, rng).deal);
    // Hidden or untargetable characters are passed over; so is everyone while off.
    s.target_corpse = false;
    TL_CHECK(!tl::core::damage_shape_reaches(s, frame, true, true, true, {1.0f, 0.0f, 0.0f}, 0.3f));
    TL_CHECK(
        !tl::core::damage_shape_reaches(s, frame, false, false, true, {1.0f, 0.0f, 0.0f}, 0.3f));
    s.enabled = false;
    TL_CHECK(
        !tl::core::damage_shape_reaches(s, frame, false, true, true, {1.0f, 0.0f, 0.0f}, 0.3f));
}

TL_TEST(damage_shape_timer_and_loops) {
    // `LOOPS` off, `LOOP COUNT` 2, `TIMER` 1, `PAUSE TIMER` 0.5, stepped by hand
    // through `updateLevelObject`.
    DamageShape s;
    s.loops = false;
    s.loop_count = 2;
    s.timer = 1.0f;
    s.pause_timer = 0.5f;
    TL_CHECK(tl::core::damage_shape_set_enabled(s, true) == DamageShapeSwitch::Listen);
    TL_CHECK_EQ(s.loops_left, 2);
    auto tick = [&s](float dt) {
        if (tl::core::damage_shape_finished(s)) {
            tl::core::damage_shape_set_enabled(s, false);
            s.done = false;
        }
        s.ticked = true;
        if (s.enabled) tl::core::damage_shape_step(s, dt);
    };
    tick(0.5f);  // pause -0.5, time left 0.5
    TL_CHECK_EQ(s.percent, 0.5f);
    tick(0.5f);  // time left 0 -> 1 again, pause 0.5, one loop left
    TL_CHECK_EQ(s.loops_left, 1);
    TL_CHECK_EQ(s.pause_left, 0.5f);
    TL_CHECK_EQ(s.percent, 0.0f);
    tick(0.5f);  // the pause runs to 0: not below it, nothing moves
    TL_CHECK_EQ(s.percent, 0.0f);
    TL_CHECK_EQ(s.time_left, 1.0f);
    tick(0.25f);  // time left 0.75
    TL_CHECK_EQ(s.percent, 0.25f);
    tick(0.75f);  // the last loop: done, time left 0, a pause of 0.5 to run
    TL_CHECK(s.done);
    TL_CHECK_EQ(s.loops_left, 0);
    TL_CHECK_EQ(s.percent, 1.0f);
    TL_CHECK(s.enabled);
    tick(0.25f);
    TL_CHECK(s.enabled);  // pause 0.25 left
    tick(0.25f);
    TL_CHECK(s.enabled);  // pause 0: finished on the next update
    tick(0.1f);
    TL_CHECK(!s.enabled);
    TL_CHECK(!s.done);

    // `LOOPS`: round and round, never done.
    DamageShape forever;
    forever.loops = true;
    forever.timer = 1.0f;
    tl::core::damage_shape_set_enabled(forever, true);
    for (int i = 0; i < 25; ++i) {
        forever.ticked = true;
        tl::core::damage_shape_step(forever, 0.25f);
        TL_CHECK(!forever.done);
    }
    TL_CHECK_EQ(forever.loops_left, 1);

    // A negative `LOOP COUNT` without `LOOPS` leaves no loops: nothing ever runs.
    DamageShape stuck;
    stuck.loops = false;
    stuck.loop_count = -1;
    stuck.timer = 1.0f;
    TL_CHECK(tl::core::damage_shape_set_enabled(stuck, true) == DamageShapeSwitch::Listen);
    tl::core::damage_shape_step(stuck, 5.0f);
    TL_CHECK_EQ(stuck.time_left, 1.0f);
    TL_CHECK(!stuck.done);
}

TL_TEST(damage_shape_enabling_clears_the_hit_list_and_disabling_keeps_it) {
    DamageShape s;
    s.hit.push_back({nullptr, 5});
    TL_CHECK(tl::core::damage_shape_set_enabled(s, false) == DamageShapeSwitch::Stop);
    TL_CHECK_EQ(s.hit.size(), size_t{1});
    // No time and no loops left: the instant pass, whatever `LOOPS` says.
    s.loop_count = 0;
    s.timer = 0.0f;
    s.loops = true;
    TL_CHECK(tl::core::damage_shape_set_enabled(s, true) == DamageShapeSwitch::Instant);
    TL_CHECK(s.hit.empty());
    s.timer = 0.002f;
    TL_CHECK(tl::core::damage_shape_set_enabled(s, true) == DamageShapeSwitch::Listen);
}

TL_TEST(damage_shape_listener_can_take_over_the_damage) {
    const tl::core::StatBook graphs = damage_shape_graphs();
    const tl::core::StatGraph* graph = graphs.find("DAMAGE_MONSTER");
    tl::core::RunicRandom rng(0);

    // Declines: no own damage, never asked to act, nothing recorded.
    {
        DamageShape s;
        s.dot = false;
        RecordingListener l;
        l.allow = false;
        s.listeners.push_back(&l);
        const DamageShapeHit hit =
            tl::core::damage_shape_do_damage_to_character(s, victim_at(1), graph, 1, 0.1f, rng);
        TL_CHECK(!hit.deal);
        TL_CHECK_EQ(l.acted, 0);
        TL_CHECK(s.hit.empty());
    }
    // Acts and takes over: no own damage, but the character is on the list.
    {
        DamageShape s;
        s.dot = false;
        RecordingListener l;
        l.take_over = true;
        s.listeners.push_back(&l);
        const DamageShapeHit hit =
            tl::core::damage_shape_do_damage_to_character(s, victim_at(1), graph, 1, 0.1f, rng);
        TL_CHECK(!hit.deal);
        TL_CHECK_EQ(s.hit.size(), size_t{1});
    }
    // Acts and leaves it: own damage too, recorded once (by the listener branch).
    {
        DamageShape s;
        s.dot = false;
        RecordingListener l;
        s.listeners.push_back(&l);
        const DamageShapeHit hit =
            tl::core::damage_shape_do_damage_to_character(s, victim_at(1), graph, 1, 0.1f, rng);
        TL_CHECK(hit.deal);
        TL_CHECK(!hit.record);
        TL_CHECK_EQ(s.hit.size(), size_t{1});
    }
    // One declines, the next acts: every listener is asked, own damage off.
    {
        DamageShape s;
        s.dot = true;
        RecordingListener no;
        no.allow = false;
        RecordingListener yes;
        s.listeners.push_back(&no);
        s.listeners.push_back(&yes);
        const DamageShapeHit hit =
            tl::core::damage_shape_do_damage_to_character(s, victim_at(1), graph, 1, 0.1f, rng);
        TL_CHECK(!hit.deal);
        TL_CHECK_EQ(yes.acted, 1);
        TL_CHECK(s.hit.empty());  // DOT keeps no list
    }
}

TL_TEST(damage_shape_enable_input_starts_it) {
    const tl::core::StatBook graphs = damage_shape_graphs();
    CmpScene scene;
    add_damage_shape(scene, 1, "Trap", {0.0f, 0.0f, 0.0f}, 3.0f);
    add_target(scene, 2, {1.0f, 0.0f, 0.0f});
    LevelScript script;
    script.set_unit_types(&basic_unit_types());
    script.set_stat_graphs(&graphs);
    script.build(scene, {});
    QuestState quests;
    std::vector<ScriptEvent> events;
    script.level_activated(quests, events);
    // Near enough to keep the target in active range (`+0x199`), outside the shape.
    const Vec3 away{6.0f, 0.0f, 0.0f};
    const float full = health_of(script, 2);

    script.update(0.1f, away, quests, events);
    TL_CHECK_EQ(health_of(script, 2), full);
    TL_CHECK(!script.node_enabled(1));

    TL_CHECK(script.send_damage_shape_input(1, 0x02, quests, events));
    TL_CHECK(script.node_enabled(1));
    // The level's character pass runs before the descriptors: hit at percent 1,
    // 40 x 50 / 100 = 20. The timer then runs out (no time, one loop).
    script.update(0.1f, away, quests, events);
    TL_CHECK_EQ(health_of(script, 2), full - 20.0f);
    // Once only, then the shape turns itself off.
    script.update(0.1f, away, quests, events);
    TL_CHECK_EQ(health_of(script, 2), full - 20.0f);
    TL_CHECK(!script.node_enabled(1));

    // On again: a fresh hit list.
    script.send_damage_shape_input(1, 0x02, quests, events);
    script.update(0.1f, away, quests, events);
    TL_CHECK_EQ(health_of(script, 2), full - 40.0f);
    script.send_damage_shape_input(1, 0x03, quests, events);
    TL_CHECK(!script.node_enabled(1));
}

TL_TEST(damage_shape_with_no_time_hits_at_once_and_turns_off) {
    const tl::core::StatBook graphs = damage_shape_graphs();
    CmpScene scene;
    CmpNode& node = add_damage_shape(scene, 1, "Burst", {0.0f, 0.0f, 0.0f}, 3.0f);
    node.fields[dsf::kLoopCount] = uint_field(0);
    add_target(scene, 2, {1.0f, 0.0f, 0.0f});
    add_target(scene, 3, {9.0f, 0.0f, 0.0f});
    LevelScript script;
    script.set_unit_types(&basic_unit_types());
    script.set_stat_graphs(&graphs);
    script.build(scene, {});
    QuestState quests;
    std::vector<ScriptEvent> events;
    script.level_activated(quests, events);
    const float full = health_of(script, 2);
    script.send_damage_shape_input(1, 0x02, quests, events);
    TL_CHECK_EQ(health_of(script, 2), full - 20.0f);
    TL_CHECK_EQ(health_of(script, 3), full);
    TL_CHECK(!script.node_enabled(1));
    script.update(0.1f, {50.0f, 0.0f, 50.0f}, quests, events);
    TL_CHECK_EQ(health_of(script, 2), full - 20.0f);
}

TL_TEST(damage_shape_on_the_player_is_a_strike_event) {
    const tl::core::StatBook graphs = damage_shape_graphs();
    CmpScene scene;
    CmpNode& node = add_damage_shape(scene, 1, "Spikes", {0.0f, 0.0f, 0.0f}, 2.0f);
    node.fields[dsf::kDot] = uint_field(1);
    LevelScript script;
    script.set_unit_types(&basic_unit_types());
    script.set_stat_graphs(&graphs);
    script.build(scene, {});
    QuestState quests;
    std::vector<ScriptEvent> events;
    script.level_activated(quests, events);
    script.send_damage_shape_input(1, 0x02, quests, events);
    events.clear();
    script.update(0.25f, {0.0f, 0.0f, 1.0f}, quests, events);
    size_t strikes = 0;
    for (const ScriptEvent& e : events) {
        if (e.action != ScriptAction::Strike) continue;
        ++strikes;
        TL_CHECK_EQ(e.target_node, 0);
        TL_CHECK_EQ(e.node_id, 1);
        TL_CHECK_EQ(e.amount, 5.0f);  // 20 x 0.25, DOT
    }
    TL_CHECK_EQ(strikes, size_t{1});
    // A dead player is passed over.
    script.set_player_alive(false);
    events.clear();
    script.update(0.25f, {0.0f, 0.0f, 1.0f}, quests, events);
    for (const ScriptEvent& e : events) TL_CHECK(e.action != ScriptAction::Strike);
}

TL_TEST(damage_shape_listeners_are_called_newest_first) {
    // `addCharacterUpdateListener` puts a new listener at the front.
    const tl::core::StatBook graphs = damage_shape_graphs();
    CmpScene scene;
    add_damage_shape(scene, 1, "First", {0.0f, 0.0f, 0.0f}, 3.0f);
    add_damage_shape(scene, 2, "Second", {0.0f, 0.0f, 0.0f}, 3.0f);
    add_target(scene, 5, {1.0f, 0.0f, 0.0f});
    LevelScript script;
    script.set_unit_types(&basic_unit_types());
    script.set_stat_graphs(&graphs);
    script.build(scene, {});
    std::vector<int64_t> order;
    RecordingListener one;
    one.order = &order;
    one.tag = 1;
    RecordingListener two;
    two.order = &order;
    two.tag = 2;
    TL_CHECK(script.damage_shape_add_listener(1, &one));
    TL_CHECK(script.damage_shape_add_listener(2, &two));
    TL_CHECK(!script.damage_shape_add_listener(5, &two));
    QuestState quests;
    std::vector<ScriptEvent> events;
    script.level_activated(quests, events);
    script.send_damage_shape_input(1, 0x02, quests, events);
    script.send_damage_shape_input(2, 0x02, quests, events);
    script.update(0.1f, {10.0f, 0.0f, 0.0f}, quests, events);
    TL_CHECK_EQ(order.size(), size_t{2});
    TL_CHECK_EQ(order[0], 2);
    TL_CHECK_EQ(order[1], 1);
}

TL_TEST(a_skill_events_corpse_shape_reaches_a_unit_killed_in_play) {
    // `CCharacter::die` leaves `TARGETABLE` alone, so a `TARGET_CORPSES` shape
    // (`addListeners`) still meets the body.
    const tl::core::StatBook graphs = damage_shape_graphs();
    CmpScene scene;
    add_damage_shape(scene, 1, "Ring", {0.0f, 0.0f, 0.0f}, 3.0f);
    add_target(scene, 2, {1.0f, 0.0f, 0.0f});
    add_target(scene, 3, {2.0f, 0.0f, 0.0f});
    LevelScript script;
    script.set_unit_types(&basic_unit_types());
    script.set_stat_graphs(&graphs);
    script.build(scene, {});
    QuestState quests;
    std::vector<ScriptEvent> events;
    script.level_activated(quests, events);
    TL_CHECK(script.damage_shape_take_event(1, /*target_corpses=*/true, /*max_units_hit=*/1));
    TL_CHECK(!script.damage_shape_take_event(2, true, 1));  // not a shape
    TL_CHECK(script.damage_shape(1)->target_corpse);
    TL_CHECK_EQ(script.damage_shape(1)->max_units_to_hit, 1);
    RecordingListener ring;
    TL_CHECK(script.damage_shape_add_listener(1, &ring));

    // The living are not offered to a corpse shape.
    script.send_damage_shape_input(1, 0x02, quests, events);
    script.update(0.1f, {10.0f, 0.0f, 0.0f}, quests, events);
    TL_CHECK_EQ(ring.asked, 0);
    script.send_damage_shape_input(1, 0x03, quests, events);

    script.damage_unit(2, 1.0e6f, LevelScript::kNoAttacker, quests, events);
    script.damage_unit(3, 1.0e6f, LevelScript::kNoAttacker, quests, events);
    TL_CHECK(script.unit_is_dead(2));
    TL_CHECK(script.unit_is_dead(3));
    script.send_damage_shape_input(1, 0x02, quests, events);
    script.update(0.1f, {10.0f, 0.0f, 0.0f}, quests, events);
    script.update(0.1f, {10.0f, 0.0f, 0.0f}, quests, events);
    TL_CHECK_EQ(ring.asked, 1);
    TL_CHECK_EQ(ring.acted, 1);
    TL_CHECK_EQ(script.damage_shape(1)->hit.size(), size_t{1});
}

TL_TEST(a_hidden_corpse_is_passed_over) {
    // `SET VISIBLE` 0 on the body (`applyInstantEffect` 0x70): `+0x70d`, which
    // `characterUpdatedInLevel` refuses.
    const tl::core::StatBook graphs = damage_shape_graphs();
    CmpScene scene;
    add_damage_shape(scene, 1, "Ring", {0.0f, 0.0f, 0.0f}, 3.0f);
    add_target(scene, 2, {1.0f, 0.0f, 0.0f});
    LevelScript script;
    script.set_stat_graphs(&graphs);
    script.build(scene, {});
    QuestState quests;
    std::vector<ScriptEvent> events;
    script.level_activated(quests, events);
    script.damage_shape_take_event(1, true, -1);
    RecordingListener ring;
    script.damage_shape_add_listener(1, &ring);
    script.damage_unit(2, 1.0e6f, LevelScript::kNoAttacker, quests, events);
    events.clear();
    TL_CHECK(script.set_unit_hidden(2, true, true, events));
    // At once: the fade written to nothing, then the node hidden.
    TL_CHECK_EQ(events.size(), size_t{2});
    TL_CHECK(events[0].action == ScriptAction::Opacity && events[0].amount == 0.0f);
    TL_CHECK(events[1].action == ScriptAction::SetVisible && !events[1].on);
    script.send_damage_shape_input(1, 0x02, quests, events);
    script.update(0.1f, {10.0f, 0.0f, 0.0f}, quests, events);
    TL_CHECK_EQ(ring.asked, 0);
}

TL_TEST(damage_shape_hit_does_not_turn_a_monster_on_the_player) {
    // No attacker: `applyDamage` skips `reactToDamage`.
    const tl::core::StatBook graphs = damage_shape_graphs();
    auto hunts = [](const std::vector<ScriptEvent>& events) {
        for (const ScriptEvent& e : events) {
            if (e.action == ScriptAction::Hunt && e.on) return true;
        }
        return false;
    };
    CmpScene scene;
    add_damage_shape(scene, 1, "Trap", {0.0f, 0.0f, 0.0f}, 3.0f);
    add_target(scene, 2, {1.0f, 0.0f, 0.0f});
    LevelScript script;
    script.set_stat_graphs(&graphs);
    script.build(scene, {});
    QuestState quests;
    std::vector<ScriptEvent> events;
    script.level_activated(quests, events);
    script.send_damage_shape_input(1, 0x02, quests, events);
    events.clear();
    script.update(0.1f, {30.0f, 0.0f, 30.0f}, quests, events);
    TL_CHECK(!hunts(events));

    // The same blow from the player does (a blow that struck).
    events.clear();
    script.damage_unit(2, 1.0f, 0, quests, events, nullptr, nullptr, /*struck=*/true);
    TL_CHECK(hunts(events));
}

TL_TEST(damage_shape_of_nothing_lands_nothing) {
    // `MIN` = `MAX` = 0, a shape left for a skill's listener: the effect's value
    // 0 reaches `modifyHP(0)` and returns; no blow on the player.
    const tl::core::StatBook graphs = damage_shape_graphs();
    CmpScene scene;
    CmpNode& node = add_damage_shape(scene, 1, "Fear", {0.0f, 0.0f, 0.0f}, 2.0f);
    node.fields[dsf::kMin] = float_field(0.0f);
    node.fields[dsf::kMax] = float_field(0.0f);
    LevelScript script;
    script.set_unit_types(&basic_unit_types());
    script.set_stat_graphs(&graphs);
    script.build(scene, {});
    QuestState quests;
    std::vector<ScriptEvent> events;
    script.level_activated(quests, events);
    script.send_damage_shape_input(1, 0x02, quests, events);
    events.clear();
    script.update(0.1f, {0.0f, 0.0f, 1.0f}, quests, events);
    for (const ScriptEvent& e : events) TL_CHECK(e.action != ScriptAction::Strike);
    // Still listed: the next pass passes the player over.
    TL_CHECK(script.damage_shape(1) != nullptr);
    TL_CHECK_EQ(script.damage_shape(1)->hit.size(), size_t{1});
}

// Only the level load reseeds: building a nested layout's machine leaves both generators as
// they were.
TL_TEST(building_a_nested_script_leaves_the_generators_running) {
    tl::test::seed_generators();
    const float first = tl::core::game_random().between(0.0f, 1.0f);
    const float first_volatile = tl::core::volatile_random().between(0.0f, 1.0f);

    LevelScript death_layout;
    death_layout.build(CmpScene{}, {});

    TL_CHECK(tl::core::game_random().between(0.0f, 1.0f) != first);
    TL_CHECK(tl::core::volatile_random().between(0.0f, 1.0f) != first_volatile);

    tl::test::seed_generators();
    TL_CHECK(tl::core::game_random().between(0.0f, 1.0f) == first);
}

namespace {

bool same_state(const tl::formats::LogicNodeState& a, const tl::formats::LogicNodeState& b) {
    return a.flags == b.flags && a.i18 == b.i18 && a.i1c == b.i1c && a.f20 == b.f20 &&
           a.f24 == b.f24 && a.descriptor == b.descriptor && a.scene == b.scene &&
           a.parent_id == b.parent_id && a.hierarchy == b.hierarchy && a.object_id == b.object_id &&
           a.position.x == b.position.x && a.position.y == b.position.y &&
           a.position.z == b.position.z;
}

CmpScene level_state_scene() {
    CmpScene scene;
    CmpNode& counter = add_node(scene, tl::formats::cmp_type::kCounter, 1, "Counter3");
    counter.fields.push_back(uint_field(1));  // 0 ENABLED
    counter.fields.push_back(uint_field(3));  // 1 EQUALS VALUE
    counter.fields.push_back(uint_field(0));  // 2 STARTING VALUE
    counter.fields.push_back(text_field(""));
    CmpNode& sphere = add_node(scene, tl::formats::cmp_type::kPlayerSphereTrigger, 4, "Sphere");
    sphere.fields.push_back(vec3_field(0.0f, 0.0f, 0.0f));
    sphere.fields.push_back(uint_field(1));
    sphere.fields.push_back(float_field(3.0f));
    add_node(scene, tl::formats::cmp_type::kMarker, 5, "Marker");
    add_node(scene, tl::formats::cmp_type::kGroup, 6, "Group");
    add_node(scene, 41, 7, "Teleport");
    return scene;
}

}  // namespace

// `storeLevelSavedState` then `restoreLevelState` round-trip, in the store's descriptor order;
// another scene's or link hash's state is not restored.
TL_TEST(a_stored_level_state_restores_into_a_fresh_script) {
    using tl::formats::LogicNodeState;
    const CmpScene scene = level_state_scene();
    LevelScript first;
    first.build(scene, {});
    std::vector<LogicNodeState> states;
    first.store_logic_states(3, 77, states);
    TL_CHECK_EQ(states.size(), size_t{5});
    TL_CHECK_EQ(states[0].descriptor, std::string("Player Sphere Trigger"));
    TL_CHECK_EQ(states[1].descriptor, std::string("Property Node"));
    TL_CHECK_EQ(states[2].descriptor, std::string("Group"));
    TL_CHECK_EQ(states[3].descriptor, std::string("Counter"));
    TL_CHECK_EQ(states[4].descriptor, std::string("Teleport"));

    // What a play-through changes.
    states[0].flags[2] = 1;  // triggered once
    states[1].flags[0] = 0;  // marker off, and moved
    states[1].position = {1.0f, 2.0f, 3.0f};
    states[2].flags[1] = 0;  // group hidden
    states[3].f20 = 2.0f;    // counter at 2
    states[4].flags[0] = 0;  // teleport off

    LevelScript second;
    second.build(scene, {});
    QuestState quests;
    std::vector<ScriptEvent> events;
    second.restore_logic_states(3, 77, states, quests, events);
    std::vector<LogicNodeState> again;
    second.store_logic_states(3, 77, again);
    TL_CHECK_EQ(again.size(), states.size());
    for (size_t i = 0; i < states.size(); ++i) TL_CHECK(same_state(again[i], states[i]));

    // The marker and the group say so when restore changes them.
    bool marker_off = false, group_hidden = false;
    for (const ScriptEvent& e : events) {
        if (e.action == ScriptAction::SetEnabled && e.node_id == 5 && !e.on) marker_off = true;
        if (e.action == ScriptAction::SetVisible && e.node_id == 6 && !e.on) group_hidden = true;
    }
    TL_CHECK(marker_off);
    TL_CHECK(group_hidden);

    LevelScript other;
    other.build(scene, {});
    std::vector<ScriptEvent> ignored;
    other.restore_logic_states(4, 77, states, quests, ignored);
    other.restore_logic_states(3, 78, states, quests, ignored);
    std::vector<LogicNodeState> untouched;
    other.store_logic_states(3, 77, untouched);
    TL_CHECK_EQ(untouched[3].f20, 0.0f);
    TL_CHECK_EQ(untouched[4].flags[0], uint8_t{1});
}

// ------------------------------------------- the level's units, across rooms

namespace {

/// A flat 100 x 100 floor with its grids, for the unit tests below.
struct FlatLevel {
    tl::core::CollisionWorld world;
    tl::core::PassabilityMap grid;
    tl::core::NavGrid nav;
    tl::core::UnitBook units;
    FlatLevel() {
        world.add_triangle({-50.0f, 0.0f, -50.0f}, {-50.0f, 0.0f, 50.0f}, {50.0f, 0.0f, 50.0f});
        world.add_triangle({-50.0f, 0.0f, -50.0f}, {50.0f, 0.0f, 50.0f}, {50.0f, 0.0f, -50.0f});
        world.build();
        grid.bake(world, nullptr);
        nav.build(grid);
        tl::core::UnitStats merc;
        merc.radius = 0.3f;
        merc.alignment = tl::core::UnitStats::Alignment::Friendly;
        merc.movement.walking_speed = 1.9f;
        merc.movement.running_speed = 6.5f;
        units.add("MercPet", merc);
    }
    void wire(LevelScript& machine, std::vector<LevelScript*>& list) {
        machine.set_units(&units);
        machine.set_navigation(&nav);
        machine.set_collision(&world);
        machine.set_passability(&grid);
        machine.set_level_machines(&list);
    }
};

}  // namespace

// A room's `Pathing` walks a unit another machine runs: `CPathController::setEnabled` uses
// `CLevel::getBaseUnitsByDataGroup`; a pet follows a path first
// (`CMonster::idleAI`).
TL_TEST(a_room_path_walks_the_players_pet_from_another_machine_to_its_end) {
    FlatLevel flat;
    std::vector<LevelScript*> list;
    LevelScript level;
    LevelScript room;
    list = {&level, &room};
    flat.wire(level, list);
    flat.wire(room, list);

    namespace p = tl::formats::cmp_field::pathing;
    CmpScene scene;
    CmpNode& path = add_node(scene, tl::formats::cmp_type::kPathing, 1, "MoveMercToPosition");
    pad_to(path, p::kUnit + 1);
    path.fields[p::kPosition] = vec3_field(0.0f, 0.0f, 0.0f);
    path.fields[p::kPoints] = vec3_field(10.0f, 0.0f, 0.0f);
    path.fields[p::kNumberOfPoints] = uint_field(1);
    path.fields[p::kEnabled] = uint_field(0);
    path.fields[p::kPathName] = text_field("MoveMerc");
    path.fields[p::kRun] = uint_field(1);
    path.fields[p::kGroup] = text_field("MONSTERS");
    path.fields[p::kUnit] = text_field("MercPet");
    CmpNode& quest = add_node(scene, tl::formats::cmp_type::kQuestController, 2, "Arrived");
    pad_to(quest, tl::formats::cmp_field::quest_controller::kQuest);
    quest.fields.push_back(text_field("BRINKARRIVED"));
    CmpNode& group = add_node(scene, tl::formats::cmp_type::kLogicGroup, 3, "Logic");
    group.fields.push_back(uint_field(0));
    tl::formats::CmpLogicGroup wiring;
    wiring.node_id = 3;
    wiring.objects.push_back({0, 3, 0.0f, 0.0f, {{1, "Level Activated", "Enable"}}});
    wiring.objects.push_back({1, 1, 0.0f, 0.0f, {{2, "End of Path Reached", "Force Complete"}}});
    wiring.objects.push_back({2, 2, 0.0f, 0.0f, {}});
    scene.logic_groups.push_back(std::move(wiring));

    tl::test::seed_generators();
    level.build(CmpScene{}, {});
    room.build(scene, {});
    level.add_spawned_unit(-2001, "MercPet", tl::core::Vec3{-5.0f, 0.0f, 0.0f}, false);
    QuestState quests;
    std::vector<ScriptEvent> events;
    TL_CHECK(level.make_player_pet(-2001, quests, events));

    // The player stands away from the mark: a pet keeping station would stay here.
    const tl::core::Vec3 player{-8.0f, 0.0f, 6.0f};
    room.level_activated(quests, events);
    for (int i = 0; i < 60 * 12 && !quests.is_complete("BRINKARRIVED"); ++i) {
        events.clear();
        level.update(1.0f / 60.0f, player, quests, events);
        room.update(1.0f / 60.0f, player, quests, events);
    }
    TL_CHECK(quests.is_complete("BRINKARRIVED"));
    tl::core::Vec3 at;
    TL_CHECK(level.unit_position(-2001, at));
    TL_CHECK(std::fabs(at.x - 10.0f) < 1.5f);
}

namespace {

// A `Pathing` node for the scene-activation tests: `ENABLED` true (written out,
// since `pad_to`'s zeros would read as false), one waypoint, no wires.
CmpNode& add_enabled_path(CmpScene& scene, int64_t id, const char* unit, tl::core::Vec3 at,
                          tl::core::Vec3 point, bool to_player) {
    namespace p = tl::formats::cmp_field::pathing;
    CmpNode& path = add_node(scene, tl::formats::cmp_type::kPathing, id, "Path");
    pad_to(path, p::kUnit + 1);
    path.fields[p::kPosition] = vec3_field(at.x, at.y, at.z);
    path.fields[p::kPoints] = vec3_field(point.x, point.y, point.z);
    path.fields[p::kNumberOfPoints] = uint_field(1);
    path.fields[p::kPathName] = text_field("Walk");
    path.fields[p::kRun] = uint_field(1);
    path.fields[p::kEnabled] = uint_field(1);
    path.fields[p::kPathToPlayer] = uint_field(to_player ? 1 : 0);
    path.fields[p::kUnit] = text_field(unit);
    return path;
}

}  // namespace

// An enabled path is handed over at scene activation, with or without `ENABLE ON START`
// (`CPathControllerDescriptor::descriptorSceneActivated`).
TL_TEST(an_enabled_path_walks_its_unit_from_scene_activation) {
    FlatLevel flat;
    std::vector<LevelScript*> list;
    LevelScript level;
    LevelScript room;
    list = {&level, &room};
    flat.wire(level, list);
    flat.wire(room, list);

    CmpScene scene;
    add_enabled_path(scene, 1, "FirstHenchmen", {0.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}, false);

    tl::test::seed_generators();
    level.build(CmpScene{}, {});
    room.build(scene, {});
    level.add_spawned_unit(-2001, "FirstHenchmen", tl::core::Vec3{-5.0f, 0.0f, 0.0f}, false);
    QuestState quests;
    std::vector<ScriptEvent> events;
    room.level_activated(quests, events);

    const tl::core::Vec3 player{-8.0f, 0.0f, 6.0f};
    for (int i = 0; i < 60 * 12; ++i) {
        events.clear();
        level.update(1.0f / 60.0f, player, quests, events);
        room.update(1.0f / 60.0f, player, quests, events);
    }
    tl::core::Vec3 at;
    TL_CHECK(level.unit_position(-2001, at));
    TL_CHECK(std::fabs(at.x - 10.0f) < 1.5f);
}

// `CMonster::setPathToFollow` raises the search cap to 800; `CCharacter::die`
// -> `setAIState(5)` ends a non-looping path.
TL_TEST(a_path_raises_the_search_cap_and_a_death_ends_it) {
    FlatLevel flat;
    std::vector<LevelScript*> list;
    LevelScript level;
    LevelScript room;
    list = {&level, &room};
    flat.wire(level, list);
    flat.wire(room, list);

    CmpScene scene;
    add_enabled_path(scene, 1, "FirstHenchmen", {0.0f, 0.0f, 0.0f}, {30.0f, 0.0f, 0.0f}, false);
    CmpNode& quest = add_node(scene, tl::formats::cmp_type::kQuestController, 2, "Ended");
    pad_to(quest, tl::formats::cmp_field::quest_controller::kQuest);
    quest.fields.push_back(text_field("PATHENDED"));
    CmpNode& group = add_node(scene, tl::formats::cmp_type::kLogicGroup, 3, "Logic");
    group.fields.push_back(uint_field(0));
    tl::formats::CmpLogicGroup wiring;
    wiring.node_id = 3;
    wiring.objects.push_back({1, 1, 0.0f, 0.0f, {{2, "End of Path Reached", "Force Complete"}}});
    wiring.objects.push_back({2, 2, 0.0f, 0.0f, {}});
    scene.logic_groups.push_back(std::move(wiring));

    tl::test::seed_generators();
    level.build(CmpScene{}, {});
    room.build(scene, {});
    level.add_spawned_unit(-2001, "FirstHenchmen", tl::core::Vec3{-5.0f, 0.0f, 0.0f}, false);
    QuestState quests;
    std::vector<ScriptEvent> events;
    TL_CHECK_EQ(level.unit_search_cap(-2001), 150);
    room.level_activated(quests, events);
    TL_CHECK_EQ(level.unit_search_cap(-2001), 800);
    TL_CHECK(!quests.is_complete("PATHENDED"));

    level.unit_killed(-2001, quests, events);
    for (int i = 0; i < 3; ++i) {
        events.clear();
        level.update(1.0f / 60.0f, {}, quests, events);
        room.update(1.0f / 60.0f, {}, quests, events);
    }
    TL_CHECK(quests.is_complete("PATHENDED"));
    TL_CHECK_EQ(level.unit_search_cap(-2001), 150);
}

// A walk-to-player path stands on its unit (`CPathController::setEnabled`), so
// `caculatePointInFrontOfPlayer` picks the player's near side.
TL_TEST(a_walk_to_player_path_brings_its_unit_to_the_near_side_of_the_player) {
    FlatLevel flat;
    std::vector<LevelScript*> list;
    LevelScript level;
    LevelScript room;
    list = {&level, &room};
    flat.wire(level, list);
    flat.wire(room, list);

    CmpScene scene;
    add_enabled_path(scene, 1, "seer", {-20.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, true);

    tl::test::seed_generators();
    level.build(CmpScene{}, {});
    room.build(scene, {});
    level.add_spawned_unit(-2001, "seer", tl::core::Vec3{8.0f, 0.0f, 0.0f}, false);
    QuestState quests;
    std::vector<ScriptEvent> events;
    const tl::core::Vec3 player{0.0f, 0.0f, 0.0f};
    for (int i = 0; i < 2; ++i) {
        events.clear();
        level.update(1.0f / 60.0f, player, quests, events);
        room.update(1.0f / 60.0f, player, quests, events);
    }
    room.level_activated(quests, events);
    for (int i = 0; i < 60 * 8; ++i) {
        events.clear();
        level.update(1.0f / 60.0f, player, quests, events);
        room.update(1.0f / 60.0f, player, quests, events);
    }
    tl::core::Vec3 at;
    TL_CHECK(level.unit_position(-2001, at));
    TL_CHECK(at.x > 0.0f);
    TL_CHECK(std::fabs(at.z) < 1.5f);
    TL_CHECK(at.x < 4.0f);
}

// `Skill Controller` on `PLAYERS` (`CSkillController::update`) casts only a skill
// the player has, on the next update.
TL_TEST(a_skill_controller_casts_on_the_player_only_a_skill_the_player_has) {
    namespace f = tl::formats::cmp_field::skill_controller;
    CmpScene scene;
    CmpNode& controller = add_node(scene, tl::formats::cmp_type::kSkillController, 1, "KillPet");
    pad_to(controller, f::kSkill + 1);
    controller.fields[f::kSkillLevel] = uint_field(1);
    controller.fields[f::kGroup] = text_field("PLAYERS");
    controller.fields[f::kUnit] = text_field("");
    controller.fields[f::kSkill] = text_field("DESTROY MERC PET");
    CmpNode& group = add_node(scene, tl::formats::cmp_type::kLogicGroup, 2, "Logic");
    group.fields.push_back(uint_field(0));
    tl::formats::CmpLogicGroup wiring;
    wiring.node_id = 2;
    wiring.objects.push_back({0, 2, 0.0f, 0.0f, {{1, "Level Activated", "Start Skill"}}});
    wiring.objects.push_back({1, 1, 0.0f, 0.0f, {}});
    scene.logic_groups.push_back(std::move(wiring));

    for (const bool has : {false, true}) {
        LevelScript script;
        std::vector<std::pair<std::string, int32_t>> casts;
        LevelScript::PlayerSkills skills;
        skills.knows = [has](const std::string&) { return has; };
        skills.updating = [](const std::string&) { return false; };
        skills.cast = [&casts](const std::string& name, int32_t level) {
            casts.emplace_back(name, level);
        };
        script.set_player_skills(skills);
        script.build(scene, {});
        QuestState quests;
        std::vector<ScriptEvent> events;
        script.level_activated(quests, events);
        TL_CHECK(casts.empty());  // the start waits for the update
        script.update(1.0f / 60.0f, {}, quests, events);
        script.update(1.0f / 60.0f, {}, quests, events);
        if (has) {
            TL_CHECK_EQ(casts.size(), size_t{1});
            TL_CHECK(casts.front().first == "DESTROY MERC PET");
            // `SKILL LEVEL` is a flag whose setter stores a word from one byte: 1 is
            // 0x01000000. Kept.
            TL_CHECK_EQ(casts.front().second, 0x01000000);
        } else {
            TL_CHECK(casts.empty());
        }
    }
}

// `DESUMMON MONSTER` (case 0x43 -> `CCharacter::removePet`): the player's pets of
// that name leave and die.
TL_TEST(desummon_monster_kills_the_players_pets_of_that_name_only) {
    FlatLevel flat;
    tl::core::UnitStats other;
    other.radius = 0.3f;
    other.alignment = tl::core::UnitStats::Alignment::Friendly;
    other.movement.walking_speed = 1.0f;
    flat.units.add("Golem", other);
    std::vector<LevelScript*> list;
    LevelScript level;
    list = {&level};
    flat.wire(level, list);
    tl::test::seed_generators();
    level.build(CmpScene{}, {});
    level.add_spawned_unit(-1, "MercPet", tl::core::Vec3{1.0f, 0.0f, 0.0f}, false);
    level.add_spawned_unit(-2, "Golem", tl::core::Vec3{2.0f, 0.0f, 0.0f}, false);
    QuestState quests;
    std::vector<ScriptEvent> events;
    TL_CHECK(level.make_player_pet(-1, quests, events));
    TL_CHECK(level.make_player_pet(-2, quests, events));
    events.clear();
    TL_CHECK_EQ(level.desummon_player_pets("mercpet", quests, events), 1);
    TL_CHECK(level.unit_is_dead(-1));
    TL_CHECK(!level.unit_is_dead(-2));
    TL_CHECK_EQ(level.player_pets().size(), size_t{1});
    bool killed = false;
    for (const ScriptEvent& e : events) {
        killed = killed || (e.action == ScriptAction::Kill && e.node_id == -1);
    }
    TL_CHECK(killed);
}

/// A mimic (`CCharacter::unitInit`, `modifyHP`) wakes only on a direct blow
/// (`CMonster::reactToDamage`) or the player's `attack`.
TL_TEST(a_mimic_lies_in_wait_until_it_is_struck) {
    tl::formats::AdmSection root;
    root.name = "HIERACHY";
    tl::formats::AdmSection types;
    types.name = "UNITTYPES";
    auto make_type = [](const std::string& name, uint32_t id,
                        const std::vector<uint32_t>& ancestors) {
        tl::formats::AdmSection s;
        s.name = name;
        tl::formats::AdmRecord rid;
        rid.key = "ID";
        rid.type = tl::formats::AdmType::Integer;
        rid.integer = id;
        s.records.push_back(rid);
        for (size_t i = 0; i < ancestors.size(); ++i) {
            tl::formats::AdmRecord r;
            r.key = i == 0 ? "child" : "child" + std::to_string(i);
            r.type = tl::formats::AdmType::Integer;
            r.integer = ancestors[i];
            s.records.push_back(r);
        }
        return s;
    };
    types.children.push_back(make_type("any", 0, {}));
    types.children.push_back(make_type("Character", 25, {0}));
    types.children.push_back(make_type("Monster", 27, {25, 0}));
    types.children.push_back(make_type("Mimic", 167, {27, 25, 0}));
    types.children.push_back(make_type("Grunt", 60, {27, 25, 0}));
    root.children.push_back(types);
    tl::core::UnitTypeHierarchy hierarchy;
    hierarchy.parse(root);

    tl::core::UnitBook units;
    tl::core::UnitStats mimic;
    mimic.unittype = "MIMIC";
    units.add("Mimic", mimic);
    tl::core::UnitStats grunt;
    grunt.unittype = "GRUNT";
    units.add("Grunt", grunt);

    const tl::core::StatBook graphs = damage_shape_graphs();
    CmpScene scene;
    CmpNode& chest = add_node(scene, tl::formats::cmp_type::kMonster, 2, "Mimic");
    pad_to(chest, 5);
    chest.fields[tl::formats::cmp_field::monster::kPosition] = vec3_field(0.0f, 0.0f, 0.0f);
    chest.fields[tl::formats::cmp_field::monster::kMonster] = text_field("Mimic");
    CmpNode& other = add_node(scene, tl::formats::cmp_type::kMonster, 3, "Grunt");
    pad_to(other, 5);
    other.fields[tl::formats::cmp_field::monster::kPosition] = vec3_field(40.0f, 0.0f, 0.0f);
    other.fields[tl::formats::cmp_field::monster::kMonster] = text_field("Grunt");

    LevelScript script;
    script.set_units(&units);
    script.set_stat_graphs(&graphs);
    script.set_unit_types(&hierarchy);
    script.build(scene, {});
    QuestState quests;
    std::vector<ScriptEvent> events;
    script.level_activated(quests, events);

    TL_CHECK(script.unit_mimic_idle(2));
    TL_CHECK(!script.unit_mimic_idle(3));
    TL_CHECK_EQ(script.idle_mimics().size(), 1u);
    TL_CHECK_EQ(script.idle_mimics().front().id, 2);

    // Nothing behind the blow (a damage shape's): no loss, and it sleeps on.
    const float full = health_of(script, 2);
    script.damage_unit(2, 5.0f, LevelScript::kNoAttacker, quests, events);
    TL_CHECK_EQ(health_of(script, 2), full);
    TL_CHECK(script.unit_mimic_idle(2));

    // The player's blow: still no loss, but it wakes.
    script.damage_unit(2, 5.0f, 0, quests, events, nullptr, nullptr, /*struck=*/true);
    TL_CHECK_EQ(health_of(script, 2), full);
    TL_CHECK(!script.unit_mimic_idle(2));
    TL_CHECK(script.idle_mimics().empty());

    // Awake, it is hurt like anything else.
    script.damage_unit(2, 5.0f, 0, quests, events);
    TL_CHECK(health_of(script, 2) < full);

    // `CCharacter::attack` on one lying in wait wakes it and strikes nothing.
    LevelScript again;
    again.set_units(&units);
    again.set_stat_graphs(&graphs);
    again.set_unit_types(&hierarchy);
    again.build(scene, {});
    again.level_activated(quests, events);
    TL_CHECK(!again.attack_idle_mimic(3, quests, events));
    TL_CHECK(again.attack_idle_mimic(2, quests, events));
    TL_CHECK(!again.unit_mimic_idle(2));
    TL_CHECK(!again.attack_idle_mimic(2, quests, events));
    TL_CHECK_EQ(health_of(again, 2), full);
}

// An `Animation Controller` with no `GROUP` plays on the player
// (`CAnimationPlayer::CAnimationPlayer`).
TL_TEST(an_animation_controller_without_a_group_plays_on_the_player) {
    CmpScene scene;
    CmpNode& node =
        add_node(scene, tl::formats::cmp_type::kAnimationController, 1, "PlayerCorrupted");
    node.fields.push_back(uint_field(1));             // 0 START ON LOAD
    node.fields.push_back(float_field(0.2f));         // 1 BLEND TIME
    node.fields.push_back(float_field(0.2f));         // 2 BLEND OUT TIME
    node.fields.push_back(float_field(0.0f));         // 3 DURATION OVERRIDE
    node.fields.push_back(uint_field(1));             // 4 PLAY IDLE
    node.fields.push_back(text_field("Corruption"));  // 5 ANIMATION

    LevelScript script;
    script.build(scene, {});
    QuestState quests;
    std::vector<ScriptEvent> events;
    script.level_activated(quests, events);

    size_t on_player = 0;
    for (const ScriptEvent& e : events) {
        if (e.action != ScriptAction::Animation) continue;
        TL_CHECK(e.to_player);
        TL_CHECK(e.on);
        TL_CHECK(e.loops);
        TL_CHECK_EQ(e.text, std::string("Corruption"));
        ++on_player;
    }
    TL_CHECK_EQ(on_player, size_t{1});
}

// `CCharacter::createDefaultEquipment`: the authored hands, then the
// rolled ones, for each character as it is made - not for its unit type.
TL_TEST(each_character_holds_what_its_own_equipment_roll_gave_it) {
    tl::core::UnitBook units;
    tl::core::UnitStats goblin;
    goblin.left_hand = "Buckler";
    goblin.spawn_right_hand = "GOBLIN_AXES";
    units.add("goblin", goblin);

    CmpScene empty_scene;
    LevelScript script;
    script.set_units(&units);
    int rolls = 0;
    script.set_equipment_roller([&rolls](const std::string& unit, LevelScript::UnitCreation& made) {
        TL_CHECK_EQ(unit, std::string("goblin"));
        // The authored hands are in before the roll.
        TL_CHECK_EQ(made.left_hand, std::string("Buckler"));
        TL_CHECK(made.right_hand.empty());
        made.right_hand = rolls++ == 0 ? "Goblin Pick" : "Spiked Star";
        made.weapon_attack.present = true;
        made.weapon_attack.cooldown = static_cast<float>(rolls);
    });
    script.build(empty_scene, {});
    script.add_spawned_unit(-1001, "goblin", tl::core::Vec3{2.0f, 0.0f, 2.0f}, false);
    script.add_spawned_unit(-1002, "goblin", tl::core::Vec3{4.0f, 0.0f, 2.0f}, false);
    TL_CHECK_EQ(rolls, 2);

    std::string right, left;
    TL_CHECK(script.unit_hands(-1001, right, left));
    TL_CHECK_EQ(right, std::string("Goblin Pick"));
    TL_CHECK_EQ(left, std::string("Buckler"));
    TL_CHECK(script.unit_hands(-1002, right, left));
    TL_CHECK_EQ(right, std::string("Spiked Star"));
    TL_CHECK(!script.unit_hands(-1003, right, left));

    // What making a unit already drew is kept, not rolled again.
    LevelScript::UnitCreation made = script.roll_unit_creation("goblin");
    TL_CHECK_EQ(rolls, 3);
    made.right_hand = "Bladed Axe";
    script.add_spawned_unit(-1004, "goblin", tl::core::Vec3{6.0f, 0.0f, 2.0f}, false, 0.0f, &made);
    TL_CHECK_EQ(rolls, 3);
    TL_CHECK(script.unit_hands(-1004, right, left));
    TL_CHECK_EQ(right, std::string("Bladed Axe"));

    // `CCharacter::unitInit`: a unit made from a save rolls nothing
    // and holds nothing until its saved inventory is applied.
    const LevelScript::UnitCreation from_save = script.roll_unit_creation("goblin", false);
    TL_CHECK_EQ(rolls, 3);
    TL_CHECK(from_save.right_hand.empty() && from_save.left_hand.empty());

    // What a stored level keeps of a character, and puts back.
    bool found = false;
    for (LevelScript::CharacterRecord record : script.living_characters()) {
        if (record.node != -1002) continue;
        found = true;
        TL_CHECK(record.equipment);
        TL_CHECK_EQ(record.right_hand, std::string("Spiked Star"));
        TL_CHECK_EQ(record.left_hand, std::string("Buckler"));
        record.right_hand = "Goblin Pick";
        record.left_hand.clear();
        std::vector<ScriptEvent> events;
        TL_CHECK(script.apply_character_record(record, events));
    }
    TL_CHECK(found);
    TL_CHECK(script.unit_hands(-1002, right, left));
    TL_CHECK_EQ(right, std::string("Goblin Pick"));
    TL_CHECK(left.empty());
}

// ------------------------------------------------- game state controller

namespace {

/// A `Game State Controller` (node 2) that the level's activation sends
/// `inputs`, in order. `state` null leaves `STATE` and `TIP` out of the node.
CmpScene game_state_scene(const char* state, bool invulnerable, const char* tip,
                          std::initializer_list<const char*> inputs) {
    CmpScene scene;
    CmpNode& group = add_node(scene, tl::formats::cmp_type::kLogicGroup, 1, "Logic");
    group.fields.push_back(uint_field(0));
    CmpNode& node = add_node(scene, tl::formats::cmp_type::kGameStateController, 2, "State");
    node.fields.push_back(uint_field(0));                       // ENABLED
    node.fields.push_back(uint_field(invulnerable ? 1u : 0u));  // INVULNERABLE
    if (state != nullptr) {
        node.fields.push_back(text_field(state));  // STATE
        if (tip != nullptr) node.fields.push_back(text_field(tip));
    }
    tl::formats::CmpLogicObject from{0, 1, 0.0f, 0.0f, {}};
    for (const char* input : inputs) from.links.push_back({1, "Level Activated", input});
    tl::formats::CmpLogicGroup wiring;
    wiring.node_id = 1;
    wiring.objects.push_back(from);
    wiring.objects.push_back({1, 2, 0.0f, 0.0f, {}});
    scene.logic_groups.push_back(std::move(wiring));
    return scene;
}

std::vector<ScriptEvent> of_action(const std::vector<ScriptEvent>& events, ScriptAction action) {
    std::vector<ScriptEvent> out;
    for (const ScriptEvent& e : events) {
        if (e.action == action) out.push_back(e);
    }
    return out;
}

std::vector<ScriptEvent> activate(const CmpScene& scene) {
    LevelScript script;
    script.build(scene, {});
    QuestState quests;
    std::vector<ScriptEvent> events;
    script.level_activated(quests, events);
    return events;
}

}  // namespace

// `doActualGameState`, state 1: the player disabled and, disabled
// again, enabled; the scene is told the type for its `stopPathing`.
TL_TEST(a_pause_player_state_disables_the_player_and_gives_it_back) {
    const std::vector<ScriptEvent> events =
        activate(game_state_scene("PAUSE PLAYER", false, nullptr, {"Enable", "Disable"}));
    const std::vector<ScriptEvent> enabled = of_action(events, ScriptAction::PlayerEnabled);
    TL_CHECK_EQ(enabled.size(), 2u);
    TL_CHECK(!enabled[0].on);
    TL_CHECK(enabled[1].on);
    const std::vector<ScriptEvent> states = of_action(events, ScriptAction::GameState);
    TL_CHECK_EQ(states.size(), 2u);
    TL_CHECK_EQ(states[0].input, 1u);
    TL_CHECK(states[0].on);
    TL_CHECK(!states[1].on);
    TL_CHECK(of_action(events, ScriptAction::PlayerInvincible).empty());
}

// No `STATE` keeps the constructor's 1; an unknown word is 0
// (`getGameStateIDByString`).
TL_TEST(a_game_state_without_a_state_pauses_the_player_and_an_unknown_one_does_nothing) {
    const std::vector<ScriptEvent> bare =
        activate(game_state_scene(nullptr, false, nullptr, {"Enable"}));
    TL_CHECK_EQ(of_action(bare, ScriptAction::PlayerEnabled).size(), 1u);
    TL_CHECK_EQ(of_action(bare, ScriptAction::GameState).at(0).input, 1u);

    const std::vector<ScriptEvent> unknown =
        activate(game_state_scene("WIDESCREEN", false, nullptr, {"Enable"}));
    TL_CHECK(of_action(unknown, ScriptAction::PlayerEnabled).empty());
    TL_CHECK_EQ(of_action(unknown, ScriptAction::GameState).at(0).input, 0u);
}

// State 2 outlives the level; `INVULNERABLE` is given on the way in and taken on the way out
// (`performStateControl`).
TL_TEST(a_pause_game_state_makes_the_level_dormant_and_the_player_invincible) {
    LevelScript::reset_client_paused();
    {
        const std::vector<ScriptEvent> events =
            activate(game_state_scene("PAUSE GAME", true, nullptr, {"Enable"}));
        TL_CHECK(LevelScript::client_paused());
        const std::vector<ScriptEvent> invincible =
            of_action(events, ScriptAction::PlayerInvincible);
        TL_CHECK_EQ(invincible.size(), 1u);
        TL_CHECK(invincible[0].on);
        TL_CHECK(!of_action(events, ScriptAction::PlayerEnabled).at(0).on);
    }
    TL_CHECK(LevelScript::client_paused());  // another level, still dormant
    {
        const std::vector<ScriptEvent> events =
            activate(game_state_scene("PAUSE GAME", true, nullptr, {"Enable", "Disable"}));
        TL_CHECK(!LevelScript::client_paused());
        const std::vector<ScriptEvent> invincible =
            of_action(events, ScriptAction::PlayerInvincible);
        TL_CHECK_EQ(invincible.size(), 2u);
        TL_CHECK(!invincible[1].on);
    }
    LevelScript::reset_client_paused();
}

// State 5: `globallyDisableSkills(on)`, which `executeSkill` tests first.
TL_TEST(a_disable_skills_state_disables_every_skill_until_it_is_disabled) {
    tl::core::globally_disable_skills(false);
    activate(game_state_scene("DISABLE SKILLS", false, nullptr, {"Enable"}));
    TL_CHECK(tl::core::skills_globally_disabled());
    activate(game_state_scene("DISABLE SKILLS", false, nullptr, {"Enable", "Disable"}));
    TL_CHECK(!tl::core::skills_globally_disabled());
    tl::core::globally_disable_skills(false);
}

// State 4: `setMeshVisible(!on, true)` on the player.
TL_TEST(a_hide_player_state_hides_the_player_and_shows_it_again) {
    const std::vector<ScriptEvent> events =
        activate(game_state_scene("HIDE PLAYER", false, nullptr, {"Enable", "Disable"}));
    const std::vector<ScriptEvent> shown = of_action(events, ScriptAction::PlayerMeshVisible);
    TL_CHECK_EQ(shown.size(), 2u);
    TL_CHECK(!shown[0].on);
    TL_CHECK(shown[1].on);
    TL_CHECK(of_action(events, ScriptAction::PlayerEnabled).empty());
}

// `Heal Player` (0x4c) and `Show Tip` (0x50), a tip only with a `TIP` (`showHelp`).
TL_TEST(a_game_state_controller_heals_the_player_and_shows_its_tip) {
    const std::vector<ScriptEvent> with_tip = activate(
        game_state_scene("PAUSE PLAYER", false, "TIP_INVENTORY", {"Heal Player", "Show Tip"}));
    TL_CHECK_EQ(of_action(with_tip, ScriptAction::HealPlayer).size(), 1u);
    const std::vector<ScriptEvent> tips = of_action(with_tip, ScriptAction::ShowTip);
    TL_CHECK_EQ(tips.size(), 1u);
    TL_CHECK_EQ(tips[0].input, 0u);
    TL_CHECK(of_action(with_tip, ScriptAction::Unhandled).empty());

    const std::vector<ScriptEvent> without =
        activate(game_state_scene("PAUSE PLAYER", false, nullptr, {"Show Tip"}));
    TL_CHECK(of_action(without, ScriptAction::ShowTip).empty());
}

// `Warp Pets to Player` (0x4b): each pet's `teleportToMaster(1.0)`.
TL_TEST(warp_pets_to_player_puts_each_pet_by_the_player) {
    tl::core::CollisionWorld world;
    world.add_triangle({-50.0f, 0.0f, -50.0f}, {-50.0f, 0.0f, 50.0f}, {50.0f, 0.0f, 50.0f});
    world.add_triangle({-50.0f, 0.0f, -50.0f}, {50.0f, 0.0f, 50.0f}, {50.0f, 0.0f, -50.0f});
    world.build();
    tl::core::PassabilityMap grid;
    grid.bake(world, nullptr);
    tl::core::NavGrid nav;
    nav.build(grid);

    tl::core::UnitBook units;
    tl::core::UnitStats merc;
    merc.radius = 0.3f;
    merc.alignment = tl::core::UnitStats::Alignment::Friendly;
    merc.movement.walking_speed = 1.9f;
    merc.movement.running_speed = 6.5f;
    units.add("merc", merc);

    LevelScript level;
    level.set_units(&units);
    level.set_navigation(&nav);
    level.set_collision(&world);
    level.set_passability(&grid);
    tl::test::seed_generators();
    level.build(game_state_scene("PAUSE PLAYER", false, nullptr, {"Warp Pets to Player"}), {});
    // Twenty away: inside the 25 past which the pet's own AI brings it back.
    level.add_spawned_unit(-2001, "merc", tl::core::Vec3{0.0f, 0.0f, 0.0f}, false);

    QuestState quests;
    std::vector<ScriptEvent> events;
    TL_CHECK(level.make_player_pet(-2001, quests, events));
    const tl::core::Vec3 player{20.0f, 0.0f, 0.0f};
    level.update(1.0f / 60.0f, player, quests, events);
    tl::core::Vec3 before;
    TL_CHECK(level.unit_position(-2001, before));
    TL_CHECK(before.x < 1.0f);

    level.level_activated(quests, events);
    tl::core::Vec3 at;
    TL_CHECK(level.unit_position(-2001, at));
    const float dx = at.x - player.x;
    const float dz = at.z - player.z;
    // One back from the player, scattered at most 8 each way.
    TL_CHECK(std::sqrt(dx * dx + dz * dz) < 1.0f + 8.0f * std::sqrt(2.0f) + 1e-3f);
    TL_CHECK(of_action(events, ScriptAction::Unhandled).empty());
}

// `CCharacter::spawn` / `spawningAI`: a spawned unit keeps its facing
// through the entrance, then hunts.
TL_TEST(a_spawning_unit_holds_its_facing_until_the_entrance_ends) {
    tl::core::CollisionWorld world;
    world.add_triangle({-10.0f, 0.0f, -10.0f}, {-10.0f, 0.0f, 10.0f}, {10.0f, 0.0f, 10.0f});
    world.add_triangle({-10.0f, 0.0f, -10.0f}, {10.0f, 0.0f, 10.0f}, {10.0f, 0.0f, -10.0f});
    world.build();
    tl::core::PassabilityMap grid;
    grid.bake(world, nullptr);
    tl::core::NavGrid nav;
    nav.build(grid);

    tl::core::UnitBook units;
    tl::core::UnitStats goblin;
    goblin.radius = 0.3f;
    goblin.movement.walking_speed = 0.0f;
    goblin.movement.running_speed = 0.0f;
    units.add("goblin", goblin);

    CmpScene empty_scene;
    LevelScript script;
    script.set_units(&units);
    script.set_navigation(&nav);
    script.set_collision(&world);
    script.set_passability(&grid);
    script.build(empty_scene, {});
    script.add_spawned_unit(-1001, "goblin", tl::core::Vec3{0.0f, 0.0f, 2.0f}, true, 0.0f);
    script.set_spawning(-1001, true);
    TL_CHECK_EQ(script.unit_ai_state(-1001), 0xc);

    QuestState quests;
    std::vector<ScriptEvent> events;
    // The player off to the side, well inside the active range.
    const tl::core::Vec3 player{4.0f, 0.0f, 2.0f};
    for (int i = 0; i < 30; ++i) script.update(1.0f / 60.0f, player, quests, events);
    float yaw = 1.0f;
    TL_CHECK(script.unit_facing(-1001, yaw));
    TL_CHECK(std::fabs(yaw) < 1e-4f);
    TL_CHECK_EQ(script.unit_ai_state(-1001), 0xc);

    script.set_spawning(-1001, false);
    TL_CHECK_EQ(script.unit_ai_state(-1001), tl::core::ai_state::kHunt);
}

// A machine joining the list mid-walk (from `damage_shape_instant`): the walk keeps to the
// machines there were when it began.
TL_TEST(machine_walk_survives_the_list_growing) {
    LevelScript first;
    LevelScript second;
    std::vector<LevelScript> late(64);
    std::vector<LevelScript*> list{&first, &second};
    list.shrink_to_fit();

    std::vector<LevelScript*> seen;
    size_t added = 0;
    for (LevelScript* machine : tl::core::MachineWalk(list)) {
        seen.push_back(machine);
        // Enough to move the storage more than once.
        for (int i = 0; i < 32; ++i) list.push_back(&late[added++]);
    }
    TL_CHECK_EQ(seen.size(), static_cast<size_t>(2));
    TL_CHECK(seen[0] == &first);
    TL_CHECK(seen[1] == &second);
    TL_CHECK_EQ(list.size(), static_cast<size_t>(66));

    const tl::core::MachineWalk again(list);
    TL_CHECK(!again.empty());
    TL_CHECK(again.front() == &first);
    size_t count = 0;
    for (LevelScript* machine : again) {
        (void)machine;
        ++count;
    }
    TL_CHECK_EQ(count, static_cast<size_t>(66));
}

// `CCharacter::selectAttack`, `performAttack`:
// a left-hand bow looses its `MISSILE` (`CEquipment::fireMissiles`).
TL_TEST(an_archer_looses_its_bows_missile) {
    tl::core::UnitBook units;
    tl::core::UnitStats archer;
    archer.spawn_left_hand = "SKELETON_BOWS";
    // A model: `selectAttack` chooses nothing for a unit without one.
    archer.mesh = "media/models/archer/archer";
    units.add("archer", archer);

    CmpScene empty_scene;
    LevelScript script;
    script.set_units(&units);
    script.set_unit_types(&basic_unit_types());
    script.set_equipment_roller([](const std::string&, LevelScript::UnitCreation& made) {
        made.left_hand = "Spike Bow";
        made.left_weapon_attack.present = true;
        made.left_weapon_attack.ranged = true;
        made.left_weapon_attack.missile = "ARROW";
        made.left_weapon_attack.attack.animation = "BOW";
        made.left_weapon_attack.attack.range = 8.0f;
    });
    script.set_animation_catalog([](const std::string&) {
        return std::vector<std::pair<std::string, float>>{
            {"Bow1", 1.0f}, {"Idle", 1.0f}, {"Walk", 1.0f}, {"Run", 1.0f}};
    });
    script.build(empty_scene, {});
    script.add_spawned_unit(-1001, "archer", tl::core::Vec3{0.0f, 0.0f, 0.0f}, true);
    QuestState quests;
    std::vector<ScriptEvent> events;
    script.level_activated(quests, events);

    // The player six units off, in the bow's reach and far past a fist's.
    const tl::core::Vec3 player{0.0f, 0.0f, 6.0f};
    size_t loosed = 0;
    size_t strikes = 0;
    for (int step = 0; step < 100 && loosed == 0; ++step) {
        events.clear();
        script.update(0.1f, player, quests, events);
        bool swung = false;
        for (const ScriptEvent& e : events) {
            if (e.action == ScriptAction::Animation && e.node_id == -1001 && e.text == "Bow1") {
                swung = true;
            }
        }
        if (!swung) continue;
        TL_CHECK(script.unit_attacks_left(-1001));
        events.clear();
        script.attack_hit(-1001, quests, events);
        for (const ScriptEvent& e : events) {
            if (e.action == ScriptAction::Strike) ++strikes;
            if (e.action != ScriptAction::WeaponMissile) continue;
            ++loosed;
            TL_CHECK_EQ(e.node_id, -1001);
            TL_CHECK_EQ(e.text, std::string("ARROW"));
            TL_CHECK(e.on);
            TL_CHECK(e.aimed);
            TL_CHECK_EQ(e.target_node, 0);
            TL_CHECK(e.target_machine == nullptr);
        }
    }
    TL_CHECK_EQ(loosed, size_t{1});
    TL_CHECK_EQ(strikes, size_t{0});
}

// `CLevel::reactToAreaDamage`: a struck unit's friends
// within their `DAMAGE_REACT_RADIUS` react too.
TL_TEST(a_struck_units_friends_near_it_react_too) {
    const tl::core::StatBook graphs = damage_shape_graphs();
    auto hunters = [](const std::vector<ScriptEvent>& events) {
        std::vector<int64_t> ids;
        for (const ScriptEvent& e : events) {
            if (e.action == ScriptAction::Hunt && e.on) ids.push_back(e.node_id);
        }
        return ids;
    };
    auto has = [](const std::vector<int64_t>& ids, int64_t id) {
        for (int64_t i : ids) {
            if (i == id) return true;
        }
        return false;
    };
    CmpScene scene;
    add_target(scene, 1, {0.0f, 0.0f, 0.0f});
    add_target(scene, 2, {2.5f, 0.0f, 0.0f});  // 2.5 - 0.3 - 0.3 = 1.9 apart
    add_target(scene, 3, {10.0f, 0.0f, 0.0f});
    LevelScript script;
    script.set_stat_graphs(&graphs);
    script.build(scene, {});
    QuestState quests;
    std::vector<ScriptEvent> events;
    script.level_activated(quests, events);

    events.clear();
    script.damage_unit(1, 1.0f, 0, quests, events);
    TL_CHECK(hunters(events).empty());

    events.clear();
    script.damage_unit(1, 1.0f, 0, quests, events, nullptr, nullptr, /*struck=*/true);
    const std::vector<int64_t> ids = hunters(events);
    TL_CHECK(has(ids, 1));
    TL_CHECK(has(ids, 2));
    TL_CHECK(!has(ids, 3));
}
