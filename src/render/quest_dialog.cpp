// The quest offer window, from `media/UI/questdialogmenu_xenon.layout`. Every offset is read from
// the file at runtime; the only numbers here are the frame's from `ceguiWidget/ItemText`'s
// look'n'feel (add_item_text_frame()).
#include "render/quest_dialog.h"

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

constexpr const char* kLayout = "media/UI/questdialogmenu_xenon.layout";

/// The console interface's four sheets and the seventeen item icon sheets, which hold the
/// speakers' portraits; all are loaded since nothing says which sheet a face is on.
constexpr const char* kImagesets[] = {
    "media/UI/icons2/widgets.imageset",
    "media/UI/icons2/widgets1.imageset",
    "media/UI/icons2/widgets2.imageset",
    "media/UI/icons2/ratings.imageset",
    "media/UI/itemicons/itemicons.imageset",
    "media/UI/itemicons/itemicons1.imageset",
    "media/UI/itemicons/itemicons2.imageset",
    "media/UI/itemicons/itemicons3.imageset",
    "media/UI/itemicons/itemicons4.imageset",
    "media/UI/itemicons/itemicons5.imageset",
    "media/UI/itemicons/itemicons6.imageset",
    "media/UI/itemicons/itemicons7.imageset",
    "media/UI/itemicons/itemicons8.imageset",
    "media/UI/itemicons/itemicons9.imageset",
    "media/UI/itemicons/itemicons10.imageset",
    "media/UI/itemicons/itemicons11.imageset",
    "media/UI/itemicons/itemicons12.imageset",
    "media/UI/itemicons/itemicons13.imageset",
    "media/UI/itemicons/itemicons14.imageset",
    "media/UI/itemicons/itemicons15.imageset",
    "media/UI/itemicons/itemicons16.imageset",
    // The Xbox's own `itemicons13`, renamed at extraction: the console-only sets' icons. Last,
    // so a name the PC sheets hold keeps theirs.
    "media/UI/itemicons/xbox_itemicons13.imageset",
};

/// The extent a descriptor gets when the file authors none - see game_hud.cpp,
/// where the same census is recorded.
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

std::string indexed(const std::string& prefix, size_t index) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04zu", index);
    return prefix + buffer;
}

/// How many body elements to keep: the longest shipped line wraps to eleven at 720p with two runs.
constexpr size_t kBodyElements = 64;

}  // namespace

QuestDialogScreen::~QuestDialogScreen() { destroy(); }

const QuestDialogScreen::Sprite* QuestDialogScreen::sprite(const std::string& name) const {
    auto found = sprites_.find(lowered(name));
    // A console image the sheets lack, drawn in the PC's like of it.
    if (found == sprites_.end()) found = sprites_.find(lowered(pc_image_for(name)));
    return found == sprites_.end() ? nullptr : &found->second;
}

bool QuestDialogScreen::load_art(const std::string& group) {
    // The PC's sets after the console's, so a name both hold keeps the console's.
    std::vector<const char*> paths(std::begin(kImagesets), std::end(kImagesets));
    for (const char* path : pc_imagesets()) paths.push_back(path);
    for (const char* path : paths) {
        std::vector<uint8_t> bytes;
        if (!read_menu_resource(path, group, bytes) || bytes.empty()) continue;
        const tl::formats::Imageset parsed =
            tl::formats::parse_imageset(bytes.data(), bytes.size());
        if (!parsed.ok) continue;
        sets_.push_back(parsed);
    }
    if (sets_.empty()) {
        TL_LOG_INFO("no quest dialog: none of its imagesets is in the mounted data");
        return false;
    }

    for (const tl::formats::Imageset& stored : sets_) {
        Ogre::TexturePtr texture;
        try {
            texture = Ogre::TextureManager::getSingleton().load(stored.image_file, group);
        } catch (const Ogre::Exception&) {
            texture.reset();
        }
        if (!texture) {
            TL_LOG_WARN("quest dialog: no texture '%s'", stored.image_file.c_str());
            continue;
        }
        textures_.push_back(texture);
        materials_.push_back(build_menu_material("tl/questdlg/" + stored.name, texture));

        for (const tl::formats::ImagesetImage& image : stored.images) {
            Sprite entry;
            entry.set = &stored;
            entry.rect = &image;
            entry.texture = texture;
            entry.material = materials_.back();
            // First wins, so the console widgets keep their names against the
            // item sheets loaded after them.
            sprites_.emplace(lowered(image.name), entry);
        }
    }
    return !sprites_.empty();
}

