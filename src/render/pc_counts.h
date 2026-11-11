// The stack numbers over a PC pane's item cells: one text element a cell, in the cell's
// top left corner, shown for a stack of two or more.
#pragma once

#include <OgrePrerequisites.h>

#include <string>
#include <vector>

namespace Ogre {
class Overlay;
class OverlayContainer;
class TextAreaOverlayElement;
}  // namespace Ogre

namespace tl::render {

class PcCounts {
public:
    PcCounts() = default;
    ~PcCounts();
    PcCounts(const PcCounts&) = delete;
    PcCounts& operator=(const PcCounts&) = delete;

    /// `cells` numbers on overlays named `tag`, drawn at `z_order`.
    void create(const std::string& tag, unsigned short z_order, int cells);
    void destroy();
    void show();
    void hide();
    /// A cell's box, 0 to 1 across and down a view `down` pixels tall, and its stack.
    void set(int cell, float left, float top, int count, float down);

private:
    Ogre::Overlay* overlay_ = nullptr;
    Ogre::OverlayContainer* root_ = nullptr;
    std::vector<Ogre::TextAreaOverlayElement*> numbers_;
    std::vector<int> shown_;
};

}  // namespace tl::render
