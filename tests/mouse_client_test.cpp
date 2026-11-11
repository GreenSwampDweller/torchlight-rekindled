// The PC's mouse (CGameClient::clickLeft, clickRight, moveToMouse and the
// pointer's share of processIngameInput), against a recording host.
#include "core/mouse_client.h"
#include "test_support.h"

#include <string>
#include <vector>

using tl::core::click_left;
using tl::core::click_right;
using tl::core::kNoSkill;
using tl::core::mouse_ingame_input;
using tl::core::MouseButtons;
using tl::core::MouseClient;
using tl::core::MouseHost;
using tl::core::MouseUnit;
using tl::core::Vec3;

namespace {

constexpr float kFrame = 1.0f / 60.0f;
constexpr MouseUnit kMonster = 11;
constexpr MouseUnit kMerchant = 12;
constexpr MouseUnit kBarrel = 21;
constexpr MouseUnit kPotion = 22;
constexpr std::int64_t kFireball = 500;

/// A host that answers from plain fields and writes down what it was told.
struct FakeHost : MouseHost {
    std::vector<std::string> calls;

    bool is_paused = false;
    bool pointer_on_ground = true;
    Vec3 pointer{5.0f, 0.0f, 7.0f};
    bool shift = false;
    bool rmb = false;
    bool alive = true;
    bool enabled = true;
    Vec3 me{0.0f, 0.0f, 0.0f};
    int state = 0;
    bool hold = false;
    bool has_path = false;
    bool attack_started = false;
    bool skill = false;
    bool skill_loose = false;
    bool attack_loose = false;
    MouseUnit targeted = 0;
    MouseUnit targeted_item = 0;
    bool in_range = false;
    bool pet = false;
    bool monster_alive = true;
    bool skill_in_range = true;
    bool cast_works = true;
    std::int64_t slots[tl::core::kMouseSlots] = {kNoSkill, kNoSkill, kNoSkill, kNoSkill, kNoSkill,
                                                 kNoSkill, kNoSkill, kNoSkill, kNoSkill, kNoSkill};
    bool keys_held[tl::core::kMouseSlots] = {};
    bool keys_pressed[tl::core::kMouseSlots] = {};
    bool has_current = false;
    std::int64_t current = kNoSkill;

    void say(const std::string& what) { calls.push_back(what); }
    bool said(const std::string& what) const {
        for (const std::string& c : calls) {
            if (c == what) return true;
        }
        return false;
    }

    bool paused() override { return is_paused; }
    bool find_world_location(Vec3& out, bool raised) override {
        out = pointer;
        say(raised ? "location raised" : "location");
        return pointer_on_ground;
    }
    bool hold_key() override { return shift; }
    bool right_button() override { return rmb; }
    std::int64_t slot_skill(int slot) override { return slots[slot]; }
    bool slot_has_item(int) override { return false; }
    bool slot_key_held(int slot) override { return keys_held[slot]; }
    bool slot_key_pressed(int slot) override { return keys_pressed[slot]; }
    bool slot_key_released(int) override { return false; }
    void activate_item_slot(int) override { say("item slot"); }
    int cursor_state() override { return 0; }
    void set_cursor_state(int) override { say("cursor"); }
    void set_pending_skill(std::int64_t) override {}
    void flush_interface_input() override {}
    void set_right_button_pressed() override {}
    bool equipment_tooltip_visible() override { return false; }

