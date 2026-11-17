#include "render/second_screen.h"

#include "core/log.h"
#include "render/automap.h"
#include "render/game_hud.h"
#include "render/item_tip.h"
#include "render/menu_chrome.h"
#include "render/overlay_set.h"

#include <OgreHardwarePixelBuffer.h>
#include <OgreMaterialManager.h>
#include <OgreOverlay.h>
#include <OgreOverlayContainer.h>
#include <OgreOverlayManager.h>
#include <OgrePanelOverlayElement.h>
#include <OgreTextAreaOverlayElement.h>
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

/// The widgets, as fractions of the window's height: the portal button's side, how far it and
/// the readout are from the edges, and the gap between the strip and the readout under it.
constexpr float kButtonSide = 0.125f;
constexpr float kEdge = 0.03f;
constexpr float kUnderStrip = 0.012f;
/// The frame the HUD's own slots are drawn in, and how far inside it a slot's picture is (36
/// of its 48).
constexpr const char* kFrameArt = "spells_window";
constexpr float kFrameInset = 6.0f / 48.0f;
/// The readout's, the count's and a spell's cooling seconds' sizes, in the interface's points.
/// This is a handheld's second display, held at arm's length: the words are read at a glance,
/// so they are about twice what the main window's interface would use.
constexpr float kReadoutPoints = 29.0f;
constexpr float kCountPoints = 21.0f;
constexpr float kCoolingPoints = 26.0f;
/// What the readout's size is kept clear of the map's bezel for: an average glyph is about this
/// much of the text's height wide, and these are the longest captions each corner is made to
/// hold ("Ember Mine Floor 12", "12,345,678 gold", "Fame 100"), in characters. A longer one
/// reaches in over the bezel rather than shrinking the text further.
constexpr float kGlyphWidth = 0.5f;
constexpr float kLongestPlace = 20.0f;
constexpr float kLongestPurse = 16.0f;
constexpr float kLongestLevel = 10.0f;
/// How far the bezel reaches past the map's circle, as draw_bezel() draws it, in pixels at 1080
/// on the shorter side.
constexpr float kBezelReach = 30.0f;

const Ogre::ColourValue kWordColour(0.92f, 0.86f, 0.70f, 1.0f);
const Ogre::ColourValue kDimColour(0.55f, 0.52f, 0.45f, 1.0f);

