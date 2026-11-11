// The PC's mouse in the game: what a left or right button does to the player's
// character. `CGameClient::clickLeft`, `clickRight`,
// `moveToMouse` and the pointer's share of
// `CGameClient::processIngameInput`.
//
// The console has no pointer: its client reads the pad and
// none of this is in it. These run only under the mouse control scheme, and the
// pad's path is untouched by them.
//
// Decisions only. Everything the original asks of the character, the level and
// the interface goes through `MouseHost`, which the game implements.
#pragma once

#include "core/collision.h"

#include <cstdint>

namespace tl::core {

/// A character or an item the client holds a pointer to; 0 is none. The two
/// kinds have separate numbering, the host's own.
using MouseUnit = std::uint64_t;

/// "No skill": the GUID the original passes for the right button's own skill.
inline constexpr std::int64_t kNoSkill = -1;

/// The client's own state for the pointer.
struct MouseClient {
    /// `+0x98`: a click that already acted must not also walk. Set when the
    /// interface took the click or an attack went out with the button still
    /// down; cleared when the left button comes up.
    bool suppress_move = false;
    /// `+0x99` / `+0x9a`: the button was down at the last dispatch.
    bool left_held = false;
    bool right_held = false;
    /// `+0x80`: seconds until a held left button asks for a destination again.
    float move_timer = 0.0f;
    /// `+0x1d8` / `+0x1e8`: what the pointer is over this frame.
    MouseUnit over_character = 0;
    MouseUnit over_item = 0;
    /// `+0x1c8` / `+0x1f8`: what the held button was pressed on.
    MouseUnit click_character = 0;
    MouseUnit click_item = 0;
};

/// The AI states the clicks set (`CPlayer::setAIState`).
namespace mouse_ai {
inline constexpr int kIdle = 0;
inline constexpr int kStop = 2;
inline constexpr int kHunt = 4;
inline constexpr int kGetItem = 7;
inline constexpr int kApproach = 9;
inline constexpr int kSkillApproach = 0xf;
}  // namespace mouse_ai

/// What the clicks ask of the game. Every call is one the original makes, in
/// the original's order.
class MouseHost {
public:
    virtual ~MouseHost() = default;

    // The client and the interface.
    virtual bool paused() = 0;
    /// `CGameClient::findWorldLocation`: where the pointer is on the ground;
    /// `raised` lifts the plane to the height a blow or a skill is aimed at. False when
    /// the pointer is over the interface or off the view.
    virtual bool find_world_location(Vec3& out, bool raised) = 0;
    /// The hold-position key (`KEYMAP_HOLDPOS`) is down.
    virtual bool hold_key() = 0;
    /// The right button is down (`CMouseManager::buttonHeld(1)`).
    virtual bool right_button() = 0;
    /// The hotbar's keys (`KSkillSlotsKeys`, ten of them): the skill a slot holds, or
    /// `kNoSkill` when it holds none the character knows; whether it holds an item; and
    /// its key this frame.
    virtual std::int64_t slot_skill(int slot) = 0;
    virtual bool slot_has_item(int slot) = 0;
    virtual bool slot_key_held(int slot) = 0;
    virtual bool slot_key_pressed(int slot) = 0;
    virtual bool slot_key_released(int slot) = 0;
    /// `CGameUI::activateItemSlot(slot, true)`.
    virtual void activate_item_slot(int slot) = 0;
    /// `CGameUI`'s cursor state (`+0x12fc`) and the skill it is waiting to aim (`+0xb0`).
    virtual int cursor_state() = 0;
    virtual void set_cursor_state(int state) = 0;
    virtual void set_pending_skill(std::int64_t guid) = 0;
    virtual void flush_interface_input() = 0;
    virtual void set_right_button_pressed() = 0;
    virtual bool equipment_tooltip_visible() = 0;

