// The on-screen pad, drawn (core/touch_pad.h). PORT: the port's own: one overlay over
// everything, built the first time the pad is on the screen (never on desktop or console).
#pragma once

#include <cstddef>
#include <vector>

namespace Ogre {
class Overlay;
class OverlayContainer;
class PanelOverlayElement;
class TextAreaOverlayElement;
}  // namespace Ogre

namespace tl::render {

class TouchOverlay {
public:
    /// Draw the pad as it is now; hide it while it is off the screen. Once a
    /// frame, before the frame is drawn.
    void step();

private:
    void build(std::size_t count);

    Ogre::Overlay* overlay_ = nullptr;
    Ogre::OverlayContainer* root_ = nullptr;
    std::vector<Ogre::PanelOverlayElement*> discs_;
    std::vector<Ogre::TextAreaOverlayElement*> labels_;  ///< null where a disc has none
    float built_width_ = 0.0f, built_height_ = 0.0f;
    float built_text_scale_ = 0.0f, built_opacity_ = 0.0f;
    int built_style_ = -1;
};

}  // namespace tl::render
