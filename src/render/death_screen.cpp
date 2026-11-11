// The death screen, from `media/UI/resurrect_xenon.layout`. Every offset is read
// out of the layout at runtime; the item-text frame's five numbers are the ones
// `ceguiWidgets.looknfeel` gives it (see quest_dialog.cpp).
#include "render/death_screen.h"

#include "core/log.h"
#include "core/text.h"
#include "render/menu_chrome.h"
#include "render/overlay_set.h"
#include "render/text_layout.h"

#include <OgreFontManager.h>
#include <OgreOverlay.h>
#include <OgreOverlayContainer.h>
#include <OgreOverlayManager.h>
#include <OgrePanelOverlayElement.h>
#include <OgreTextAreaOverlayElement.h>
#include <OgreTextureManager.h>

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <vector>

namespace tl::render {
namespace {

constexpr const char* kLayout = "media/UI/resurrect_xenon.layout";

constexpr const char* kImagesets[] = {
    "media/UI/icons2/widgets.imageset",
    "media/UI/icons2/widgets1.imageset",
    "media/UI/icons2/widgets2.imageset",
};

float default_extent(const std::string& descriptor, bool horizontal) {
    if (descriptor == "Button") return horizontal ? 250.0f : 30.0f;
    if (descriptor == "Check Box") return horizontal ? 50.0f : 40.0f;
    return 250.0f;
}

int anchor_of(const std::string& word) {
    if (word == "CENTER") return 1;
    if (word == "RIGHT" || word == "BOTTOM") return 2;
    return 0;
}

float font_points(const std::string& name) {
    if (name == "SerifSmall") return kSmallFontSize;
    if (name == "Serif14") return 14.0f;
    if (name == "SerifBig") return kBigFontSize;
    if (name == "SerifHuge") return kHugeFontSize;
    return kFontSize;
}

std::string lowered(const std::string& text) {
    std::string out = text;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    return out;
}

std::string element_name(size_t index, const char* suffix) {
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "tl/death/%04zu%s", index, suffix);
    return buffer;
}

}  // namespace

DeathScreen::~DeathScreen() { destroy(); }

const DeathScreen::Sprite* DeathScreen::sprite(const std::string& name) const {
    auto found = sprites_.find(lowered(name));
    // A console image the sheets lack, drawn in the PC's like of it.
    if (found == sprites_.end()) found = sprites_.find(lowered(pc_image_for(name)));
    return found == sprites_.end() ? nullptr : &found->second;
}

bool DeathScreen::load_art(const std::string& group) {
    // The PC's sets after the console's, so a name both hold keeps the console's.
    std::vector<const char*> paths(std::begin(kImagesets), std::end(kImagesets));
    for (const char* path : pc_imagesets()) paths.push_back(path);
    for (const char* path : paths) {
        std::vector<uint8_t> bytes;
        if (!read_menu_resource(path, group, bytes) || bytes.empty()) continue;
        tl::formats::Imageset parsed = tl::formats::parse_imageset(bytes.data(), bytes.size());
        if (parsed.ok) sets_.push_back(std::move(parsed));
    }
    if (sets_.empty()) {
        TL_LOG_INFO("no death screen: none of its imagesets is in the mounted data");
        return false;
    }
    for (const tl::formats::Imageset& stored : sets_) {
        Ogre::TexturePtr texture;
        try {
            texture = Ogre::TextureManager::getSingleton().load(stored.image_file, group);
        } catch (const Ogre::Exception&) {
            texture.reset();
        }
        if (!texture) continue;
        textures_.push_back(texture);
        materials_.push_back(build_menu_material("tl/death/" + stored.name, texture));
        apply_set_tint(materials_.back(), stored);
        for (const tl::formats::ImagesetImage& image : stored.images) {
            Sprite entry;
            entry.rect = &image;
            entry.texture = texture;
            entry.material = materials_.back();
            sprites_.emplace(lowered(image.name), entry);
        }
    }
    return !sprites_.empty();
}

void DeathScreen::paint(Node& node, const std::string& image) {
    if (node.panel == nullptr) return;
    const Sprite* art = image.empty() ? nullptr : sprite(image);
    if (art == nullptr || !art->texture) {
        node.panel->setTransparent(true);
        return;
    }
    node.panel->setTransparent(false);
    node.panel->setMaterial(art->material);
    const float tw = static_cast<float>(art->texture->getWidth());
    const float th = static_cast<float>(art->texture->getHeight());
    node.panel->setUV(static_cast<float>(art->rect->x) / tw, static_cast<float>(art->rect->y) / th,
                      static_cast<float>(art->rect->x + art->rect->width) / tw,
                      static_cast<float>(art->rect->y + art->rect->height) / th);
}

