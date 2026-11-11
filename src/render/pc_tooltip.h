// An item's details window beside the mouse pointer (`media/UI/tooltip.layout`,
// `GuiLook/ItemTooltip`): what `tl::core::equipment_tooltip` says, stacked as the
// console's is (render/item_tip.h), in the PC's frame art.
#pragma once

#include "core/equipment_tooltip.h"

#include <OgrePrerequisites.h>

#include <array>
#include <vector>

namespace Ogre {
class Overlay;
class OverlayContainer;
class PanelOverlayElement;
class TextAreaOverlayElement;
}  // namespace Ogre

namespace tl::render {

class LayoutScreen;

class PcTooltip {
public:
    PcTooltip() = default;
    ~PcTooltip();
    PcTooltip(const PcTooltip&) = delete;
    PcTooltip& operator=(const PcTooltip&) = delete;

    /// The window shown for `tip` beside the pointer at (`x`, `y`), 0 to 1 across and down
    /// a view of `width` by `height`. `art` is a screen whose imagesets hold the frame.
    // INVENTED: where the window stands - right of the pointer, or left of it where the
    // right has no room, kept inside the view.
    void show(const tl::core::EquipmentTooltip& tip, float x, float y, unsigned width,
              unsigned height, const LayoutScreen& art) {
        show_at(tip, Place{x, y, width, height, 160.0f}, art);
    }
    /// Where a window is asked for: the pointer, the view, and the least it is wide in
    /// authored pixels (an item's is 160).
    struct Place {
        float x = 0.0f, y = 0.0f;
        unsigned width = 0, height = 0;
        float least_wide = 160.0f;
    };
    void show_at(const tl::core::EquipmentTooltip& tip, const Place& place,
                 const LayoutScreen& art);
    void hide();
    void destroy();
    bool shown() const { return shown_; }

private:
    void make();

    Ogre::Overlay* panels_ = nullptr;
    Ogre::Overlay* text_ = nullptr;
    Ogre::OverlayContainer* panel_root_ = nullptr;
    Ogre::OverlayContainer* text_root_ = nullptr;
    std::array<Ogre::PanelOverlayElement*, 9> frame_{};
    std::vector<Ogre::TextAreaOverlayElement*> lines_;
    bool shown_ = false;
};

}  // namespace tl::render