// PORT: drawing, `ceguiWidget/StaticShared`'s nine-slice frame.
void QuestDialogScreen::add_item_text_frame(int owner, const std::string& prefix, size_t& serial) {
    // `ceguiWidget/StaticShared`'s `frame` (`ceguiWidgets.looknfeel`): the nine `tooltip*` images
    // over `LeftEdge -24, TopEdge -9, Width +48, Height +18`. A CEGUI `FrameComponent` draws the
    // corners at their own size, the edges stretched between, the background over the rest.
    const Node& box = nodes_[static_cast<size_t>(owner)];
    const float outset_x = 24.0f;
    const float outset_y = 9.0f;
    const float w = box.wide + 2.0f * outset_x;
    const float h = box.tall + 2.0f * outset_y;

    // The corner size is the artwork's own, which is what a FrameComponent uses.
    float corner_w = 24.0f;
    float corner_h = 24.0f;
    if (const Sprite* art = sprite("tooltiptopleft")) {
        corner_w = static_cast<float>(art->rect->width);
        corner_h = static_cast<float>(art->rect->height);
    }
    const float mid_w = std::max(0.0f, w - 2.0f * corner_w);
    const float mid_h = std::max(0.0f, h - 2.0f * corner_h);

    struct Slice {
        const char* image;
        float x, y, wide, tall;
    };
    const Slice slices[] = {
        {"tooltipmiddle", corner_w, corner_h, mid_w, mid_h},
        {"tooltiptopedge", corner_w, 0.0f, mid_w, corner_h},
        {"tooltipbottomedge", corner_w, h - corner_h, mid_w, corner_h},
        {"tooltipleftedge", 0.0f, corner_h, corner_w, mid_h},
        {"tooltiprightedge", w - corner_w, corner_h, corner_w, mid_h},
        {"tooltiptopleft", 0.0f, 0.0f, corner_w, corner_h},
        {"tooltiptopright", w - corner_w, 0.0f, corner_w, corner_h},
        {"tooltipbottomleft", 0.0f, h - corner_h, corner_w, corner_h},
        {"tooltipbottomright", w - corner_w, h - corner_h, corner_w, corner_h},
    };

    auto& overlays = Ogre::OverlayManager::getSingleton();
    for (const Slice& slice : slices) {
        Node node;
        node.parent = owner;
        node.image = slice.image;
        node.off_x = slice.x - outset_x;
        node.off_y = slice.y - outset_y;
        node.wide = slice.wide;
        node.tall = slice.tall;
        node.widget = "frame/" + std::string(slice.image);

        const Sprite* art = sprite(node.image);
        auto* panel = static_cast<Ogre::PanelOverlayElement*>(
            overlays.createOverlayElement("Panel", prefix + indexed("/slice", serial)));
        ++serial;
        panel->setMetricsMode(Ogre::GMM_RELATIVE);
        if (art != nullptr && art->texture) {
            panel->setMaterial(art->material);
            const float tw = static_cast<float>(art->texture->getWidth());
            const float th = static_cast<float>(art->texture->getHeight());
            panel->setUV(static_cast<float>(art->rect->x) / tw,
                         static_cast<float>(art->rect->y) / th,
                         static_cast<float>(art->rect->x + art->rect->width) / tw,
                         static_cast<float>(art->rect->y + art->rect->height) / th);
        } else {
            panel->setTransparent(true);
        }
        node.panel = panel;
        nodes_.push_back(node);
    }
}

