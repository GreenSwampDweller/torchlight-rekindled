#include "core/input.h"
#include "core/touch_pad.h"
#include "platform/ios/ios_app.h"
#include <cmath>
#include <array>
#include <cstring>
extern "C" bool tl_ios_text_entry(const char* title, const char* guide, const char* initial,
                                  unsigned max_chars, char* output, size_t output_size);
namespace tl::core {
InputState poll_platform_input() {
    TouchPad& pad = touch_pad();
    const TouchPad::State p = pad.take();
    float controller_lx = 0.0f, controller_ly = 0.0f;
    float controller_rx = 0.0f, controller_ry = 0.0f;
    std::uint32_t controller_buttons = 0;
    bool controller_connected = false;
    tl_ios_read_controller(&controller_lx, &controller_ly, &controller_rx, &controller_ry,
                          &controller_buttons, &controller_connected);
    // Make controller presses take over immediately; after disconnect the
    // touch overlay returns on the user's next tap.
    static bool was_connected = false;
    if (controller_connected) {
        pad.set_visible(false);
        pad.release_all();
    } else if (was_connected) {
        // Return to the touch-first behavior; the next tap reveals the overlay.
        pad.set_visible(false);
        pad.release_all();
    }
    was_connected = controller_connected;
    InputState in;
    in.controller_connected = controller_connected;
    in.touch_layout_changed = p.layout_changed;
    in.touch_layout_offsets = p.layout_offsets;
    in.touch_tap = p.tap;
    in.touch_x = p.tap_x;
    in.touch_y = p.tap_y;
    in.stick_x = controller_connected ? controller_lx : p.lx;
    in.stick_y = controller_connected ? controller_ly : -p.ly;
    in.zoom = 0.0f;
    in.zoom_delta = p.pinch_dolly;
    in.camera_pitch = controller_connected ? -controller_ry : -p.ry;
    in.look_x = controller_connected ? controller_rx : p.rx;
    float x = in.stick_x, y = in.stick_y;
    const std::uint32_t b = p.held | p.pressed | controller_buttons;
    if (b & TouchPad::kLeft) x -= 1.0f, in.dpad_x -= 1.0f;
    if (b & TouchPad::kRight) x += 1.0f, in.dpad_x += 1.0f;
    if (b & TouchPad::kDown) y -= 1.0f;
    if (b & TouchPad::kUp) y += 1.0f;
    in.confirm = (b & TouchPad::kA) != 0;
    in.skip = (b & TouchPad::kB) != 0;
    in.attack = (b & TouchPad::kX) != 0;
    in.action_y = (b & TouchPad::kY) != 0;
    in.quit = (b & TouchPad::kStart) != 0;
    in.inventory = (b & TouchPad::kSelect) != 0;
    in.menu_prev = (b & TouchPad::kL2) != 0;
    in.menu_next = (b & TouchPad::kR2) != 0;
    in.tab_prev = (b & TouchPad::kL1) != 0;
    in.tab_next = (b & TouchPad::kR1) != 0;
    in.map_toggle = (b & TouchPad::kThumbR) != 0;
    in.fps_toggle = (b & TouchPad::kThumbL) != 0;
    in.pet_stance = (b & TouchPad::kUp) != 0;
    in.pet_town = (b & TouchPad::kDown) != 0;
    const float magnitude = std::sqrt(x*x + y*y);
    if (magnitude > 1.0f) { x /= magnitude; y /= magnitude; }
    in.move_x = x;
    in.move_y = y;
    return in;
}
bool text_entry(const std::string& title, const std::string& guide, std::string& text, unsigned max_chars) {
    std::array<char, 1024> output{};
    if (!tl_ios_text_entry(title.c_str(), guide.c_str(), text.c_str(), max_chars,
                           output.data(), output.size())) {
        return false;
    }
    text.assign(output.data(), ::strnlen(output.data(), output.size()));
    return true;
}
}  // namespace tl::core
