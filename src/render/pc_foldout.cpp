#include "render/pc_foldout.h"

#include "render/item_tip.h"
#include "render/layout_screen.h"

#include <OgreOverlay.h>
#include <OgreOverlayContainer.h>
#include <OgreOverlayManager.h>
#include <OgrePanelOverlayElement.h>

#include <algorithm>

namespace tl::render {

namespace {

/// The PC's interface is authored for a view 768 tall.
constexpr float kNativeTall = 768.0f;
// CSkillFoldout::showFoldout, in authored pixels: a cell every 50, its
// icon 4 in and 48 square; the window as wide as its last column's far edge and 5 taller
// than its rows; opened 16 + 26 right of the point and its own height + 26 above it, kept
// 26 inside the view.
constexpr float kPitch = 50.0f, kInset = 4.0f, kIcon = 48.0f;
constexpr float kTallExtra = 5.0f;
constexpr float kRight = 16.0f + 26.0f, kMargin = 26.0f;

}  // namespace

PcFoldout::~PcFoldout() {
    if (Ogre::OverlayManager::getSingletonPtr() != nullptr) destroy();
}

void PcFoldout::make() {
    if (overlay_ != nullptr) return;
    auto& overlays = Ogre::OverlayManager::getSingleton();
    overlay_ = overlays.create("tl/pcfoldout");
    overlay_->setZOrder(590);
    root_ = static_cast<Ogre::OverlayContainer*>(
        overlays.createOverlayElement("Panel", "tl/pcfoldout/root"));
    root_->setMetricsMode(Ogre::GMM_RELATIVE);
    root_->setPosition(0.0f, 0.0f);
    root_->setDimensions(1.0f, 1.0f);
    static_cast<Ogre::PanelOverlayElement*>(root_)->setTransparent(true);
    overlay_->add2D(root_);
    for (size_t piece = 0; piece < frame_.size(); ++piece) {
        frame_[piece] = static_cast<Ogre::PanelOverlayElement*>(
            overlays.createOverlayElement("Panel", "tl/pcfoldout/frame" + std::to_string(piece)));
        frame_[piece]->setMetricsMode(Ogre::GMM_RELATIVE);
        root_->addChild(frame_[piece]);
    }
}

void PcFoldout::show(const std::vector<Entry>& entries, const Place& place,
                     const LayoutScreen& art) {
    if (entries.empty() || place.width == 0 || place.height == 0) return hide();
    make();
    auto& overlays = Ogre::OverlayManager::getSingleton();
    const float across = static_cast<float>(place.width);
    const float down = static_cast<float>(place.height);
    const float scale = down / kNativeTall;

    int columns = 0;
    int rows = 0;
    for (const Entry& entry : entries) {
        columns = std::max(columns, entry.column + 1);
        rows = std::max(rows, entry.row + 1);
    }
    const float wide = (static_cast<float>(columns - 1) * kPitch + kIcon) * scale;
    const float tall = (static_cast<float>(rows) * kPitch + kTallExtra) * scale;
    float left = place.x * across + kRight * scale;
    float top = place.y * down - tall - kMargin * scale;
    left = std::min(left, across - wide - kMargin * scale);
    top = std::min(top, down - tall - kMargin * scale);
    left = std::max(left, kMargin * scale);
    top = std::max(top, kMargin * scale);
    window_ = Box{left / across, top / down, wide / across, tall / down};

    const std::array<TipSlice, 9> slices = item_tip_slices(left, top, wide, tall);
    for (size_t piece = 0; piece < frame_.size(); ++piece) {
        Ogre::MaterialPtr material;
        std::array<float, 4> uv{};
        if (!art.art(kItemTipFrame[0][piece], material, uv)) {
            frame_[piece]->hide();
            continue;
        }
        frame_[piece]->setMaterial(material);
        frame_[piece]->setUV(uv[0], uv[1], uv[2], uv[3]);
        frame_[piece]->setPosition(slices[piece].x / across, slices[piece].y / down);
        frame_[piece]->setDimensions(slices[piece].w / across, slices[piece].h / down);
        frame_[piece]->show();
    }

    cells_.clear();
    for (size_t index = 0; index < entries.size(); ++index) {
        if (index == icons_.size()) {
            auto* icon = static_cast<Ogre::PanelOverlayElement*>(overlays.createOverlayElement(
                "Panel", "tl/pcfoldout/icon" + std::to_string(index)));
            icon->setMetricsMode(Ogre::GMM_RELATIVE);
            root_->addChild(icon);
            // Over the frame: children of one container share its depth otherwise.
            icon->_notifyZOrder(static_cast<Ogre::ushort>(root_->getZOrder() + 2));
            icons_.push_back(icon);
        }
        const Entry& entry = entries[index];
        // An item's picture keeps its proportion: 48 tall, centred across the cell.
        const float icon_wide = kIcon * entry.aspect;
        const Box cell{(left + (static_cast<float>(entry.column) * kPitch + kInset +
                                (kIcon - icon_wide) * 0.5f) *
                                   scale) /
                           across,
                       (top + (static_cast<float>(entry.row) * kPitch + kInset) * scale) / down,
                       icon_wide * scale / across, kIcon * scale / down};
        cells_.push_back(cell);
        Ogre::MaterialPtr material;
        std::array<float, 4> uv{};
        if (!art.art(entry.icon, material, uv)) {
            icons_[index]->hide();
            continue;
        }
        icons_[index]->setMaterial(material);
        icons_[index]->setUV(uv[0], uv[1], uv[2], uv[3]);
        icons_[index]->setPosition(cell.left, cell.top);
        icons_[index]->setDimensions(cell.wide, cell.tall);
        icons_[index]->show();
    }
    for (size_t rest = entries.size(); rest < icons_.size(); ++rest) icons_[rest]->hide();
    overlay_->show();
    shown_ = true;
}

void PcFoldout::hide() {
    if (overlay_ != nullptr) overlay_->hide();
    shown_ = false;
}

void PcFoldout::destroy() {
    if (overlay_ == nullptr) return;
    auto& overlays = Ogre::OverlayManager::getSingleton();
    for (Ogre::PanelOverlayElement* piece : frame_) {
        root_->removeChild(piece->getName());
        overlays.destroyOverlayElement(piece);
    }
    frame_ = {};
    for (Ogre::PanelOverlayElement* icon : icons_) {
        root_->removeChild(icon->getName());
        overlays.destroyOverlayElement(icon);
    }
    icons_.clear();
    overlay_->remove2D(root_);
    overlays.destroyOverlayElement(root_);
    overlays.destroy(overlay_);
    overlay_ = nullptr;
    root_ = nullptr;
    shown_ = false;
}

int PcFoldout::point(float x, float y) const {
    if (!shown_) return -1;
    for (size_t index = 0; index < cells_.size(); ++index) {
        if (cells_[index].holds(x, y)) return static_cast<int>(index);
    }
    return -1;
}

bool PcFoldout::holds(float x, float y) const { return shown_ && window_.holds(x, y); }

}  // namespace tl::render
