// The in-game HUD, from `media/UI/hud_xenon.layout` and `pethud_xenon.layout`. Every offset is read
// from those files at runtime; the only numbers here are the format's own (src/formats/uilayout.h).
#include "render/game_hud.h"

#include "core/item_label.h"
#include "core/log.h"
#include "render/menu_chrome.h"
#include "render/overlay_set.h"
#include "render/text_layout.h"

#include <OgreFont.h>
#include <OgreOverlay.h>
#include <OgreOverlayContainer.h>
#include <OgreOverlayManager.h>
#include <OgrePanelOverlayElement.h>
#include <OgrePass.h>
#include <OgreTechnique.h>
#include <OgreTextAreaOverlayElement.h>
#include <OgreTextureManager.h>

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <vector>

namespace tl::render {
namespace {

/// The console HUD's own two layouts. Full archive paths, because OGRE indexes
/// archive entries by basename alone and these names are not unique.
constexpr const char* kHudLayout = "media/UI/hud_xenon.layout";
constexpr const char* kPetLayout = "media/UI/pethud_xenon.layout";

/// Where the console HUD's art lives: four imagesets over four 1024x1024 textures, whose names do
/// not collide, so `IMAGE:hpmanaball` names a picture without its sheet.
constexpr const char* kImagesets[] = {
    "media/UI/icons2/widgets.imageset",
    "media/UI/icons2/widgets1.imageset",
    "media/UI/icons2/widgets2.imageset",
    "media/UI/icons2/ratings.imageset",
};

/// The item icons, for whatever `SkillSlot1`/`SkillSlot2` carry: the layout gives the potion slots
/// a frame and a count but the picture is the item's ICON, on any sheet, so all are loaded (as in
/// `inventory_menu.cpp`).
constexpr const char* kItemIconImagesets[] = {
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
    // The Xbox's own `itemicons13`, renamed at extraction: the console-only sets' icons. Last, so
    // a name the PC sheets hold keeps theirs.
    "media/UI/itemicons/xbox_itemicons13.imageset",
};

/// The extent a descriptor gets when the file authors none: 250 wide, except a `Button` (30 tall)
/// and a `Check Box` (50x40), as the CEGUI exporter writes them.
float default_extent(const std::string& descriptor, bool horizontal) {
    if (descriptor == "Button") return horizontal ? 250.0f : 30.0f;
    if (descriptor == "Check Box") return horizontal ? 50.0f : 40.0f;
    return 250.0f;
}

/// `HORIZONTAL` / `VERTICLE`: which edge of the parent the rectangle hangs from (CEGUI's
/// `HorizontalAlignment` and `VerticalAlignment`).
int anchor_of(const std::string& word) {
    if (word == "CENTER") return 1;
    if (word == "RIGHT" || word == "BOTTOM") return 2;
    return 0;
}

/// The point size behind each `Serif*.font`, all of which name `BRLNSDB.TTF`.
/// A window that names no font gets `Serif`, which is what the CEGUI export
/// writes onto the skill-slot labels that author none.
float font_points(const std::string& name) {
    if (name == "SerifSmall") return kSmallFontSize;
    if (name == "Serif14") return 14.0f;
    if (name == "SerifBig") return kBigFontSize;
    if (name == "SerifHuge") return kHugeFontSize;
    return kFontSize;
}

/// Windows the layouts author that stay hidden until the thing behind them exists: the target
/// panel (`TargetHealth`, `TargetInteractHover`), the `+` over the bag (`AvailableSkills*`), the
/// second banner (`SmallMessageText`) and the pet's `FleeingText`. (`SkillSet1`/`SkillSet2` are
/// swapped by `HudState::skill_set`; see `apply_state`.)
const char* const kRuntimeOnly[] = {
    "TargetHealth",     "TargetInteractHover", "AvailableSkillsIcon", "AvailableSkillsGlowIcon",
    "SmallMessageText", "FleeingText",
};

/// The hotbar slot a `SkillSlot<N>` or `SkillText<N>` window stands for, N - 1,
/// or -1 for any other widget. `hud_xenon.layout` numbers them 1 to 10.
int hotbar_slot_of(const std::string& widget, const char* prefix) {
    const size_t length = std::char_traits<char>::length(prefix);
    if (widget.size() <= length || widget.compare(0, length, prefix) != 0) return -1;
    int number = 0;
    for (size_t i = length; i < widget.size(); ++i) {
        if (widget[i] < '0' || widget[i] > '9') return -1;
        number = number * 10 + (widget[i] - '0');
    }
    return number >= 1 && number <= tl::core::kSkillFunctionSlots ? number - 1 : -1;
}

bool is_runtime_only(const std::string& widget) {
    if (widget.empty()) return false;
    for (const char* name : kRuntimeOnly) {
        if (widget == name) return true;
    }
    return false;
}

std::string lowered(const std::string& text) {
    std::string out = text;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    return out;
}

/// A four-digit draw index, so that the name-keyed map an `OverlayContainer`
/// keeps its children in iterates them in the order they should be drawn. The
/// same trick character_create.cpp uses for a button's shadow and its face.
std::string indexed(const char* prefix, size_t index) {
    char digits[8];
    std::snprintf(digits, sizeof(digits), "%04zu", index);
    return std::string(prefix) + digits;
}

}  // namespace

GameHud::~GameHud() { destroy(); }

const GameHud::Sprite* GameHud::sprite(const std::string& name) const {
    auto found = sprites_.find(lowered(name));
    // A console image the sheets lack, drawn in the PC's like of it.
    if (found == sprites_.end()) found = sprites_.find(lowered(pc_image_for(name)));
    return found == sprites_.end() ? nullptr : &found->second;
}

bool GameHud::load_art(const std::string& group) {
    // The item icons, then the PC's own sets (the panel looks the interface pack leaves out),
    // both optional.
    std::vector<const char*> extra(std::begin(kItemIconImagesets), std::end(kItemIconImagesets));
    for (const char* path : pc_imagesets()) extra.push_back(path);
    // Reserved up front for both loops below: a `Sprite` holds a pointer into one of these.
    sets_.reserve(std::size(kImagesets) + extra.size());

    for (const char* path : kImagesets) {
        std::vector<uint8_t> bytes;
        if (!read_menu_resource(path, group, bytes) || bytes.empty()) {
            TL_LOG_INFO("no game HUD: '%s' is not in the mounted data", path);
            TL_LOG_INFO(
                "the console HUD's art ships only in the Xbox release; "
                "mount it with --extra-data (see scripts/extract_console_ui.py)");
            return false;
        }
        tl::formats::Imageset set = tl::formats::parse_imageset(bytes.data(), bytes.size());
        if (!set.ok) {
            TL_LOG_WARN("game HUD: %s: %s", path, set.error.c_str());
            return false;
        }
        sets_.push_back(std::move(set));

        const tl::formats::Imageset& stored = sets_.back();
        Ogre::TexturePtr texture =
            Ogre::TextureManager::getSingleton().load(stored.image_file, group);
        if (!texture) {
            TL_LOG_WARN("game HUD: no texture '%s'", stored.image_file.c_str());
            return false;
        }
        textures_.push_back(texture);
        materials_.push_back(build_menu_material("tl/hud/" + stored.name, texture));

        for (const tl::formats::ImagesetImage& image : stored.images) {
            Sprite entry;
            entry.set = &stored;
            entry.rect = &image;
            entry.texture = texture;
            entry.material = materials_.back();
            // First wins, so a modded set cannot replace art an earlier one named.
            sprites_.emplace(lowered(image.name), entry);
        }
    }

    // The item icons are supplementary: a missing or short sheet is logged and skipped rather than
    // failing the build.
    for (const char* path : extra) {
        std::vector<uint8_t> bytes;
        if (!read_menu_resource(path, group, bytes) || bytes.empty()) continue;
        tl::formats::Imageset set = tl::formats::parse_imageset(bytes.data(), bytes.size());
        if (!set.ok) continue;
        sets_.push_back(std::move(set));

        const tl::formats::Imageset& stored = sets_.back();
        Ogre::TexturePtr texture =
            Ogre::TextureManager::getSingleton().load(stored.image_file, group);
        if (!texture) continue;
        textures_.push_back(texture);
        materials_.push_back(build_menu_material("tl/hud/item_" + stored.name, texture));

        for (const tl::formats::ImagesetImage& image : stored.images) {
            Sprite entry;
            entry.set = &stored;
            entry.rect = &image;
            entry.texture = texture;
            entry.material = materials_.back();
            sprites_.emplace(lowered(image.name), entry);
        }
    }

    TL_LOG_INFO("game HUD: %zu named images over %zu sheets", sprites_.size(), sets_.size());
    return true;
}

bool GameHud::add_layout(const std::string& path, const std::string& group, const char* what) {
    std::vector<uint8_t> bytes;
    if (!read_menu_resource(path, group, bytes) || bytes.empty()) {
        TL_LOG_INFO("no game HUD: '%s' is not in the mounted data", path.c_str());
        return false;
    }
    const tl::formats::UiLayout parsed = tl::formats::parse_ui_layout(bytes.data(), bytes.size());
    if (!parsed.ok) {
        TL_LOG_WARN("game HUD: %s: %s", path.c_str(), parsed.error.c_str());
        return false;
    }

    // Siblings in draw order: file order, `ALWAYS ON TOP` after the rest (the potion slot over
    // the orb, the experience bar's frame over its fill).
    const size_t before = nodes_.size();
    std::vector<const tl::formats::UiObject*> order;
    order.reserve(parsed.objects.size());
    for (const tl::formats::UiObject& object : parsed.objects) order.push_back(&object);
    std::stable_sort(order.begin(), order.end(),
                     [](const tl::formats::UiObject* a, const tl::formats::UiObject* b) {
                         return a->bool_or("ALWAYS ON TOP", false) <
                                b->bool_or("ALWAYS ON TOP", false);
                     });

    size_t serial = nodes_.size();
    for (const tl::formats::UiObject* object : order) build(*object, -1, "", serial, false);

    TL_LOG_INFO("game HUD: %s, %zu window(s) from %s", what, nodes_.size() - before, path.c_str());
    return true;
}

void GameHud::build(const tl::formats::UiObject& object, int parent, const std::string& prefix,
                    size_t& serial, bool inherited_hidden) {
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
    node.hidden = inherited_hidden || !object.bool_or("VISIBLE", true) || is_runtime_only(widget);
    node.skill_set = widget == "SkillSet1"   ? 0
                     : widget == "SkillSet2" ? 1
                     : parent >= 0           ? nodes_[static_cast<size_t>(parent)].skill_set
                                             : -1;

    // Which bar this is, and which way it empties. Nothing in the data says from which edge: the
    // orbs drain downwards (a ball of liquid), the straight bars fill from the left.
    if (widget == "PlayerHealthBarSub" || widget == "PlayerManaBarSub") node.fill = kFillDown;
    if (widget == "ExperienceBar" || widget == "PetHealthBar" || widget == "PetManaBar" ||
        widget == "TargetHealthBar") {
        node.fill = kFillRight;
    }

    // The target panel's subtree: `TargetHealth` is `runtime only` but its children are ordinary
    // windows, so they are bracketed as an index range (the walk is depth-first).
    const bool target_root = widget == "TargetHealth";
    if (target_root) target_first_ = nodes_.size();

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
        auto make = [&](const std::string& element_name, const Ogre::ColourValue& tint) {
            auto* area = static_cast<Ogre::TextAreaOverlayElement*>(
                overlays.createOverlayElement("TextArea", element_name));
            area->setMetricsMode(Ogre::GMM_RELATIVE);
            area->setFontName(kFontName, Ogre::RGN_INTERNAL);
            area->setColour(tint);
            if (node.h_text == 1) area->setAlignment(Ogre::TextAreaOverlayElement::Center);
            else if (node.h_text == 2) area->setAlignment(Ogre::TextAreaOverlayElement::Right);
            text_root_->addChild(area);
            return area;
        };
        // `Text Outlined` is drawn as a single dark copy behind the glyphs, not a true outline.
        if (descriptor == "Text Outlined") {
            node.drop = make("tl/hud/text" + name + "a", Ogre::ColourValue(0.0f, 0.0f, 0.0f, a));
        }
        node.text = make("tl/hud/text" + name + "b", colour);
    } else {
        const std::string image = object.string_or("IMAGE", object.string_or("NORMAL", ""));
        auto* panel = static_cast<Ogre::PanelOverlayElement*>(
            overlays.createOverlayElement("Panel", "tl/hud/panel" + name));
        panel->setMetricsMode(Ogre::GMM_RELATIVE);
        node.normal_image = object.string_or("NORMAL", "");
        node.disabled_image = object.string_or("DISABLED", "");
        const Sprite* art = image.empty() ? nullptr : sprite(image);
        if (art != nullptr) {
            panel->setMaterial(art->material);
            node.image = image;
            // The rectangle this name picks out of the sheet (left at 0..1 a panel draws the
            // whole atlas).
            const float tw = static_cast<float>(art->texture->getWidth());
            const float th = static_cast<float>(art->texture->getHeight());
            panel->setUV(static_cast<float>(art->rect->x) / tw,
                         static_cast<float>(art->rect->y) / th,
                         static_cast<float>(art->rect->x + art->rect->width) / tw,
                         static_cast<float>(art->rect->y + art->rect->height) / th);
        } else {
            // A window with no picture (common: an orb's holder, a skill set); it carries the
            // offsets its children hang from.
            panel->setTransparent(true);
            if (!image.empty()) {
                TL_LOG_WARN("game HUD: no image called '%s'", image.c_str());
            }
        }
        node.panel = panel;
    }

