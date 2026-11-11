// What only a mod reaches: `CMoneyTaker::takeMoney`, `CLayout::processInputs`
// and `CLayout::setDurationModification`.
#include "core/logic_events.h"
#include "core/quest.h"
#include "core/script.h"
#include "test_support.h"

#include <cstring>
#include <string>
#include <vector>

namespace {

using tl::core::LevelScript;
using tl::core::QuestState;
using tl::core::ScriptAction;
using tl::core::ScriptEvent;
using tl::formats::CmpField;
using tl::formats::CmpNode;
using tl::formats::CmpScene;

CmpField text_field(const std::string& text) {
    const size_t units = text.size() + 1;
    const size_t dwords = (units * 2 + 3) / 4;
    std::vector<uint8_t> payload(dwords * 4, 0);
    for (size_t i = 0; i < text.size(); ++i) payload[2 * i] = static_cast<uint8_t>(text[i]);
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

void add_quest(CmpScene& scene, int64_t id, const std::string& quest) {
    CmpNode& node = add_node(scene, tl::formats::cmp_type::kQuestController, id, quest);
    pad_to(node, tl::formats::cmp_field::quest_controller::kQuest);
    node.fields.push_back(text_field(quest));
}

/// A trigger about the origin wired to a `Money Taker` of `amount`, whose two
/// outputs each complete a quest.
CmpScene money_scene(uint32_t amount) {
    CmpScene scene;
    CmpNode& trigger = add_node(scene, tl::formats::cmp_type::kPlayerSphereTrigger, 4, "Sphere");
    trigger.fields.push_back(vec3_field(0.0f, 0.0f, 0.0f));
    trigger.fields.push_back(uint_field(1));
    trigger.fields.push_back(float_field(3.0f));
    CmpNode& taker = add_node(scene, tl::formats::cmp_type::kMoneyTaker, 1, "Toll");
    taker.fields.push_back(uint_field(amount));  // 0 AMOUNT
    add_quest(scene, 2, "PAID");
    add_quest(scene, 3, "BROKE");
    CmpNode& group = add_node(scene, tl::formats::cmp_type::kLogicGroup, 5, "Logic");
    group.fields.push_back(uint_field(0));
    tl::formats::CmpLogicGroup wiring;
    wiring.node_id = 5;
    wiring.objects.push_back({0, 4, 0.0f, 0.0f, {{1, "Triggered First Time", "Take Money"}}});
    wiring.objects.push_back(
        {1,
         1,
         0.0f,
         0.0f,
         {{2, "Money Taken", "Force Complete"}, {3, "Insufficient funds", "Force Complete"}}});
    wiring.objects.push_back({2, 2, 0.0f, 0.0f, {}});
    wiring.objects.push_back({3, 3, 0.0f, 0.0f, {}});
    scene.logic_groups.push_back(std::move(wiring));
    return scene;
}

/// One timeline of `duration` seconds, `LAYOUT CONTROL` or not, of a `TYPE`,
/// that completes `DONE` when it stops.
CmpScene timeline_scene(float duration, bool layout_control, const std::string& type) {
    CmpScene scene;
    CmpNode& timeline = add_node(scene, tl::formats::cmp_type::kTimeline, 1, "Timeline");
    timeline.fields.push_back(float_field(duration));  // 0 DURATION
    pad_to(timeline, 5);
    timeline.fields.push_back(text_field("Linear"));                // 5 DEFAULT INTERPOLATION TYPE
    timeline.fields.push_back(uint_field(layout_control ? 1 : 0));  // 6 LAYOUT CONTROL
    timeline.fields.push_back(text_field(type));                    // 7 TYPE
    add_quest(scene, 2, "DONE");
    // Something for it to move: a timeline with no track is not one.
    add_node(scene, tl::formats::cmp_type::kMonster, 5, "Roach");
    tl::formats::CmpTimeline keys;
    keys.node_id = 1;
    tl::formats::CmpTimelineTrack track;
    track.tag = 0;
    track.is_event = false;
    for (const float t : {0.0f, 1.0f}) {
        tl::formats::CmpTimelineKey k;
        k.time = t;
        for (const float v : {t * 10.0f, 0.0f, 0.0f}) {
            uint32_t bits = 0;
            std::memcpy(&bits, &v, 4);
            k.words.push_back(bits);
        }
        track.keys.push_back(k);
    }
    keys.targets.push_back({5, {track}});
    scene.timelines.push_back(std::move(keys));
    // A trigger about the origin that plays it, for the timeline a link cannot.
    CmpNode& trigger = add_node(scene, tl::formats::cmp_type::kPlayerSphereTrigger, 4, "Sphere");
    trigger.fields.push_back(vec3_field(0.0f, 0.0f, 0.0f));
    trigger.fields.push_back(uint_field(1));
    trigger.fields.push_back(float_field(3.0f));
    CmpNode& group = add_node(scene, tl::formats::cmp_type::kLogicGroup, 3, "Logic");
    group.fields.push_back(uint_field(0));
    tl::formats::CmpLogicGroup wiring;
    wiring.node_id = 3;
    wiring.objects.push_back({0, 1, 0.0f, 0.0f, {{1, "Stopped", "Force Complete"}}});
    wiring.objects.push_back({1, 2, 0.0f, 0.0f, {}});
    wiring.objects.push_back({2, 4, 0.0f, 0.0f, {{0, "Triggered First Time", "Play"}}});
    scene.logic_groups.push_back(std::move(wiring));
    return scene;
}

const tl::core::Vec3 kFarAway{500.0f, 0.0f, 0.0f};

/// A trigger about the origin wired to a `Layout Link Timeline` with `input`.
CmpScene link_scene(const std::string& input) {
    CmpScene scene;
    CmpNode& trigger = add_node(scene, tl::formats::cmp_type::kPlayerSphereTrigger, 4, "Sphere");
    trigger.fields.push_back(vec3_field(0.0f, 0.0f, 0.0f));
    trigger.fields.push_back(uint_field(1));
    trigger.fields.push_back(float_field(3.0f));
    add_node(scene, tl::formats::cmp_type::kTimelineLink, 1, "Link");
    CmpNode& group = add_node(scene, tl::formats::cmp_type::kLogicGroup, 3, "Logic");
    group.fields.push_back(uint_field(0));
    tl::formats::CmpLogicGroup wiring;
    wiring.node_id = 3;
    wiring.objects.push_back({0, 4, 0.0f, 0.0f, {{1, "Triggered First Time", input}}});
    wiring.objects.push_back({1, 1, 0.0f, 0.0f, {}});
    scene.logic_groups.push_back(std::move(wiring));
    return scene;
}

/// How long the machine's layout timelines run once played, in frames at 60.
int frames_to_finish(LevelScript& script) {
    QuestState quests;
    std::vector<ScriptEvent> events;
    script.play_layout_timelines(quests, events, false);
    for (int frame = 1; frame <= 1200; ++frame) {
        script.update(1.0f / 60.0f, kFarAway, quests, events);
        if (quests.is_complete("DONE")) return frame;
    }
    return -1;
}

}  // namespace

/// `CMoneyTaker::takeMoney`: `Money Taken` or `Insufficient funds`; exactly the
/// amount is enough.
TL_TEST(a_money_taker_takes_its_amount_or_says_the_player_is_short) {
    for (const int held : {29, 30, 75}) {
        LevelScript script;
        script.build(money_scene(30), {});
        int gold = held;
        LevelScript::PlayerGold hooks;
        hooks.held = [&gold]() { return gold; };
        hooks.give = [&gold](int32_t amount) { gold += amount; };
        script.set_player_gold(hooks);
        QuestState quests;
        std::vector<ScriptEvent> events;
        script.update(0.016f, tl::core::Vec3{0.0f, 0.0f, 0.0f}, quests, events);
        if (held < 30) {
            TL_CHECK(quests.is_complete("BROKE"));
            TL_CHECK(!quests.is_complete("PAID"));
            TL_CHECK_EQ(gold, held);
        } else {
            TL_CHECK(quests.is_complete("PAID"));
            TL_CHECK(!quests.is_complete("BROKE"));
            TL_CHECK_EQ(gold, held - 30);
        }
    }
}

/// With no player to ask (`+0x58` of the first client null), it does nothing.
TL_TEST(a_money_taker_without_a_player_does_nothing) {
    LevelScript script;
    script.build(money_scene(30), {});
    QuestState quests;
    std::vector<ScriptEvent> events;
    script.update(0.016f, tl::core::Vec3{0.0f, 0.0f, 0.0f}, quests, events);
    TL_CHECK(!quests.is_complete("PAID"));
    TL_CHECK(!quests.is_complete("BROKE"));
}

/// `CLayoutDescriptor::InputLogicEvent`: a link hands `Hide` to its layout first,
/// and hides only when it is handed back.
TL_TEST(a_timeline_link_offers_hide_to_its_layout_before_taking_it) {
    const tl::core::Vec3 inside{0.0f, 0.0f, 0.0f};
    QuestState quests;
    {
        LevelScript script;
        script.build(link_scene("Hide"), {});
        std::vector<ScriptEvent> events;
        script.update(0.016f, inside, quests, events);
        int offered = 0;
        for (const ScriptEvent& e : events) {
            TL_CHECK(!(e.action == ScriptAction::SetVisible && e.node_id == 1));
            if (e.action == ScriptAction::LinkInput && e.node_id == 1) {
                ++offered;
                TL_CHECK_EQ(e.input, static_cast<uint32_t>(tl::core::logic::kHide));
            }
        }
        TL_CHECK_EQ(offered, 1);

        events.clear();
        script.link_input_declined(1, tl::core::logic::kHide, quests, events);
        bool hidden = false;
        for (const ScriptEvent& e : events) {
            TL_CHECK(e.action != ScriptAction::LinkInput);
            hidden = hidden || (e.action == ScriptAction::SetVisible && e.node_id == 1 && !e.on);
        }
        TL_CHECK(hidden);
    }
    {
        LevelScript script;
        script.build(link_scene("Disable and Hide"), {});
        std::vector<ScriptEvent> events;
        script.update(0.016f, inside, quests, events);
        bool hidden = false;
        for (const ScriptEvent& e : events) {
            TL_CHECK(e.action != ScriptAction::LinkInput);
            hidden = hidden || (e.action == ScriptAction::SetVisible && e.node_id == 1 && !e.on);
        }
        TL_CHECK(hidden);
    }
}

/// `CLayout::processInputs` on the link's layout: its `LAYOUT
/// CONTROL` timelines take the input, and the answer is whether there was one.
TL_TEST(a_links_layout_takes_an_input_only_through_its_layout_control_timelines) {
    QuestState quests;
    std::vector<ScriptEvent> events;
    {
        LevelScript inner;
        inner.build(timeline_scene(0.5f, /*layout_control=*/true, "None"), {});
        TL_CHECK(inner.process_inputs(tl::core::logic::kTimelinePlay, quests, events));
        TL_CHECK(inner.busy());
    }
    {
        LevelScript inner;
        inner.build(timeline_scene(0.5f, /*layout_control=*/false, "None"), {});
        TL_CHECK(!inner.process_inputs(tl::core::logic::kTimelinePlay, quests, events));
        TL_CHECK(!inner.busy());
    }
}

/// `CTimeline::setDurationModificationTime`: `DURATION MODIFY` by the timeline's
/// own type; only `LAYOUT CONTROL` timelines are asked.
TL_TEST(a_links_duration_modify_acts_by_the_timelines_own_type) {
    const auto frames = [](const std::string& type, float modify, bool layout_control) {
        LevelScript script;
        script.build(timeline_scene(0.5f, true, type), {});
        if (!layout_control) {
            // Played by the trigger the player stands in, not by a link.
            LevelScript other;
            other.build(timeline_scene(0.5f, false, type), {});
            other.set_duration_modification(modify);
            QuestState quests;
            std::vector<ScriptEvent> events;
            for (int frame = 1; frame <= 1200; ++frame) {
                other.update(1.0f / 60.0f, tl::core::Vec3{0.0f, 0.0f, 0.0f}, quests, events);
                if (quests.is_complete("DONE")) return frame;
            }
            return -1;
        }
        script.set_duration_modification(modify);
        return frames_to_finish(script);
    };
    const int plain = frames("None", 0.0f, true);
    TL_CHECK(plain > 20 && plain < 45);
    TL_CHECK_EQ(frames("None", 2.0f, true), plain);
    TL_CHECK_EQ(frames("Multiply", 0.0f, true), plain);
    const int doubled = frames("Multiply", 2.0f, true);
    TL_CHECK(doubled > 2 * plain - 6 && doubled < 2 * plain + 6);
    const int set = frames("Set", 2.0f, true);
    TL_CHECK(set > 4 * plain - 8 && set < 4 * plain + 8);
    // Not `LAYOUT CONTROL`: the same whatever the link says (started by the trigger,
    // so measured against itself).
    TL_CHECK_EQ(frames("Multiply", 2.0f, false), frames("Multiply", 0.0f, false));
}

// `CDungeonObjectDescriptor::InputLogicEvent`: only
// `Clear History` does anything.
TL_TEST(a_dungeon_object_clears_the_history_of_its_dungeon) {
    CmpScene scene;
    CmpNode& group = add_node(scene, tl::formats::cmp_type::kLogicGroup, 1, "Logic");
    group.fields.push_back(uint_field(0));
    CmpNode& node = add_node(scene, tl::formats::cmp_type::kDungeonObject, 2, "Dungeon");
    for (int i = 0; i < 5; ++i) node.fields.push_back(CmpField{});  // VISIBLE..UP unset
    node.fields.push_back(text_field("Mines"));                     // DUNGEON NAME
    tl::formats::CmpLogicObject from{0, 1, 0.0f, 0.0f, {}};
    from.links.push_back({1, "Level Activated", "Enable"});
    from.links.push_back({1, "Level Activated", "Clear History"});
    tl::formats::CmpLogicGroup wiring;
    wiring.node_id = 1;
    wiring.objects.push_back(from);
    wiring.objects.push_back({1, 2, 0.0f, 0.0f, {}});
    scene.logic_groups.push_back(std::move(wiring));

    LevelScript script;
    script.build(scene, {});
    QuestState quests;
    std::vector<ScriptEvent> events;
    script.level_activated(quests, events);
    std::vector<ScriptEvent> cleared;
    for (const ScriptEvent& e : events) {
        if (e.action == ScriptAction::ClearDungeonHistory) cleared.push_back(e);
    }
    TL_CHECK_EQ(cleared.size(), size_t{1});
    TL_CHECK_EQ(cleared.front().name, std::string("Mines"));
}
