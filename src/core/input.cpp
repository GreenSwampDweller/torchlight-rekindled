#include "core/input.h"

#include "core/log.h"

#include <cmath>
#include <cstdlib>
#include <deque>
#include <string>
#include <utility>

namespace tl::core {

namespace {

/// The presses still to come, first at the front.
std::deque<std::string> g_script;
std::deque<std::string> g_script_text;
std::string g_pending_shot;
std::string g_script_typed;
bool g_typing = false;
/// Frames into the current step. A press is held for `kHeld` frames and then
/// released for `kReleased`, so a screen that acts on the edge sees exactly one.
int g_step_frame = 0;
constexpr int kHeld = 3;
constexpr int kReleased = 6;

bool g_mouse_controls = false;

/// The scripted pointer: where it is and what is held, kept across steps.
struct ScriptPointer {
    bool present = false;
    float x = 0.5f;
    float y = 0.5f;
    bool left = false;
    bool right = false;
    bool shift = false;
    bool ctrl = false;
    /// `mouse_on=<unit>`: the pointer follows that unit until the next `mouse=`.
    std::string unit;
};
ScriptPointer g_script_pointer;

/// A step that changes the scripted pointer's kept state; applied once, when reached.
void apply_pointer_step(const std::string& press) {
    ScriptPointer& p = g_script_pointer;
    if (press.rfind("mouse=", 0) == 0) {
        const std::string at = press.substr(6);
        const size_t colon = at.find(':');
        if (colon == std::string::npos) return;
        p.present = true;
        p.unit.clear();
        p.x = std::strtof(at.substr(0, colon).c_str(), nullptr);
        p.y = std::strtof(at.substr(colon + 1).c_str(), nullptr);
    } else if (press.rfind("mouse_on=", 0) == 0) {
        p.present = true;
        p.unit = press.substr(9);
    } else if (press == "lmb_down") {
        p.present = p.left = true;
    } else if (press == "lmb_up") {
        p.left = false;
    } else if (press == "rmb_down") {
        p.present = p.right = true;
    } else if (press == "rmb_up") {
        p.right = false;
    } else if (press == "shift_down") {
        p.shift = true;
    } else if (press == "shift_up") {
        p.shift = false;
    } else if (press == "ctrl_down") {
        p.ctrl = true;
    } else if (press == "ctrl_up") {
        p.ctrl = false;
    }
}

/// The kept pointer over a frame of the script.
void add_script_pointer(InputState& in) {
    const ScriptPointer& p = g_script_pointer;
    if (!p.present) return;
    in.pointer = true;
    in.pointer_x = p.x;
    in.pointer_y = p.y;
    in.pointer_left = in.pointer_left || p.left;
    in.pointer_right = in.pointer_right || p.right;
    in.hold_position = in.hold_position || p.shift;
    in.control = in.control || p.ctrl;
}

InputState scripted_frame(const std::string& press) {
    InputState in;
    // `up`..`right` are the left stick; `dup`/`ddown` the D-pad's two.
    if (press == "up") in.move_y = in.stick_y = 1.0f;
    else if (press == "down") in.move_y = in.stick_y = -1.0f;
    else if (press == "left") in.move_x = in.stick_x = -1.0f;
    else if (press == "right") in.move_x = in.stick_x = 1.0f;
    else if (press == "dleft") in.dpad_x = -1.0f;
    else if (press == "dright") in.dpad_x = 1.0f;
    else if (press == "dup") in.pet_stance = true;
    else if (press == "ddown") in.pet_town = true;
    else if (press == "a") in.confirm = true;
    else if (press == "b") in.skip = true;
    else if (press == "x") in.attack = true;
    else if (press == "y") in.action_y = true;
    else if (press == "start") in.quit = true;
    else if (press == "back") in.inventory = true;
    else if (press == "lt") in.menu_prev = true;
    else if (press == "rt") in.menu_next = true;
    else if (press == "lb") in.tab_prev = true;
    else if (press == "rb") in.tab_next = true;
    else if (press == "rs_up") in.zoom = 1.0f;
    else if (press == "rs_down") in.zoom = -1.0f;
    else if (press == "lmb") in.pointer_left = true;
    else if (press == "rmb") in.pointer_right = true;
    else if (press == "mmb") in.pointer_middle = true;
    else if (press == "pc_stats") in.pc_keys = pc_key::kStats;
    else if (press == "pc_skills") in.pc_keys = pc_key::kSkills;
    else if (press == "pc_quests") in.pc_keys = pc_key::kQuests;
    else if (press == "pc_pet") in.pc_keys = pc_key::kPet;
    else if (press == "pc_inventory") in.pc_keys = pc_key::kInventory;
    else if (press == "pc_automap") in.pc_keys = pc_key::kAutomap;
    else if (press == "pc_closeall") in.pc_keys = pc_key::kCloseAll;
    else if (press == "pc_weaponset") in.pc_keys = pc_key::kWeaponSet;
    else if (press == "pc_zoomin") in.pc_keys = pc_key::kZoomIn;
    else if (press == "pc_zoomout") in.pc_keys = pc_key::kZoomOut;
    // `key1`..`key9` are slots 0..8, `key0` the tenth.
    if (press.size() == 4 && press.rfind("key", 0) == 0 && press[3] >= '0' && press[3] <= '9') {
        const int digit = press[3] - '0';
        in.slot_keys = static_cast<std::uint16_t>(1U << (digit == 0 ? 9 : digit - 1));
    }
    return in;
}

}  // namespace

void set_mouse_controls(bool on) { g_mouse_controls = on; }

const std::string& scripted_pointer_unit() { return g_script_pointer.unit; }

bool mouse_controls() { return g_mouse_controls; }

void set_pad_script(const std::string& script) {
    g_script_pointer = ScriptPointer{};
    g_script.clear();
    g_script_text.clear();
    g_step_frame = 0;
    size_t at = 0;
    while (at <= script.size()) {
        size_t comma = script.find(',', at);
        if (comma == std::string::npos) comma = script.size();
        std::string token = script.substr(at, comma - at);
        at = comma + 1;
        if (token.empty()) continue;
        if (token.rfind("text=", 0) == 0) {
            g_script_text.push_back(token.substr(5));
            continue;
        }
        // `<press> x<N>`: the press N times over.
        int times = 1;
        const size_t repeat = token.rfind(" x");
        if (repeat != std::string::npos && repeat + 2 < token.size() &&
            token.find_first_not_of("0123456789", repeat + 2) == std::string::npos) {
            times = static_cast<int>(std::strtol(token.c_str() + repeat + 2, nullptr, 10));
            token.resize(repeat);
        }
        for (int n = 0; n < times; ++n) g_script.push_back(token);
    }
    if (!g_script.empty() || !g_script_text.empty()) {
        TL_LOG_INFO("pad script: %zu press(es), %zu text answer(s)", g_script.size(),
                    g_script_text.size());
    }
}

bool pad_script_running() { return !g_script.empty(); }

void queue_frame_shot(const std::string& png) { g_pending_shot = png; }

bool take_scripted_shot(std::string& png) {
    if (g_pending_shot.empty()) return false;
    png = g_pending_shot;
    g_pending_shot.clear();
    return true;
}

bool take_scripted_text(std::string& text) {
    if (g_script_text.empty()) return false;
    text = g_script_text.front();
    g_script_text.pop_front();
    return true;
}

namespace {
bool g_fps_toggle_held = false;
int g_fps_toggles = 0;

// L3's press counted here, whichever screen polled the pad: the FPS overlay is
// every screen's, and its step (the renderer's, once a frame) takes the count.
InputState noticing_fps_toggle(const InputState& in) {
    if (in.fps_toggle && !g_fps_toggle_held) ++g_fps_toggles;
    g_fps_toggle_held = in.fps_toggle;
    return in;
}
}  // namespace

bool take_fps_toggle() {
    if (g_fps_toggles == 0) return false;
    g_fps_toggles = 0;
    return true;
}

InputState poll_input() {
    if (g_script.empty()) return noticing_fps_toggle(poll_platform_input());
    const std::string press = g_script.front();
    InputState in;
    if (g_step_frame == 0) {
        TL_LOG_INFO("pad script: %s", press.c_str());
        if (press.rfind("shot=", 0) == 0) g_pending_shot = press.substr(5);
        if (press.rfind("type=", 0) == 0) g_script_typed += press.substr(5);
        apply_pointer_step(press);
    }
    if (g_step_frame < kHeld) in = scripted_frame(press);
    if (g_step_frame == 0 && press.rfind("wheel=", 0) == 0) {
        in.pointer_wheel = static_cast<int>(std::strtol(press.c_str() + 6, nullptr, 10));
    }
    add_script_pointer(in);
    if (++g_step_frame >= kHeld + kReleased) {
        g_step_frame = 0;
        g_script.pop_front();
        if (g_script.empty()) TL_LOG_INFO("pad script: done");
    }
    return in;
}

void set_typing(bool on) { g_typing = on; }

bool typing() { return g_typing; }

std::string take_typed_text() {
    std::string typed = std::move(g_script_typed);
    g_script_typed.clear();
    typed += take_platform_typed_text();
    return typed;
}

float InputState::magnitude() const {
    const float m = std::sqrt(move_x * move_x + move_y * move_y);
    return m > 1.0f ? 1.0f : m;
}

bool InputState::moving() const { return magnitude() > 0.0f; }

InputState scripted_walk(float elapsed_seconds) {
    // Full deflection throughout, so the speed is RUNNINGSPEED exactly; three
    // seconds a leg (about 18.75 units) reaches a level's scenery.
    constexpr float kLeg = 3.0f;
    constexpr float kLegs = 4.0f;

    InputState in;
    if (!(elapsed_seconds >= 0.0f)) return in;

    const float cycle =
        elapsed_seconds - kLeg * kLegs * std::floor(elapsed_seconds / (kLeg * kLegs));
    const int leg = static_cast<int>(cycle / kLeg);
    switch (leg) {
        case 0: in.move_y = 1.0f; break;
        case 1: in.move_x = 1.0f; break;
        case 2: in.move_y = -1.0f; break;
        default: in.move_x = -1.0f; break;
    }
    return in;
}

}  // namespace tl::core
