// The on-screen pad, drawn (touch_overlay.h). PORT: the port's own throughout.
#include "render/touch_overlay.h"

#include "core/touch_pad.h"
#include "render/menu_chrome.h"
#include "render/overlay_set.h"

#include <OgreFontManager.h>
#include <OgreHardwarePixelBuffer.h>
#include <OgreMaterialManager.h>
#include <OgreOverlay.h>
#include <OgreOverlayContainer.h>
#include <OgreOverlayManager.h>
#include <OgrePanelOverlayElement.h>
#include <OgrePass.h>
#include <OgreTechnique.h>
#include <OgreTextAreaOverlayElement.h>
#include <OgreTextureManager.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

namespace tl::render {

namespace {

constexpr const char* kOverlayName = "tl/touchpad";
/// Over the performance line (640) too: nothing may cover a button.
constexpr unsigned short kZOrder = 649;
constexpr unsigned kTextureSize = 128;

using Shape = tl::core::TouchPad::Shape;

/// A white disc whose alpha is its shape: a solid rim and, inside it,
/// `fill` - so a button reads as a ring with a glassy middle, and a knob
/// (fill 1) as a solid dot. The edge is smoothed over a texel and a half.
Ogre::TexturePtr disc_texture(const char* name, float fill) {
    auto& textures = Ogre::TextureManager::getSingleton();
    Ogre::TexturePtr existing = textures.getByName(name, Ogre::RGN_INTERNAL);
    if (existing) return existing;
    // PF_BYTE_RGBA: the only manual format the console's GLES allocates.
    Ogre::TexturePtr texture =
        textures.createManual(name, Ogre::RGN_INTERNAL, Ogre::TEX_TYPE_2D, kTextureSize,
                              kTextureSize, 0, Ogre::PF_BYTE_RGBA);
    const Ogre::HardwarePixelBufferSharedPtr buffer = texture->getBuffer();
    buffer->lock(Ogre::HardwareBuffer::HBL_DISCARD);
    const Ogre::PixelBox& box = buffer->getCurrentLock();
    auto* data = static_cast<std::uint8_t*>(box.data);
    const float half = 0.5f * static_cast<float>(kTextureSize);
    const float rim = 0.12f * half;
    for (unsigned y = 0; y < kTextureSize; ++y) {
        std::uint8_t* row = data + static_cast<std::size_t>(y) * box.rowPitch * 4;
        for (unsigned x = 0; x < kTextureSize; ++x) {
            const float dx = static_cast<float>(x) + 0.5f - half;
            const float dy = static_cast<float>(y) + 0.5f - half;
            const float d = std::sqrt(dx * dx + dy * dy);
            const float edge = std::clamp((half - 1.0f - d) / 1.5f, 0.0f, 1.0f);
            const float inner = std::clamp((half - rim - d) / 1.5f, 0.0f, 1.0f);
            const float alpha = edge * (1.0f - inner + inner * fill);
            std::uint8_t* pixel = row + static_cast<std::size_t>(x) * 4;
            pixel[0] = pixel[1] = pixel[2] = 255;
            pixel[3] = static_cast<std::uint8_t>(std::lround(alpha * 255.0f));
        }
    }
    buffer->unlock();
    return texture;
}

/// A disc's texture tinted and faded: the colour and alpha are manual
/// arguments multiplied into the texel, because a panel takes its colour from
/// its material (render/hud.cpp's `solid_material`).
Ogre::MaterialPtr disc_material(const std::string& name, const Ogre::TexturePtr& texture,
                                const Ogre::ColourValue& colour) {
    auto& materials = Ogre::MaterialManager::getSingleton();
    Ogre::MaterialPtr existing = materials.getByName(name, Ogre::RGN_INTERNAL);
    if (existing) return existing;
    Ogre::MaterialPtr material = materials.create(name, Ogre::RGN_INTERNAL);
    Ogre::Pass* pass = material->getTechnique(0)->getPass(0);
    pass->setLightingEnabled(false);
    pass->setDepthCheckEnabled(false);
    pass->setDepthWriteEnabled(false);
    pass->setCullingMode(Ogre::CULL_NONE);
    pass->setSceneBlending(Ogre::SBT_TRANSPARENT_ALPHA);
    Ogre::TextureUnitState* unit = pass->createTextureUnitState();
    unit->setTexture(texture);
    unit->setTextureFiltering(Ogre::FO_LINEAR, Ogre::FO_LINEAR, Ogre::FO_NONE);
    unit->setTextureAddressingMode(Ogre::TextureUnitState::TAM_CLAMP);
    unit->setColourOperationEx(Ogre::LBX_MODULATE, Ogre::LBS_TEXTURE, Ogre::LBS_MANUAL,
                               Ogre::ColourValue::White, colour);
    unit->setAlphaOperation(Ogre::LBX_MODULATE, Ogre::LBS_TEXTURE, Ogre::LBS_MANUAL, 1.0f,
                            colour.a);
    return material;
}

/// The face buttons' colours, as the Xbox pad prints its letters.
Ogre::ColourValue tint_colour(int tint) {
    switch (tint) {
        case 1: return Ogre::ColourValue(0.35f, 0.85f, 0.35f);
        case 2: return Ogre::ColourValue(0.95f, 0.35f, 0.30f);
        case 3: return Ogre::ColourValue(0.30f, 0.55f, 1.00f);
        case 4: return Ogre::ColourValue(1.00f, 0.82f, 0.25f);
        default: return Ogre::ColourValue::White;
    }
}

Ogre::ColourValue style_colour(const Shape& shape, int style) {
    if (style == 0) return tint_colour(shape.tint);
    if (shape.tint != 0) return tint_colour(shape.tint);
    if (style == 1) {
        if (shape.kind == Shape::kStickBase || shape.kind == Shape::kStickKnob)
            return Ogre::ColourValue(0.2f, 0.92f, 1.0f);
        if (shape.kind == Shape::kDpadBase) return Ogre::ColourValue(0.55f, 0.35f, 1.0f);
        return Ogre::ColourValue(0.38f, 0.76f, 1.0f);
    }
    if (shape.kind == Shape::kStickBase || shape.kind == Shape::kDpadBase)
        return Ogre::ColourValue(0.24f, 0.31f, 0.42f);
    if (shape.kind == Shape::kStickKnob) return Ogre::ColourValue(0.75f, 0.83f, 0.95f);
    return Ogre::ColourValue(0.67f, 0.75f, 0.88f);
}

/// How opaque a shape is, at rest and while held.
float shape_alpha(const Shape& s) {
    const float opacity = tl::core::touch_pad().opacity();
    switch (s.kind) {
        case Shape::kButton: return (s.pressed ? 0.80f : 0.40f) * opacity;
        case Shape::kStickBase: return (s.pressed ? 0.40f : 0.25f) * opacity;
        case Shape::kStickKnob: return (s.pressed ? 0.80f : 0.45f) * opacity;
        case Shape::kDpadBase: return 0.18f * opacity;
    }
    return 0.4f;
}

/// A label's height in pixels, by its length: a letter fills most of its
/// disc, "Start" has to fit across one.
float label_height(const Shape& s) {
    const std::size_t n = std::strlen(s.label);
    return s.radius * (n <= 1 ? 1.0f : n == 2 ? 0.75f : 0.5f) * ui_text_scale();
}

/// Give a disc the material of its kind, tint and state, made on first use.
/// Looked up by name, and only when the state changed: the panels hold the
/// only references, and they go with the overlay system before OGRE does.
void apply_material(Ogre::PanelOverlayElement* disc, const Shape& s) {
    const int tint = std::clamp(s.tint, 0, 4);
    const int style = tl::core::touch_pad().style();
    const int alpha = static_cast<int>(std::lround(shape_alpha(s) * 100.0f));
    const std::string name = "tl/touchpad/m" + std::to_string(s.kind) + "_" + std::to_string(tint) +
                             "_s" + std::to_string(style) +
                             (s.pressed ? "_on_" : "_off_") + std::to_string(alpha);
    if (disc->getMaterialName() == name) return;
    const float fill = s.kind == Shape::kStickKnob ? 1.0f : style == 2 ? 0.82f
                                                     : style == 1 ? 0.16f
                                                                  : 0.45f;
    std::string texture_name = s.kind == Shape::kStickKnob ? "tl/touchpad/knob_s"
                                                           : "tl/touchpad/disc_s";
    texture_name += std::to_string(style);
    const Ogre::TexturePtr texture = disc_texture(texture_name.c_str(), fill);
    Ogre::ColourValue colour = style_colour(s, style);
    colour.a = std::clamp(shape_alpha(s), 0.0f, 1.0f);
    disc->setMaterial(disc_material(name, texture, colour));
}

}  // namespace

void TouchOverlay::build(std::size_t count) {
    auto& overlays = Ogre::OverlayManager::getSingleton();
    if (overlay_ != nullptr) {
        // The screen changed size: everything again, at the new size.
        for (Ogre::TextAreaOverlayElement* label : labels_) {
            if (label != nullptr) overlays.destroyOverlayElement(label);
        }
        for (Ogre::PanelOverlayElement* disc : discs_) overlays.destroyOverlayElement(disc);
        overlays.destroyOverlayElement(root_);
        overlays.destroy(overlay_);
    }
    discs_.clear();
    labels_.clear();

    overlay_ = overlays.create(kOverlayName);
    built_text_scale_ = ui_text_scale();
    built_opacity_ = tl::core::touch_pad().opacity();
    built_style_ = tl::core::touch_pad().style();
    overlay_->setZOrder(kZOrder);
    root_ = static_cast<Ogre::OverlayContainer*>(
        overlays.createOverlayElement("Panel", std::string(kOverlayName) + "/root"));
    root_->setMetricsMode(Ogre::GMM_PIXELS);
    root_->setPosition(0, 0);
    root_->setDimensions(built_width_, built_height_);
    static_cast<Ogre::PanelOverlayElement*>(root_)->setTransparent(true);
    overlay_->add2D(root_);

    // The font is the HUD's; without it the discs go unlabelled. Loaded, not
    // just made: a label asking for a font that failed to load throws.
    Ogre::FontManager* fonts = Ogre::FontManager::getSingletonPtr();
    const Ogre::FontPtr hud_font =
        fonts != nullptr ? fonts->getByName(kFontName, Ogre::RGN_INTERNAL) : Ogre::FontPtr();
    const bool font = hud_font && hud_font->isLoaded();
    const std::vector<Shape> shapes = tl::core::touch_pad().shapes();
    for (std::size_t i = 0; i < count; ++i) {
        auto* disc = static_cast<Ogre::PanelOverlayElement*>(overlays.createOverlayElement(
            "Panel", std::string(kOverlayName) + "/disc" + std::to_string(i)));
        disc->setMetricsMode(Ogre::GMM_PIXELS);
        root_->addChild(disc);
        discs_.push_back(disc);
    }
    // The labels after every disc, so a knob never covers one.
    for (std::size_t i = 0; i < count; ++i) {
        Ogre::TextAreaOverlayElement* label = nullptr;
        if (font && i < shapes.size() && shapes[i].label[0] != '\0') {
            label = static_cast<Ogre::TextAreaOverlayElement*>(overlays.createOverlayElement(
                "TextArea", std::string(kOverlayName) + "/label" + std::to_string(i)));
            label->setMetricsMode(Ogre::GMM_PIXELS);
            label->setFontName(kFontName, Ogre::RGN_INTERNAL);
            label->setAlignment(Ogre::TextAreaOverlayElement::Center);
            label->setColour(Ogre::ColourValue(
                1.0f, 1.0f, 1.0f, std::clamp(0.72f * built_opacity_, 0.45f, 1.0f)));
            label->setCaption(shapes[i].label);
            const float height = label_height(shapes[i]);
            set_char_height(label, height);
            label->setPosition(shapes[i].x, shapes[i].y - 0.5f * height);
            root_->addChild(label);
        }
        labels_.push_back(label);
    }
}

void TouchOverlay::step() {
    Ogre::OverlayManager* overlays = Ogre::OverlayManager::getSingletonPtr();
    if (overlays == nullptr) return;
    // A restart throws the overlay system away with everything in it
    // (Hud::destroy); what this held went with it.
    if (overlay_ != nullptr && overlays->getByName(kOverlayName) == nullptr) {
        overlay_ = nullptr;
        root_ = nullptr;
        discs_.clear();
        labels_.clear();
    }
    const tl::core::TouchPad& pad = tl::core::touch_pad();
    if (!pad.visible() || pad.width() <= 0.0f || pad.height() <= 0.0f) {
        if (overlay_ != nullptr && overlay_->isVisible()) overlay_->hide();
        return;
    }
    const std::vector<Shape> shapes = pad.shapes();
    if (overlay_ == nullptr || discs_.size() != shapes.size() || built_width_ != pad.width() ||
        built_height_ != pad.height() || built_text_scale_ != ui_text_scale() ||
        built_opacity_ != pad.opacity() || built_style_ != pad.style()) {
        built_width_ = pad.width();
        built_height_ = pad.height();
        build(shapes.size());
    }
    for (std::size_t i = 0; i < shapes.size(); ++i) {
        const Shape& s = shapes[i];
        Ogre::PanelOverlayElement* disc = discs_[i];
        set_position(disc, s.x - s.radius, s.y - s.radius);
        set_dimensions(disc, 2.0f * s.radius, 2.0f * s.radius);
        apply_material(disc, s);
    }
    if (!overlay_->isVisible()) overlay_->show();
}

}  // namespace tl::render
