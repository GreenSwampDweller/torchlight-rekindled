#include "render/second_screen.h"

#include "core/log.h"
#include "render/automap.h"
#include "render/game_hud.h"
#include "render/menu_chrome.h"

#include <OgreHardwarePixelBuffer.h>
#include <OgreMaterialManager.h>
#include <OgreOverlay.h>
#include <OgreOverlayContainer.h>
#include <OgreOverlayManager.h>
#include <OgrePanelOverlayElement.h>
#include <OgreRenderWindow.h>
#include <OgreTextureManager.h>
#include <OgreViewport.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace tl::render {

namespace {

/// The full-window viewport on the second window: Automap's backdrop.
constexpr int kBackdropZOrder = 0;

constexpr float kPi = 3.14159265358979f;

/// The picture, 8 bits a channel, added to and clamped.
struct Canvas {
    unsigned width = 0;
    unsigned height = 0;
    std::vector<float> rgb;  // 0..1, three a pixel

    void add(int x, int y, float r, float g, float b, float amount) {
        if (x < 0 || y < 0 || x >= static_cast<int>(width) || y >= static_cast<int>(height)) return;
        float* at = &rgb[(static_cast<size_t>(y) * width + static_cast<size_t>(x)) * 3];
        at[0] += r * amount;
        at[1] += g * amount;
        at[2] += b * amount;
    }
};

/// A small repeatable generator: the same scatter every run.
struct Dice {
    uint32_t state;
    float next() {  // 0..1
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(state >> 8) / 16777216.0f;
    }
};

/// How lit a pixel `distance` from a line `half` wide is: 1 on it, 0 a pixel past its edge.
float line(float distance, float half) { return std::clamp(half + 0.5f - distance, 0.0f, 1.0f); }

/// The bezel round the map: two thin rings with ticks between them every five degrees, longer
/// and brighter on the eighths, in the HUD frames' dull gold.
void draw_bezel(Canvas& canvas, float cx, float cy, float radius, float unit) {
    const float inner = radius + 8.0f * unit;
    const float outer = radius + 30.0f * unit;
    const float r = 0.78f, g = 0.56f, b = 0.24f;
    const int x0 = std::max(0, static_cast<int>(cx - outer - 2.0f));
    const int x1 = std::min(static_cast<int>(canvas.width) - 1, static_cast<int>(cx + outer + 2.0f));
    const int y0 = std::max(0, static_cast<int>(cy - outer - 2.0f));
    const int y1 = std::min(static_cast<int>(canvas.height) - 1, static_cast<int>(cy + outer + 2.0f));
    const float step = kPi / 36.0f;  // five degrees
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const float dx = static_cast<float>(x) + 0.5f - cx;
            const float dy = static_cast<float>(y) + 0.5f - cy;
            const float at = std::sqrt(dx * dx + dy * dy);
            if (at < inner - 2.0f || at > outer + 2.0f) continue;
            float lit = 0.45f * line(std::fabs(at - inner), 0.7f * unit) +
                        0.70f * line(std::fabs(at - outer), 1.1f * unit);
            // The nearest tick, as a distance along the arc.
            const float angle = std::atan2(dy, dx);
            const float nearest = std::round(angle / step);
            const float along = std::fabs(angle - nearest * step) * at;
            const bool eighth = (static_cast<int>(nearest) % 9) == 0;
            const float from = inner + (eighth ? 4.0f : 12.0f) * unit;
            const float to = outer - 4.0f * unit;
            if (at >= from && at <= to) {
                lit += (eighth ? 0.95f : 0.42f) * line(along, (eighth ? 1.8f : 0.8f) * unit);
            }
            if (lit > 0.0f) canvas.add(x, y, r, g, b, lit);
        }
    }
}

/// One speck: a soft dot, brightest at its middle.
void draw_speck(Canvas& canvas, float cx, float cy, float size, float r, float g, float b,
                float amount) {
    const int reach = static_cast<int>(size) + 1;
    for (int y = -reach; y <= reach; ++y) {
        for (int x = -reach; x <= reach; ++x) {
            const float away = std::sqrt(static_cast<float>(x * x + y * y)) / size;
            if (away >= 1.0f) continue;
            const float glow = (1.0f - away) * (1.0f - away);
            canvas.add(static_cast<int>(cx) + x, static_cast<int>(cy) + y, r, g, b, amount * glow);
        }
    }
}

