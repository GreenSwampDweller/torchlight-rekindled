#include "render/pc_doll.h"

#include <OgreOverlay.h>
#include <OgreOverlayContainer.h>
#include <OgreOverlayManager.h>
#include <OgrePanelOverlayElement.h>

namespace tl::render {

PcDoll::~PcDoll() {
    if (Ogre::OverlayManager::getSingletonPtr() != nullptr) destroy();
}

void PcDoll::create(const std::string& tag, unsigned short z_order, const std::string& material) {
    if (overlay_ != nullptr) return;
    auto& overlays = Ogre::OverlayManager::getSingleton();
    overlay_ = overlays.create(tag + "/doll");
    overlay_->setZOrder(z_order);
    panel_ = static_cast<Ogre::PanelOverlayElement*>(
        overlays.createOverlayElement("Panel", tag + "/dollpanel"));
    panel_->setMetricsMode(Ogre::GMM_RELATIVE);
    panel_->setMaterialName(material);
    overlay_->add2D(panel_);
    overlay_->hide();
}

void PcDoll::destroy() {
    if (overlay_ == nullptr) return;
    auto& overlays = Ogre::OverlayManager::getSingleton();
    overlay_->remove2D(panel_);
    overlays.destroyOverlayElement(panel_);
    overlays.destroy(overlay_);
    overlay_ = nullptr;
    panel_ = nullptr;
}

void PcDoll::show() {
    if (overlay_ != nullptr) overlay_->show();
}

void PcDoll::hide() {
    if (overlay_ != nullptr) overlay_->hide();
}

void PcDoll::place(const Box& box, float view_wide, float view_tall, float aspect, bool live) {
    if (panel_ == nullptr) return;
    panel_->setPosition(box.left, box.top);
    panel_->setDimensions(box.wide, box.tall);
    if (!live) {
        // The preview's corner is its backdrop.
        panel_->setUV(0.0f, 0.0f, 0.001f, 0.001f);
        return;
    }
    const float pixels_wide = box.wide * view_wide;
    const float pixels_tall = box.tall * view_tall;
    if (!(pixels_tall > 0.0f) || !(aspect > 0.0f)) return;
    const float across = pixels_wide / (pixels_tall * aspect);
    panel_->setUV(0.5f - across * 0.5f, 0.0f, 0.5f + across * 0.5f, 1.0f);
}

}  // namespace tl::render
