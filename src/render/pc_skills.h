// The PC's skills pane (`media/UI/skillmenu.layout`), the right pane: three tabs of a
// class's skills and its four spells. The layout holds only the frame; the skills' own
// windows are made from each skill's place in its tree, as
// CSkillMenu::updateLayout makes them.
#pragma once

#include "render/layout_screen.h"
#include "render/pc_pane.h"

#include <OgrePrerequisites.h>

#include <array>
#include <string>
#include <vector>

namespace Ogre {
class Overlay;
class OverlayContainer;
class PanelOverlayElement;
class TextAreaOverlayElement;
}  // namespace Ogre

namespace tl::render {

class PcSkills {
public:
    static constexpr int kSpells = 4;

    /// One skill of the open tab.
    struct Cell {
        std::string icon;      ///< `SKILL_ICON`, or `SKILL_ICON_INACTIVE` with no level
        bool passive = false;  ///< which housing it sits in
        int column = 0;        ///< `COLUMN`
        int row = 0;           ///< `ROW`, or a fifth of the level it needs
        std::string rank;      ///< the points in it; empty for none
        bool plus = false;     ///< a point can go into it
    };

    /// What the pane shows.
    struct View {
        std::array<std::string, 3> tabs;  ///< `SKILL_TAB1` .. `3`
        int tab = 0;
        std::string points;
        std::vector<Cell> cells;
        std::array<std::string, kSpells> spells;  ///< icons; empty for none
    };

    PcSkills() = default;
    ~PcSkills();
    PcSkills(const PcSkills&) = delete;
    PcSkills& operator=(const PcSkills&) = delete;

    /// Build it against the mounted data. False when `skillmenu.layout` is not there.
    bool create(const std::string& group, unsigned width, unsigned height);
    void destroy();
    bool ready() const { return screen_.ready(); }
    bool visible() const { return screen_.visible(); }
    void set_visible(bool on);
    void resize(unsigned width, unsigned height);

    void set(const View& view);

    /// What the pointer is over: `Skill<N>` or `Plus<N>` for the Nth cell of the view,
    /// `Spell<N>`, else the layout's window; empty over none.
    std::string point(float x, float y) const;
    /// The pane's `Blocker`: the pointer inside it is the pane's.
    bool blocks(float x, float y) const;
    /// The pane's windows, for the art of its imagesets.
    const LayoutScreen& screen() const { return screen_; }

private:
    /// A box, 0 to 1 across and down the view.
    struct Box {
        float left = 0.0f, top = 0.0f, wide = 0.0f, tall = 0.0f;
        bool holds(float x, float y) const {
            return x >= left && y >= top && x <= left + wide && y <= top + tall;
        }
    };
    /// One skill's elements, made as cells are needed.
    struct Drawn {
        Ogre::PanelOverlayElement* housing = nullptr;
        Ogre::PanelOverlayElement* icon = nullptr;
        Ogre::PanelOverlayElement* plus = nullptr;
        Ogre::TextAreaOverlayElement* rank = nullptr;
        Box box;
        Box plus_box;
        bool shown = false;
        bool plus_shown = false;
    };
    Drawn& drawn(size_t index);
    void paint(Ogre::PanelOverlayElement* panel, const std::string& image, const Box& box);

    PcPane pane_;
    LayoutScreen screen_;
    Ogre::Overlay* overlay_ = nullptr;
    Ogre::OverlayContainer* root_ = nullptr;
    std::vector<Drawn> cells_;
    size_t cells_used_ = 0;
    unsigned width_ = 0;
    unsigned height_ = 0;
};

}  // namespace tl::render