/// The corners: ember, the game's blue crystal, and a few sparks of its orange, thinning out
/// away from the map. None over the map or its bezel.
void draw_embers(Canvas& canvas, float cx, float cy, float radius, float unit,
                 float strip_bottom) {
    Dice dice{0x7041c417u};
    const float clear = radius + 46.0f * unit;
    int placed = 0;
    for (int tries = 0; tries < 6000 && placed < 140; ++tries) {
        const float x = dice.next() * static_cast<float>(canvas.width);
        const float y = dice.next() * static_cast<float>(canvas.height);
        const float away = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy));
        const float kind = dice.next();
        const float size = (2.0f + 9.0f * dice.next() * dice.next()) * unit;
        const float amount = 0.40f + 0.60f * dice.next();
        if (away < clear + size || y < strip_bottom + size) continue;
        ++placed;
        if (kind < 0.72f) draw_speck(canvas, x, y, size, 0.30f, 0.66f, 1.00f, amount);
        else draw_speck(canvas, x, y, size * 0.8f, 1.00f, 0.55f, 0.16f, amount);
    }
}

}  // namespace

SecondScreen::~SecondScreen() { detach(); }

void SecondScreen::attach(Ogre::RenderWindow* window, GameHud* hud, const Shape& shape) {
    if (window == nullptr) return;
    if (window_ != window) {
        detach();
        window_ = window;
        window_->addListener(this);
    }
    hud_ = hud;
    if (window_->hasViewportWithZOrder(kBackdropZOrder)) {
        window_->getViewportByZOrder(kBackdropZOrder)->setOverlaysEnabled(true);
    }
    build_backdrop(window_->getWidth(), window_->getHeight(), shape);
}

void SecondScreen::detach() {
    if (window_ != nullptr) {
        window_->removeListener(this);
        if (window_->hasViewportWithZOrder(kBackdropZOrder)) {
            window_->getViewportByZOrder(kBackdropZOrder)->setOverlaysEnabled(false);
        }
    }
    window_ = nullptr;
    hud_ = nullptr;
    hidden_.clear();
    shown_.clear();
    swapped_ = false;
    destroy_backdrop();
}

void SecondScreen::preViewportUpdate(const Ogre::RenderTargetViewportEvent& evt) {
    if (evt.source == nullptr || evt.source->getTarget() != window_) return;
    if (!evt.source->getOverlaysEnabled()) return;  // the map's own viewport
    swapped_ = true;
    Ogre::OverlayManager::OverlayMapIterator all =
        Ogre::OverlayManager::getSingleton().getOverlayIterator();
    while (all.hasMoreElements()) {
        Ogre::Overlay* overlay = all.getNext();
        if (overlay == nullptr || !overlay->isVisible()) continue;
        overlay->hide();
        hidden_.push_back(overlay);
    }
    const auto show = [this](Ogre::Overlay* overlay) {
        if (overlay == nullptr) return;
        overlay->show();
        shown_.push_back(overlay);
    };
    show(backdrop_);
    if (hud_ != nullptr && hud_->second_shown()) {
        for (Ogre::Overlay* overlay : hud_->second_overlays()) show(overlay);
    }
}

void SecondScreen::postViewportUpdate(const Ogre::RenderTargetViewportEvent&) {
    if (!swapped_) return;
    swapped_ = false;
    for (Ogre::Overlay* overlay : shown_) overlay->hide();
    for (Ogre::Overlay* overlay : hidden_) overlay->show();
    shown_.clear();
    hidden_.clear();
}