// `ceguiWidget/StaticShared`'s frame: nine `tooltip*` slices over
// `LeftEdge -24, TopEdge -9, Width +48, Height +18`.
// PORT: drawing, `ceguiWidget/StaticShared`'s nine-slice frame.
void DeathScreen::add_item_text_frame(int owner, size_t& serial) {
    const Node box = nodes_[static_cast<size_t>(owner)];
    const float outset_x = 24.0f;
    const float outset_y = 9.0f;
    const float w = box.wide + 2.0f * outset_x;
    const float h = box.tall + 2.0f * outset_y;
    float cw = 24.0f;
    float ch = 24.0f;
    if (const Sprite* art = sprite("tooltiptopleft")) {
        cw = static_cast<float>(art->rect->width);
        ch = static_cast<float>(art->rect->height);
    }
    const float mw = std::max(0.0f, w - 2.0f * cw);
    const float mh = std::max(0.0f, h - 2.0f * ch);
    struct Slice {
        const char* image;
        float x, y, wide, tall;
    };
    const Slice slices[] = {
        {"tooltipmiddle", cw, ch, mw, mh},
        {"tooltiptopedge", cw, 0.0f, mw, ch},
        {"tooltipbottomedge", cw, h - ch, mw, ch},
        {"tooltipleftedge", 0.0f, ch, cw, mh},
        {"tooltiprightedge", w - cw, ch, cw, mh},
        {"tooltiptopleft", 0.0f, 0.0f, cw, ch},
        {"tooltiptopright", w - cw, 0.0f, cw, ch},
        {"tooltipbottomleft", 0.0f, h - ch, cw, ch},
        {"tooltipbottomright", w - cw, h - ch, cw, ch},
    };
    auto& overlays = Ogre::OverlayManager::getSingleton();
    for (const Slice& slice : slices) {
        Node node;
        node.parent = owner;
        node.off_x = slice.x - outset_x;
        node.off_y = slice.y - outset_y;
        node.wide = slice.wide;
        node.tall = slice.tall;
        node.normal = slice.image;
        node.panel = static_cast<Ogre::PanelOverlayElement*>(
            overlays.createOverlayElement("Panel", element_name(serial++, "f")));
        node.panel->setMetricsMode(Ogre::GMM_RELATIVE);
        paint(node, node.normal);
        nodes_.push_back(node);
    }
}