/// 12345 as "12,345".
std::string grouped(int value) {
    std::string digits = std::to_string(value < 0 ? -value : value);
    for (int at = static_cast<int>(digits.size()) - 3; at > 0; at -= 3) {
        digits.insert(static_cast<size_t>(at), ",");
    }
    return value < 0 ? "-" + digits : digits;
}

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
    shape_ = shape;
    if (window_->hasViewportWithZOrder(kBackdropZOrder)) {
        window_->getViewportByZOrder(kBackdropZOrder)->setOverlaysEnabled(true);
    }
    build_backdrop(window_->getWidth(), window_->getHeight(), shape);
    build_widgets();
    layout_widgets();
    apply_readout();
    build_item_overlays();
    layout_item_tips();
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
    destroy_widgets();
    destroy_item_overlays();
    item_tips_.clear();
    item_signature_.clear();
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
    // The item windows have the screen to themselves under the strip: no bezel with no map
    // in it, no readout or buttons under them.
    const bool items = !item_tips_.empty() && item_plates_ != nullptr;
    if (!items) show(backdrop_);
    if (hud_ != nullptr && hud_->second_shown()) {
        for (Ogre::Overlay* overlay : hud_->second_overlays()) show(overlay);
        if (!items) {
            show(widgets_);
            show(words_);
        }
    }
    if (items) {
        show(item_plates_);
        show(item_words_);
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

void SecondScreen::set_readout(const Readout& readout) {
    const bool same = readout.level == readout_.level && readout.fame_level == readout_.fame_level &&
                      readout.gold == readout_.gold && readout.floor == readout_.floor &&
                      readout.portal_scrolls == readout_.portal_scrolls &&
                      readout.skill_set == readout_.skill_set &&
                      readout.portal_icon == readout_.portal_icon &&
                      readout.spells == readout_.spells;
    if (same) return;
    // The scroll's picture is kept once seen: the button shows it, dimmed, with none carried.
    const std::string icon = readout.portal_icon.empty() ? readout_.portal_icon : readout.portal_icon;
    readout_ = readout;
    readout_.portal_icon = icon;
    apply_readout();
}

SecondScreen::Tap SecondScreen::hit(float x, float y, int* spell) const {
    if (window_ == nullptr || hud_ == nullptr || !hud_->second_shown()) return Tap::None;
    // Under the item windows there is nothing to press: only the strip.
    if (!item_tips_.empty()) return y > shape_.strip_bottom ? Tap::None : Tap::Strip;
    const auto on = [&](float left, float top) {
        return portal_size_ > 0.0f && x >= left && x <= left + portal_size_ && y >= top &&
               y <= top + portal_size_;
    };
    if (on(portal_left_, portal_top_)) return Tap::Portal;
    if (set_size_ > 0.0f && x >= set_left_ && x <= set_left_ + set_size_ && y >= set_top_ &&
        y <= set_top_ + set_size_) {
        return Tap::SkillSet;
    }
    for (size_t slot = 0; slot < spell_buttons_.size(); ++slot) {
        if (!readout_.spells[slot].known || !on(spell_buttons_[slot].left, spell_buttons_[slot].top)) {
            continue;
        }
        if (spell != nullptr) *spell = static_cast<int>(slot);
        return Tap::Spell;
    }
    return y > shape_.strip_bottom ? Tap::Map : Tap::Strip;
}

void SecondScreen::build_widgets() {
    if (widgets_ != nullptr) return;
    if (!ensure_menu_font()) return;
    try {
        auto& overlays = Ogre::OverlayManager::getSingleton();
        widgets_ = overlays.create("tl/second/widgets");
        widgets_->setZOrder(475);  // over the HUD's strip (473, 474), which it never overlaps
        words_ = overlays.create("tl/second/words");
        words_->setZOrder(476);
        const auto root = [&](const char* name, Ogre::Overlay* into) {
            auto* box =
                static_cast<Ogre::OverlayContainer*>(overlays.createOverlayElement("Panel", name));
            box->setMetricsMode(Ogre::GMM_RELATIVE);
            box->setPosition(0.0f, 0.0f);
            box->setDimensions(1.0f, 1.0f);
            static_cast<Ogre::PanelOverlayElement*>(box)->setTransparent(true);
            into->add2D(box);
            return box;
        };
        widget_root_ = root("tl/second/widgetroot", widgets_);
        word_root_ = root("tl/second/wordroot", words_);
        const auto panel = [&](const char* name) {
            auto* made = static_cast<Ogre::PanelOverlayElement*>(
                overlays.createOverlayElement("Panel", name));
            made->setMetricsMode(Ogre::GMM_RELATIVE);
            made->setTransparent(true);  // until it has a picture
            widget_root_->addChild(made);
            return made;
        };
        // The frame's name sorts before the icon's: a container draws its children by name.
        portal_frame_ = panel("tl/second/portal/0frame");
        set_frame_ = panel("tl/second/set/0frame");
        portal_icon_ = panel("tl/second/portal/1icon");
        for (size_t slot = 0; slot < spell_buttons_.size(); ++slot) {
            const std::string stem = "tl/second/spell/" + std::to_string(slot);
            spell_buttons_[slot].frame = panel((stem + "/0frame").c_str());
            spell_buttons_[slot].icon = panel((stem + "/1icon").c_str());
        }
        const auto words = [&](const char* name, Ogre::TextAreaOverlayElement::Alignment align) {
            auto* made = static_cast<Ogre::TextAreaOverlayElement*>(
                overlays.createOverlayElement("TextArea", name));
            made->setMetricsMode(Ogre::GMM_RELATIVE);
            made->setFontName(kFontName, Ogre::RGN_INTERNAL);
            made->setColour(kWordColour);
            made->setAlignment(align);
            word_root_->addChild(made);
            return made;
        };
        using Align = Ogre::TextAreaOverlayElement;
        portal_count_ = words("tl/second/portal/count", Align::Right);
        set_text_ = words("tl/second/set/label", Align::Center);
        portal_label_ = words("tl/second/portal/label", Align::Left);
        level_text_ = words("tl/second/level", Align::Left);
        fame_text_ = words("tl/second/fame", Align::Left);
        floor_text_ = words("tl/second/floor", Align::Right);
        gold_text_ = words("tl/second/gold", Align::Right);
        for (size_t slot = 0; slot < spell_buttons_.size(); ++slot) {
            const std::string name = "tl/second/spell/" + std::to_string(slot) + "/cooling";
            spell_buttons_[slot].cooling = words(name.c_str(), Align::Center);
        }
        // Never shown here: preViewportUpdate() shows them for the second window's viewport.
    } catch (const Ogre::Exception& e) {
        TL_LOG_WARN("second screen: no readout: %s", e.getDescription().c_str());
        destroy_widgets();
    }
}

void SecondScreen::destroy_widgets() {
    if (Ogre::OverlayManager* overlays = Ogre::OverlayManager::getSingletonPtr()) {
        for (Ogre::Overlay* overlay : {widgets_, words_}) {
            if (overlay != nullptr) overlay->hide();
        }
        for (SpellButton& button : spell_buttons_) {
            for (Ogre::OverlayElement* element : {static_cast<Ogre::OverlayElement*>(button.frame),
                                                  static_cast<Ogre::OverlayElement*>(button.icon),
                                                  static_cast<Ogre::OverlayElement*>(button.cooling)}) {
                if (element == nullptr) continue;
                if (Ogre::OverlayContainer* parent = element->getParent()) {
                    parent->removeChild(element->getName());
                }
                overlays->destroyOverlayElement(element);
            }
        }
        for (Ogre::OverlayElement* element :
             {static_cast<Ogre::OverlayElement*>(portal_frame_),
              static_cast<Ogre::OverlayElement*>(set_frame_),
              static_cast<Ogre::OverlayElement*>(set_text_),
              static_cast<Ogre::OverlayElement*>(portal_icon_),
              static_cast<Ogre::OverlayElement*>(portal_count_),
              static_cast<Ogre::OverlayElement*>(portal_label_),
              static_cast<Ogre::OverlayElement*>(level_text_),
              static_cast<Ogre::OverlayElement*>(fame_text_),
              static_cast<Ogre::OverlayElement*>(floor_text_),
              static_cast<Ogre::OverlayElement*>(gold_text_)}) {
            if (element == nullptr) continue;
            if (Ogre::OverlayContainer* parent = element->getParent()) {
                parent->removeChild(element->getName());
            }
            overlays->destroyOverlayElement(element);
        }
        if (widget_root_ != nullptr && widgets_ != nullptr) widgets_->remove2D(widget_root_);
        if (word_root_ != nullptr && words_ != nullptr) words_->remove2D(word_root_);
        if (widget_root_ != nullptr) overlays->destroyOverlayElement(widget_root_);
        if (word_root_ != nullptr) overlays->destroyOverlayElement(word_root_);
        if (widgets_ != nullptr) overlays->destroy(widgets_);
        if (words_ != nullptr) overlays->destroy(words_);
    }
    widgets_ = nullptr;
    words_ = nullptr;
    widget_root_ = nullptr;
    word_root_ = nullptr;
    portal_frame_ = nullptr;
    set_frame_ = nullptr;
    set_text_ = nullptr;
    set_size_ = 0.0f;
    portal_icon_ = nullptr;
    portal_count_ = nullptr;
    portal_label_ = nullptr;
    level_text_ = nullptr;
    fame_text_ = nullptr;
    floor_text_ = nullptr;
    gold_text_ = nullptr;
    shown_icon_.clear();
    portal_size_ = 0.0f;
    spell_buttons_ = {};
}

// The readout in the two corners under the strip - the character on the left, the place and
// the purse on the right - and the portal button in the bottom left corner, all clear of the
// map's circle.
void SecondScreen::layout_widgets() {
    if (window_ == nullptr || widgets_ == nullptr) return;
    const float across = static_cast<float>(window_->getWidth());
    const float down = static_cast<float>(window_->getHeight());
    if (across <= 0.0f || down <= 0.0f) return;
    const float edge = kEdge * down;
    // The interface's own rule for a text's size: its points at 768 lines, scaled to the screen.
    const float scale = down / kNativeHeight;
    float line = kReadoutPoints * ui_points_to_pixels() * scale;
    const float count_line = kCountPoints * ui_points_to_pixels() * scale;
    const float cooling_line = kCoolingPoints * ui_points_to_pixels() * scale;

    const float first = shape_.strip_bottom + kUnderStrip * down;

    // The readout is made as large as it can be without reaching the map's bezel. A caption of
    // `chars` characters on the text height `h`, starting at `top` and hugging the left or the
    // right edge, is clear when its inner end is outside the circle's widest part at those
    // heights (which is where the circle is nearest the text's middle).
    if (shape_.radius > 0.0f) {
        const float reach = shape_.radius + kBezelReach * std::min(across, down) / 1080.0f;
        const auto clear = [&](float h, float top, float chars, bool right) {
            const float nearest = std::min(std::max(shape_.centre_y, top), top + h);
            const float dy = shape_.centre_y - nearest;
            if (std::fabs(dy) >= reach) return true;
            const float half = std::sqrt(reach * reach - dy * dy);
            const float width = chars * kGlyphWidth * h;
            return right ? across - edge - width >= shape_.centre_x + half
                         : edge + width <= shape_.centre_x - half;
        };
        const float floor_size = 1.0f;  // never smaller than this: a bound for the loop
        while (line > floor_size &&
               !(clear(line, first, kLongestPlace, true) &&
                 clear(line, first + line * 1.15f, kLongestPurse, true) &&
                 clear(line, first, kLongestLevel, false) &&
                 clear(line, first + line * 1.15f, kLongestLevel, false))) {
            line -= 0.5f;
        }
    }
    const auto place = [&](Ogre::TextAreaOverlayElement* text, float x, float y, float height) {
        if (text == nullptr) return;
        set_char_height(text, height / down);
        text->setPosition(x / across, y / down);
    };
    place(level_text_, edge, first, line);
    place(fame_text_, edge, first + line * 1.15f, line);
    place(floor_text_, across - edge, first, line);
    place(gold_text_, across - edge, first + line * 1.15f, line);

    portal_size_ = kButtonSide * down;
    portal_left_ = edge;
    portal_top_ = down - edge - portal_size_;
    if (portal_frame_ != nullptr) {
        portal_frame_->setPosition(portal_left_ / across, portal_top_ / down);
        portal_frame_->setDimensions(portal_size_ / across, portal_size_ / down);
    }
    if (portal_icon_ != nullptr) {
        const float inset = portal_size_ * kFrameInset;
        portal_icon_->setPosition((portal_left_ + inset) / across, (portal_top_ + inset) / down);
        portal_icon_->setDimensions((portal_size_ - inset * 2.0f) / across,
                                    (portal_size_ - inset * 2.0f) / down);
    }
    // The count in the button's bottom right corner, the word beside the button.
    place(portal_count_, portal_left_ + portal_size_ * 0.86f,
          portal_top_ + portal_size_ * 0.86f - count_line, count_line);
    place(portal_label_, portal_left_ + portal_size_ + edge * 0.4f,
          portal_top_ + (portal_size_ - count_line) * 0.5f, count_line);

    // The skill set's button: in the strip's own band, in the room left of its first slot.
    set_size_ = std::min(portal_size_, shape_.strip_bottom * 0.6f);
    set_left_ = edge * 0.5f;
    set_top_ = std::max(0.0f, (shape_.strip_bottom - set_size_) * 0.3f);
    if (set_frame_ != nullptr) {
        set_frame_->setPosition(set_left_ / across, set_top_ / down);
        set_frame_->setDimensions(set_size_ / across, set_size_ / down);
    }
    place(set_text_, set_left_ + set_size_ * 0.5f, set_top_ + (set_size_ - count_line) * 0.5f,
          count_line);

    // The spells: the first slot in the bottom right corner, the rest above it. The seconds a
    // spell is cooling for are written across the middle of its picture.
    const float inset = portal_size_ * kFrameInset;
    const float gap = edge * 0.3f;
    for (size_t slot = 0; slot < spell_buttons_.size(); ++slot) {
        SpellButton& button = spell_buttons_[slot];
        button.left = across - edge - portal_size_;
        button.top = down - edge - portal_size_ - static_cast<float>(slot) * (portal_size_ + gap);
        if (button.frame != nullptr) {
            button.frame->setPosition(button.left / across, button.top / down);
            button.frame->setDimensions(portal_size_ / across, portal_size_ / down);
        }
        if (button.icon != nullptr) {
            button.icon->setPosition((button.left + inset) / across, (button.top + inset) / down);
            button.icon->setDimensions((portal_size_ - inset * 2.0f) / across,
                                       (portal_size_ - inset * 2.0f) / down);
        }
        place(button.cooling, button.left + portal_size_ * 0.5f,
              button.top + (portal_size_ - cooling_line) * 0.5f, cooling_line);
    }
}

void SecondScreen::apply_readout() {
    if (widgets_ == nullptr) return;
    if (level_text_ != nullptr) set_caption(level_text_, "Level " + std::to_string(readout_.level));
    if (fame_text_ != nullptr) {
        set_caption(fame_text_, "Fame " + std::to_string(readout_.fame_level));
    }
    if (floor_text_ != nullptr) set_caption(floor_text_, readout_.floor);
    if (gold_text_ != nullptr) set_caption(gold_text_, grouped(readout_.gold) + " gold");

    const bool carried = readout_.portal_scrolls > 0;
    if (portal_count_ != nullptr) {
        set_caption(portal_count_, std::to_string(readout_.portal_scrolls));
        set_colour(portal_count_, carried ? kWordColour : kDimColour);
    }
    if (portal_label_ != nullptr) {
        set_caption(portal_label_, "Town Portal");
        set_colour(portal_label_, carried ? kWordColour : kDimColour);
    }
    // The pictures are the HUD's: its slot frame, and the scroll's own icon.
    const auto picture = [this](Ogre::PanelOverlayElement* panel, const std::string& name) {
        if (panel == nullptr) return false;
        Ogre::MaterialPtr material;
        float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
        if (hud_ == nullptr || !hud_->icon_art(name, material, u0, v0, u1, v1)) return false;
        panel->setMaterial(material);
        panel->setUV(u0, v0, u1, v1);
        panel->setTransparent(false);
        return true;
    };
    if (portal_frame_ != nullptr && portal_frame_->isTransparent()) picture(portal_frame_, kFrameArt);
    if (set_frame_ != nullptr && set_frame_->isTransparent()) picture(set_frame_, kFrameArt);
    if (set_text_ != nullptr) {
        set_caption(set_text_, readout_.skill_set == 0 ? "Set 1" : "Set 2");
    }
    if (portal_icon_ != nullptr && readout_.portal_icon != shown_icon_) {
        if (picture(portal_icon_, readout_.portal_icon)) shown_icon_ = readout_.portal_icon;
    }

    // A button for each slot that holds a spell; nothing at all for an empty one.
    for (size_t slot = 0; slot < spell_buttons_.size(); ++slot) {
        SpellButton& button = spell_buttons_[slot];
        const Readout::Spell& spell = readout_.spells[slot];
        if (button.frame == nullptr || button.icon == nullptr) continue;
        if (!spell.known) {
            button.frame->hide();
            button.icon->hide();
            if (button.cooling != nullptr) set_caption(button.cooling, "");
            continue;
        }
        if (button.frame->isTransparent()) picture(button.frame, kFrameArt);
        if (spell.icon != button.shown_icon) {
            // A picture the sheets have not got leaves the frame bare.
            if (picture(button.icon, spell.icon)) {
                button.shown_icon = spell.icon;
            } else {
                button.icon->setTransparent(true);
                button.shown_icon.clear();
            }
        }
        button.frame->show();
        button.icon->show();
        if (button.cooling != nullptr) {
            set_caption(button.cooling, spell.cooling > 0 ? std::to_string(spell.cooling) : "");
        }
    }
}

// ---------------------------------------------------------------------------
// The item windows
// ---------------------------------------------------------------------------

namespace {

/// How far the windows keep from the screen's edges and from each other, and the line round
/// each, as fractions of the screen's height.
constexpr float kItemMargin = 0.022f;
constexpr float kItemGap = 0.016f;
constexpr float kItemEdge = 0.0025f;
/// The most a window is drawn over the interface's own size: past it the glyphs, cut for
/// the main screen, go soft.
constexpr float kItemMostScale = 3.2f;

/// One colour, as a texture a panel can be given.
Ogre::TexturePtr colour_texture(const char* name, uint8_t r, uint8_t g, uint8_t b) {
    Ogre::TexturePtr texture = Ogre::TextureManager::getSingleton().createManual(
        name, Ogre::RGN_DEFAULT, Ogre::TEX_TYPE_2D, 2, 2, 0, Ogre::PF_BYTE_RGBA, Ogre::TU_STATIC);
    Ogre::HardwarePixelBufferSharedPtr buffer = texture->getBuffer();
    buffer->lock(Ogre::HardwareBuffer::HBL_DISCARD);
    const Ogre::PixelBox& box = buffer->getCurrentLock();
    auto* data = static_cast<uint8_t*>(box.data);
    for (unsigned y = 0; y < 2; ++y) {
        uint8_t* row = data + static_cast<size_t>(y) * box.rowPitch * 4;
        for (unsigned x = 0; x < 2; ++x) {
            row[x * 4 + 0] = r;
            row[x * 4 + 1] = g;
            row[x * 4 + 2] = b;
            row[x * 4 + 3] = 255;
        }
    }
    buffer->unlock();
    return texture;
}

/// Every text of a window, one after another.
void sign(std::string& into, const tl::core::EquipmentTooltip& tip) {
    const auto add = [&into](const tl::core::TooltipText& text) {
        if (text.shown) into += text.text;
        into += static_cast<char>(31);
    };
    add(tip.name);
    add(tip.type);
    add(tip.big);
    add(tip.big_label);
    add(tip.stats);
    add(tip.effects);
    for (const tl::core::TooltipText& requirement : tip.requirements) add(requirement);
    add(tip.description);
    add(tip.price);
    into += static_cast<char>(30);
}

}  // namespace

void SecondScreen::set_item_tips(std::vector<tl::core::EquipmentTooltip> tips) {
    if (tips.size() > kItemWindows) tips.resize(kItemWindows);
    std::string signature;
    for (const tl::core::EquipmentTooltip& tip : tips) sign(signature, tip);
    if (signature == item_signature_ && tips.size() == item_tips_.size()) return;
    item_signature_ = std::move(signature);
    item_tips_ = std::move(tips);
    layout_item_tips();
}

void SecondScreen::build_item_overlays() {
    if (item_plates_ != nullptr) return;
    if (!ensure_menu_font()) return;
    try {
        auto& overlays = Ogre::OverlayManager::getSingleton();
        item_plates_ = overlays.create("tl/second/item/plates");
        item_plates_->setZOrder(477);  // over everything else this screen shows
        item_words_ = overlays.create("tl/second/item/words");
        item_words_->setZOrder(478);
        const auto root = [&](const char* name, Ogre::Overlay* into) {
            auto* box =
                static_cast<Ogre::OverlayContainer*>(overlays.createOverlayElement("Panel", name));
            box->setMetricsMode(Ogre::GMM_RELATIVE);
            box->setPosition(0.0f, 0.0f);
            box->setDimensions(1.0f, 1.0f);
            static_cast<Ogre::PanelOverlayElement*>(box)->setTransparent(true);
            into->add2D(box);
            return box;
        };
        item_plate_root_ = root("tl/second/item/plateroot", item_plates_);
        item_word_root_ = root("tl/second/item/wordroot", item_words_);
        // Nearly black, with a line of the HUD frames' dull gold round it.
        item_plate_texture_ = colour_texture("tl/second/item/plate_texture", 10, 9, 8);
        item_edge_texture_ = colour_texture("tl/second/item/edge_texture", 120, 88, 40);
        item_plate_material_ = build_menu_material("tl/second/item/plate", item_plate_texture_);
        item_edge_material_ = build_menu_material("tl/second/item/edge", item_edge_texture_);
        for (size_t index = 0; index < item_windows_.size(); ++index) {
            const std::string stem = "tl/second/item/" + std::to_string(index);
            const auto panel = [&](const std::string& name, const Ogre::MaterialPtr& material) {
                auto* made = static_cast<Ogre::PanelOverlayElement*>(
                    overlays.createOverlayElement("Panel", name));
                made->setMetricsMode(Ogre::GMM_RELATIVE);
                made->setMaterial(material);
                made->hide();
                item_plate_root_->addChild(made);
                return made;
            };
            // The edge sorts before the plate by name: a container draws its children by name.
            item_windows_[index].edge = panel(stem + "/0edge", item_edge_material_);
            item_windows_[index].plate = panel(stem + "/1plate", item_plate_material_);
        }
        // Never shown here: preViewportUpdate() shows them for the second window's viewport.
    } catch (const Ogre::Exception& e) {
        TL_LOG_WARN("second screen: no item windows: %s", e.getDescription().c_str());
        destroy_item_overlays();
    }
}

void SecondScreen::destroy_item_overlays() {
    if (Ogre::OverlayManager* overlays = Ogre::OverlayManager::getSingletonPtr()) {
        for (Ogre::Overlay* overlay : {item_plates_, item_words_}) {
            if (overlay != nullptr) overlay->hide();
        }
        for (ItemWindow& window : item_windows_) {
            for (Ogre::OverlayElement* element : {static_cast<Ogre::OverlayElement*>(window.edge),
                                                  static_cast<Ogre::OverlayElement*>(window.plate)}) {
                if (element == nullptr) continue;
                if (Ogre::OverlayContainer* parent = element->getParent()) {
                    parent->removeChild(element->getName());
                }
                overlays->destroyOverlayElement(element);
            }
            for (Ogre::TextAreaOverlayElement* line : window.lines) {
                if (line == nullptr) continue;
                if (Ogre::OverlayContainer* parent = line->getParent()) {
                    parent->removeChild(line->getName());
                }
                overlays->destroyOverlayElement(line);
            }
        }
        if (item_plate_root_ != nullptr && item_plates_ != nullptr) {
            item_plates_->remove2D(item_plate_root_);
        }
        if (item_word_root_ != nullptr && item_words_ != nullptr) {
            item_words_->remove2D(item_word_root_);
        }
        if (item_plate_root_ != nullptr) overlays->destroyOverlayElement(item_plate_root_);
        if (item_word_root_ != nullptr) overlays->destroyOverlayElement(item_word_root_);
        if (item_plates_ != nullptr) overlays->destroy(item_plates_);
        if (item_words_ != nullptr) overlays->destroy(item_words_);
    }
    item_windows_ = {};
    item_plates_ = nullptr;
    item_words_ = nullptr;
    item_plate_root_ = nullptr;
    item_word_root_ = nullptr;
    for (Ogre::MaterialPtr* material : {&item_plate_material_, &item_edge_material_}) {
        if (!*material) continue;
        if (Ogre::MaterialManager* materials = Ogre::MaterialManager::getSingletonPtr()) {
            materials->remove(*material);
        }
        material->reset();
    }
    for (Ogre::TexturePtr* texture : {&item_plate_texture_, &item_edge_texture_}) {
        if (!*texture) continue;
        if (Ogre::TextureManager* textures = Ogre::TextureManager::getSingletonPtr()) {
            textures->remove(*texture);
        }
        texture->reset();
    }
}

// The windows side by side under the strip, the item's own last: all at one size, the
// largest at which the row fits the screen's width and the tallest fits its height.
void SecondScreen::layout_item_tips() {
    for (ItemWindow& window : item_windows_) {
        if (window.edge != nullptr) window.edge->hide();
        if (window.plate != nullptr) window.plate->hide();
        for (Ogre::TextAreaOverlayElement* line : window.lines) line->hide();
    }
    if (window_ == nullptr || item_plates_ == nullptr || item_tips_.empty()) return;
    const float across = static_cast<float>(window_->getWidth());
    const float down = static_cast<float>(window_->getHeight());
    if (across <= 0.0f || down <= 0.0f) return;

    std::vector<size_t> shown;
    for (size_t index = 0; index < item_tips_.size() && index < kItemWindows; ++index) {
        if (item_tips_[index].name.shown) shown.push_back(index);
    }
    if (shown.empty()) return;

    const float margin = kItemMargin * down;
    const float gap = kItemGap * down;
    const float gaps = gap * static_cast<float>(shown.size() - 1);
    const float top = shape_.strip_bottom + margin;
    const float room_wide = across - margin * 2.0f - gaps;
    const float room_tall = down - top - margin;
    if (room_wide <= 0.0f || room_tall <= 0.0f) return;

    // The windows at a scale, and how much of the room they take.
    std::array<TipBuilt, kItemWindows> built;
    const auto build = [&](float scale, float& wide, float& tall) {
        wide = 0.0f;
        tall = 0.0f;
        const TipView view{scale, across, down};
        for (size_t index : shown) {
            built[index] = build_item_tip(item_tips_[index], kMainTipFonts, view);
            wide += built[index].wide;
            tall = std::max(tall, built[index].tall);
        }
    };
    const float native = down / kNativeHeight;
    float scale = native;
    float wide = 0.0f, tall = 0.0f;
    build(scale, wide, tall);
    if (wide <= 0.0f || tall <= 0.0f) return;
    // A window's width and height follow its scale closely but not exactly (its lines wrap):
    // aim for the fit, then come down a little at a time while it is still over.
    scale = std::min(native * kItemMostScale,
                     scale * std::min(room_wide / wide, room_tall / tall));
    build(scale, wide, tall);
    for (int tries = 0; tries < 12 && (wide > room_wide || tall > room_tall); ++tries) {
        scale *= 0.95f;
        build(scale, wide, tall);
    }

    const TipView view{scale, across, down};
    const float edge = std::max(1.0f, kItemEdge * down);
    float x = (across - (wide + gaps)) * 0.5f;
    // The item's own window on the right and the worn ones to its left, as the main
    // screen's item compare sets them out.
    for (size_t at = shown.size(); at-- > 0;) {
        const size_t index = shown[at];
        ItemWindow& window = item_windows_[index];
        const TipBuilt& made = built[index];
        if (window.edge != nullptr) {
            window.edge->setPosition((x - edge) / across, (top - edge) / down);
            window.edge->setDimensions((made.wide + edge * 2.0f) / across,
                                       (made.tall + edge * 2.0f) / down);
            window.edge->show();
        }
        if (window.plate != nullptr) {
            window.plate->setPosition(x / across, top / down);
            window.plate->setDimensions(made.wide / across, made.tall / down);
            window.plate->show();
        }
        const std::string prefix = "tl/second/item/" + std::to_string(index) + "/line_";
        draw_item_tip_text(made, x, top, view, TipTextHome{item_word_root_, prefix, &window.lines});
        x += made.wide + gap;
    }
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