void SecondScreen::build_backdrop(unsigned width, unsigned height, const Shape& shape) {
    if (width == 0 || height == 0) return;
    if (backdrop_ != nullptr && width == backdrop_width_ && height == backdrop_height_ &&
        shape == backdrop_shape_) {
        return;
    }
    destroy_backdrop();
    backdrop_width_ = width;
    backdrop_height_ = height;
    backdrop_shape_ = shape;

    Canvas canvas;
    canvas.width = width;
    canvas.height = height;
    canvas.rgb.assign(static_cast<size_t>(width) * height * 3, 0.0f);
    const float shorter = static_cast<float>(std::min(width, height));
    const float unit = shorter / 1080.0f;
    if (shape.radius > 0.0f) {
        draw_bezel(canvas, shape.centre_x, shape.centre_y, shape.radius, unit);
    }
    draw_embers(canvas, shape.centre_x, shape.centre_y, shape.radius, unit, shape.strip_bottom);

    try {
        // PF_BYTE_RGBA: the one manual format every target's GL takes (see automap.cpp).
        texture_ = Ogre::TextureManager::getSingleton().createManual(
            "tl/second/backdrop_texture", Ogre::RGN_DEFAULT, Ogre::TEX_TYPE_2D, width, height, 0,
            Ogre::PF_BYTE_RGBA, Ogre::TU_STATIC);
        Ogre::HardwarePixelBufferSharedPtr buffer = texture_->getBuffer();
        buffer->lock(Ogre::HardwareBuffer::HBL_DISCARD);
        const Ogre::PixelBox& box = buffer->getCurrentLock();
        auto* data = static_cast<uint8_t*>(box.data);
        for (unsigned y = 0; y < height; ++y) {
            uint8_t* row = data + static_cast<size_t>(y) * box.rowPitch * 4;
            const float* from = &canvas.rgb[static_cast<size_t>(y) * width * 3];
            for (unsigned x = 0; x < width; ++x) {
                for (unsigned c = 0; c < 3; ++c) {
                    const float value = std::clamp(from[x * 3 + c], 0.0f, 1.0f);
                    row[x * 4 + c] = static_cast<uint8_t>(value * 255.0f + 0.5f);
                }
                row[x * 4 + 3] = 255;
            }
        }
        buffer->unlock();

        material_ = build_menu_material("tl/second/backdrop", texture_);
        auto& overlays = Ogre::OverlayManager::getSingleton();
        backdrop_ = overlays.create("tl/second/backdrop");
        backdrop_->setZOrder(1);  // under everything else this screen shows
        panel_ = static_cast<Ogre::PanelOverlayElement*>(
            overlays.createOverlayElement("Panel", "tl/second/backdrop_panel"));
        panel_->setMetricsMode(Ogre::GMM_RELATIVE);
        panel_->setPosition(0.0f, 0.0f);
        panel_->setDimensions(1.0f, 1.0f);
        panel_->setMaterial(material_);
        backdrop_->add2D(panel_);
        // Never shown here: preViewportUpdate() shows it for the second window's viewport.
        TL_LOG_INFO("second screen: backdrop drawn at %u x %u", width, height);
    } catch (const Ogre::Exception& e) {
        TL_LOG_WARN("second screen: no backdrop: %s", e.getDescription().c_str());
        destroy_backdrop();
    }
}

void SecondScreen::destroy_backdrop() {
    if (Ogre::OverlayManager* overlays = Ogre::OverlayManager::getSingletonPtr()) {
        if (backdrop_ != nullptr) {
            backdrop_->hide();
            if (panel_ != nullptr) backdrop_->remove2D(panel_);
        }
        if (panel_ != nullptr) overlays->destroyOverlayElement(panel_);
        if (backdrop_ != nullptr) overlays->destroy(backdrop_);
    }
    panel_ = nullptr;
    backdrop_ = nullptr;
    if (material_) {
        if (Ogre::MaterialManager* materials = Ogre::MaterialManager::getSingletonPtr()) {
            materials->remove(material_);
        }
        material_.reset();
    }
    if (texture_) {
        if (Ogre::TextureManager* textures = Ogre::TextureManager::getSingletonPtr()) {
            textures->remove(texture_);
        }
        texture_.reset();
    }
    backdrop_width_ = 0;
    backdrop_height_ = 0;
    backdrop_shape_ = Shape{};
}

}  // namespace tl::render