    const int self = static_cast<int>(nodes_.size());
    nodes_.push_back(std::move(node));

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
    // Every node the walk pushed under this one is part of the panel.
    if (target_root) target_last_ = nodes_.empty() ? 0 : nodes_.size() - 1;
}

bool GameHud::create(const std::string& group, unsigned width, unsigned height) {
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
        // Two overlays with ascending z-orders, above the menus' 460-462 and below the dialogue
        // HUD's 500: the pictures, then the words over them (see menu.h).
        overlay_ = overlays.create("tl/hud/game");
        overlay_->setZOrder(470);
        text_overlay_ = overlays.create("tl/hud/gametext");
        text_overlay_->setZOrder(472);

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
        panel_root_ = root("tl/hud/panelroot", overlay_);
        text_root_ = root("tl/hud/textroot", text_overlay_);

        if (!add_layout(kHudLayout, group, "the screen")) {
            destroy();
            return false;
        }
        // The pet panel is a second file and a second failure: a run whose data
        // has the HUD but not the pet HUD gets the HUD.
        pet_first_ = nodes_.size();
        add_layout(kPetLayout, group, "the pet panel");

        // Flat under one container, in draw order, each with its own render priority: OGRE sorts
        // equal-depth transparent renderables by `Pass` pointer (`DistanceSortDescendingLess`),
        // so the sheet would decide what covers what. Each priority is its own
        // `RenderPriorityGroup`, ordered by key. After every `addChild`: `Overlay::add2D` re-runs
        // `assignZOrders()`.
        for (Node& node : nodes_) {
            if (node.panel != nullptr) panel_root_->addChild(node.panel);
        }
        Ogre::ushort z = static_cast<Ogre::ushort>(panel_root_->getZOrder() + 1);
        for (Node& node : nodes_) {
            if (node.panel == nullptr) continue;
            node.panel->_notifyZOrder(z);
            // Room for 100 per overlay (`Overlay::setZOrder` multiplies by that); stopping at the
            // top of the band keeps a modded layout out of the next overlay.
            if (z < static_cast<Ogre::ushort>(panel_root_->getZOrder() + 99)) ++z;
        }

        build_labels();

        built_ = true;
        layout();  // and apply_state(), which it ends with
        TL_LOG_INFO("game HUD: %zu window(s) at %u x %u", nodes_.size(), width_, height_);
        return true;
    } catch (const Ogre::Exception& e) {
        TL_LOG_WARN("no game HUD: %s", e.getDescription().c_str());
        destroy();
        return false;
    }
}

