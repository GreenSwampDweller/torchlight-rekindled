#include "core/mouse_client.h"

namespace tl::core {

namespace {

/// Both clicks open the same way: a dead click
/// target gives way to what the pointer is over, and a fresh press takes what is under it.
void adopt_dead_target(MouseClient& client, MouseHost& host) {
    if (client.over_character != 0 && client.click_character != 0 &&
        !host.unit_alive(client.click_character) &&
        client.over_character != client.click_character) {
        client.click_character = client.over_character;
    }
}

/// The swing or cast that went out since the last click: with the button still down the
/// character must not walk on after it.
void note_attack_started(MouseClient& client, MouseHost& host) {
    if (!host.take_attack_started()) return;
    if (!host.hold_position()) client.suppress_move = true;
}

/// The skill of the first hotbar slot whose key is down.
std::int64_t first_held_slot_skill(MouseHost& host) {
    for (int slot = 0; slot < kMouseSlots; ++slot) {
        const std::int64_t guid = host.slot_skill(slot);
        if (guid != kNoSkill && host.slot_key_held(slot)) return guid;
    }
    return kNoSkill;
}

}  // namespace

// CGameClient::moveToMouse
void move_to_mouse(MouseClient& client, MouseHost& host) {
    Vec3 point;
    if (!host.find_world_location(point, host.hold_position())) return;
    const Vec3 me = host.player_position();
    // Pointing at the character's own position asks at once, whatever the timer.
    if (point.x == me.x && point.y == me.y && point.z == me.z) {
        host.set_destination(point.x, point.z);
        client.move_timer = 0.1f;
        return;
    }
    if (client.move_timer <= 0.0f) {
        host.set_destination(point.x, point.z);
        client.move_timer = 0.1f;
    }
}

// CGameClient::clickLeft
// NOLINTNEXTLINE(readability-function-cognitive-complexity): one function in the original
void click_left(MouseClient& client, MouseHost& host) {
    if (host.paused()) return;
    if (!host.player_alive()) return;
    if (!host.player_enabled()) return;
    const int state = host.ai_state();
    if (state == 0x22 || state == 0x14) return;
    if (!client.left_held) host.set_path_timer(10.0f);

    adopt_dead_target(client, host);
    if (client.over_item != 0 && client.click_item == 0 && client.click_character == 0 &&
        !client.left_held) {
        client.click_item = client.over_item;
    }
    if (client.over_character != 0 && client.click_character == 0 && client.click_item == 0 &&
        !client.left_held) {
        client.click_character = client.over_character;
    }
    note_attack_started(client, host);
    if (host.performing_skill_loose()) return;

    MouseUnit previous_item = host.target_item();
    MouseUnit previous_target = host.target();
    if (!host.performing_skill_loose() && !host.performing_attack_loose()) {
        if (client.click_item != 0) host.set_target_item(client.click_item);
        if (client.click_character != 0) host.set_target(client.click_character);
    }
    // A click target no longer shown or enabled is forgotten.
    if (client.click_character != 0 && !host.unit_present(client.click_character)) {
        client.click_character = 0;
    }

    const auto hunt = [&]() {
        host.set_ai_state(mouse_ai::kHunt);
        host.hunt_ai();
        if (host.ai_state() == 3) return;
        if (host.performing_skill()) return;
        if (host.pathing()) return;
        host.attack();
    };
    const auto swing_in_place = [&]() {
        if (!host.performing_attack_loose() && host.ai_state() != 3 && !host.performing_skill()) {
            host.stop_pathing();
            host.attack();
        }
    };

    if (client.click_item != 0 && host.item_breakable(client.click_item)) {
        if (!host.performing_attack_loose() && !host.in_attack_range()) {
            if (previous_item != client.click_item) {
                host.stop_pathing();
                previous_item = client.click_item;
            }
            host.set_target_item(previous_item);
            hunt();
            return;
        }
        swing_in_place();
        return;
    }

    if (client.click_character != 0) {
        if (host.is_enemy(client.click_character)) {
            if (!host.performing_attack_loose() && !host.in_attack_range()) {
                if (previous_target != client.click_character) {
                    host.stop_pathing();
                    previous_target = client.click_character;
                }
                host.set_target(previous_target);
                hunt();
                return;
            }
            swing_in_place();
            return;
        }
        if (host.unit_interactive(client.click_character)) {
            host.set_target(client.click_character);
            host.set_ai_state(mouse_ai::kApproach);
            const Vec3 at = host.unit_position(client.click_character);
            host.set_destination(at.x, at.z);
            return;
        }
    }

    if (client.click_item == 0) {
        if (!client.left_held) {
            client.suppress_move = false;
            host.set_target(0);
            host.set_target_item(0);
            host.set_ai_state(mouse_ai::kIdle);
        }
        if (!client.suppress_move || host.hold_position()) move_to_mouse(client, host);
        return;
    }
    if (host.item_usable(client.click_item)) {
        // With the hold-position key down a pet that can go is sent instead.
        const bool by_pet = host.hold_key() && host.pet_can_fetch();
        host.fetch_item(client.click_item, by_pet, previous_item);
    }
}

// CGameClient::clickRight
// NOLINTNEXTLINE(readability-function-cognitive-complexity): one function in the original
bool click_right(MouseClient& client, MouseHost& host, std::int64_t guid, bool repeat,
                 bool only_on_target) {
    if (!host.player_alive()) return false;
    if (!host.player_enabled()) return false;
    if (!repeat) {
        host.set_path_timer(10.0f);
        host.set_target(client.click_character);
        host.set_target_item(client.click_item);
    }

    // The skill asked for, or the right button's own. A skill aimed at a point needs the
    // pointer on the ground; one aimed at an item of the player's (types 7, 8) arms the
    // cursor instead, and a second click while it is armed disarms it.
    {
        bool have = false;
        std::int64_t skill = kNoSkill;
        if (host.has_skills()) {
            if (host.skill_known(guid)) {
                have = true;
                skill = guid;
            } else if (host.skill_known(host.right_skill())) {
                have = true;
                skill = host.right_skill();
            }
        }
        bool needs_point = true;
        if (have) {
            bool aimed_in_world = true;
            const int type = host.skill_target_type(skill);
            if (type == 7 || type == 8) {
                if (repeat) return false;
                const int cursor = host.cursor_state();
                if (cursor == 4 || cursor == 3) {
                    host.set_pending_skill(kNoSkill);
                    host.set_cursor_state(0);
                    return false;
                }
                aimed_in_world = false;
            }
            needs_point = type != 3 && type != 9 && type != 10 && aimed_in_world;
        }
        if (needs_point) {
            Vec3 unused;
            if (!host.find_world_location(unused, true)) return false;
        }
    }

    adopt_dead_target(client, host);
    if (client.over_item != 0 && client.click_item == 0 && client.click_character == 0 &&
        !client.left_held && !client.right_held) {
        client.click_item = client.over_item;
    }
    if (client.over_character != 0 && client.click_character == 0 && client.click_item == 0 &&
        !client.left_held && !client.right_held) {
        // Not while the first hotbar key held is the skill being performed.
        const std::int64_t held = first_held_slot_skill(host);
        const bool performing_held = held != kNoSkill && host.has_current_skill() &&
                                     held == host.current_skill() && host.performing_skill_loose();
        if (!performing_held && client.over_character != client.click_character) {
            client.click_character = client.over_character;
        }
    }
    note_attack_started(client, host);
    if (host.performing_attack_loose()) return false;

    if (host.has_skills()) {
        bool have = host.skill_known(guid);
        if (!have) {
            guid = host.right_skill();
            have = host.skill_known(guid);
        }
        if (have && (!host.equipment_tooltip_visible() || !host.right_button())) {
            const int type = host.skill_target_type(guid);
            if (type == 7 || type == 8) {
                host.set_cursor_state(4);
                host.set_pending_skill(guid);
                host.flush_interface_input();
                host.set_right_button_pressed();
                return true;
            }
        }
        if (host.paused() && have && host.skill_animation_index(guid) != -1) return false;
    }

    // A skill that moves the character while it runs follows the pointer.
    bool follow_current = false;
    if (!host.paused()) {
        follow_current = true;
    } else if (host.has_current_skill()) {
        if (host.skill_animation_index(host.current_skill()) != -1) return false;
        follow_current = true;
    }
    if (follow_current && host.has_current_skill() && host.current_skill_moves()) {
        if (host.performing_skill()) {
            move_to_mouse(client, host);
            return true;
        }
        if (!host.cast_skill(guid) && host.ai_state() != 0xd) host.set_ai_state(mouse_ai::kStop);
    }

    if (only_on_target) {
        if (client.click_item == 0) {
            if (client.click_character == 0) return false;
            if (!host.is_enemy(client.click_character)) return false;
        } else if (client.click_character != 0) {
            if (!host.is_enemy(client.click_character)) return false;
        }
        if (client.click_item != 0 && !host.item_breakable(client.click_item)) return false;
    }

    const MouseUnit previous_item = host.target_item();
    const MouseUnit previous_target = host.target();
    if (!host.performing_skill_loose() && !host.performing_attack_loose()) {
        if (client.click_item != 0) host.set_target_item(client.click_item);
        if (client.click_character != 0) host.set_target(client.click_character);
    }

    const auto finish = [&](bool cast) {
        if (!cast && host.ai_state() != 0xd) {
            if (only_on_target) return false;
            host.set_ai_state(mouse_ai::kStop);
        }
        return true;
    };

    if (client.click_item != 0 && host.item_breakable(client.click_item)) {
        if (previous_item != client.click_item) host.stop_pathing();
        if (host.performing_skill() || host.in_skill_range(guid) == 0) {
            if (host.performing_skill()) return true;
            return finish(host.cast_skill(guid));
        }
        host.set_target_item(client.click_item);
        host.set_ai_state(mouse_ai::kSkillApproach);
        host.skill_approach_ai(guid);
        if (host.performing_skill()) return true;
        if (host.in_skill_range(guid) == 0) return true;
        if (host.pathing()) return true;
        return finish(host.cast_skill(guid));
    }

    if (client.click_character == 0 || !host.is_enemy(client.click_character)) {
        // Nothing to hit under the click: cast at the pointer.
        if (!repeat) {
            client.suppress_move = false;
            host.set_target(0);
            host.set_target_item(0);
            if (!host.performing_skill_loose()) host.set_ai_state(mouse_ai::kIdle);
        }
        Vec3 point;
        host.find_world_location(point, true);
        host.set_target(0);
        host.set_target_item(0);
        host.set_target_direction(point.x, point.z);
        // Twice, the first result dropped, as the console's skill call also ends.
        host.cast_skill(guid);
        if (host.cast_skill(guid)) return true;
        if (host.ai_state() == 0xd) return true;
        return !only_on_target;
    }

    if (previous_target != client.click_character) host.stop_pathing();
    if (!host.performing_skill() && host.in_skill_range(guid) != 0) {
        host.set_target(client.click_character);
        host.set_ai_state(mouse_ai::kSkillApproach);
        host.skill_approach_ai(guid);
        if (host.performing_skill()) return true;
        if (host.in_skill_range(guid) == 0) return true;
        if (host.pathing()) return true;
        if (!host.cast_skill(guid)) {
            if (only_on_target) return false;
            host.set_ai_state(mouse_ai::kStop);
        }
        return true;
    }
    if (host.performing_skill()) return true;
    return finish(host.cast_skill(guid));
}

// CGameClient::processIngameInput,
void mouse_taken_by_interface(MouseClient& client, const MouseButtons& buttons) {
    if (buttons.left) client.left_held = true;
    if (buttons.right) client.right_held = true;
    client.suppress_move = true;
}

// CGameClient::processIngameInput,
// NOLINTNEXTLINE(readability-function-cognitive-complexity): one function in the original
void mouse_ingame_input(MouseClient& client, MouseHost& host, const MouseButtons& buttons,
                        float delta) {
    if (0.0f < client.move_timer) client.move_timer -= delta;
    if (!host.paused()) {
        const bool hold = host.hold_key();
        host.set_hold_position(hold);
        if (hold) {
            // Standing, the character turns to the pointer.
            Vec3 point;
            host.find_world_location(point, true);
            host.set_target_direction(point.x, point.z);
        }
    }
    if (buttons.left_pressed || buttons.right_pressed) {
        if (!client.left_held) {
            client.move_timer = 0.0f;
            host.set_path_timer(10.0f);
        }
    } else if (host.has_current_skill() && host.performing_skill()) {
        // A held skill is let go once nothing holds it: not the right button with the
        // right button's skill, and not the hotbar key of the skill itself.
        const bool right_holds = buttons.right && host.current_skill() == host.right_skill();
        const bool key_holds = first_held_slot_skill(host) == host.current_skill();
        if (!key_holds && !right_holds) host.attempt_to_stop_skill(!client.left_held);
    }

    // The hotbar's keys. One that acts takes the right button's turn.
    bool slot_acted = false;
    for (int slot = 0; slot < kMouseSlots && !slot_acted; ++slot) {
        const std::int64_t guid = host.slot_skill(slot);
        if (guid != kNoSkill) {
            if (host.slot_key_held(slot)) {
                click_right(client, host, guid, !host.slot_key_pressed(slot), false);
                slot_acted = true;
            } else if (host.slot_key_released(slot) && !client.right_held) {
                client.click_character = 0;
                client.click_item = 0;
            }
        } else if (host.slot_has_item(slot) && host.slot_key_pressed(slot)) {
            host.activate_item_slot(slot);
            slot_acted = true;
        }
    }

    if (!slot_acted) {
        if (buttons.right) {
            click_right(client, host, kNoSkill, client.right_held, false);
            client.right_held = true;
        } else {
            if (client.right_held) {
                client.click_character = 0;
                client.click_item = 0;
            }
            client.right_held = false;
        }
    }

    if (!buttons.left) {
        if (client.suppress_move) client.suppress_move = false;
        if (client.left_held) {
            client.click_character = 0;
            client.click_item = 0;
        }
        client.left_held = false;
    } else {
        if (!buttons.right) {
            // The left button's own skill, when the character has one that is ready; with
            // the hold-position key down it is cast whatever is under the click.
            bool used = false;
            const std::int64_t guid = host.left_skill();
            if (guid != kNoSkill && host.skill_known(guid) && host.skill_ready(guid)) {
                const bool only_on_target =
                    !host.hold_key() ||
                    (client.click_item != 0 && host.item_takeable(client.click_item));
                used = click_right(client, host, guid, client.left_held, only_on_target);
            }
            if (!used) click_left(client, host);
        }
        client.left_held = true;
    }
    if (client.click_character != 0 && !host.unit_alive(client.click_character)) {
        client.click_character = 0;
        host.set_target(0);
    }
    if (client.click_item != 0 && !host.item_selectable(client.click_item)) {
        client.click_item = 0;
        host.set_target_item(0);
    }
}

}  // namespace tl::core