void QuestDialogScreen::build(const tl::formats::UiObject& object, int parent,
                              const std::string& prefix, size_t& serial, bool inherited_hidden) {
    const std::string descriptor = object.descriptor();
    const std::string widget = object.string_or("WIDGET NAME", std::string());

    Node node;
    node.parent = parent;
    node.widget = widget;
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
    node.hidden = inherited_hidden || !object.bool_or("VISIBLE", true);

    auto& overlays = Ogre::OverlayManager::getSingleton();
    const std::string name = prefix + indexed("/", serial);
    ++serial;

    if (descriptor == "Text" || descriptor == "Text Outlined") {
        node.points = font_points(object.string_or("FONT", "Serif"));
        Ogre::ColourValue colour = Ogre::ColourValue::White;
        float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
        if (tl::formats::parse_colour(object.string_or("COLOR", std::string()), r, g, b, a)) {
            colour = Ogre::ColourValue(r, g, b, a);
        }
        auto make = [&](const std::string& element, const Ogre::ColourValue& tint) {
            auto* area = static_cast<Ogre::TextAreaOverlayElement*>(
                overlays.createOverlayElement("TextArea", element));
            area->setMetricsMode(Ogre::GMM_RELATIVE);
            area->setFontName(kFontName, Ogre::RGN_INTERNAL);
            area->setColour(tint);
            if (node.h_text == 1) area->setAlignment(Ogre::TextAreaOverlayElement::Center);
            else if (node.h_text == 2) area->setAlignment(Ogre::TextAreaOverlayElement::Right);
            area->setCaption(tl::core::strip_markup(object.string_or("TEXT", std::string())));
            text_root_->addChild(area);
            return area;
        };
        if (descriptor == "Text Outlined") {
            node.drop = make(name + "a", Ogre::ColourValue(0.0f, 0.0f, 0.0f, a));
        }
        node.text = make(name + "b", colour);
    } else {
        const std::string image = object.string_or("IMAGE", object.string_or("NORMAL", ""));
        auto* panel =
            static_cast<Ogre::PanelOverlayElement*>(overlays.createOverlayElement("Panel", name));
        panel->setMetricsMode(Ogre::GMM_RELATIVE);
        const Sprite* art = image.empty() ? nullptr : sprite(image);
        if (art != nullptr && art->texture) {
            panel->setMaterial(art->material);
            node.image = image;
            const float tw = static_cast<float>(art->texture->getWidth());
            const float th = static_cast<float>(art->texture->getHeight());
            panel->setUV(static_cast<float>(art->rect->x) / tw,
                         static_cast<float>(art->rect->y) / th,
                         static_cast<float>(art->rect->x + art->rect->width) / tw,
                         static_cast<float>(art->rect->y + art->rect->height) / th);
        } else {
            panel->setTransparent(true);
        }
        node.panel = panel;
    }

    const int self = static_cast<int>(nodes_.size());
    nodes_.push_back(std::move(node));

    // The window's own skin, behind its children: `LOOK N FEEL` is what makes
    // the offer window a dark panel rather than words on the scenery.
    if (object.string_or("LOOK N FEEL", "") == "ceguiWidget/ItemText") {
        add_item_text_frame(self, name, serial);
    }

    std::vector<const tl::formats::UiObject*> order;
    order.reserve(object.children.size());
    for (const tl::formats::UiObject& child : object.children) order.push_back(&child);
    std::stable_sort(order.begin(), order.end(),
                     [](const tl::formats::UiObject* a, const tl::formats::UiObject* b) {
                         return a->bool_or("ALWAYS ON TOP", false) <
                                b->bool_or("ALWAYS ON TOP", false);
                     });
    for (const tl::formats::UiObject* child : order) {
        build(*child, self, name, serial, nodes_[static_cast<size_t>(self)].hidden);
    }
}

