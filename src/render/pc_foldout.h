// The PC's skill foldout (`media/UI/skillfoldout.layout`, a `GuiLook/ItemTooltip` frame over
// a grid of 48-pixel icons): the skills that can go on a button or a numbered slot, opened
// from the HUD. Placed as CSkillFoldout::showFoldout places it.
#pragma once

#include <OgrePrerequisites.h>

#include <array>
#include <string>
#include <vector>

namespace Ogre {
class Overlay;
class OverlayContainer;
class PanelOverlayElement;
}  // namespace Ogre

namespace tl::render {

class LayoutScreen;

class PcFoldout {
public:
    PcFoldout() = default;
    ~PcFoldout();
    PcFoldout(const PcFoldout&) = delete;
    PcFoldout& operator=(const PcFoldout&) = delete;

    /// One skill's picture and its cell: `column` across, `row` down.
    struct Entry {
        std::string icon;
        int column = 0;
        int row = 0;
        /// How wide the picture is for one tall: it is drawn 48 tall and centred in its cell.
        float aspect = 1.0f;
    };
    /// Where it is asked for: the point of the HUD it opens from, 0 to 1 across and down
    /// a view of `width` by `height`.
    struct Place {
        float x = 0.0f, y = 0.0f;
        unsigned width = 0, height = 0;
    };

    /// Shown with `entries`; `art` is a screen whose imagesets hold the frame and the icons.
    void show(const std::vector<Entry>& entries, const Place& place, const LayoutScreen& art);
    void hide();
    void destroy();
    bool shown() const { return shown_; }

    /// The entry under (x, y), 0 to 1 across and down the view; -1 for none.
    int point(float x, float y) const;
    /// Whether (x, y) is inside the window.
    bool holds(float x, float y) const;

private:
    struct Box {
        float left = 0.0f, top = 0.0f, wide = 0.0f, tall = 0.0f;
        bool holds(float x, float y) const {
            return x >= left && y >= top && x <= left + wide && y <= top + tall;
        }
    };
    void make();

    Ogre::Overlay* overlay_ = nullptr;
    Ogre::OverlayContainer* root_ = nullptr;
    std::array<Ogre::PanelOverlayElement*, 9> frame_{};
    std::vector<Ogre::PanelOverlayElement*> icons_;
    std::vector<Box> cells_;
    Box window_;
    bool shown_ = false;
};

}  // namespace tl::render
