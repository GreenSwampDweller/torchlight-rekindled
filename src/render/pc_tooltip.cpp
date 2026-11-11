#include "render/pc_tooltip.h"

#include "render/item_tip.h"
#include "render/layout_screen.h"

#include <OgreOverlay.h>
#include <OgreOverlayContainer.h>
#include <OgreOverlayManager.h>
#include <OgrePanelOverlayElement.h>
#include <OgreTextAreaOverlayElement.h>

#include <algorithm>
#include <string>

namespace tl::render {

namespace {

/// The PC's interface is authored for a view 768 tall.
constexpr float kNativeTall = 768.0f;
/// How far from the pointer the window stands, in authored pixels.
constexpr float kPointerGap = 24.0f;
/// The frame reaches 26 past the window's sides and 13 past its top and bottom.
constexpr float kFrameSide = 26.0f, kFrameEnd = 13.0f;
/// Below this share of the view's height the pointer is over the HUD.
constexpr float kHudBand = 0.85f;

Ogre::OverlayContainer* full_view_root(Ogre::Overlay* overlay, const char* name) {
    auto* root = static_cast<Ogre::OverlayContainer*>(
        Ogre::OverlayManager::getSingleton().createOverlayElement("Panel", name));
    root->setMetricsMode(Ogre::GMM_RELATIVE);
    root->setPosition(0.0f, 0.0f);
    root->setDimensions(1.0f, 1.0f);
    static_cast<Ogre::PanelOverlayElement*>(root)->setTransparent(true);
    overlay->add2D(root);
    return root;
}

}  // namespace

PcTooltip::~PcTooltip() {
    if (Ogre::OverlayManager::getSingletonPtr() != nullptr) destroy();
}

void PcTooltip::make() {
    if (panels_ != nullptr) return;
    auto& overlays = Ogre::OverlayManager::getSingleton();
    panels_ = overlays.create("tl/pctip/panels");
    panels_->setZOrder(592);
    text_ = overlays.create("tl/pctip/text");
    text_->setZOrder(593);
    panel_root_ = full_view_root(panels_, "tl/pctip/panelroot");
    text_root_ = full_view_root(text_, "tl/pctip/textroot");
    for (size_t piece = 0; piece < frame_.size(); ++piece) {
        frame_[piece] = static_cast<Ogre::PanelOverlayElement*>(
            overlays.createOverlayElement("Panel", "tl/pctip/frame" + std::to_string(piece)));
        frame_[piece]->setMetricsMode(Ogre::GMM_RELATIVE);
        panel_root_->addChild(frame_[piece]);
    }
}

void PcTooltip::show_at(const tl::core::EquipmentTooltip& tip, const Place& place,
                        const LayoutScreen& art) {
    const float x = place.x;
    const float y = place.y;
    const unsigned width = place.width;
    const unsigned height = place.height;
    if (!tip.name.shown || width == 0 || height == 0) return hide();
    make();
    const float across = static_cast<float>(width);
    const float down = static_cast<float>(height);
    const float scale = down / kNativeTall;
    const TipView view{scale, across, down, place.least_wide};
    const TipBuilt built = build_item_tip(tip, kMainTipFonts, view);

    const float gap = kPointerGap * scale + kFrameSide;
    float left = x * across + gap;
    if (left + built.wide + kFrameSide > across) left = x * across - gap - built.wide;
    float top = y * down - built.tall * 0.5f;
    // Over the HUD, at the foot of the view: above the pointer, so the HUD is not covered.
    if (y > kHudBand) {
        left = x * across - built.wide * 0.5f;
        top = y * down - built.tall - gap;
    }
    left = std::max(kFrameSide, std::min(left, across - built.wide - kFrameSide));
    top = std::max(kFrameEnd, std::min(top, down - built.tall - kFrameEnd));

    const std::array<TipSlice, 9> slices = item_tip_slices(left, top, built.wide, built.tall);
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
    for (Ogre::TextAreaOverlayElement* line : lines_) line->hide();
    draw_item_tip_text(built, left, top, view, TipTextHome{text_root_, "tl/pctip/line", &lines_});
    panels_->show();
    text_->show();
    shown_ = true;
}

void PcTooltip::hide() {
    if (panels_ != nullptr) panels_->hide();
    if (text_ != nullptr) text_->hide();
    shown_ = false;
}

void PcTooltip::destroy() {
    if (panels_ == nullptr) return;
    auto& overlays = Ogre::OverlayManager::getSingleton();
    for (Ogre::PanelOverlayElement*& piece : frame_) {
        panel_root_->removeChild(piece->getName());
        overlays.destroyOverlayElement(piece);
        piece = nullptr;
    }
    for (Ogre::TextAreaOverlayElement* line : lines_) {
        text_root_->removeChild(line->getName());
        overlays.destroyOverlayElement(line);
    }
    lines_.clear();
    panels_->remove2D(panel_root_);
    text_->remove2D(text_root_);
    overlays.destroyOverlayElement(panel_root_);
    overlays.destroyOverlayElement(text_root_);
    overlays.destroy(panels_);
    overlays.destroy(text_);
    panels_ = nullptr;
    text_ = nullptr;
    panel_root_ = nullptr;
    text_root_ = nullptr;
    shown_ = false;
}

}  // namespace tl::render
