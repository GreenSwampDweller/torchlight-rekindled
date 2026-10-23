// The per-frame input snapshot, and the platform's way of producing one:
// polled, a plain struct, because libnx hands out the pad's state once a frame
// and the desktop is read the same way.
//
// Everything here is a pad, as on the console: the left stick puts a
// destination ahead and routes to it, the target selector
// decides what the face buttons act, the right stick zooms,
// and every menu is walked with the D-pad. The desktop reads
// an XInput pad, or a keyboard laid out like one.
#pragma once

#include <string>
#include <array>

namespace tl::core {

/// What the player is asking for this frame.
///
/// The move axes are screen-relative and already deadzoned and clamped to the
/// unit disc, so a diagonal is not faster than a cardinal - the classic bug of
/// summing two normalised axes. `move_y` is positive *away* from the viewer,
/// matching the direction the camera looks.
struct InputState {
    /// iOS: a hardware controller is connected; the HUD can yield screen space.
    bool controller_connected = false;
    bool touch_layout_changed = false;
    std::array<float, 32> touch_layout_offsets{};
    bool touch_tap = false;
    float touch_x = 0.0f, touch_y = 0.0f;
    float move_x = 0.0f;
    float move_y = 0.0f;

    /// The right stick's vertical deflection, -1 to 1 after the console's
    /// deadzone (`kRightStickDeadzone`), up positive. It only
    /// zooms the camera (`zoom_dolly()`).
    float zoom = 0.0f;
    /// Direct dolly delta from a pinch gesture; spreading fingers is negative (zoom in).
    float zoom_delta = 0.0f;
    /// Independent camera pitch axis, positive up. iOS uses right-stick vertical here.
    float camera_pitch = 0.0f;

    /// The sticks alone, without the D-pad folded in: the left stick's two
    /// axes (up positive) and the right stick's horizontal one, deadzoned like
    /// the rest. The skill screen needs the difference - its handler
    /// moves its cursor on the eight stick directions
    /// (button codes 17-24) and gives the D-pad's up and down to the two skill
    /// sets. On a desktop keyboard the movement keys count as the left stick.
    float stick_x = 0.0f;
    float stick_y = 0.0f;
    /// The D-pad's left and right alone, -1 and +1: the housing screens'
    /// button codes 3 and 4, which an item tray gives to item compare.
    /// On a desktop keyboard, C.
    float dpad_x = 0.0f;
    float look_x = 0.0f;

    /// A: interact, confirm, move a line of dialogue on early. Space or Enter on
    /// a desktop keyboard.
    bool confirm = false;

    /// Attack: X on the console (`howto_xenon.layout`: X = Attack, A = Interact;
    /// buttons 7 and 5). Held, the character stands, aims with
    /// the stick and swings in place at whatever is in front
    /// (`core/attack_button.h`).
    bool attack = false;

    /// Skip the cutscene (`Skip Cutscene`): B on the console, Backspace on the
    /// desktop. Not Escape, which quits.
    bool skip = false;

    /// The player asked to stop playing: Plus on the console, Escape on the
    /// desktop. The window closing is handled separately, by the run loop.
    bool quit = false;

    /// The player asked to toggle inventory: Minus on the console, 'I' / Tab / '-' on
    /// the desktop.
    bool inventory = false;

    /// The housing screen's ring, Quest / Inventory / Stats: LT / RT
    /// (`mastermenu_xenon.layout`'s `lefttrigger`/`righttrigger` hints).
    bool menu_prev = false;
    bool menu_next = false;

    /// The inventory screen's six item-category filters: LB / RB
    /// (`inventorymenu_xenon.layout`'s `leftbumper`/`rightbumper` hints).
    bool tab_prev = false;
    bool tab_next = false;

    /// Secondary action: Y button on controller ('Y' on keyboard, e.g. remove stat point)
    bool action_y = false;

    /// Minimap / Automap toggle: 'M' on desktop; Right Stick Click (R3) on Switch
    bool map_toggle = false;

    /// PORT: the frame-rate overlay's toggle, kept in config.txt (core/run_config.h):
    /// Left Stick Click (L3), 'F' on desktop. The console's controls page names the
    /// right stick only (`howto_xenon.layout`'s `rstick`); L3 is the port's to give.
    bool fps_toggle = false;

    /// Automap zoom: '+' / ']' zoom in, '-' / '[' zoom out
    bool map_zoom_in = false;
    bool map_zoom_out = false;

    /// Send the pet to town with its bag: 'P' on the desktop, the D-pad's Down
    /// on the console (`CGameUI::togglePet`, `handle_TogglePet`). The
    /// original's toggle also calls a pet back; this only sends.
    bool pet_town = false;

    /// Step the pet's stance (Aggressive, Defensive, Passive): 'O' on the
    /// desktop, the D-pad's Up on the console - one button for the tray of
    /// three a pad cannot point at.
    bool pet_stance = false;

    /// Magnitude of the stick, 0 to 1. Torchlight's units author both a
    /// WALKINGSPEED and a RUNNINGSPEED, so a partly-pushed stick has somewhere
    /// meaningful to sit.
    float magnitude() const;

    bool moving() const;
};

/// Read the current state of whatever the platform has. Call once per frame,
/// after pump_events().
///
/// While a pad script is running (`set_pad_script`) this returns the script's
/// frame instead of the platform's, on both targets.
InputState poll_input();

/// Whether L3 (`fps_toggle`) was pressed since the last call: once per press,
/// whichever screen polled the pad. The FPS overlay takes it.
bool take_fps_toggle();

/// The platform's own pad, with no script in the way. Each target defines it
/// in `platform/<target>/platform_input.cpp`.
InputState poll_platform_input();

/// Drive `poll_input()` from a script rather than a pad, for verification: a
/// comma-separated list of presses - `up`, `down`, `left`, `right` (the left
/// stick), `dup`, `ddown`, `a`, `b`, `x`, `y`, `start`, `back`, `lt`, `rt`,
/// `lb`, `rb`, `rs_up`, `rs_down` and `wait` - each held a few frames and
/// released a few more. `text=<words>` queues an answer for the next
/// `text_entry()`; `shot=<png>` saves the next frame drawn. Empty stops it.
void set_pad_script(const std::string& script);

/// Whether a pad script is still running.
bool pad_script_running();

/// The next `text=` answer the script queued, if any.
bool take_scripted_text(std::string& text);

/// A `shot=<png>` step the script has reached, if any: the run loop that
/// polled this frame writes what it just drew to that file. Taken once.
bool take_scripted_shot(std::string& png);

/// Ask the loop that is running to save its next frame to `png`, exactly as a
/// pad script's `shot=` does. The console's own way to show what it draws: both
/// sticks clicked together save `shot_<n>.png` beside the log.
void queue_frame_shot(const std::string& png);

/// Ask the player for a line of text on the platform's own keyboard, the way
/// `CNewGameMenu` asks for the two names with `XamShowKeyboardUI`:
/// a title, a line of guidance, what is there already, and
/// at most `max_chars` characters. Blocks until the keyboard closes.
///
/// False when the player cancelled or the platform has no keyboard to show;
/// `text` is then left as it was. The Switch opens its software keyboard
/// applet; the desktop has no such thing and answers only from a pad script.
bool text_entry(const std::string& title, const std::string& guide, std::string& text,
                unsigned max_chars);

/// A fixed, deterministic stick script, for verifying movement without a
/// person holding a controller: a square - forward, right, back, left, 1.5
/// seconds each - which returns to where it started in open ground and does not
/// when something is in the way.
InputState scripted_walk(float elapsed_seconds);

}  // namespace tl::core
