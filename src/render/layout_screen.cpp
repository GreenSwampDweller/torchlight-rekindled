// A console `[UI]` layout, drawn as `DeathScreen` draws `resurrect_xenon.layout`:
// every offset read out of the layout at runtime, the item-text frame's numbers
// the ones `ceguiWidgets.looknfeel` gives it (see quest_dialog.cpp).
#include "render/layout_screen.h"

#include "core/log.h"
#include "core/text.h"
#include "render/menu_chrome.h"
#include "render/overlay_set.h"
#include "render/text_layout.h"

#include <OgreFontManager.h>
#include <OgreMaterialManager.h>
#include <OgreOverlay.h>
#include <OgreOverlayContainer.h>
#include <OgreOverlayManager.h>
#include <OgrePanelOverlayElement.h>
#include <OgreTechnique.h>
#include <OgreTextAreaOverlayElement.h>
#include <OgreTextureManager.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <vector>

namespace tl::render {
namespace {

constexpr const char* kImagesets[] = {
    "media/UI/icons2/widgets.imageset",
    "media/UI/icons2/widgets1.imageset",
    "media/UI/icons2/widgets2.imageset",
    // Help's pad picture (`controller`) and the D-pad-left icon
    // (`analog_button_left_down`); none of its ten names is in the three above.
    "media/UI/icons2/ratings.imageset",
    // The loading screen's artwork (`set:Loading image:loading`,
    // `media/UI/loading.uilayout`).
    "media/UI/loading.imageset",
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

bool always_on_top_last(const tl::formats::UiObject* a, const tl::formats::UiObject* b) {
    return a->bool_or("ALWAYS ON TOP", false) < b->bool_or("ALWAYS ON TOP", false);
}

/// The four `{scale,offset}` pairs of a `UnifiedAreaRect`: left, top, right,
/// bottom.
bool export_area(const std::string& text, tl::formats::UDim dims[4]) {
    size_t at = 0;
    for (int i = 0; i < 4; ++i) {
        const size_t open = text.find('{', at);
        if (open == std::string::npos) return false;
        const size_t close = text.find('}', open);
        if (close == std::string::npos) return false;
        std::string pair = text.substr(open + 1, close - open - 1);
        while (!pair.empty() && pair.front() == '{') pair.erase(pair.begin());
        const size_t comma = pair.find(',');
        if (comma == std::string::npos) return false;
        dims[i].scale = std::strtof(pair.substr(0, comma).c_str(), nullptr);
        dims[i].offset = std::strtof(pair.substr(comma + 1).c_str(), nullptr);
        at = close + 1;
    }
    return true;
}

std::string number(float value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%g", static_cast<double>(value));
    return buffer;
}

bool ends_with(const std::string& text, const char* tail) {
    const std::string end = tail;
    return text.size() >= end.size() &&
           text.compare(text.size() - end.size(), end.size(), end) == 0;
}

/// A window of a CEGUI export as the `[UI]` object `build` reads. What each
/// property becomes:
///
///   `UnifiedAreaRect`   `OFFSET X/Y` and `SCALE X/Y` from its top-left corner,
///                       `WIDTH`/`HEIGHT` and `SCALE WIDTH/HEIGHT` from the
///                       difference of the two corners
///   `HorizontalAlignment`, `VerticalAlignment`   `HORIZONTAL`, `VERTICLE`
///   `Text`, `Font`, `TextColour`                 `TEXT`, `FONT`, `COLOR`
///   `HorzTextFormatting`, `VertFormatting`       the text's alignment and wrap
///     (`ceguiWidgets.looknfeel` starts both text widgets `LeftAligned` and
///     `TopAligned`)
///   `Image`             `IMAGE`, the image's name without its set
///   `Visible` `False`   hidden at run time, so it can be shown
///   the window's type   `LOOK N FEEL`, which picks the frame
tl::formats::UiObject export_object(const tl::formats::GuiWindow& window) {
    tl::formats::UiObject object;
    const std::string empty;
    auto put = [&object](const char* key, const std::string& value) {
        object.properties.emplace_back(key, value);
    };
    std::string set_name;
    std::string image_name;
    const bool has_image =
        tl::formats::parse_image_reference(window.property("Image", empty), set_name, image_name);
    if (ends_with(window.type, "StaticTextOutline")) {
        put("DESCRIPTOR", "Text Outlined");
    } else if (ends_with(window.type, "StaticText")) {
        put("DESCRIPTOR", "Text");
    } else {
        put("DESCRIPTOR", has_image ? "Image" : "Window");
    }
    put("WIDGET NAME", window.name);
    put("LOOK N FEEL", window.type);

    tl::formats::UDim dims[4];
    if (export_area(window.property("UnifiedAreaRect", empty), dims)) {
        put("OFFSET X", number(dims[0].offset));
        put("OFFSET Y", number(dims[1].offset));
        put("SCALE X", number(dims[0].scale));
        put("SCALE Y", number(dims[1].scale));
        put("WIDTH", number(dims[2].offset - dims[0].offset));
        put("HEIGHT", number(dims[3].offset - dims[1].offset));
        put("SCALE WIDTH", number(dims[2].scale - dims[0].scale));
        put("SCALE HEIGHT", number(dims[3].scale - dims[1].scale));
    } else {
        put("WIDTH", "0");
        put("HEIGHT", "0");
        put("SCALE WIDTH", "1");
        put("SCALE HEIGHT", "1");
    }
    const std::string horizontal = window.property("HorizontalAlignment", empty);
    if (horizontal == "Centre") put("HORIZONTAL", "CENTER");
    if (horizontal == "Right") put("HORIZONTAL", "RIGHT");
    const std::string vertical = window.property("VerticalAlignment", empty);
    if (vertical == "Centre") put("VERTICLE", "CENTER");
    if (vertical == "Bottom") put("VERTICLE", "BOTTOM");

    if (window.has_property("Text")) put("TEXT", window.property("Text", empty));
    if (window.has_property("Font")) put("FONT", window.property("Font", empty));
    if (window.has_property("TextColour")) put("COLOR", window.property("TextColour", empty));
    const std::string across = window.property("HorzTextFormatting", empty);
    if (across.find("Centre") != std::string::npos) put("HORIZONTAL ALIGN", "CENTER");
    if (across.find("Right") != std::string::npos) put("HORIZONTAL ALIGN", "RIGHT");
    if (across.compare(0, 8, "WordWrap") == 0) put("WORD WRAP", "true");
    const std::string down = window.property("VertFormatting", empty);
    if (down.find("Centre") != std::string::npos) put("VERTICAL ALIGN", "CENTER");
    if (down.find("Bottom") != std::string::npos) put("VERTICAL ALIGN", "BOTTOM");

    if (has_image) put("IMAGE", image_name);
    if (window.has_property("Alpha")) put("ALPHA", window.property("Alpha", empty));
    if (window.property("Visible", empty) == "False") put("RUNTIME HIDDEN", "true");
    if (window.property("AlwaysOnTop", empty) == "True") put("ALWAYS ON TOP", "true");

    for (const tl::formats::GuiWindow& child : window.children) {
        object.children.push_back(export_object(child));
    }
    return object;
}

}  // namespace

LayoutScreen::~LayoutScreen() { destroy(); }

std::string LayoutScreen::element_name(size_t index, const char* suffix) const {
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "/%04zu%s", index, suffix);
    return tag_ + buffer;
}

const LayoutScreen::Sprite* LayoutScreen::sprite(const std::string& name) const {
    auto found = sprites_.find(lowered(name));
    // A console image the sheets lack, drawn in the PC's like of it.
    if (found == sprites_.end()) found = sprites_.find(lowered(pc_image_for(name)));
    return found == sprites_.end() ? nullptr : &found->second;
}

bool LayoutScreen::load_art(const std::string& group) {
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
        TL_LOG_INFO("no %s: none of its imagesets is in the mounted data", tag_.c_str());
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
        materials_.push_back(build_menu_material(tag_ + "/" + stored.name, texture));
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

void LayoutScreen::paint(Node& node, const std::string& image) {
    if (node.panel == nullptr) return;
    const Sprite* art = image.empty() ? nullptr : sprite(image);
    if (art == nullptr || !art->texture) {
        node.panel->setTransparent(true);
        return;
    }
    node.panel->setTransparent(false);
    Ogre::MaterialPtr material = node.tint_on && node.tinted ? node.tinted : art->material;
    if (node.alpha < 1.0f && material == art->material) {
        if (!node.faded) {
            node.faded =
                build_menu_material(node.panel->getName() + "/a", art->texture, node.alpha);
        }
        material = node.faded;
    }
    node.panel->setMaterial(material);
    const float tw = static_cast<float>(art->texture->getWidth());
    const float th = static_cast<float>(art->texture->getHeight());
    node.panel->setUV(static_cast<float>(art->rect->x) / tw, static_cast<float>(art->rect->y) / th,
                      static_cast<float>(art->rect->x + art->rect->width) / tw,
                      static_cast<float>(art->rect->y + art->rect->height) / th);
}

// A frame of `ceguiWidget/StaticShared` (`ceguiWidgets.looknfeel`): nine slices over `LeftEdge
// -24, TopEdge -9, Width +48, Height +18`. Section `frame` (`tooltip*`, `widgets1`) is
// `ceguiWidget/ItemText`'s, `frame1` (`frame*`, `widgets2`) `ceguiWidget/LineFrame`'s. Placed
// against the owner's own box, so a resized owner keeps its frame.
void LayoutScreen::add_frame(int owner, size_t& serial, const std::string& art, float outset_x,
                             float outset_y) {
    float cw = 24.0f;
    float ch = 24.0f;
    if (const Sprite* corner = sprite(art + "topleft")) {
        cw = static_cast<float>(corner->rect->width);
        ch = static_cast<float>(corner->rect->height);
    }
    // What the middle and the edges add to the owner's width and height.
    const float mw = 2.0f * outset_x - 2.0f * cw;
    const float mh = 2.0f * outset_y - 2.0f * ch;
    struct Slice {
        const char* image;
        int h_anchor, v_anchor;  ///< 0 the owner's left or top, 2 its right or bottom
        bool spans_wide, spans_tall;
    };
    const Slice slices[] = {
        {"middle", 0, 0, true, true},        {"topedge", 0, 0, true, false},
        {"bottomedge", 0, 2, true, false},   {"leftedge", 0, 0, false, true},
        {"rightedge", 2, 0, false, true},    {"topleft", 0, 0, false, false},
        {"topright", 2, 0, false, false},    {"bottomleft", 0, 2, false, false},
        {"bottomright", 2, 2, false, false},
    };
    auto& overlays = Ogre::OverlayManager::getSingleton();
    for (const Slice& slice : slices) {
        Node node;
        node.parent = owner;
        node.frame_of = owner;
        node.h_anchor = slice.h_anchor;
        node.v_anchor = slice.v_anchor;
        node.off_x = slice.spans_wide ? cw - outset_x : slice.h_anchor == 2 ? outset_x : -outset_x;
        node.off_y = slice.spans_tall ? ch - outset_y : slice.v_anchor == 2 ? outset_y : -outset_y;
        node.scale_wide = slice.spans_wide ? 1.0f : 0.0f;
        node.scale_tall = slice.spans_tall ? 1.0f : 0.0f;
        node.wide = slice.spans_wide ? mw : cw;
        node.tall = slice.spans_tall ? mh : ch;
        node.image = art + slice.image;
        node.panel = static_cast<Ogre::PanelOverlayElement*>(
            overlays.createOverlayElement("Panel", element_name(serial++, "f")));
        node.panel->setMetricsMode(Ogre::GMM_RELATIVE);
        paint(node, node.image);
        nodes_.push_back(node);
    }
}

void LayoutScreen::build(const tl::formats::UiObject& object, int parent, size_t& serial,
                         bool inherited_hidden) {
    const std::string descriptor = object.descriptor();
    Node node;
    node.parent = parent;
    node.widget = object.string_or("WIDGET NAME", std::string());
    node.name = object.string_or("NAME", std::string());
    node.alpha = object.float_or("ALPHA", 1.0f);
    node.off_x = object.float_or("OFFSET X", 0.0f);
    node.off_y = object.float_or("OFFSET Y", 0.0f);
    node.wide =
        object.has("WIDTH") ? object.float_or("WIDTH", 0.0f) : default_extent(descriptor, true);
    node.tall =
        object.has("HEIGHT") ? object.float_or("HEIGHT", 0.0f) : default_extent(descriptor, false);
    node.scale_wide = object.float_or("SCALE WIDTH", 0.0f);
    node.scale_tall = object.float_or("SCALE HEIGHT", 0.0f);
    node.scale_x = object.float_or("SCALE X", 0.0f);
    node.scale_y = object.float_or("SCALE Y", 0.0f);
    node.runtime_hidden = object.bool_or("RUNTIME HIDDEN", false);
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
        node.image = object.string_or("IMAGE", object.string_or("NORMAL", ""));
        node.hover = object.string_or("HOVER", "");
        if (descriptor == "Check Box") {
            // `ceguiWidget/Checkbox` in `ceguiWidgets.looknfeel`: the box is
            // `checkbox_off`, and `checkbox_on` under the pointer.
            node.image = "checkbox_off";
            node.hover = "checkbox_on";
        }
        node.panel = static_cast<Ogre::PanelOverlayElement*>(
            overlays.createOverlayElement("Panel", element_name(index, "")));
        node.panel->setMetricsMode(Ogre::GMM_RELATIVE);
        paint(node, node.image);
    }

    const int self = static_cast<int>(nodes_.size());
    nodes_.push_back(std::move(node));
    const std::string look = object.string_or("LOOK N FEEL", "");
    if (look == "ceguiWidget/ItemText") {
        add_frame(self, serial, "tooltip");
    } else if (look == "ceguiWidget/LineFrame") {
        add_frame(self, serial, "frame");
    } else if (look == "ceguiWidget/ItemTooltip") {
        // Section `itemframe`: `LeftEdge -26, TopEdge -13, Width +52, Height +26`.
        add_frame(self, serial, "itemtooltip", 26.0f, 13.0f);
    } else if (look == "ceguiWidget/ItemTooltip2") {
        // Section `itemframe2`, the same area in the `itemtooltip2*` art.
        add_frame(self, serial, "itemtooltip2", 26.0f, 13.0f);
    }
    if (descriptor == "Check Box") {
        // The look's `select_mark`: `check` over the whole window, in the
        // selected states only.
        Node tick;
        tick.parent = self;
        tick.scale_wide = 1.0f;
        tick.scale_tall = 1.0f;
        tick.image = "check";
        tick.runtime_hidden = true;
        tick.panel = static_cast<Ogre::PanelOverlayElement*>(
            overlays.createOverlayElement("Panel", element_name(serial++, "k")));
        tick.panel->setMetricsMode(Ogre::GMM_RELATIVE);
        paint(tick, tick.image);
        nodes_[static_cast<size_t>(self)].tick = static_cast<int>(nodes_.size());
        nodes_.push_back(tick);
    }

    std::vector<const tl::formats::UiObject*> order;
    for (const tl::formats::UiObject& child : object.children) order.push_back(&child);
    std::stable_sort(order.begin(), order.end(), always_on_top_last);
    const bool hidden = nodes_[static_cast<size_t>(self)].hidden;
    for (const tl::formats::UiObject* child : order) build(*child, self, serial, hidden);
}

bool LayoutScreen::prepare(const std::string& group, unsigned width, unsigned height,
                           const std::string& tag) {
    destroy();
    tag_ = tag;
    width_ = width;
    height_ = height;
    if (!load_art(group) || !ensure_menu_font()) {
        destroy();
        return false;
    }
    return true;
}

// PORT: a PC window carried onto a console screen (the PC's `.layout` read as authored).
tl::formats::GuiWindow pc_layout_window(const std::string& group, const std::string& layout,
                                        const std::string& name) {
    tl::formats::GuiWindow out;
    std::vector<uint8_t> bytes;
    if (!read_menu_resource(layout, group, bytes) || bytes.empty()) return out;
    const tl::formats::GuiLayout parsed = tl::formats::parse_gui_layout(bytes.data(), bytes.size());
    const tl::formats::GuiWindow* window = parsed.ok ? parsed.find(name) : nullptr;
    tl::formats::UVector2 at, size;
    if (window == nullptr ||
        !tl::formats::parse_uvector2(window->property("UnifiedPosition", ""), at) ||
        !tl::formats::parse_uvector2(window->property("UnifiedSize", ""), size)) {
        return out;
    }
    out.type = "ceguiWidget/StaticText";
    out.name = name;
    // A property authored twice (`Text` "1", then the words) is its last.
    for (const auto& [key, value] : window->properties) {
        if (key != "UnifiedPosition" && key != "UnifiedSize")
            out.properties.emplace_back(key, value);
    }
    std::reverse(out.properties.begin(), out.properties.end());
    char area[160];
    std::snprintf(area, sizeof area, "{{%g,%g},{%g,%g},{%g,%g},{%g,%g}}",
                  static_cast<double>(at.x.scale), static_cast<double>(at.x.offset),
                  static_cast<double>(at.y.scale), static_cast<double>(at.y.offset),
                  static_cast<double>(at.x.scale + size.x.scale),
                  static_cast<double>(at.x.offset + size.x.offset),
                  static_cast<double>(at.y.scale + size.y.scale),
                  static_cast<double>(at.y.offset + size.y.offset));
    out.properties.emplace_back("UnifiedAreaRect", area);
    out.properties.emplace_back("Visible", "False");
    return out;
}

bool LayoutScreen::create_export(const std::string& group, unsigned width, unsigned height,
                                 const std::string& layout_path, const std::string& tag,
                                 unsigned short z_order,
                                 const std::vector<std::string>& pc_windows) {
    if (!prepare(group, width, height, tag)) return false;
    std::vector<uint8_t> bytes;
    if (!read_menu_resource(layout_path, group, bytes) || bytes.empty()) {
        TL_LOG_INFO(
            "no %s: '%s' is not in the mounted data - re-run "
            "scripts/extract_console_ui.py",
            tag_.c_str(), layout_path.c_str());
        destroy();
        return false;
    }
    tl::formats::GuiLayout parsed = tl::formats::parse_gui_layout(bytes.data(), bytes.size());
    if (!parsed.ok) {
        TL_LOG_WARN("%s: %s: %s", tag_.c_str(), layout_path.c_str(), parsed.error.c_str());
        destroy();
        return false;
    }
    const size_t dot = layout_path.rfind(".uilayout");
    for (const std::string& name : pc_windows) {
        if (dot == std::string::npos) break;
        tl::formats::GuiWindow window =
            pc_layout_window(group, layout_path.substr(0, dot) + ".layout", name);
        if (!window.type.empty()) parsed.root.children.push_back(std::move(window));
    }
    std::vector<tl::formats::UiObject> objects;
    objects.push_back(export_object(parsed.root));
    return construct(objects, layout_path, z_order);
}

bool LayoutScreen::create(const std::string& group, unsigned width, unsigned height,
                          const std::string& layout_path, const std::string& tag,
                          unsigned short z_order) {
    if (!prepare(group, width, height, tag)) return false;

    std::vector<uint8_t> bytes;
    if (!read_menu_resource(layout_path, group, bytes) || bytes.empty()) {
        TL_LOG_INFO(
            "no %s: '%s' is not in the mounted data - re-run "
            "scripts/extract_console_ui.py",
            tag_.c_str(), layout_path.c_str());
        destroy();
        return false;
    }
    const tl::formats::UiLayout parsed = tl::formats::parse_ui_layout(bytes.data(), bytes.size());
    if (!parsed.ok) {
        TL_LOG_WARN("%s: %s: %s", tag_.c_str(), layout_path.c_str(), parsed.error.c_str());
        destroy();
        return false;
    }
    return construct(parsed.objects, layout_path, z_order);
}

bool LayoutScreen::construct(const std::vector<tl::formats::UiObject>& objects,
                             const std::string& layout_path, unsigned short z_order) {
    try {
        auto& overlays = Ogre::OverlayManager::getSingleton();
        overlay_ = overlays.create(tag_ + "/panels");
        overlay_->setZOrder(z_order);
        text_overlay_ = overlays.create(tag_ + "/text");
        text_overlay_->setZOrder(static_cast<unsigned short>(z_order + 2));
        auto root = [&](const std::string& name, Ogre::Overlay* into) {
            auto* box =
                static_cast<Ogre::OverlayContainer*>(overlays.createOverlayElement("Panel", name));
            box->setMetricsMode(Ogre::GMM_RELATIVE);
            box->setPosition(0.0f, 0.0f);
            box->setDimensions(1.0f, 1.0f);
            static_cast<Ogre::PanelOverlayElement*>(box)->setTransparent(true);
            into->add2D(box);
            return box;
        };
        panel_root_ = root(tag_ + "/panelroot", overlay_);
        text_root_ = root(tag_ + "/textroot", text_overlay_);

        std::vector<const tl::formats::UiObject*> order;
        for (const tl::formats::UiObject& object : objects) order.push_back(&object);
        std::stable_sort(order.begin(), order.end(), always_on_top_last);
        size_t serial = 0;
        for (const tl::formats::UiObject* object : order) build(*object, -1, serial, false);

        for (Node& node : nodes_) {
            if (node.panel != nullptr) panel_root_->addChild(node.panel);
        }
        // One render priority per element; see game_hud.cpp. The nine slices
        // of one frame do not overlap and share one, which keeps a screen of
        // many framed rows inside the ninety-nine an overlay has.
        Ogre::ushort z = static_cast<Ogre::ushort>(panel_root_->getZOrder() + 1);
        Ogre::ushort given = z;
        int frame = -1;
        for (Node& node : nodes_) {
            if (node.panel == nullptr) continue;
            if (node.frame_of >= 0 && node.frame_of == frame) {
                node.panel->_notifyZOrder(given);
                continue;
            }
            frame = node.frame_of;
            given = z;
            node.panel->_notifyZOrder(z);
            if (z < static_cast<Ogre::ushort>(panel_root_->getZOrder() + 99)) ++z;
        }

        native_height_ = sets_.front().native_height > 0
                             ? static_cast<float>(sets_.front().native_height)
                             : kNativeHeight;
        built_ = true;
        layout();
        hide();
        TL_LOG_INFO("%s: %zu window(s) from %s at %u x %u", tag_.c_str(), nodes_.size(),
                    layout_path.c_str(), width_, height_);
        return true;
    } catch (const Ogre::Exception& e) {
        TL_LOG_WARN("no %s: %s", tag_.c_str(), e.getDescription().c_str());
        destroy();
        return false;
    }
}

void LayoutScreen::destroy() {
    if (Ogre::OverlayManager::getSingletonPtr() != nullptr) {
        auto& overlays = Ogre::OverlayManager::getSingleton();
        for (Ogre::Overlay* overlay : {overlay_, text_overlay_}) {
            if (overlay != nullptr) overlay->hide();
        }
        for (Node& node : nodes_) {
            if (node.panel != nullptr && panel_root_ != nullptr)
                panel_root_->removeChild(node.panel->getName());
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
    }
    if (Ogre::MaterialManager::getSingletonPtr() != nullptr) {
        for (Node& node : nodes_) {
            if (node.tinted) Ogre::MaterialManager::getSingleton().remove(node.tinted);
            if (node.faded) Ogre::MaterialManager::getSingleton().remove(node.faded);
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

void LayoutScreen::show() {
    if (!built_ || visible_) return;
    visible_ = true;
    overlay_->show();
    text_overlay_->show();
}

void LayoutScreen::set_z_order(unsigned short z_order) {
    if (overlay_ != nullptr) overlay_->setZOrder(z_order);
    if (text_overlay_ != nullptr)
        text_overlay_->setZOrder(static_cast<unsigned short>(z_order + 2));
}

void LayoutScreen::hide() {
    visible_ = false;
    if (!built_) return;
    overlay_->hide();
    text_overlay_->hide();
}

void LayoutScreen::resize(unsigned width, unsigned height) {
    width_ = width;
    height_ = height;
    if (built_) layout();
}

int LayoutScreen::find_widget(const std::string& name) const {
    for (size_t i = 0; i < nodes_.size(); ++i) {
        if (nodes_[i].widget == name) return static_cast<int>(i);
    }
    // A window the layout gives no `WIDGET NAME` is found by its `NAME`.
    for (size_t i = 0; i < nodes_.size(); ++i) {
        if (nodes_[i].widget.empty() && nodes_[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

bool LayoutScreen::is_hidden(int index) const {
    while (index >= 0 && index < static_cast<int>(nodes_.size())) {
        const Node& node = nodes_[static_cast<size_t>(index)];
        if (node.hidden || node.runtime_hidden) return true;
        index = node.parent;
    }
    return false;
}

void LayoutScreen::set_caption(const std::string& widget, const std::string& text) {
    const int i = find_widget(widget);
    if (i >= 0) nodes_[static_cast<size_t>(i)].caption = text;
}

void LayoutScreen::set_text_colour(const std::string& widget, const Ogre::ColourValue& colour) {
    const int i = find_widget(widget);
    if (i < 0) return;
    Node& node = nodes_[static_cast<size_t>(i)];
    if (node.text != nullptr) node.text->setColour(colour);
}

bool LayoutScreen::offset(const std::string& widget, float& x, float& y) const {
    const int i = find_widget(widget);
    if (i < 0) return false;
    x = nodes_[static_cast<size_t>(i)].off_x;
    y = nodes_[static_cast<size_t>(i)].off_y;
    return true;
}

void LayoutScreen::set_hidden(const std::string& widget, bool hidden) {
    const int i = find_widget(widget);
    if (i >= 0) nodes_[static_cast<size_t>(i)].runtime_hidden = hidden;
}

void LayoutScreen::set_extent(const std::string& widget, float wide, float tall) {
    const int i = find_widget(widget);
    if (i < 0) return;
    Node& node = nodes_[static_cast<size_t>(i)];
    node.wide = wide;
    node.tall = tall;
}

void LayoutScreen::raise(const std::string& widget) {
    const int i = find_widget(widget);
    if (i < 0 || panel_root_ == nullptr) return;
    Node& node = nodes_[static_cast<size_t>(i)];
    // One past every priority `create` handed out (see there).
    if (node.panel != nullptr) {
        node.panel->_notifyZOrder(static_cast<Ogre::ushort>(panel_root_->getZOrder() + 99));
    }
}

void LayoutScreen::set_tint(const std::string& widget, const Ogre::ColourValue& tint) {
    const int i = find_widget(widget);
    if (i < 0) return;
    Node& node = nodes_[static_cast<size_t>(i)];
    const Sprite* art = node.image.empty() ? nullptr : sprite(node.image);
    if (art == nullptr || !art->texture) return;
    node.tint_on = tint != Ogre::ColourValue::White;
    // A material of its own per colour, built rather than cloned: a clone carries the RTSS
    // technique already generated for the art's material, which a pass change never reaches.
    if (node.tint_on && (!node.tinted || node.tint_colour != tint)) {
        if (node.tinted) Ogre::MaterialManager::getSingleton().remove(node.tinted);
        node.tinted = build_menu_material(element_name(static_cast<size_t>(i), "t"), art->texture);
        Ogre::TextureUnitState* unit =
            node.tinted->getTechnique(0)->getPass(0)->getTextureUnitState(0);
        unit->setColourOperationEx(Ogre::LBX_MODULATE, Ogre::LBS_TEXTURE, Ogre::LBS_MANUAL,
                                   Ogre::ColourValue::White, tint);
        node.tint_colour = tint;
    }
    paint(node, node.image);
}

void LayoutScreen::set_checked(const std::string& widget, bool checked) {
    const int i = find_widget(widget);
    if (i < 0) return;
    const int tick = nodes_[static_cast<size_t>(i)].tick;
    if (tick >= 0) nodes_[static_cast<size_t>(tick)].runtime_hidden = !checked;
}

void LayoutScreen::set_offset(const std::string& widget, float x, float y) {
    const int i = find_widget(widget);
    if (i < 0) return;
    nodes_[static_cast<size_t>(i)].off_x = x;
    nodes_[static_cast<size_t>(i)].off_y = y;
}

void LayoutScreen::set_anchor(const std::string& widget, int horizontal, int vertical) {
    const int i = find_widget(widget);
    if (i < 0) return;
    nodes_[static_cast<size_t>(i)].h_anchor = horizontal;
    nodes_[static_cast<size_t>(i)].v_anchor = vertical;
}

bool LayoutScreen::extent(const std::string& widget, float& wide, float& tall) const {
    const int i = find_widget(widget);
    if (i < 0) return false;
    wide = nodes_[static_cast<size_t>(i)].wide;
    tall = nodes_[static_cast<size_t>(i)].tall;
    return true;
}

void LayoutScreen::set_hover(const std::string& widget, bool hover) {
    const int i = find_widget(widget);
    if (i < 0) return;
    Node& node = nodes_[static_cast<size_t>(i)];
    paint(node, hover && !node.hover.empty() ? node.hover : node.image);
}

void LayoutScreen::layout() {
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

        float left = node.scale_x * parent_wide + node.off_x * scale;
        float top = node.scale_y * parent_tall + node.off_y * scale;
        // Never negative: a frame's middle and edges are the owner's size less
        // two corners, which a small owner does not have.
        const float wide = std::max(0.0f, node.scale_wide * parent_wide + node.wide * scale);
        const float tall = std::max(0.0f, node.scale_tall * parent_tall + node.tall * scale);
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
            // An authored line break is a break of its own; each piece wraps.
            size_t start = 0;
            while (start <= node.caption.size()) {
                const size_t end = node.caption.find('\n', start);
                const std::string piece = node.caption.substr(
                    start, end == std::string::npos ? std::string::npos : end - start);
                const std::vector<std::string> wrapped =
                    wrap_text(font, piece, node.width, line / down, across, down);
                if (wrapped.empty()) lines.push_back(std::string());
                lines.insert(lines.end(), wrapped.begin(), wrapped.end());
                if (end == std::string::npos) break;
                start = end + 1;
            }
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
