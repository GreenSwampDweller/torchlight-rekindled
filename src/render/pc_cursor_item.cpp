#include "render/pc_cursor_item.h"

#include "render/layout_screen.h"

#include <OgreOverlay.h>
#include <OgreOverlayContainer.h>
#include <OgreOverlayManager.h>
#include <OgrePanelOverlayElement.h>

#include <array>

namespace tl::render {

namespace {

/// The PC's interface is authored for a view 768 tall; an item slot is 32 by 48.
constexpr float kNativeTall = 768.0f;
constexpr float kWide = 32.0f, kTall = 48.0f;

}  // namespace

PcCursorItem::~PcCursorItem() {
    if (Ogre::OverlayManager::getSingletonPtr() != nullptr) destroy();
}

void PcCursorItem::show(const std::string& icon, const Place& place, const LayoutScreen& art) {
    Ogre::MaterialPtr material;
    std::array<float, 4> uv{};
    if (place.width == 0 || place.height == 0 || !art.art(icon, material, uv)) return hide();
    if (overlay_ == nullptr) {
        auto& overlays = Ogre::OverlayManager::getSingleton();
        overlay_ = overlays.create("tl/pccursoritem");
        overlay_->setZOrder(594);
        panel_ = static_cast<Ogre::PanelOverlayElement*>(
            overlays.createOverlayElement("Panel", "tl/pccursoritem/icon"));
        panel_->setMetricsMode(Ogre::GMM_RELATIVE);
        overlay_->add2D(panel_);
    }
    const float scale = static_cast<float>(place.height) / kNativeTall;
    const float wide = kWide * scale / static_cast<float>(place.width);
    const float tall = kTall * scale / static_cast<float>(place.height);
    panel_->setMaterial(material);
    panel_->setUV(uv[0], uv[1], uv[2], uv[3]);
    panel_->setPosition(place.x - wide * 0.5f, place.y - tall * 0.5f);
    panel_->setDimensions(wide, tall);
    overlay_->show();
}

void PcCursorItem::hide() {
    if (overlay_ != nullptr) overlay_->hide();
}

void PcCursorItem::destroy() {
    if (overlay_ == nullptr) return;
    auto& overlays = Ogre::OverlayManager::getSingleton();
    overlay_->remove2D(panel_);
    overlays.destroyOverlayElement(panel_);
    overlays.destroy(overlay_);
    overlay_ = nullptr;
    panel_ = nullptr;
}

}  // namespace tl::render