void DeathScreen::build(const tl::formats::UiObject& object, int parent, size_t& serial,
                        bool inherited_hidden) {
    const std::string descriptor = object.descriptor();
    Node node;
    node.parent = parent;
    node.widget = object.string_or("WIDGET NAME", std::string());
    node.off_x = object.float_or("OFFSET X", 0.0f);
    node.off_y = object.float_or("OFFSET Y", 0.0f);
    node.wide =
        object.has("WIDTH") ? object.float_or("WIDTH", 0.0f) : default_extent(descriptor, true);
    node.tall =
        object.has("HEIGHT") ? object.float_or("HEIGHT", 0.0f) : default_extent(descriptor, false);
    node.scale_wide = object.float_or("SCALE WIDTH", 0.0f);
    node.scale_tall = object.float_or("SCALE HEIGHT", 0.0f);
    node.h_anchor = anchor_of(object.string_or("HORIZONTAL", "LEFT"));
    node.v_anchor = anchor_of(object.string_or("VERTICLE", "TOP"));
    node.h_text = anchor_of(object.string_or("HORIZONTAL ALIGN", "LEFT"));
    node.v_text = anchor_of(object.string_or("VERTICAL ALIGN", "TOP"));
    node.wrap = object.bool_or("WORD WRAP", false);
    node.hidden = inherited_hidden || !object.bool_or("VISIBLE", true);

    auto& overlays = Ogre::OverlayManager::getSingleton();
    const size_t index = serial++;

    if (descriptor == "Text" || descriptor == "Text Outlined") {
        node.points = font_points(object.string_or("FONT", "Serif"));
        node.caption = tl::core::strip_markup(object.string_or("TEXT", std::string()));
        Ogre::ColourValue colour = Ogre::ColourValue::White;
        float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
        if (tl::formats::parse_colour(object.string_or("COLOR", std::string()), r, g, b, a)) {
            colour = Ogre::ColourValue(r, g, b, a);
        }
        auto make = [&](const char* suffix, const Ogre::ColourValue& tint) {
            auto* area = static_cast<Ogre::TextAreaOverlayElement*>(
                overlays.createOverlayElement("TextArea", element_name(index, suffix)));
            area->setMetricsMode(Ogre::GMM_RELATIVE);
            area->setFontName(kFontName, Ogre::RGN_INTERNAL);
            area->setColour(tint);
            if (node.h_text == 1) area->setAlignment(Ogre::TextAreaOverlayElement::Center);
            else if (node.h_text == 2) area->setAlignment(Ogre::TextAreaOverlayElement::Right);
            text_root_->addChild(area);
            return area;
        };
        if (descriptor == "Text Outlined") node.drop = make("a", Ogre::ColourValue(0, 0, 0, a));
        node.text = make("b", colour);
    } else {
        node.normal = object.string_or("IMAGE", object.string_or("NORMAL", ""));
        node.hover = object.string_or("HOVER", "");
        node.panel = static_cast<Ogre::PanelOverlayElement*>(
            overlays.createOverlayElement("Panel", element_name(index, "")));
        node.panel->setMetricsMode(Ogre::GMM_RELATIVE);
        paint(node, node.normal);
    }

    const int self = static_cast<int>(nodes_.size());
    nodes_.push_back(std::move(node));
    if (object.string_or("LOOK N FEEL", "") == "ceguiWidget/ItemText") {
        add_item_text_frame(self, serial);
    }

    std::vector<const tl::formats::UiObject*> order;
    for (const tl::formats::UiObject& child : object.children) order.push_back(&child);
    std::stable_sort(order.begin(), order.end(),
                     [](const tl::formats::UiObject* a, const tl::formats::UiObject* b) {
                         return a->bool_or("ALWAYS ON TOP", false) <
                                b->bool_or("ALWAYS ON TOP", false);
                     });
    const bool hidden = nodes_[static_cast<size_t>(self)].hidden;
    for (const tl::formats::UiObject* child : order) build(*child, self, serial, hidden);
}

bool DeathScreen::create(const std::string& group, unsigned width, unsigned height) {
    destroy();
    width_ = width;
    height_ = height;
    if (!load_art(group) || !ensure_menu_font()) {
        destroy();
        return false;
    }

    std::vector<uint8_t> bytes;
    if (!read_menu_resource(kLayout, group, bytes) || bytes.empty()) {
        TL_LOG_INFO(
            "no death screen: '%s' is not in the mounted data - re-run "
            "scripts/extract_console_ui.py",
            kLayout);
        destroy();
        return false;
    }
    const tl::formats::UiLayout parsed = tl::formats::parse_ui_layout(bytes.data(), bytes.size());
    if (!parsed.ok) {
        TL_LOG_WARN("death screen: %s: %s", kLayout, parsed.error.c_str());
        destroy();
        return false;
    }

    try {
        auto& overlays = Ogre::OverlayManager::getSingleton();
        // Above the quest window's 590/592: the death screen replaces every other menu.
        overlay_ = overlays.create("tl/death/panels");
        overlay_->setZOrder(600);
        text_overlay_ = overlays.create("tl/death/text");
        text_overlay_->setZOrder(602);
        auto root = [&](const char* name, Ogre::Overlay* into) {
            auto* box =
                static_cast<Ogre::OverlayContainer*>(overlays.createOverlayElement("Panel", name));
            box->setMetricsMode(Ogre::GMM_RELATIVE);
            box->setPosition(0.0f, 0.0f);
            box->setDimensions(1.0f, 1.0f);
            static_cast<Ogre::PanelOverlayElement*>(box)->setTransparent(true);
            into->add2D(box);
            return box;
        };
        panel_root_ = root("tl/death/panelroot", overlay_);
        text_root_ = root("tl/death/textroot", text_overlay_);

        std::vector<const tl::formats::UiObject*> order;
        for (const tl::formats::UiObject& object : parsed.objects) order.push_back(&object);
        std::stable_sort(order.begin(), order.end(),
                         [](const tl::formats::UiObject* a, const tl::formats::UiObject* b) {
                             return a->bool_or("ALWAYS ON TOP", false) <
                                    b->bool_or("ALWAYS ON TOP", false);
                         });
        size_t serial = 0;
        for (const tl::formats::UiObject* object : order) build(*object, -1, serial, false);

        for (Node& node : nodes_) {
            if (node.panel != nullptr) panel_root_->addChild(node.panel);
        }
        // One render priority per element; see game_hud.cpp.
        Ogre::ushort z = static_cast<Ogre::ushort>(panel_root_->getZOrder() + 1);
        for (Node& node : nodes_) {
            if (node.panel == nullptr) continue;
            node.panel->_notifyZOrder(z);
            if (z < static_cast<Ogre::ushort>(panel_root_->getZOrder() + 99)) ++z;
        }

        native_height_ = sets_.front().native_height > 0
                             ? static_cast<float>(sets_.front().native_height)
                             : kNativeHeight;
        built_ = true;
        set_state(state_);
        hide();
        TL_LOG_INFO("death screen: %zu window(s) at %u x %u", nodes_.size(), width_, height_);
        return true;
    } catch (const Ogre::Exception& e) {
        TL_LOG_WARN("no death screen: %s", e.getDescription().c_str());
        destroy();
        return false;
    }
}