bool QuestDialogScreen::add_layout(const std::string& path, const std::string& group) {
    std::vector<uint8_t> bytes;
    if (!read_menu_resource(path, group, bytes) || bytes.empty()) {
        TL_LOG_INFO(
            "no quest dialog: '%s' is not in the mounted data - re-run "
            "scripts/extract_console_ui.py",
            path.c_str());
        return false;
    }
    const tl::formats::UiLayout parsed = tl::formats::parse_ui_layout(bytes.data(), bytes.size());
    if (!parsed.ok) {
        TL_LOG_WARN("quest dialog: %s: %s", path.c_str(), parsed.error.c_str());
        return false;
    }

    std::vector<const tl::formats::UiObject*> order;
    order.reserve(parsed.objects.size());
    for (const tl::formats::UiObject& object : parsed.objects) order.push_back(&object);
    std::stable_sort(order.begin(), order.end(),
                     [](const tl::formats::UiObject* a, const tl::formats::UiObject* b) {
                         return a->bool_or("ALWAYS ON TOP", false) <
                                b->bool_or("ALWAYS ON TOP", false);
                     });
    size_t serial = 0;
    for (const tl::formats::UiObject* object : order)
        build(*object, -1, "tl/questdlg", serial, false);
    return true;
}

bool QuestDialogScreen::create(const std::string& group, unsigned width, unsigned height) {
    destroy();
    width_ = width;
    height_ = height;

    if (!load_art(group)) {
        destroy();
        return false;
    }
    if (!ensure_menu_font()) {
        destroy();
        return false;
    }

    try {
        auto& overlays = Ogre::OverlayManager::getSingleton();
        // Above the game HUD's 470/472 and above the menus, because it stops
        // the player: nothing else on screen may cover it.
        overlay_ = overlays.create("tl/questdlg/panels");
        overlay_->setZOrder(590);
        text_overlay_ = overlays.create("tl/questdlg/text");
        text_overlay_->setZOrder(592);

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
        panel_root_ = root("tl/questdlg/panelroot", overlay_);
        text_root_ = root("tl/questdlg/textroot", text_overlay_);

        if (!add_layout(kLayout, group)) {
            destroy();
            return false;
        }

        for (size_t i = 0; i < kBodyElements; ++i) {
            auto* area = static_cast<Ogre::TextAreaOverlayElement*>(
                overlays.createOverlayElement("TextArea", indexed("tl/questdlg/body", i)));
            area->setMetricsMode(Ogre::GMM_RELATIVE);
            area->setFontName(kFontName, Ogre::RGN_INTERNAL);
            area->setColour(Ogre::ColourValue::White);
            area->hide();
            text_root_->addChild(area);
            body_.push_back(area);
        }

        for (Node& node : nodes_) {
            if (node.panel != nullptr) panel_root_->addChild(node.panel);
        }
        // Its own render priority per element: OGRE sorts equal-depth transparent overlay
        // elements by `Pass` pointer, so the sheet would otherwise decide what covers what.
        Ogre::ushort z = static_cast<Ogre::ushort>(panel_root_->getZOrder() + 1);
        for (Node& node : nodes_) {
            if (node.panel == nullptr) continue;
            node.panel->_notifyZOrder(z);
            if (z < static_cast<Ogre::ushort>(panel_root_->getZOrder() + 99)) ++z;
        }

        built_ = true;
        layout();
        hide();
        TL_LOG_INFO("quest dialog: %zu window(s) at %u x %u", nodes_.size(), width_, height_);
        return true;
    } catch (const Ogre::Exception& e) {
        TL_LOG_WARN("no quest dialog: %s", e.getDescription().c_str());
        destroy();
        return false;
    }
}

