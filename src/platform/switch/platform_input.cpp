// Switch input: both sticks, the D-pad and the buttons through libnx's pad API, and the software
// keyboard for the names on the creation screen. `padConfigureInput(1,
// HidNpadStyleSet_NpadStandard)` makes handheld, a Joy-Con pair and a Pro Controller one pad.
#include "core/input.h"

#include "core/gamepad.h"
#include "core/run_config.h"

#include <switch.h>
#include <sys/stat.h>

#include <cmath>
#include <cstdint>
#include <string>

namespace tl::core {

namespace {

::PadState g_pad;  // libnx's, not core/gamepad.h's
bool g_ready = false;

// libnx's axes are signed 16-bit, the 360 pad's range, so `core/gamepad.h`'s `kStickMax`,
// deadzones and rescale apply unchanged.

// DIVERGENCE (platform): two fingers on the handheld's screen zoom, under the free camera
// (render/touch_camera.h): the change in their distance in dolly units, spreading closer,
// scaled as the phones' touch pad scales it (60 over the screen's short side, 720).
float touch_pinch() {
    static bool ready = false;
    static float last = 0.0f;
    if (!ready) {
        hidInitializeTouchScreen();
        ready = true;
    }
    HidTouchScreenState touch{};
    if (hidGetTouchScreenStates(&touch, 1) == 0 || touch.count < 2) {
        last = 0.0f;
        return 0.0f;
    }
    const float dx =
        static_cast<float>(touch.touches[0].x) - static_cast<float>(touch.touches[1].x);
    const float dy =
        static_cast<float>(touch.touches[0].y) - static_cast<float>(touch.touches[1].y);
    const float distance = std::sqrt(dx * dx + dy * dy);
    const float delta = last > 0.0f ? -(distance - last) * (60.0f / 720.0f) : 0.0f;
    last = distance;
    return delta;
}

}  // namespace

InputState poll_platform_input() {
    if (!g_ready) {
        padConfigureInput(1, HidNpadStyleSet_NpadStandard);
        padInitializeDefault(&g_pad);
        g_ready = true;
    }
    padUpdate(&g_pad);

    InputState in;

    // The 360's own radial deadzone and rescale, on the raw axes
    // (with 0x1EA9, from the pad poll).
    const HidAnalogStickState stick = padGetStickPos(&g_pad, 0);
    std::int16_t raw_x = static_cast<std::int16_t>(stick.x);
    std::int16_t raw_y = static_cast<std::int16_t>(stick.y);
    tl::core::filter_stick(raw_x, raw_y, tl::core::kLeftStickDeadzone);
    float x = static_cast<float>(raw_x) / kStickMax;
    float y = static_cast<float>(raw_y) / kStickMax;
    in.stick_x = x;
    in.stick_y = y;

    const u64 held = padGetButtons(&g_pad);
    if (held & HidNpadButton_Left) x -= 1.0f, in.dpad_x -= 1.0f;
    if (held & HidNpadButton_Right) x += 1.0f, in.dpad_x += 1.0f;
    if (held & HidNpadButton_Down) y -= 1.0f;
    if (held & HidNpadButton_Up) y += 1.0f;

    float m = std::sqrt(x * x + y * y);
    if (m > 1.0f) {
        x /= m;
        y /= m;
    }

    in.move_x = x;
    in.move_y = y;

    // The right stick zooms the camera, which is all the console does with it.
    // Its own deadzone, 8689 (0x21F1), from the same poll.
    const HidAnalogStickState look = padGetStickPos(&g_pad, 1);
    std::int16_t look_raw_x = static_cast<std::int16_t>(look.x);
    std::int16_t look_raw_y = static_cast<std::int16_t>(look.y);
    tl::core::filter_stick(look_raw_x, look_raw_y, tl::core::kRightStickDeadzone);
    in.zoom = static_cast<float>(look_raw_y) / kStickMax;
    in.look_x = static_cast<float>(look_raw_x) / kStickMax;
    in.zoom_delta = touch_pinch();

    in.confirm = (held & HidNpadButton_A) != 0;
    // X is Attack and A is Interact, which is the console's own map by label
    // (`howto_xenon.layout`; the DIVERGENCE note below covers what "by label"
    // costs on a Nintendo pad).
    in.attack = (held & HidNpadButton_X) != 0;
    in.skip = (held & HidNpadButton_B) != 0;
    in.quit = (held & HidNpadButton_Plus) != 0;
    in.inventory = (held & HidNpadButton_Minus) != 0;
    in.menu_prev = (held & HidNpadButton_ZL) != 0;
    in.menu_next = (held & HidNpadButton_ZR) != 0;
    in.tab_prev = (held & HidNpadButton_L) != 0;
    in.tab_next = (held & HidNpadButton_R) != 0;
    in.action_y = (held & HidNpadButton_Y) != 0;
    in.map_toggle = (held & HidNpadButton_StickR) != 0;
    in.fps_toggle = (held & HidNpadButton_StickL) != 0;
    // Both sticks clicked together: the frame on screen goes to the card as `shot_<n>.png`,
    // numbered past the shots already there. A reporting tool, not part of the game.
    {
        static bool both_held = false;
        static int shots = -1;
        const bool both = (held & HidNpadButton_StickR) != 0 && (held & HidNpadButton_StickL) != 0;
        if (both) in.map_toggle = in.fps_toggle = false;
        if (both && !both_held && tl::core::debug_tools()) {
            const auto shot_path = [](int n) {
                return "sdmc:/switch/torchlight/shot_" + std::to_string(n) + ".png";
            };
            if (shots < 0) {
                shots = 0;
                struct stat st;
                while (stat(shot_path(shots).c_str(), &st) == 0) ++shots;
            }
            tl::core::queue_frame_shot(shot_path(shots++));
        }
        both_held = both;
    }
    // The D-pad, which nothing else on the game screen uses: Up cycles the pet's stance and Down
    // sends it shopping.
    in.pet_stance = (held & HidNpadButton_Up) != 0;
    in.pet_town = (held & HidNpadButton_Down) != 0;
    return in;
}

// `XamShowKeyboardUI`'s counterpart: libnx's software keyboard applet with the
// console's title and guide line, capped at `max_chars`. Blocks until it closes, as on the 360.
bool text_entry(const std::string& title, const std::string& guide, std::string& text,
                unsigned max_chars) {
    if (take_scripted_text(text)) return true;
    SwkbdConfig keyboard;
    if (R_FAILED(swkbdCreate(&keyboard, 0))) return false;
    swkbdConfigMakePresetDefault(&keyboard);
    swkbdConfigSetHeaderText(&keyboard, title.c_str());
    swkbdConfigSetGuideText(&keyboard, guide.c_str());
    swkbdConfigSetInitialText(&keyboard, text.c_str());
    swkbdConfigSetStringLenMax(&keyboard, max_chars);
    char out[128] = {};
    const Result shown = swkbdShow(&keyboard, out, sizeof(out));
    swkbdClose(&keyboard);
    if (R_FAILED(shown)) return false;
    text = out;
    return true;
}

// The software keyboard is the applet's (text_entry); nothing is typed into the window.
bool typing_in_window() { return false; }
std::string take_platform_typed_text() { return {}; }

}  // namespace tl::core