void DeathScreen::destroy() {
    if (Ogre::OverlayManager::getSingletonPtr() != nullptr &&
        (overlay_ != nullptr || text_overlay_ != nullptr)) {
        auto& overlays = Ogre::OverlayManager::getSingleton();
        for (Ogre::Overlay* overlay : {overlay_, text_overlay_}) {
            if (overlay != nullptr) overlay->hide();
        }
        for (Node& node : nodes_) {
            if (node.panel != nullptr && panel_root_ != nullptr) {
                panel_root_->removeChild(node.panel->getName());
            }
            for (Ogre::TextAreaOverlayElement* area : {node.drop, node.text}) {
                if (area != nullptr && text_root_ != nullptr)
                    text_root_->removeChild(area->getName());
            }
            if (node.panel != nullptr) overlays.destroyOverlayElement(node.panel);
            if (node.drop != nullptr) overlays.destroyOverlayElement(node.drop);
            if (node.text != nullptr) overlays.destroyOverlayElement(node.text);
        }
        if (panel_root_ != nullptr && overlay_ != nullptr) overlay_->remove2D(panel_root_);
        if (text_root_ != nullptr && text_overlay_ != nullptr) text_overlay_->remove2D(text_root_);
        if (panel_root_ != nullptr) overlays.destroyOverlayElement(panel_root_);
        if (text_root_ != nullptr) overlays.destroyOverlayElement(text_root_);
        if (overlay_ != nullptr) overlays.destroy(overlay_);
        if (text_overlay_ != nullptr) overlays.destroy(text_overlay_);
    } else {
        // Elements made before the overlays existed (a failed create).
        if (Ogre::OverlayManager::getSingletonPtr() != nullptr) {
            auto& overlays = Ogre::OverlayManager::getSingleton();
            for (Node& node : nodes_) {
                if (node.panel != nullptr) overlays.destroyOverlayElement(node.panel);
                if (node.drop != nullptr) overlays.destroyOverlayElement(node.drop);
                if (node.text != nullptr) overlays.destroyOverlayElement(node.text);
            }
        }
    }
    nodes_.clear();
    sprites_.clear();
    materials_.clear();
    textures_.clear();
    sets_.clear();
    panel_root_ = nullptr;
    text_root_ = nullptr;
    overlay_ = nullptr;
    text_overlay_ = nullptr;
    built_ = false;
    visible_ = false;
}

void DeathScreen::show() {
    if (!built_ || visible_) return;
    visible_ = true;
    overlay_->show();
    text_overlay_->show();
}

void DeathScreen::hide() {
    visible_ = false;
    if (!built_) return;
    overlay_->hide();
    text_overlay_->hide();
}

void DeathScreen::resize(unsigned width, unsigned height) {
    width_ = width;
    height_ = height;
    if (built_) layout();
}

int DeathScreen::find_widget(const std::string& name) const {
    for (size_t i = 0; i < nodes_.size(); ++i) {
        if (nodes_[i].widget == name) return static_cast<int>(i);
    }
    return -1;
}

bool DeathScreen::is_hidden(int index) const {
    while (index >= 0 && index < static_cast<int>(nodes_.size())) {
        const Node& node = nodes_[static_cast<size_t>(index)];
        if (node.hidden || node.runtime_hidden) return true;
        index = node.parent;
    }
    return false;
}