// Releases everything create() may have taken, whatever point it got to; not gated on `built_`
// (see `InventoryMenu::destroy()`).
void QuestDialogScreen::destroy() {
    if (Ogre::OverlayManager::getSingletonPtr() != nullptr &&
        (overlay_ != nullptr || text_overlay_ != nullptr)) {
        auto& overlays = Ogre::OverlayManager::getSingleton();
        for (Ogre::Overlay* overlay : {overlay_, text_overlay_}) {
            if (overlay != nullptr) overlay->hide();
        }
        for (Ogre::TextAreaOverlayElement* area : body_) {
            if (area == nullptr) continue;
            if (text_root_ != nullptr) text_root_->removeChild(area->getName());
            overlays.destroyOverlayElement(area);
        }
        for (Node& node : nodes_) {
            if (node.panel != nullptr && panel_root_ != nullptr) {
                panel_root_->removeChild(node.panel->getName());
            }
            for (Ogre::TextAreaOverlayElement* area : {node.drop, node.text}) {
                if (area == nullptr || text_root_ == nullptr) continue;
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
    body_.clear();
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

void QuestDialogScreen::show() {
    if (!built_ || visible_) return;
    visible_ = true;
    if (overlay_ != nullptr) overlay_->show();
    if (text_overlay_ != nullptr) text_overlay_->show();
}

void QuestDialogScreen::hide() {
    if (!built_) return;
    visible_ = false;
    if (overlay_ != nullptr) overlay_->hide();
    if (text_overlay_ != nullptr) text_overlay_->hide();
}

void QuestDialogScreen::resize(unsigned width, unsigned height) {
    width_ = width;
    height_ = height;
    if (built_) layout();
}

void QuestDialogScreen::toggle_rewards() {
    state_.rewards = !state_.rewards;
    apply_state();
}

bool QuestDialogScreen::pointed(const tl::core::InputState& in, const std::string& widget) const {
    if (!visible_ || !in.pointer) return false;
    const int index = find_widget(widget);
    if (index < 0 || is_hidden(index)) return false;
    const Node& node = nodes_[static_cast<size_t>(index)];
    return in.pointer_x >= node.left && in.pointer_x < node.left + node.width &&
           in.pointer_y >= node.top && in.pointer_y < node.top + node.height;
}

int QuestDialogScreen::find_widget(const std::string& name) const {
    for (size_t i = 0; i < nodes_.size(); ++i) {
        if (nodes_[i].widget == name) return static_cast<int>(i);
    }
    return -1;
}

void QuestDialogScreen::set_widget_text(const std::string& name, const std::string& text) {
    const int index = find_widget(name);
    if (index < 0) return;
    Node& node = nodes_[static_cast<size_t>(index)];
    if (node.text != nullptr) node.text->setCaption(text);
    if (node.drop != nullptr) node.drop->setCaption(text);
}

void QuestDialogScreen::set_widget_visible(const std::string& name, bool visible) {
    const int index = find_widget(name);
    if (index < 0) return;
    nodes_[static_cast<size_t>(index)].runtime_hidden = !visible;
}

bool QuestDialogScreen::is_hidden(int index) const {
    while (index >= 0 && index < static_cast<int>(nodes_.size())) {
        const Node& node = nodes_[static_cast<size_t>(index)];
        if (node.hidden || node.runtime_hidden) return true;
        index = node.parent;
    }
    return false;
}

// PORT: the offer window's state taken and drawn.
void QuestDialogScreen::set_state(const QuestOfferState& state) {
    state_ = state;
    if (!built_) return;
    apply_state();
}

void QuestDialogScreen::apply_state() {
    if (!built_) return;

    // The portrait. `CQuestDialogMenu::setNPC` reads `PORTRAIT` off the unit
    // and hands it to `getImageFromImageSet`; a unit with none leaves the frame
    // empty, so the window is taken down rather than drawn as a blank square.
    const int portrait = find_widget("Portrait");
    if (portrait >= 0) {
        Node& node = nodes_[static_cast<size_t>(portrait)];
        const Sprite* art = state_.portrait.empty() ? nullptr : sprite(state_.portrait);
        if (!state_.portrait.empty() && art == nullptr) {
            TL_LOG_WARN("quest dialog: no picture called '%s' among the %zu loaded",
                        state_.portrait.c_str(), sprites_.size());
        }
        node.runtime_hidden = art == nullptr;
        if (art != nullptr && node.panel != nullptr && art->texture) {
            node.image = state_.portrait;
            // `setMaterial` does not undo `setTransparent`: `Portrait` has no `IMAGE`, so build()
            // made it transparent, and `PanelOverlayElement::_updateRenderQueue` skips those.
            node.panel->setTransparent(false);
            node.panel->setMaterial(art->material);
            const float tw = static_cast<float>(art->texture->getWidth());
            const float th = static_cast<float>(art->texture->getHeight());
            node.panel->setUV(static_cast<float>(art->rect->x) / tw,
                              static_cast<float>(art->rect->y) / th,
                              static_cast<float>(art->rect->x + art->rect->width) / tw,
                              static_cast<float>(art->rect->y + art->rect->height) / th);
        }
    }

    // The layout authors `DialogText` with the placeholder "Text"; the line
    // itself is drawn by lay_out_body(), a run at a time, so the authored
    // element is taken down rather than fought with.
    set_widget_visible("DialogText", false);

    // The three figures, each hidden when it is zero - `setNPC` calls
    // `setVisible` either side of each `getQuestReward*()`.
    const bool rewards = state_.rewards;
    const auto box = [&](const char* box_name, const char* text_name, const char* label,
                         int value) {
        const bool on = rewards && value > 0;
        set_widget_visible(box_name, on);
        set_widget_visible(text_name, on);
        set_widget_text(text_name, std::string(label) + ":" + std::to_string(value));
    };
    box("RewardGoldBox", "RewardGold", "GP", state_.reward.gold);
    box("RewardXPBox", "RewardXP", "XP", state_.reward.xp);
    box("RewardFameBox", "RewardFame", "Fame", state_.reward.fame);

    // The three reward item slots, each shown only when filled. Only the first is: `[REWARD]`
    // carries one `TREASURECLASS`; the others are for `[UNITREWARD]`.
    const Sprite* reward = state_.reward_icon.empty() ? nullptr : sprite(state_.reward_icon);
    if (!state_.reward_icon.empty() && reward == nullptr) {
        TL_LOG_WARN("quest dialog: no item icon called '%s'", state_.reward_icon.c_str());
    }
    const bool show_item = rewards && reward != nullptr;

    for (int i = 1; i <= 3; ++i) {
        const std::string n = std::to_string(i);
        const bool on = (i == 1) && show_item;
        set_widget_visible("ItemBkg" + n, on);
        set_widget_visible("ItemSlot" + n, on);
        // The sockets are the item's, and a quest reward has none to show yet.
        set_widget_visible("EmptySocket" + n + "_0", false);
        set_widget_visible("EmptySocket" + n + "_1", false);
    }
    set_widget_visible("ItemHighlight", false);

    if (show_item) {
        const int slot = find_widget("ItemSlot1");
        if (slot >= 0) {
            Node& node = nodes_[static_cast<size_t>(slot)];
            if (node.panel != nullptr && reward->texture) {
                node.image = state_.reward_icon;
                // As for the portrait, in case a layout drops the `IMAGE`.
                node.panel->setTransparent(false);
                node.panel->setMaterial(reward->material);
                const float tw = static_cast<float>(reward->texture->getWidth());
                const float th = static_cast<float>(reward->texture->getHeight());
                node.panel->setUV(static_cast<float>(reward->rect->x) / tw,
                                  static_cast<float>(reward->rect->y) / th,
                                  static_cast<float>(reward->rect->x + reward->rect->width) / tw,
                                  static_cast<float>(reward->rect->y + reward->rect->height) / th);
            }
        }
    }

    // `Accept`, `Decline` and `Ok` are all authored in the same place; which are up is the
    // difference between an offer and a line to be read.
    set_widget_visible("Accept", state_.offer);
    set_widget_visible("Decline", state_.offer && !state_.force_accept);
    set_widget_visible("Ok", !state_.offer);

    // `<LStick> Rewards` shows whenever there is something to toggle, offer or not.
    const bool any_reward =
        state_.reward.gold > 0 || state_.reward.xp > 0 || state_.reward.fame > 0;
    set_widget_visible("RewardsTooltip", any_reward);

    // `X Fast Scroll` skips the type-on, which this engine does not have: taken down.
    set_widget_visible("Fast Scroll", false);

    layout();
}

void QuestDialogScreen::lay_out_body() {
    for (Ogre::TextAreaOverlayElement* area : body_) area->hide();

    // Not gated on `visible_`: set_state() is called before show().
    const int frame = find_widget("DialogTextFrame");
    if (frame < 0) return;
    const Node& box = nodes_[static_cast<size_t>(frame)];

    const float across = static_cast<float>(width_ > 0 ? width_ : 1);
    const float down = static_cast<float>(height_ > 0 ? height_ : 1);

    // `DialogText`'s authored font and size; the frame is the box it wraps inside.
    float points = kBigFontSize;
    const int text_node = find_widget("DialogText");
    if (text_node >= 0) points = nodes_[static_cast<size_t>(text_node)].points;
    const float line = points * ui_points_to_pixels() * scale_ / down;

    Ogre::FontPtr font = Ogre::FontManager::getSingleton().getByName(kFontName, Ogre::RGN_INTERNAL);
    if (font && !font->isLoaded()) font->load();

    const std::vector<std::vector<TextSpan>> lines =
        wrap_runs(font, tl::core::markup_runs(state_.text), box.width, line, across, down);

    // 1.35 line spacing: nothing in the `[UI]` data or the look'n'feel gives a paragraph's
    // leading.
    const float leading = line * 1.35f;

    size_t used = 0;
    for (size_t row = 0; row < lines.size() && used < body_.size(); ++row) {
        for (const TextSpan& span : lines[row]) {
            if (used >= body_.size()) break;
            Ogre::TextAreaOverlayElement* area = body_[used++];
            area->setCaption(span.text);
            set_char_height(area, line);
            area->setColour(span.coloured ? Ogre::ColourValue(span.r, span.g, span.b, span.a)
                                          : Ogre::ColourValue::White);
            area->setPosition(box.left + span.x, box.top + static_cast<float>(row) * leading);
            area->show();
        }
    }
}

void QuestDialogScreen::layout() {
    if (!built_) return;
    const float across = static_cast<float>(width_ > 0 ? width_ : 1);
    const float down = static_cast<float>(height_ > 0 ? height_ : 1);

    // The one scaling rule the whole interface obeys: `XRATIO = width/1024`,
    // `YRATIO = height/768`, and `CMainMenu::createMenus()` passes `useX=false`.
    const float native = sets_.empty() || sets_.front().native_height <= 0
                             ? kNativeHeight
                             : static_cast<float>(sets_.front().native_height);
    scale_ = down / native;

    for (size_t i = 0; i < nodes_.size(); ++i) {
        Node& node = nodes_[i];
        const float parent_left =
            node.parent < 0 ? 0.0f : nodes_[static_cast<size_t>(node.parent)].left;
        const float parent_top =
            node.parent < 0 ? 0.0f : nodes_[static_cast<size_t>(node.parent)].top;
        const float parent_wide =
            node.parent < 0 ? across : nodes_[static_cast<size_t>(node.parent)].width * across;
        const float parent_tall =
            node.parent < 0 ? down : nodes_[static_cast<size_t>(node.parent)].height * down;

        float left = node.off_x * scale_;
        float top = node.off_y * scale_;
        const float wide = node.scale_wide * parent_wide + node.wide * scale_;
        const float tall = node.scale_tall * parent_tall + node.tall * scale_;

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
        for (Ogre::TextAreaOverlayElement* area : {node.drop, node.text}) {
            if (area == nullptr) continue;
            if (hidden) {
                area->hide();
                continue;
            }
            area->show();
            const float line = node.points * ui_points_to_pixels() * scale_;
            float x = node.left;
            if (node.h_text == 1) x = node.left + node.width * 0.5f;
            else if (node.h_text == 2) x = node.left + node.width;
            float y = node.top;
            if (node.v_text == 1) y = node.top + (tall - line) * 0.5f / down;
            else if (node.v_text == 2) y = node.top + (tall - line) / down;
            const float nudge = (area == node.drop) ? kLabelShadow * scale_ : 0.0f;
            set_char_height(area, line / down);
            area->setPosition(x + nudge / across, y + nudge / down);
        }
    }

    lay_out_body();
}

}  // namespace tl::render
