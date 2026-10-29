#include <array>
#include <cmath>
#include <cstring>
#include "core/input.h"
#include "core/touch_pad.h"
extern "C" bool tl_ios_text_entry(const char* title, const char* guide, const char* initial,
                                  unsigned max_chars, char* output, size_t output_size);
namespace tl::core {
InputState poll_platform_input() {
    const TouchPad::State p = touch_pad().take();
    InputState in;
    in.stick_x = p.lx;
    in.stick_y = -p.ly;
    in.zoom = 0.0f;
    in.zoom_delta = p.pinch_dolly;
    in.camera_pitch = -p.ry;
    in.look_x = p.rx;
    float x = p.lx, y = -p.ly;
    const std::uint32_t b = p.held | p.pressed;
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
    const float magnitude = std::sqrt(x * x + y * y);
    if (magnitude > 1.0f) {
        x /= magnitude;
        y /= magnitude;
    }
    in.move_x = x;
    in.move_y = y;
    return in;
}
bool text_entry(const std::string& title, const std::string& guide, std::string& text,
                unsigned max_chars) {
    std::array<char, 1024> output{};
    if (!tl_ios_text_entry(title.c_str(), guide.c_str(), text.c_str(), max_chars, output.data(),
                           output.size())) {
        return false;
    }
    text.assign(output.data(), ::strnlen(output.data(), output.size()));
    return true;
}
// The system keyboard is UIKit's (text_entry); nothing is typed into the window.
bool typing_in_window() { return false; }
std::string take_platform_typed_text() { return {}; }
}  // namespace tl::core