    bool player_alive() override { return alive; }
    bool player_enabled() override { return enabled; }
    Vec3 player_position() override { return me; }
    int ai_state() override { return state; }
    void set_ai_state(int s) override {
        state = s;
        say("state " + std::to_string(s));
    }
    bool hold_position() override { return hold; }
    void set_hold_position(bool h) override { hold = h; }
    bool pathing() override { return has_path; }
    void set_path_timer(float) override { say("path timer"); }
    bool take_attack_started() override {
        const bool was = attack_started;
        attack_started = false;
        return was;
    }
    bool performing_skill() override { return skill; }
    bool performing_skill_loose() override { return skill_loose; }
    bool performing_attack_loose() override { return attack_loose; }
    MouseUnit target() override { return targeted; }
    MouseUnit target_item() override { return targeted_item; }
    void set_target(MouseUnit c) override {
        targeted = c;
        say("target " + std::to_string(c));
    }
    void set_target_item(MouseUnit i) override {
        targeted_item = i;
        say("target item " + std::to_string(i));
    }
    void set_target_direction(float, float) override { say("direction"); }
    void stop_pathing() override { say("stop"); }
    void set_destination(float, float) override { say("destination"); }
    bool in_attack_range() override { return in_range; }
    void attack() override { say("attack"); }
    void hunt_ai() override { say("hunt"); }
    void fetch_item(MouseUnit, bool by_pet, MouseUnit) override {
        say(by_pet ? "pet fetches" : "fetch");
    }
    bool pet_can_fetch() override { return pet; }

    bool unit_alive(MouseUnit c) override { return c != kMonster || monster_alive; }
    bool unit_present(MouseUnit) override { return true; }
    bool is_enemy(MouseUnit c) override { return c == kMonster; }
    bool unit_interactive(MouseUnit c) override { return c == kMerchant; }
    Vec3 unit_position(MouseUnit) override { return Vec3{3.0f, 0.0f, 4.0f}; }
    bool item_breakable(MouseUnit i) override { return i == kBarrel; }
    bool item_takeable(MouseUnit i) override { return i == kPotion; }
    bool item_usable(MouseUnit) override { return true; }
    bool item_selectable(MouseUnit) override { return true; }

    bool has_skills() override { return true; }
    bool skill_known(std::int64_t guid) override { return guid == kFireball; }
    std::int64_t right_skill() override { return kFireball; }
    std::int64_t left_skill() override { return kNoSkill; }
    bool skill_ready(std::int64_t) override { return true; }
    int skill_target_type(std::int64_t) override { return 0; }
    int skill_animation_index(std::int64_t) override { return 0; }
    bool has_current_skill() override { return has_current; }
    std::int64_t current_skill() override { return current; }
    bool current_skill_moves() override { return false; }
    bool cast_skill(std::int64_t) override {
        say("cast");
        return cast_works;
    }
    void attempt_to_stop_skill(bool) override { say("stop skill"); }
    int in_skill_range(std::int64_t) override { return skill_in_range ? 0 : 1; }
    void skill_approach_ai(std::int64_t) override { say("skill approach"); }
};

MouseButtons left(bool pressed) {
    MouseButtons b;
    b.left = true;
    b.left_pressed = pressed;
    return b;
}

MouseButtons right(bool pressed) {
    MouseButtons b;
    b.right = true;
    b.right_pressed = pressed;
    return b;
}

}  // namespace

// A click on open ground: the targets let go, the AI idle, a destination at the
// pointer, and held, another every tenth of a second.
TL_TEST(mouse_left_on_ground_walks_and_repeats_every_tenth) {
    MouseClient client;
    FakeHost host;
    mouse_ingame_input(client, host, left(true), kFrame);
    TL_CHECK(host.said("target 0"));
    TL_CHECK(host.said("state 0"));
    TL_CHECK(host.said("destination"));
    TL_CHECK(client.left_held);
    int destinations = 1;
    for (int i = 1; i < 60; ++i) {
        host.calls.clear();
        mouse_ingame_input(client, host, left(false), kFrame);
        if (host.said("destination")) ++destinations;
        // Held, the targets are not let go again.
        TL_CHECK(!host.said("state 0"));
    }
    TL_CHECK(destinations >= 9 && destinations <= 11);
    host.calls.clear();
    mouse_ingame_input(client, host, MouseButtons{}, kFrame);
    TL_CHECK(!client.left_held);
    TL_CHECK(host.calls.empty());
}

// The pointer over the interface: no destination.
TL_TEST(mouse_left_over_the_interface_does_not_walk) {
    MouseClient client;
    FakeHost host;
    host.pointer_on_ground = false;
    mouse_ingame_input(client, host, left(true), kFrame);
    TL_CHECK(!host.said("destination"));
}

