#include "render/pc_counts.h"

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

/// The PC's interface is authored for a view 768 tall; the number is its `Serif` (11).
constexpr float kNativeTall = 768.0f;
constexpr float kPoints = 11.0f;

}  // namespace

PcCounts::~PcCounts() {
    if (Ogre::OverlayManager::getSingletonPtr() != nullptr) destroy();
}

void PcCounts::create(const std::string& tag, unsigned short z_order, int cells) {
    if (overlay_ != nullptr) return;
    auto& overlays = Ogre::OverlayManager::getSingleton();
    overlay_ = overlays.create(tag + "/counts");
    overlay_->setZOrder(z_order);
    root_ = static_cast<Ogre::OverlayContainer*>(
        overlays.createOverlayElement("Panel", tag + "/countsroot"));
    root_->setMetricsMode(Ogre::GMM_RELATIVE);
    root_->setPosition(0.0f, 0.0f);
    root_->setDimensions(1.0f, 1.0f);
    static_cast<Ogre::PanelOverlayElement*>(root_)->setTransparent(true);
    overlay_->add2D(root_);
    for (int cell = 0; cell < cells; ++cell) {
        auto* number = static_cast<Ogre::TextAreaOverlayElement*>(
            overlays.createOverlayElement("TextArea", tag + "/count" + std::to_string(cell)));
        number->setMetricsMode(Ogre::GMM_RELATIVE);
        number->setFontName(kFontName, Ogre::RGN_INTERNAL);
        number->setColour(Ogre::ColourValue::White);
        number->hide();
        root_->addChild(number);
        numbers_.push_back(number);
    }
    shown_.assign(static_cast<size_t>(cells), 0);
    overlay_->hide();
}

void PcCounts::destroy() {
    if (overlay_ == nullptr) return;
    auto& overlays = Ogre::OverlayManager::getSingleton();
    for (Ogre::TextAreaOverlayElement* number : numbers_) {
        root_->removeChild(number->getName());
        overlays.destroyOverlayElement(number);
    }
    numbers_.clear();
    shown_.clear();
    overlay_->remove2D(root_);
    overlays.destroyOverlayElement(root_);
    overlays.destroy(overlay_);
    overlay_ = nullptr;
    root_ = nullptr;
}

void PcCounts::show() {
    if (overlay_ != nullptr) overlay_->show();
}

void PcCounts::hide() {
    if (overlay_ != nullptr) overlay_->hide();
}

void PcCounts::set(int cell, float left, float top, int count, float down) {
    if (cell < 0 || static_cast<size_t>(cell) >= numbers_.size()) return;
    Ogre::TextAreaOverlayElement* number = numbers_[static_cast<size_t>(cell)];
    if (count < 2) {
        if (shown_[static_cast<size_t>(cell)] != 0) number->hide();
        shown_[static_cast<size_t>(cell)] = 0;
        return;
    }
    number->setPosition(left, top);
    set_char_height(number, kPoints * points_to_char_pixels() * (down / kNativeTall) / down);
    if (shown_[static_cast<size_t>(cell)] != count) {
        number->setCaption(std::to_string(count));
        number->show();
        shown_[static_cast<size_t>(cell)] = count;
    }
}

}  // namespace tl::render
