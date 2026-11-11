// The paperdoll of a PC pane: the pane's model is open where `PaperdollEquip` stands, and
// the character's preview (the console menu's render) is drawn there, under the pane.
#pragma once

#include <OgrePrerequisites.h>

#include <string>

namespace Ogre {
class Overlay;
class PanelOverlayElement;
}  // namespace Ogre

namespace tl::render {

class PcDoll {
public:
    PcDoll() = default;
    ~PcDoll();
    PcDoll(const PcDoll&) = delete;
    PcDoll& operator=(const PcDoll&) = delete;

    /// An overlay named `tag` at `z_order`, drawing `material` (the preview's).
    void create(const std::string& tag, unsigned short z_order, const std::string& material);
    void destroy();
    bool made() const { return overlay_ != nullptr; }
    void show();
    void hide();

    /// Where it stands: a window's box, 0 to 1 across and down the view.
    struct Box {
        float left = 0.0f, top = 0.0f, wide = 0.0f, tall = 0.0f;
    };
    /// The box filled, the preview - `aspect` wide for one tall - kept in proportion in a
    /// view of `view_wide` by `view_tall` pixels: what is left over at the sides repeats
    /// its edge, which is its backdrop. `live` false draws the backdrop alone.
    void place(const Box& box, float view_wide, float view_tall, float aspect, bool live);

private:
    Ogre::Overlay* overlay_ = nullptr;
    Ogre::PanelOverlayElement* panel_ = nullptr;
};

}  // namespace tl::render