    // The player's character.
    virtual bool player_alive() = 0;
    virtual bool player_enabled() = 0;
    virtual Vec3 player_position() = 0;
    /// `+0x330`.
    virtual int ai_state() = 0;
    /// `CPlayer::setAIState`.
    virtual void set_ai_state(int state) = 0;
    /// `+0x266`.
    virtual bool hold_position() = 0;
    virtual void set_hold_position(bool hold) = 0;
    /// `+0x264`.
    virtual bool pathing() = 0;
    /// `+0x278`: seconds since the route last ended.
    virtual void set_path_timer(float seconds) = 0;
    /// `+0x4d0` or `+0x4d1` was set; both are cleared.
    virtual bool take_attack_started() = 0;
    virtual bool performing_skill() = 0;
    virtual bool performing_skill_loose() = 0;
    virtual bool performing_attack_loose() = 0;
    /// `+0x340` / `+0x350`.
    virtual MouseUnit target() = 0;
    virtual MouseUnit target_item() = 0;
    virtual void set_target(MouseUnit character) = 0;
    virtual void set_target_item(MouseUnit item) = 0;
    virtual void set_target_direction(float x, float z) = 0;
    virtual void stop_pathing() = 0;
    virtual void set_destination(float x, float z) = 0;
    /// `CCharacter::inAttackRange(0)` of the current target.
    virtual bool in_attack_range() = 0;
    virtual void attack() = 0;
    /// `CCharacter::huntAI(0, level)`, run once at the click.
    virtual void hunt_ai() = 0;
    /// The item branch's tail: the player, or with `by_pet` the pet, stops if `previous`
    /// is not its target item, takes `item` as the target, enters state 7, stops and runs
    /// `getItemAI` once.
    virtual void fetch_item(MouseUnit item, bool by_pet, MouseUnit previous) = 0;
    /// A pet that can be sent: there is one, not near death, alive, and not on its way to
    /// or in town (states 0x29, 0x2a).
    virtual bool pet_can_fetch() = 0;

    // Characters and items by handle.
    virtual bool unit_alive(MouseUnit character) = 0;
    /// Shown (`+0x81`) and enabled.
    virtual bool unit_present(MouseUnit character) = 0;
    virtual bool is_enemy(MouseUnit character) = 0;
    /// `ISA(0x83)`: someone to talk to.
    virtual bool unit_interactive(MouseUnit character) = 0;
    virtual Vec3 unit_position(MouseUnit character) = 0;
    /// `ISA(0x1d)`: something to break.
    virtual bool item_breakable(MouseUnit item) = 0;
    /// `ISA(0x1f)`: something to pick up.
    virtual bool item_takeable(MouseUnit item) = 0;
    /// Usable (`+0x198`) and enabled.
    virtual bool item_usable(MouseUnit item) = 0;
    /// Still clickable: `+0x1f0` set and `+0x1f1` clear.
    virtual bool item_selectable(MouseUnit item) = 0;

    // Skills.
    /// The character has a skill manager (`+0x1c8`).
    virtual bool has_skills() = 0;
    virtual bool skill_known(std::int64_t guid) = 0;
    /// `+0x3b0`: the right button's skill. `+0x3c0`: the left button's, or `kNoSkill`.
    virtual std::int64_t right_skill() = 0;
    virtual std::int64_t left_skill() = 0;
    /// The left button's skill is off cooldown and paid for (`getSkillIsCooling`,
    /// `getManaCost`, `getManaCostOT`).
    virtual bool skill_ready(std::int64_t guid) = 0;
    virtual int skill_target_type(std::int64_t guid) = 0;
    virtual int skill_animation_index(std::int64_t guid) = 0;
    /// The current skill (`+0x398`) exists; its GUID; and its `+0x68` flag.
    virtual bool has_current_skill() = 0;
    virtual std::int64_t current_skill() = 0;
    virtual bool current_skill_moves() = 0;
    virtual bool cast_skill(std::int64_t guid) = 0;
    /// `CPlayer::attemptToStopPlayerSkill`.
    virtual void attempt_to_stop_skill(bool now) = 0;
    /// `CCharacter::inSkillRange`: 0 when in range.
    virtual int in_skill_range(std::int64_t guid) = 0;
    /// `CCharacter::skillApproachAI(0, level, guid)`, run once at the click.
    virtual void skill_approach_ai(std::int64_t guid) = 0;
};

/// `CGameClient::moveToMouse`.
void move_to_mouse(MouseClient& client, MouseHost& host);

/// `CGameClient::clickLeft`.
void click_left(MouseClient& client, MouseHost& host);

/// `CGameClient::clickRight(guid, repeat, only_on_target)`. `repeat` is the
/// button already down at the last dispatch; `only_on_target` refuses a cast with no enemy
/// or breakable under the click. True when the click was used.
bool click_right(MouseClient& client, MouseHost& host, std::int64_t guid, bool repeat,
                 bool only_on_target);

/// The buttons this frame.
struct MouseButtons {
    bool left = false;
    bool right = false;
    bool left_pressed = false;
    bool right_pressed = false;
};

/// The interface took this frame's input (`CGameUI::processInput` false): the buttons are
/// remembered as held and the walk suppressed (`processIngameInput`).
void mouse_taken_by_interface(MouseClient& client, const MouseButtons& buttons);

/// How many hotbar keys there are.
inline constexpr int kMouseSlots = 10;

/// The pointer's and the hotbar keys' share of `processIngameInput` for a player who is
/// alive and enabled: the destination timer, the hold-position key,
/// a held skill let go, the hotbar keys, then the right and the left button.
void mouse_ingame_input(MouseClient& client, MouseHost& host, const MouseButtons& buttons,
                        float delta);

}  // namespace tl::core