// A click on an enemy out of reach: it becomes the target and the hunt starts;
// in reach, the character stops and swings.
TL_TEST(mouse_left_on_enemy_hunts_then_attacks) {
    MouseClient client;
    FakeHost host;
    client.over_character = kMonster;
    host.has_path = true;
    mouse_ingame_input(client, host, left(true), kFrame);
    TL_CHECK_EQ(client.click_character, kMonster);
    TL_CHECK_EQ(host.targeted, kMonster);
    TL_CHECK(host.said("state 4"));
    TL_CHECK(host.said("hunt"));
    TL_CHECK(!host.said("attack"));
    TL_CHECK(!host.said("destination"));

    host.calls.clear();
    host.in_range = true;
    mouse_ingame_input(client, host, left(false), kFrame);
    TL_CHECK(host.said("stop"));
    TL_CHECK(host.said("attack"));
}

// The held button keeps its target while the pointer wanders, lets it go on
// release, and drops a dead one for whatever the pointer is over.
TL_TEST(mouse_left_keeps_its_click_target_until_release) {
    MouseClient client;
    FakeHost host;
    client.over_character = kMonster;
    mouse_ingame_input(client, host, left(true), kFrame);
    client.over_character = 0;
    mouse_ingame_input(client, host, left(false), kFrame);
    TL_CHECK_EQ(client.click_character, kMonster);
    host.monster_alive = false;
    host.calls.clear();
    mouse_ingame_input(client, host, left(false), kFrame);
    TL_CHECK_EQ(client.click_character, MouseUnit{0});
    TL_CHECK(host.said("target 0"));
    mouse_ingame_input(client, host, MouseButtons{}, kFrame);
    TL_CHECK_EQ(client.click_character, MouseUnit{0});
}

// A click on someone to talk to: the approach state and a destination at them.
TL_TEST(mouse_left_on_interactive_character_approaches) {
    MouseClient client;
    FakeHost host;
    client.over_character = kMerchant;
    mouse_ingame_input(client, host, left(true), kFrame);
    TL_CHECK_EQ(host.targeted, kMerchant);
    TL_CHECK(host.said("state 9"));
    TL_CHECK(host.said("destination"));
}

// A click on a floor item fetches it; with the hold-position key and a pet that
// can go, the pet is sent.
TL_TEST(mouse_left_on_item_fetches_and_shift_sends_the_pet) {
    {
        MouseClient client;
        FakeHost host;
        client.over_item = kPotion;
        mouse_ingame_input(client, host, left(true), kFrame);
        TL_CHECK(host.said("fetch"));
        TL_CHECK(!host.said("destination"));
    }
    {
        MouseClient client;
        FakeHost host;
        client.over_item = kPotion;
        host.shift = true;
        host.pet = true;
        mouse_ingame_input(client, host, left(true), kFrame);
        TL_CHECK(host.said("pet fetches"));
    }
}

// A breakable is attacked like an enemy: hunted out of reach, struck in reach.
TL_TEST(mouse_left_on_breakable_hunts_or_strikes) {
    MouseClient client;
    FakeHost host;
    client.over_item = kBarrel;
    host.has_path = true;
    mouse_ingame_input(client, host, left(true), kFrame);
    TL_CHECK_EQ(host.targeted_item, kBarrel);
    TL_CHECK(host.said("hunt"));
    host.calls.clear();
    host.in_range = true;
    mouse_ingame_input(client, host, left(false), kFrame);
    TL_CHECK(host.said("attack"));
}

// After a swing went out with the button still down, the character does not walk
// on; with the hold-position key it still may.
TL_TEST(mouse_left_does_not_walk_on_after_an_attack) {
    MouseClient client;
    FakeHost host;
    mouse_ingame_input(client, host, left(true), kFrame);
    host.attack_started = true;
    client.move_timer = 0.0f;
    host.calls.clear();
    mouse_ingame_input(client, host, left(false), kFrame);
    TL_CHECK(client.suppress_move);
    TL_CHECK(!host.said("destination"));
    mouse_ingame_input(client, host, MouseButtons{}, kFrame);
    TL_CHECK(!client.suppress_move);
}

