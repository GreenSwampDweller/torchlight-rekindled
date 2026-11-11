// The renderer: the Pause Menu and Options under the mouse controls.
#include "render/mouse_game.h"

namespace tl::render {

/// The row of Options under (x, y), given the focus when `take`. True when there is one.
bool Renderer::Impl::point_settings_row(float x, float y, bool take) {
    for (int index = 0; index < tl::core::kSettingsControlCount; ++index) {
        const auto control = static_cast<tl::core::SettingsControl>(index + 1);
        if (!settings_screen.window_contains(tl::core::settings_control_window(control), x, y)) {
            continue;
        }
        if (take && settings_menu.focus != index) {
            settings_menu.focus = index;
            refresh_settings_menu();
        }
        return true;
    }
    return false;
}

/// The row of the Pause Menu under (x, y), given the focus when `take`. True when there is one.
bool Renderer::Impl::point_pause_row(float x, float y, bool take) {
    for (int index = 0; index < tl::core::kOptionsRowCount; ++index) {
        if (!tl::core::options_row_offered(index) ||
            !pause_screen.window_contains(tl::core::options_row(index).window, x, y)) {
            continue;
        }
        if (take && pause_menu.focus != index) {
            pause_menu.focus = index;
            refresh_pause_menu();
        }
        return true;
    }
    return false;
}

/// The wheel over the Help screen: a tick scrolls it as the stick held that way for a
/// fifth of a second does.
void Renderer::Impl::MouseGame::help_wheel(tl::core::InputState& in) {
    constexpr int kFrames = 12;
    if (in.pointer_wheel != 0) {
        help_scroll_frames = kFrames;
        help_scroll_up = in.pointer_wheel > 0;
    }
    if (help_scroll_frames <= 0) return;
    --help_scroll_frames;
    in.stick_y = help_scroll_up ? 1.0f : -1.0f;
}

// DIVERGENCE (mouse controls): the console's two menus have no pointer. Here a pointer that
// moves onto a row gives it the focus, a press of the left button on a row is A on it, a
// press of the right button is B, and the wheel over a row of Options is the D-pad's left
// and right, which change its value. On the Help screen the wheel scrolls and the right
// button is B.
tl::core::InputState Renderer::Impl::menu_pointer(tl::core::InputState in) {
    if (!tl::core::mouse_controls() || !in.pointer) return in;
    if (!mouse) mouse = std::make_shared<MouseGame>(*this);
    MouseGame& state = *mouse;
    const bool moved = in.pointer_x != state.menu_x || in.pointer_y != state.menu_y;
    const bool left = in.pointer_left && !state.menu_left_was;
    const bool right = in.pointer_right && !state.menu_right_was;
    state.menu_x = in.pointer_x;
    state.menu_y = in.pointer_y;
    state.menu_left_was = in.pointer_left;
    state.menu_right_was = in.pointer_right;

    if (howto_menu.open) state.help_wheel(in);
    const bool take = moved || left;
    const bool on_row = settings_menu.open
                            ? point_settings_row(in.pointer_x, in.pointer_y, take)
                            : pause_menu.open && point_pause_row(in.pointer_x, in.pointer_y, take);
    if (on_row && settings_menu.open && in.pointer_wheel != 0) {
        in.dpad_x = in.pointer_wheel > 0 ? 1.0f : -1.0f;
    }
    if (on_row && left) in.confirm = true;
    if (right) in.skip = true;
    return in;
}

}  // namespace tl::render
