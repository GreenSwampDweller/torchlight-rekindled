// DIVERGENCE (desktop mouse): the PC's pointer on the console's menus. The console's screens are
// run by the pad: a focus moved by the D-pad and A pressed on it. The pointer is carried out as the
// same two orders - over a box, the focus moves to it; a click on it is A - so each screen keeps
// its own logic and only asks which of its boxes lies under the pointer.
#pragma once

#include "core/input.h"

#include <OgreOverlayContainer.h>
#include <OgreOverlayElement.h>

#include <cmath>

namespace tl::render {

/// One screen's view of the pointer, stepped once a frame: whether it moved, and the buttons'
/// presses (edges, as the screens read A's).
struct MenuPointer {
    float last_x = -1.0f;
    float last_y = -1.0f;
    bool held = false;
    bool alt_held = false;
    bool moved = false;
    bool pressed = false;
    bool alt_pressed = false;
    bool released = false;

    void step(const tl::core::InputState& in) {
        moved = in.pointer &&
                (std::abs(in.pointer_x - last_x) > 1e-4f || std::abs(in.pointer_y - last_y) > 1e-4f);
        if (in.pointer) {
            last_x = in.pointer_x;
            last_y = in.pointer_y;
        }
        pressed = in.pointer && in.click && !held;
        alt_pressed = in.pointer && in.click_alt && !alt_held;
        released = held && !(in.pointer && in.click);
        held = in.pointer && in.click;
        alt_held = in.pointer && in.click_alt;
    }
    /// The pointer has a say this frame: moved onto something, or clicked.
    bool acting() const { return moved || pressed; }
};

/// Whether the pointer lies on `element`, shown along with every window it hangs from.
inline bool pointer_over(const tl::core::InputState& in, Ogre::OverlayElement* element) {
    if (!in.pointer || element == nullptr) return false;
    for (Ogre::OverlayElement* at = element; at != nullptr; at = at->getParent()) {
        if (!at->isVisible()) return false;
    }
    const float left = element->_getDerivedLeft();
    const float top = element->_getDerivedTop();
    return in.pointer_x >= left && in.pointer_x < left + element->_getWidth() &&
           in.pointer_y >= top && in.pointer_y < top + element->_getHeight();
}

}  // namespace tl::render
