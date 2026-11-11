// The front end's menu under the mouse controls.
#include "render/menu.h"

#include "core/log.h"

#include <OgrePanelOverlayElement.h>

#include <algorithm>
#include <string>
#include <vector>

namespace tl::render {

/// The button whose window holds (x, y), 0 to 1 across and down the view; -1 for none.
int MainMenu::button_at(float x, float y) const {
    for (size_t index = 0; index < buttons_.size(); ++index) {
        Ogre::PanelOverlayElement* const panel = buttons_[index].piece.panel;
        if (panel == nullptr || !panel->isVisible()) continue;
        const float left = panel->_getDerivedLeft();
        const float top = panel->_getDerivedTop();
        if (x >= left && y >= top && x <= left + panel->_getWidth() &&
            y <= top + panel->_getHeight()) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

/// The focus onto button `index`, as the pointer puts it there.
void MainMenu::focus_button(int index) {
    selected_ = index;
    // The difficulty list keeps its own focus, by the button's place in that list.
    const auto place =
        std::find(difficulty_button_.begin(), difficulty_button_.end(), static_cast<size_t>(index));
    if (difficulty_mode_ && difficulty_state_ != nullptr && place != difficulty_button_.end()) {
        difficulty_state_->focus = static_cast<int>(place - difficulty_button_.begin());
    }
    apply_button_look();
    TL_LOG_INFO("menu: the pointer is on '%s'", buttons_[static_cast<size_t>(index)].text.c_str());
}

// DIVERGENCE (mouse controls): the console's menu has no pointer. Here a pointer that moves
// onto a button gives it the focus, the left button on a button is A on it, and the right
// button is B.
tl::core::InputState MainMenu::with_pointer(tl::core::InputState input) {
    if (!tl::core::mouse_controls() || !input.pointer || !created_) return input;
    const int over = button_at(input.pointer_x, input.pointer_y);
    const bool moved = input.pointer_x != pointer_x_ || input.pointer_y != pointer_y_;
    pointer_x_ = input.pointer_x;
    pointer_y_ = input.pointer_y;
    if (over >= 0 && over != selected_ && (moved || input.pointer_left)) focus_button(over);
    if (over >= 0) input.confirm = input.confirm || input.pointer_left;
    input.skip = input.skip || input.pointer_right;
    return input;
}

/// The layout window of the character creation screen whose picture holds (x, y), 0 to 1
/// across and down the view; empty for none.
std::string CharacterCreate::window_at(float x, float y) const {
    for (const std::vector<Widget>* list : {&icons_, &buttons_, &console_frame_}) {
        for (const Widget& widget : *list) {
            Ogre::PanelOverlayElement* const panel = widget.panel;
            if (panel == nullptr || !panel->isVisible() || widget.window.empty()) continue;
            const float left = panel->_getDerivedLeft();
            const float top = panel->_getDerivedTop();
            if (x >= left && y >= top && x <= left + panel->_getWidth() &&
                y <= top + panel->_getHeight()) {
                return widget.window;
            }
        }
    }
    return {};
}

// DIVERGENCE (mouse controls): as the main menu's. A pointer that moves onto a window of
// the focus grid puts the focus there, the left button on one is A, the right button B.
tl::core::InputState CharacterCreate::with_pointer(tl::core::InputState input) {
    if (!tl::core::mouse_controls() || !input.pointer || !created_ || typing_ || !console_mode_) {
        return input;
    }
    const bool moved = input.pointer_x != pointer_x_ || input.pointer_y != pointer_y_;
    pointer_x_ = input.pointer_x;
    pointer_y_ = input.pointer_y;
    const std::string over = window_at(input.pointer_x, input.pointer_y);
    // A window can fill several cells of the grid (`DoneButton` spans its row): the focus
    // already on one of them stays.
    const bool in_grid =
        focus_.row >= 0 && static_cast<size_t>(focus_.row) < grid_.size() && focus_.column >= 0 &&
        static_cast<size_t>(focus_.column) < grid_[static_cast<size_t>(focus_.row)].size();
    bool on_grid =
        !over.empty() && in_grid &&
        grid_[static_cast<size_t>(focus_.row)][static_cast<size_t>(focus_.column)] == over;
    for (size_t row = 0; !over.empty() && !on_grid && row < grid_.size(); ++row) {
        for (size_t column = 0; !on_grid && column < grid_[row].size(); ++column) {
            if (grid_[row][column] != over) continue;
            on_grid = true;
            if (!(moved || input.pointer_left)) continue;
            focus_.row = static_cast<int>(row);
            focus_.column = static_cast<int>(column);
            apply_look();
            TL_LOG_INFO("menu: the pointer is on '%s'", over.c_str());
        }
    }
    if (on_grid) input.confirm = input.confirm || input.pointer_left;
    input.skip = input.skip || input.pointer_right;
    return input;
}

/// `destroy()` at the end of a headless run, after a pad script still pending under the
/// mouse controls has been played against the screen, as `MainMenu::finish_headless`.
void CharacterCreate::finish_headless() {
    constexpr int kFrames = 600;
    for (int frame = 0; tl::core::mouse_controls() && frame < kFrames; ++frame) {
        const MenuAction action = update(with_pointer(tl::core::poll_input()));
        if (action == MenuAction::None) continue;
        TL_LOG_INFO("menu: the script's press asks for action %d, on '%s' with the %s",
                    static_cast<int>(action), chosen().c_str(), pet().c_str());
        break;
    }
    destroy();
}

/// The end of a headless run, which has nobody at the menu: under the mouse controls a pad
/// script still pending is played against the menu first, so what its pointer and its
/// clicks do can be read in the log.
void MainMenu::finish_headless() {
    constexpr int kFrames = 600;
    for (int frame = 0; tl::core::mouse_controls() && frame < kFrames; ++frame) {
        const MenuAction action = update(with_pointer(tl::core::poll_input()));
        if (action == MenuAction::None) continue;
        TL_LOG_INFO("menu: the script's press asks for action %d", static_cast<int>(action));
        break;
    }
    destroy();
}

}  // namespace tl::render