void DeathScreen::set_state(const DeathScreenState& state) {
    state_ = state;
    if (!built_) return;

    const auto caption = [&](const char* widget, const std::string& text) {
        const int i = find_widget(widget);
        if (i >= 0) nodes_[static_cast<size_t>(i)].caption = text;
    };
    caption("Title", state_.title);
    const char* choice_widgets[] = {"Choice1", "Choice2", "Choice3"};
    const char* button_widgets[] = {"Resurrect1", "Resurrect2", "Resurrect3"};
    for (int i = 0; i < 3; ++i) {
        caption(choice_widgets[i], state_.choices[i]);
        const int b = find_widget(button_widgets[i]);
        if (b < 0) continue;
        Node& button = nodes_[static_cast<size_t>(b)];
        // `CDieMenu::setOpen` hides the three buttons and texts for a hardcore
        // character and shows them otherwise.
        button.runtime_hidden = state_.hardcore;
        paint(button, i == state_.selected && !button.hover.empty() ? button.hover : button.normal);
    }
    // DIVERGENCE: `CDieMenu` predates this window (the PC layout has none); it is
    // shown exactly when the choices are not.
    const int hardcore = find_widget("hardcoretext");
    if (hardcore >= 0) nodes_[static_cast<size_t>(hardcore)].runtime_hidden = !state_.hardcore;
    layout();
}

void DeathScreen::layout() {
    if (!built_) return;
    const float across = static_cast<float>(width_ > 0 ? width_ : 1);
    const float down = static_cast<float>(height_ > 0 ? height_ : 1);
    const float scale = down / native_height_;

    Ogre::FontPtr font = Ogre::FontManager::getSingleton().getByName(kFontName, Ogre::RGN_INTERNAL);
    if (font && !font->isLoaded()) font->load();

    for (size_t i = 0; i < nodes_.size(); ++i) {
        Node& node = nodes_[i];
        const Node* parent = node.parent < 0 ? nullptr : &nodes_[static_cast<size_t>(node.parent)];
        const float parent_left = parent == nullptr ? 0.0f : parent->left;
        const float parent_top = parent == nullptr ? 0.0f : parent->top;
        const float parent_wide = parent == nullptr ? across : parent->width * across;
        const float parent_tall = parent == nullptr ? down : parent->height * down;

        float left = node.off_x * scale;
        float top = node.off_y * scale;
        const float wide = node.scale_wide * parent_wide + node.wide * scale;
        const float tall = node.scale_tall * parent_tall + node.tall * scale;
        if (node.h_anchor == 1) left += (parent_wide - wide) * 0.5f;
        else if (node.h_anchor == 2) left += parent_wide - wide;
        if (node.v_anchor == 1) top += (parent_tall - tall) * 0.5f;
        else if (node.v_anchor == 2) top += parent_tall - tall;

        node.left = parent_left + left / across;
        node.top = parent_top + top / down;
        node.width = wide / across;
        node.height = tall / down;

        const bool hidden = is_hidden(static_cast<int>(i));
        if (node.panel != nullptr) {
            if (hidden) {
                node.panel->hide();
            } else {
                node.panel->show();
                node.panel->setPosition(node.left, node.top);
                node.panel->setDimensions(node.width, node.height);
            }
        }
        if (node.text == nullptr) continue;

        const float line = node.points * ui_points_to_pixels() * scale;
        std::vector<std::string> lines;
        if (node.wrap && font) {
            lines = wrap_text(font, node.caption, node.width, line / down, across, down);
        } else {
            lines.push_back(node.caption);
        }
        std::string joined;
        for (size_t l = 0; l < lines.size(); ++l) {
            if (l > 0) joined += '\n';
            joined += lines[l];
        }
        const float block = line * static_cast<float>(std::max<size_t>(1, lines.size()));
        for (Ogre::TextAreaOverlayElement* area : {node.drop, node.text}) {
            if (area == nullptr) continue;
            if (hidden) {
                area->hide();
                continue;
            }
            area->show();
            area->setCaption(joined);
            float x = node.left;
            if (node.h_text == 1) x = node.left + node.width * 0.5f;
            else if (node.h_text == 2) x = node.left + node.width;
            float y = node.top;
            if (node.v_text == 1) y = node.top + (tall - block) * 0.5f / down;
            else if (node.v_text == 2) y = node.top + (tall - block) / down;
            const float nudge = area == node.drop ? kLabelShadow * scale : 0.0f;
            set_char_height(area, line / down);
            area->setPosition(x + nudge / across, y + nudge / down);
        }
    }
}

}  // namespace tl::render