void GameHud::destroy() {
    destroy_labels();
    if (overlay_ != nullptr || text_overlay_ != nullptr) {
        auto& overlays = Ogre::OverlayManager::getSingleton();
        for (Ogre::Overlay* overlay : {overlay_, text_overlay_}) {
            if (overlay != nullptr) overlay->hide();
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
    nodes_.clear();
    sprites_.clear();
    materials_.clear();
    grey_materials_.clear();
    textures_.clear();
    sets_.clear();
    pet_first_ = 0;
    target_first_ = 0;
    target_last_ = 0;
    panel_root_ = nullptr;
    text_root_ = nullptr;
    overlay_ = nullptr;
    text_overlay_ = nullptr;
    built_ = false;
    visible_ = false;
}

void GameHud::show() {
    if (!built_ || visible_) return;
    visible_ = true;
    if (overlay_ != nullptr) overlay_->show();
    if (text_overlay_ != nullptr) text_overlay_->show();
    if (label_overlay_ != nullptr) label_overlay_->show();
    if (label_text_overlay_ != nullptr) label_text_overlay_->show();
}

void GameHud::hide() {
    if (!built_) return;
    visible_ = false;
    if (overlay_ != nullptr) overlay_->hide();
    if (text_overlay_ != nullptr) text_overlay_->hide();
    if (label_overlay_ != nullptr) label_overlay_->hide();
    if (label_text_overlay_ != nullptr) label_text_overlay_->hide();
}

void GameHud::resize(unsigned width, unsigned height) {
    width_ = width;
    height_ = height;
    layout();
    layout_labels();
}

void GameHud::set_compact(bool compact) {
    if (compact_ == compact) return;
    compact_ = compact;
    compact_scale_ = compact ? 0.86f : 1.0f;
    layout();
    layout_labels();
}

// PORT: the HUD's state taken and drawn.
void GameHud::set_state(const HudState& state) {
    state_ = state;
    apply_state();
}

void GameHud::layout() {
    if (!built_) return;
    const float across = static_cast<float>(width_ > 0 ? width_ : 1);
    const float down = static_cast<float>(height_ > 0 ? height_ : 1);

    // The interface's scaling rule: `CGameUI::convertToScreenScale(window, useX)` (Linux)
    // multiplies offsets by `XRATIO = width/1024` or `YRATIO = height/768`, and the menu passes
    // `useX = false`.
    const float native = sets_.empty() || sets_.front().native_height <= 0
                             ? kNativeHeight
                             : static_cast<float>(sets_.front().native_height);
    scale_ = (down / native) * compact_scale_;

    // One pass, parents first: `build()` pushes an object before its children.
    for (Node& node : nodes_) {
        const float parent_left =
            node.parent < 0 ? 0.0f : nodes_[static_cast<size_t>(node.parent)].left;
        const float parent_top =
            node.parent < 0 ? 0.0f : nodes_[static_cast<size_t>(node.parent)].top;
        const float parent_wide =
            node.parent < 0 ? across : nodes_[static_cast<size_t>(node.parent)].width * across;
        const float parent_tall =
            node.parent < 0 ? down : nodes_[static_cast<size_t>(node.parent)].height * down;

        // The rectangle, exactly as the `.uilayout` export writes it:
        //     min = {0, OFFSET}          max = {SCALE, OFFSET + EXTENT}
        // resolved against the parent's pixel size, with the offsets scaled.
        float left = node.off_x * scale_;
        float top = node.off_y * scale_;
        const float wide = node.scale_wide * parent_wide + node.wide * scale_;
        const float tall = node.scale_tall * parent_tall + node.tall * scale_;

        // ...then shifted by the anchor, which is CEGUI's own rule: an aligned
        // window keeps the size its area rect gave it and is moved so that it
        // sits centred in, or flush against, its parent.
        if (node.h_anchor == 1) left += (parent_wide - wide) * 0.5f;
        else if (node.h_anchor == 2) left += parent_wide - wide;
        if (node.v_anchor == 1) top += (parent_tall - tall) * 0.5f;
        else if (node.v_anchor == 2) top += parent_tall - tall;

        node.left = parent_left + left / across;
        node.top = parent_top + top / down;
        node.width = wide / across;
        node.height = tall / down;

        if (node.panel != nullptr) {
            node.panel->setPosition(node.left, node.top);
            node.panel->setDimensions(node.width, node.height);
        }
        for (Ogre::TextAreaOverlayElement* area : {node.drop, node.text}) {
            if (area == nullptr) continue;
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
    apply_state();
}

void GameHud::set_uv(Node& node, float u0, float v0, float u1, float v1) const {
    if (node.panel == nullptr) return;
    const Sprite* art = sprite(node.image);
    if (art == nullptr || !art->texture) return;
    const float tw = static_cast<float>(art->texture->getWidth());
    const float th = static_cast<float>(art->texture->getHeight());
    const float x = static_cast<float>(art->rect->x);
    const float y = static_cast<float>(art->rect->y);
    const float w = static_cast<float>(art->rect->width);
    const float h = static_cast<float>(art->rect->height);
    tl::render::set_uv(node.panel, (x + w * u0) / tw, (y + h * v0) / th, (x + w * u1) / tw,
                       (y + h * v1) / th);
}

void GameHud::set_node_image(Node& node, const std::string& name) {
    if (node.panel == nullptr || name.empty() || node.image == name) return;
    const Sprite* art = sprite(name);
    if (art == nullptr) return;
    node.image = name;
    // A window built with no picture (`SkillSlot1`/`SkillSlot2`) is `setTransparent(true)`, and a
    // transparent panel skips `_updateRenderQueue` whatever material it gets (as `Portrait` in
    // quest_dialog.cpp).
    node.panel->setTransparent(false);
    node.panel->setMaterial(art->material);
    const float tw = static_cast<float>(art->texture->getWidth());
    const float th = static_cast<float>(art->texture->getHeight());
    node.panel->setUV(static_cast<float>(art->rect->x) / tw, static_cast<float>(art->rect->y) / th,
                      static_cast<float>(art->rect->x + art->rect->width) / tw,
                      static_cast<float>(art->rect->y + art->rect->height) / th);
}

Ogre::MaterialPtr GameHud::grey_material(const Sprite& art) {
    const std::string name = art.material->getName() + "@grey";
    const auto found = grey_materials_.find(name);
    if (found != grey_materials_.end()) return found->second;
    Ogre::MaterialPtr material = build_menu_material(name, art.texture);
    // `ImageColours` set to `colour(0.6, 0.6, 0.6, 1.0)` on all four corners.
    material->getTechnique(0)->getPass(0)->getTextureUnitState(0)->setColourOperationEx(
        Ogre::LBX_MODULATE, Ogre::LBS_TEXTURE, Ogre::LBS_MANUAL, Ogre::ColourValue::White,
        Ogre::ColourValue(0.6f, 0.6f, 0.6f, 1.0f));
    grey_materials_.emplace(name, material);
    return material;
}

void GameHud::set_slot_image(Node& node, const std::string& name, bool grey) {
    if (node.panel == nullptr) return;
    const Sprite* art = name.empty() ? nullptr : sprite(name);
    if (art == nullptr) {
        // The slot is empty, or its picture is on no sheet: the bare window.
        if (!node.image.empty()) node.panel->setTransparent(true);
        node.image.clear();
        node.grey = false;
        return;
    }
    if (node.image == name && node.grey == grey) return;
    node.image = name;
    node.grey = grey;
    // Built `setTransparent(true)` for want of a picture; see set_node_image().
    node.panel->setTransparent(false);
    node.panel->setMaterial(grey ? grey_material(*art) : art->material);
    const float tw = static_cast<float>(art->texture->getWidth());
    const float th = static_cast<float>(art->texture->getHeight());
    node.panel->setUV(static_cast<float>(art->rect->x) / tw, static_cast<float>(art->rect->y) / th,
                      static_cast<float>(art->rect->x + art->rect->width) / tw,
                      static_cast<float>(art->rect->y + art->rect->height) / th);
}

void GameHud::apply_state() {
    if (!built_ || nodes_.empty()) return;

    auto clamp01 = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
    const float health = clamp01(state_.health);
    const float mana = clamp01(state_.mana);

    for (size_t index = 0; index < nodes_.size(); ++index) {
        Node& node = nodes_[index];
        bool on = !node.hidden;
        // Everything from `pethud_xenon.layout` at once, while the player has a pet.
        if (index >= pet_first_) on = on && state_.pet;

        // "Fleeing" only while the animal flees (`CCharacter::isPetNearDeath()`,
        // `tl::core::Pet::near_death()`); the stance tray lights the chosen button.
        if (node.widget == "FleeingText") on = on && state_.pet_fleeing;

        // The `+` over the bag and its glow, both authored visible and hidden
        // by `CGameUI::create` until there are
        // points to spend. Assignment, not `&&`: they are in `kRuntimeOnly`.
        if (node.widget == "AvailableSkillsIcon") on = state_.available_points;
        if (node.widget == "AvailableSkillsGlowIcon") on = state_.available_points_glow;

        // The target panel and everything under it (`target_first_`..`target_last_`). Assignment,
        // not `&&`: `TargetHealth` is authored `runtime only`.
        if (target_last_ > 0 && index >= target_first_ && index <= target_last_) {
            on = state_.target;
        }

        // The second banner is authored `runtime only` - it is blank most of
        // the time - and comes up whenever there is a line for it.
        if (node.widget == "SmallMessageText") on = !state_.sub_announce.empty();
        static const char* const kStanceWidgets[3] = {"PetAggressive", "PetDefensive",
                                                      "PetPassive"};
        for (int stance = 0; stance < 3; ++stance) {
            if (node.widget != kStanceWidgets[stance]) continue;
            set_node_image(node,
                           state_.pet_stance == stance ? node.normal_image : node.disabled_image);
        }

        // The bank of four that is up: `SkillSet1` or `SkillSet2`, which sit
        // at the same four places.
        if (node.skill_set >= 0) on = on && node.skill_set == state_.skill_set;

        // The ten hotbar slots, `SkillSlot1`..`SkillSlot10` (slot 0..9), dressed as
        // `CGameUI::updateSlots` does.
        if (const int slot = hotbar_slot_of(node.widget, "SkillSlot"); slot >= 0) {
            const tl::core::HotbarSlotView& view = state_.slots[static_cast<size_t>(slot)];
            set_slot_image(node, view.icon, view.dimmed);
        }

        if (node.text != nullptr) {
            std::string caption;
            if (node.widget == "LevelName") caption = state_.level_name;
            else if (node.widget == "LargeMessageText") caption = state_.announce;
            else if (node.widget == "SmallMessageText") caption = state_.sub_announce;
            else if (node.widget == "POTIONCOUNT") caption = state_.slots[0].text;
            else if (node.widget == "MANACOUNT") caption = state_.slots[1].text;
            else if (const int slot = hotbar_slot_of(node.widget, "SkillText"); slot >= 0) {
                caption = state_.slots[static_cast<size_t>(slot)].text;
            } else if (node.widget == "TargetName") caption = state_.target_name;
            else if (node.widget == "TargetDescription") caption = state_.target_description;
            else if (node.widget == "TargetLevel") {
                // "Level 1", not the layout's bare "1": the console prints the game's translatable
                // `Level` in front.
                caption = state_.target_level > 0 ? "Level " + std::to_string(state_.target_level)
                                                  : std::string();
            }
            // Any other runtime readout with nothing behind it is blank, not the layout's "999".
            const bool banner =
                node.widget == "LargeMessageText" || node.widget == "SmallMessageText";
            // `CGameUI::updateIngameUI`: the target's name in
            // `CEGUI::colour(1, 0.88, 0.24, 1)` for a champion, else white.
            if (node.widget == "TargetName" && node.text != nullptr) {
                Ogre::ColourValue tint = node.text->getColour();
                tint.r = 1.0f;
                tint.g = state_.target_champion ? 0.88f : 1.0f;
                tint.b = state_.target_champion ? 0.24f : 1.0f;
                set_colour(node.text, tint);
            }
            for (Ogre::TextAreaOverlayElement* area : {node.drop, node.text}) {
                if (area == nullptr) continue;
                set_caption(area, on ? caption : std::string());
                if (!banner) continue;
                // The two banners fade rather than vanish; everything else on
                // the HUD is drawn at whatever alpha its colour already has.
                Ogre::ColourValue tint = area->getColour();
                tint.a = node.widget == "LargeMessageText" ? state_.announce_alpha
                                                           : state_.sub_announce_alpha;
                set_colour(area, tint);
            }
        }

        if (node.panel == nullptr) continue;

        if (node.fill != kFillNone) {
            const float amount = node.widget == "PlayerHealthBarSub" ? health
                                 : node.widget == "PlayerManaBarSub" ? mana
                                 : node.widget == "ExperienceBar"    ? clamp01(state_.experience)
                                 : node.widget == "PetHealthBar"     ? clamp01(state_.pet_health)
                                 : node.widget == "TargetHealthBar"  ? clamp01(state_.target_health)
                                                                     : clamp01(state_.pet_mana);
            if (amount <= 0.0f) {
                on = false;
            } else if (node.fill == kFillDown) {
                // The liquid level falls: the top of the ball is cut away, the rest stays.
                set_position(node.panel, node.left, node.top + node.height * (1.0f - amount));
                set_dimensions(node.panel, node.width, node.height * amount);
                set_uv(node, 0.0f, 1.0f - amount, 1.0f, 1.0f);
            } else {
                set_position(node.panel, node.left, node.top);
                set_dimensions(node.panel, node.width * amount, node.height);
                set_uv(node, 0.0f, 0.0f, amount, 1.0f);
            }
        }

        if (on) node.panel->show();
        else node.panel->hide();
    }
}

// ---------------------------------------------------------------------------
// The floor labels
// ---------------------------------------------------------------------------

namespace {

/// `ceguiWidget/StaticShared`'s `frame` section, which an `ItemTextButton`
/// draws in both states: the nine `widgets1` pieces in the look's order
/// (top-left, top-right, bottom-left, bottom-right, left, right, top, bottom,
/// middle) over the window moved by (-24, -9) and grown by (48, 18).
constexpr const char* kLabelFrame[9] = {
    "tooltiptopleft",     "tooltiptopright",   "tooltipbottomleft",
    "tooltipbottomright", "tooltipleftedge",   "tooltiprightedge",
    "tooltiptopedge",     "tooltipbottomedge", "tooltipmiddle"};
constexpr float kLabelFrameLeft = -24.0f;
constexpr float kLabelFrameTop = -9.0f;
constexpr float kLabelFrameGrowWide = 48.0f;
constexpr float kLabelFrameGrowTall = 18.0f;
/// The look's `Button` section, drawn in `Enabled` only: `NormalImage`
/// (`set:widgets1 image:a_button`) at (-20, -1), 21 x 21.
constexpr const char* kLabelButton = "a_button";
constexpr float kLabelButtonLeft = -20.0f;
constexpr float kLabelButtonTop = -1.0f;
constexpr float kLabelButtonSize = 21.0f;
/// A look's `AbsoluteDim`, and every pixel figure, is a
/// pixel of the console's 720-line output.
constexpr float kConsoleLines = 720.0f;

}  // namespace

void GameHud::build_labels() {
    auto& overlays = Ogre::OverlayManager::getSingleton();
    // The labels are windows of the in-game sheet, which the HUD's own windows
    // cover: under the HUD's 470/472, the text over its panels for the reason
    // `create` gives.
    label_overlay_ = overlays.create("tl/hud/labels");
    label_overlay_->setZOrder(465);
    label_text_overlay_ = overlays.create("tl/hud/labeltext");
    label_text_overlay_->setZOrder(467);
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
    label_root_ = root("tl/hud/labelroot", label_overlay_);
    label_text_root_ = root("tl/hud/labeltextroot", label_text_overlay_);

    labels_.resize(kMaxGroundLabels);
    for (size_t i = 0; i < labels_.size(); ++i) {
        LabelSlot& slot = labels_[i];
        const std::string stem = "tl/hud/label/" + std::to_string(i);
        auto panel = [&](const std::string& name, const Sprite* art) {
            auto* p = static_cast<Ogre::PanelOverlayElement*>(
                overlays.createOverlayElement("Panel", name));
            p->setMetricsMode(Ogre::GMM_RELATIVE);
            if (art != nullptr && art->texture) {
                p->setMaterial(art->material);
                const float tw = static_cast<float>(art->texture->getWidth());
                const float th = static_cast<float>(art->texture->getHeight());
                const float x = static_cast<float>(art->rect->x);
                const float y = static_cast<float>(art->rect->y);
                p->setUV(x / tw, y / th, (x + static_cast<float>(art->rect->width)) / tw,
                         (y + static_cast<float>(art->rect->height)) / th);
            } else {
                p->setTransparent(true);
            }
            p->hide();
            label_root_->addChild(p);
            return p;
        };
        for (size_t piece = 0; piece < 9; ++piece) {
            slot.frame[piece] =
                panel(stem + "/frame/" + std::to_string(piece), sprite(kLabelFrame[piece]));
        }
        slot.button = panel(stem + "/button", sprite(kLabelButton));
        auto* text = static_cast<Ogre::TextAreaOverlayElement*>(
            overlays.createOverlayElement("TextArea", stem + "/text"));
        text->setMetricsMode(Ogre::GMM_RELATIVE);
        text->setFontName(kFontName, Ogre::RGN_INTERNAL);
        text->setAlignment(Ogre::TextAreaOverlayElement::Center);
        text->hide();
        label_text_root_->addChild(text);
        slot.text = text;
    }
    // One render priority per label, in order, so a later label - the
    // highlighted one is given last - covers an earlier one, and a button its
    // own frame. See `create` for why the order would otherwise be the passes'.
    const Ogre::ushort panel_top = static_cast<Ogre::ushort>(label_root_->getZOrder() + 99);
    Ogre::ushort z = static_cast<Ogre::ushort>(label_root_->getZOrder() + 1);
    for (LabelSlot& slot : labels_) {
        for (Ogre::PanelOverlayElement* piece : slot.frame) piece->_notifyZOrder(z);
        if (z < panel_top) ++z;
        slot.button->_notifyZOrder(z);
        if (z < panel_top) ++z;
    }
    const Ogre::ushort text_top = static_cast<Ogre::ushort>(label_text_root_->getZOrder() + 99);
    Ogre::ushort text_z = static_cast<Ogre::ushort>(label_text_root_->getZOrder() + 1);
    for (LabelSlot& slot : labels_) {
        slot.text->_notifyZOrder(text_z);
        if (text_z < text_top) ++text_z;
    }
}

void GameHud::destroy_labels() {
    if (labels_.empty() && label_overlay_ == nullptr && label_text_overlay_ == nullptr) return;
    auto& overlays = Ogre::OverlayManager::getSingleton();
    for (LabelSlot& slot : labels_) {
        for (Ogre::PanelOverlayElement* piece : slot.frame) {
            if (piece == nullptr) continue;
            if (label_root_ != nullptr) label_root_->removeChild(piece->getName());
            overlays.destroyOverlayElement(piece);
        }
        if (slot.button != nullptr) {
            if (label_root_ != nullptr) label_root_->removeChild(slot.button->getName());
            overlays.destroyOverlayElement(slot.button);
        }
        if (slot.text != nullptr) {
            if (label_text_root_ != nullptr) label_text_root_->removeChild(slot.text->getName());
            overlays.destroyOverlayElement(slot.text);
        }
    }
    labels_.clear();
    ground_labels_.clear();
    if (label_root_ != nullptr && label_overlay_ != nullptr) label_overlay_->remove2D(label_root_);
    if (label_text_root_ != nullptr && label_text_overlay_ != nullptr) {
        label_text_overlay_->remove2D(label_text_root_);
    }
    if (label_root_ != nullptr) overlays.destroyOverlayElement(label_root_);
    if (label_text_root_ != nullptr) overlays.destroyOverlayElement(label_text_root_);
    if (label_overlay_ != nullptr) overlays.destroy(label_overlay_);
    if (label_text_overlay_ != nullptr) overlays.destroy(label_text_overlay_);
    label_root_ = nullptr;
    label_text_root_ = nullptr;
    label_overlay_ = nullptr;
    label_text_overlay_ = nullptr;
}

void GameHud::set_ground_labels(const std::vector<GroundLabel>& labels) {
    ground_labels_ = labels;
    layout_labels();
}

void GameHud::layout_labels() {
    if (labels_.empty()) return;
    const float across = static_cast<float>(width_ > 0 ? width_ : 1);
    const float down = static_cast<float>(height_ > 0 ? height_ : 1);
    // Everything below is in the console's pixels, turned into the screen's at
    // the end: `pixel` screen pixels to one of them.
    const float pixel = down / kConsoleLines;
    const float screen_wide = across / pixel;
    // The setting multiplies by, the console's `YRATIO`: its
    // 720 lines over the 768 the interface was authored for.
    const float ratio = kConsoleLines / kNativeHeight;
    // `Serif`, 11 points, autoscaled by the same ratio. DIVERGENCE: OGRE's
    // font has no FreeType ascender and descender, so the glyph cell stands in
    // for the line height (font `+0x1ec` minus `+0x1f0`).
    const float line = kFontSize * points_to_char_pixels() * ratio;
    const float char_height = line * pixel / down;
    const Ogre::FontPtr font = labels_.front().text->getFont();
    const Sprite* corner = sprite(kLabelFrame[0]);
    const float cw = corner != nullptr ? static_cast<float>(corner->rect->width) : 0.0f;
    const float ch = corner != nullptr ? static_cast<float>(corner->rect->height) : 0.0f;

    auto put = [&](Ogre::PanelOverlayElement* p, float left, float top, float wide, float tall) {
        set_position(p, left * pixel / across, top * pixel / down);
        set_dimensions(p, std::max(wide, 0.0f) * pixel / across,
                       std::max(tall, 0.0f) * pixel / down);
        p->show();
    };

    const size_t shown = std::min(ground_labels_.size(), labels_.size());
    for (size_t i = 0; i < labels_.size(); ++i) {
        LabelSlot& slot = labels_[i];
        if (i >= shown) {
            for (Ogre::PanelOverlayElement* piece : slot.frame) piece->hide();
            slot.button->hide();
            slot.text->hide();
            continue;
        }
        const GroundLabel& label = ground_labels_[i];
        // `Font::getTextExtent(text, 1.0)`, in the console's pixels.
        const float extent =
            measure_text(font, label.text, char_height, across, down) * across / pixel;
        const tl::core::ItemLabelRect size = tl::core::item_label_size(extent, line, ratio);
        const tl::core::ItemLabelRect at = tl::core::place_item_label(
            size, label.at_x / pixel, label.at_y / pixel, screen_wide, kConsoleLines);

        // The frame: corners at their own size, edges stretched between them,
        // the middle filling what is left.
        const float left = at.x + kLabelFrameLeft;
        const float top = at.y + kLabelFrameTop;
        const float wide = at.width + kLabelFrameGrowWide;
        const float tall = at.height + kLabelFrameGrowTall;
        const float right = left + wide - cw;
        const float bottom = top + tall - ch;
        put(slot.frame[0], left, top, cw, ch);
        put(slot.frame[1], right, top, cw, ch);
        put(slot.frame[2], left, bottom, cw, ch);
        put(slot.frame[3], right, bottom, cw, ch);
        put(slot.frame[4], left, top + ch, cw, tall - 2.0f * ch);
        put(slot.frame[5], right, top + ch, cw, tall - 2.0f * ch);
        put(slot.frame[6], left + cw, top, wide - 2.0f * cw, ch);
        put(slot.frame[7], left + cw, bottom, wide - 2.0f * cw, ch);
        put(slot.frame[8], left + cw, top + ch, wide - 2.0f * cw, tall - 2.0f * ch);

        if (label.enabled) {
            put(slot.button, at.x + kLabelButtonLeft, at.y + kLabelButtonTop, kLabelButtonSize,
                kLabelButtonSize);
        } else {
            slot.button->hide();
        }

        // `VertFormatting` and `HorzTextFormatting` both `CentreAligned`.
        set_char_height(slot.text, char_height);
        set_caption(slot.text, label.text);
        set_colour(slot.text, label.colour);
        set_position(slot.text, (at.x + at.width * 0.5f) * pixel / across,
                     (at.y + (at.height - line) * 0.5f) * pixel / down);
        slot.text->show();
    }
}

}  // namespace tl::render
