#include "render/pc_skills.h"

#include "render/menu_chrome.h"
#include "render/overlay_set.h"

#include <OgreFontManager.h>
#include <OgreOverlay.h>
#include <OgreOverlayContainer.h>
#include <OgreOverlayManager.h>
#include <OgrePanelOverlayElement.h>
#include <OgreTextAreaOverlayElement.h>

namespace tl::render {

namespace {

/// The PC's interface is authored for a view 768 tall.
constexpr float kNativeTall = 768.0f;

// CSkillMenu::updateLayout, in authored pixels from the tab's frame: a
// skill's housing at (column * 86, row * 75), 55 by 55.4; its icon 6.5 in, 42 square;
// its rank in a box 37.8 by 42 over the icon, right and bottom aligned; its plus button
// 21 by 17.25, centred under the housing.
constexpr float kColumnPitch = 86.0f, kRowPitch = 75.0f;
constexpr float kHousingWide = 55.0f, kHousingTall = 55.4f;
constexpr float kIconInset = 6.5f, kIconSize = 42.0f;
constexpr float kRankWide = 37.8f, kRankTall = 42.0f;
constexpr float kPlusWide = 21.0f, kPlusTall = 17.25f, kPlusLeft = 27.5f - 10.5f;
// A spell's housing at (slot * 64, 0) of the spells' frame, 54 square; its icon 3 in, 48.
constexpr float kSpellPitch = 64.0f, kSpellHousing = 54.0f, kSpellInset = 3.0f, kSpellIcon = 48.0f;
/// The rank's size, the layout's `Serif` (11).
constexpr float kRankPoints = 11.0f;

constexpr const char* kTabs[3] = {"TabA", "TabB", "TabC"};
constexpr const char* kTabTexts[3] = {"TabTextA", "TabTextB", "TabTextC"};

}  // namespace

PcSkills::~PcSkills() {
    if (Ogre::OverlayManager::getSingletonPtr() != nullptr) destroy();
}

bool PcSkills::create(const std::string& group, unsigned width, unsigned height) {
    pane_.create(group, "skill", /*right=*/true, width, height, "tl/pcskills", 545);
    // The skills' icons are among the item icons' sheets.
    std::vector<std::string> sets = {"media/UI/hud.imageset", "media/UI/ui2.imageset",
                                     "media/UI/uiicons.imageset",
                                     "media/UI/itemicons/itemicons.imageset"};
    for (int sheet = 1; sheet <= 16; ++sheet) {
        sets.push_back("media/UI/itemicons/itemicons" + std::to_string(sheet) + ".imageset");
    }
    screen_.use_imagesets(std::move(sets));
    if (!screen_.create_pc(group, width, height, "media/UI/skillmenu.layout", "tl/pcskills", 546)) {
        pane_.destroy();
        return false;
    }
    screen_.refresh();
    auto& overlays = Ogre::OverlayManager::getSingleton();
    overlay_ = overlays.create("tl/pcskills/cells");
    overlay_->setZOrder(549);
    root_ = static_cast<Ogre::OverlayContainer*>(
        overlays.createOverlayElement("Panel", "tl/pcskills/cellsroot"));
    root_->setMetricsMode(Ogre::GMM_RELATIVE);
    root_->setPosition(0.0f, 0.0f);
    root_->setDimensions(1.0f, 1.0f);
    static_cast<Ogre::PanelOverlayElement*>(root_)->setTransparent(true);
    overlay_->add2D(root_);
    overlay_->hide();
    width_ = width;
    height_ = height;
    return true;
}

void PcSkills::destroy() {
    if (overlay_ != nullptr) {
        auto& overlays = Ogre::OverlayManager::getSingleton();
        for (Drawn& cell : cells_) {
            for (Ogre::OverlayElement* element : {static_cast<Ogre::OverlayElement*>(cell.housing),
                                                  static_cast<Ogre::OverlayElement*>(cell.icon),
                                                  static_cast<Ogre::OverlayElement*>(cell.plus),
                                                  static_cast<Ogre::OverlayElement*>(cell.rank)}) {
                root_->removeChild(element->getName());
                overlays.destroyOverlayElement(element);
            }
        }
        cells_.clear();
        overlay_->remove2D(root_);
        overlays.destroyOverlayElement(root_);
        overlays.destroy(overlay_);
        overlay_ = nullptr;
        root_ = nullptr;
    }
    screen_.destroy();
    pane_.destroy();
}

void PcSkills::set_visible(bool on) {
    if (!screen_.ready() || on == screen_.visible()) return;
    if (on) {
        pane_.show();
        screen_.show();
        overlay_->show();
    } else {
        overlay_->hide();
        screen_.hide();
        pane_.hide();
    }
}

void PcSkills::resize(unsigned width, unsigned height) {
    pane_.resize(width, height);
    screen_.resize(width, height);
    width_ = width;
    height_ = height;
}

PcSkills::Drawn& PcSkills::drawn(size_t index) {
    auto& overlays = Ogre::OverlayManager::getSingleton();
    while (cells_.size() <= index) {
        const std::string name = "tl/pcskills/cell" + std::to_string(cells_.size());
        Drawn cell;
        // Housing, then the icon and the button, then the rank: children of one container
        // share its depth unless each is told its own.
        const auto base = static_cast<Ogre::ushort>(root_->getZOrder() + 1);
        const auto panel = [&](const char* part, Ogre::ushort above) {
            auto* made = static_cast<Ogre::PanelOverlayElement*>(
                overlays.createOverlayElement("Panel", name + part));
            made->setMetricsMode(Ogre::GMM_RELATIVE);
            made->hide();
            root_->addChild(made);
            made->_notifyZOrder(static_cast<Ogre::ushort>(base + above));
            return made;
        };
        cell.housing = panel("/housing", 0);
        cell.icon = panel("/icon", 1);
        cell.plus = panel("/plus", 1);
        cell.rank = static_cast<Ogre::TextAreaOverlayElement*>(
            overlays.createOverlayElement("TextArea", name + "/rank"));
        cell.rank->setMetricsMode(Ogre::GMM_RELATIVE);
        cell.rank->setFontName(kFontName, Ogre::RGN_INTERNAL);
        cell.rank->setAlignment(Ogre::TextAreaOverlayElement::Right);
        cell.rank->setColour(Ogre::ColourValue::White);
        cell.rank->hide();
        root_->addChild(cell.rank);
        cell.rank->_notifyZOrder(static_cast<Ogre::ushort>(base + 2));
        cells_.push_back(cell);
    }
    return cells_[index];
}

/// `image` of the pane's imagesets over `box`; hidden when the sets hold none of it.
void PcSkills::paint(Ogre::PanelOverlayElement* panel, const std::string& image, const Box& box) {
    Ogre::MaterialPtr material;
    std::array<float, 4> uv{};
    if (image.empty() || !screen_.art(image, material, uv)) {
        panel->hide();
        return;
    }
    panel->setMaterial(material);
    panel->setUV(uv[0], uv[1], uv[2], uv[3]);
    panel->setPosition(box.left, box.top);
    panel->setDimensions(box.wide, box.tall);
    panel->show();
}

void PcSkills::set(const View& view) {
    if (!screen_.ready() || width_ == 0 || height_ == 0) return;
    for (int tab = 0; tab < 3; ++tab) {
        screen_.set_hover(kTabs[tab], tab == view.tab);
        screen_.set_caption(kTabTexts[tab], view.tabs[static_cast<size_t>(tab)]);
    }
    screen_.set_caption("Skill Points", view.points);
    screen_.refresh();

    const float down = static_cast<float>(height_);
    const float scale = down / kNativeTall;
    const float x_unit = scale / static_cast<float>(width_);
    const float y_unit = scale / down;
    const auto box_at = [&](const Box& frame, float x, float y, float wide, float tall) {
        return Box{frame.left + x * x_unit, frame.top + y * y_unit, wide * x_unit, tall * y_unit};
    };
    const float rank_height = kRankPoints * points_to_char_pixels() * scale / down;

    Box frame;
    screen_.window_box("SkillFrameA", frame.left, frame.top, frame.wide, frame.tall);
    size_t used = 0;
    for (const Cell& cell : view.cells) {
        Drawn& made = drawn(used++);
        const float x = static_cast<float>(cell.column) * kColumnPitch;
        const float y = static_cast<float>(cell.row) * kRowPitch;
        made.box = box_at(frame, x, y, kHousingWide, kHousingTall);
        made.plus_box = box_at(frame, x + kPlusLeft, y + kHousingTall, kPlusWide, kPlusTall);
        made.shown = true;
        made.plus_shown = cell.plus;
        paint(made.housing, cell.passive ? "PassiveSkillHousing" : "ActiveSkillHousing", made.box);
        paint(made.icon, cell.icon,
              box_at(frame, x + kIconInset, y + kIconInset, kIconSize, kIconSize));
        paint(made.plus, cell.plus ? "SkillPointPlus" : "", made.plus_box);
        if (cell.rank.empty()) {
            made.rank->hide();
        } else {
            const Box rank = box_at(frame, x + kIconInset, y + kIconInset, kRankWide, kRankTall);
            made.rank->setCaption(cell.rank);
            set_char_height(made.rank, rank_height);
            made.rank->setPosition(rank.left + rank.wide, rank.top + rank.tall - rank_height);
            made.rank->show();
        }
    }
    screen_.window_box("SpellFrame", frame.left, frame.top, frame.wide, frame.tall);
    for (int spell = 0; spell < kSpells; ++spell) {
        Drawn& made = drawn(used++);
        const float x = static_cast<float>(spell) * kSpellPitch;
        made.box = box_at(frame, x, 0.0f, kSpellHousing, kSpellHousing);
        made.shown = true;
        made.plus_shown = false;
        paint(made.housing, "ActiveSkillHousing", made.box);
        paint(made.icon, view.spells[static_cast<size_t>(spell)],
              box_at(frame, x + kSpellInset, kSpellInset, kSpellIcon, kSpellIcon));
        made.plus->hide();
        made.rank->hide();
    }
    for (size_t rest = used; rest < cells_.size(); ++rest) {
        Drawn& idle = cells_[rest];
        idle.shown = false;
        idle.plus_shown = false;
        idle.housing->hide();
        idle.icon->hide();
        idle.plus->hide();
        idle.rank->hide();
    }
    cells_used_ = used;
}

std::string PcSkills::point(float x, float y) const {
    if (!screen_.visible()) return {};
    // The view's cells first, the four spells after them.
    const size_t skills = cells_used_ >= kSpells ? cells_used_ - kSpells : 0;
    for (size_t index = 0; index < cells_used_ && index < cells_.size(); ++index) {
        const Drawn& cell = cells_[index];
        if (index >= skills) {
            if (cell.box.holds(x, y)) return "Spell" + std::to_string(index - skills);
            continue;
        }
        if (cell.plus_shown && cell.plus_box.holds(x, y)) return "Plus" + std::to_string(index);
        if (cell.box.holds(x, y)) return "Skill" + std::to_string(index);
    }
    return screen_.window_at(x, y);
}

bool PcSkills::blocks(float x, float y) const {
    return screen_.visible() && screen_.window_contains("Blocker", x, y);
}

}  // namespace tl::render