// Nothing while paused, dead, disabled, or in the fishing catch.
TL_TEST(mouse_left_refused_states) {
    for (int which = 0; which < 4; ++which) {
        MouseClient client;
        FakeHost host;
        if (which == 0) host.is_paused = true;
        if (which == 1) host.alive = false;
        if (which == 2) host.enabled = false;
        if (which == 3) host.state = 0x22;
        click_left(client, host);
        TL_CHECK(host.calls.empty());
    }
}

// The right button with nothing under it: the skill cast at the pointer, twice.
TL_TEST(mouse_right_on_ground_casts_at_the_pointer) {
    MouseClient client;
    FakeHost host;
    mouse_ingame_input(client, host, right(true), kFrame);
    TL_CHECK(host.said("location raised"));
    TL_CHECK(host.said("direction"));
    int casts = 0;
    for (const std::string& c : host.calls) {
        if (c == "cast") ++casts;
    }
    TL_CHECK_EQ(casts, 2);
    TL_CHECK(client.right_held);
}

// The right button on an enemy out of the skill's range: the skill approach.
TL_TEST(mouse_right_on_enemy_out_of_range_approaches) {
    MouseClient client;
    FakeHost host;
    client.over_character = kMonster;
    host.skill_in_range = false;
    host.has_path = true;
    TL_CHECK(click_right(client, host, kNoSkill, false, false));
    TL_CHECK_EQ(host.targeted, kMonster);
    TL_CHECK(host.said("state 15"));
    TL_CHECK(host.said("skill approach"));
    TL_CHECK(!host.said("cast"));
}

// In range, the cast goes out with the enemy as the target.
TL_TEST(mouse_right_on_enemy_in_range_casts) {
    MouseClient client;
    FakeHost host;
    client.over_character = kMonster;
    TL_CHECK(click_right(client, host, kNoSkill, false, false));
    TL_CHECK_EQ(host.targeted, kMonster);
    TL_CHECK(host.said("cast"));
    TL_CHECK(!host.said("direction"));
}

// `only_on_target` refuses a click with no enemy or breakable under it.
TL_TEST(mouse_right_only_on_target_needs_one) {
    MouseClient client;
    FakeHost host;
    TL_CHECK(!click_right(client, host, kFireball, false, true));
    TL_CHECK(!host.said("cast"));
    client.over_character = kMerchant;
    TL_CHECK(!click_right(client, host, kFireball, false, true));
}

// A hotbar key casts its skill and takes the right button's turn.
TL_TEST(mouse_slot_key_casts_and_skips_the_right_button) {
    MouseClient client;
    FakeHost host;
    host.slots[2] = kFireball;
    host.keys_held[2] = true;
    host.keys_pressed[2] = true;
    mouse_ingame_input(client, host, right(true), kFrame);
    TL_CHECK(host.said("cast"));
    TL_CHECK(!client.right_held);
}

// A held skill is let go when neither its key nor the right button holds it.
TL_TEST(mouse_held_skill_is_let_go) {
    MouseClient client;
    FakeHost host;
    host.has_current = true;
    host.current = kFireball;
    host.skill = true;
    mouse_ingame_input(client, host, MouseButtons{}, kFrame);
    TL_CHECK(host.said("stop skill"));

    host.calls.clear();
    host.slots[0] = kFireball;
    host.keys_held[0] = true;
    mouse_ingame_input(client, host, MouseButtons{}, kFrame);
    TL_CHECK(!host.said("stop skill"));
}

// The hold-position key: the flag set, and the character turned to the pointer.
TL_TEST(mouse_hold_key_sets_hold_and_aims) {
    MouseClient client;
    FakeHost host;
    host.shift = true;
    mouse_ingame_input(client, host, MouseButtons{}, kFrame);
    TL_CHECK(host.hold);
    TL_CHECK(host.said("direction"));
    host.shift = false;
    mouse_ingame_input(client, host, MouseButtons{}, kFrame);
    TL_CHECK(!host.hold);
}
