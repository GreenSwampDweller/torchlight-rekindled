// The item picked up onto the mouse pointer: its picture, drawn at the pointer over
// everything else of the PC's panes.
#pragma once

#include <OgrePrerequisites.h>

#include <string>

namespace Ogre {
class Overlay;
class PanelOverlayElement;
}  // namespace Ogre

namespace tl::render {

class LayoutScreen;

class PcCursorItem {
public:
    PcCursorItem() = default;
    ~PcCursorItem();
    PcCursorItem(const PcCursorItem&) = delete;
    PcCursorItem& operator=(const PcCursorItem&) = delete;

    /// Where the pointer is, 0 to 1 across and down a view of `width` by `height`.
    struct Place {
        float x = 0.0f, y = 0.0f;
        unsigned width = 0, height = 0;
    };
    /// `icon` of `art`'s imagesets centred on the pointer, the size of an item slot.
    void show(const std::string& icon, const Place& place, const LayoutScreen& art);
    void hide();
    void destroy();

private:
    Ogre::Overlay* overlay_ = nullptr;
    Ogre::PanelOverlayElement* panel_ = nullptr;
};

}  // namespace tl::render
