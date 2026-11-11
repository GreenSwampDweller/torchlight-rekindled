// The character creation screen. Two screens: the console's own
// (`media/UI/charactercreate.uilayout`, Xbox data only), placed from that layout and driven by
// `CNewGameMenu`'s pad logic in `core/new_game_menu.h`; and the PC's
// (`media/UI/charactercreate.layout`) for a PC-only install. Authored offsets are pixels against
// 1024x768, scaled by `height / 768` in both axes (`MainMenu::layout()`).
#include "render/menu.h"

#include "core/log.h"
#include "render/menu_chrome.h"
#include "render/overlay_set.h"
#include "render/text_layout.h"

#include <OgreFont.h>
#include <OgreFontManager.h>
#include <OgreOverlay.h>
#include <OgreOverlayContainer.h>
#include <OgreOverlayManager.h>
#include <OgrePanelOverlayElement.h>
#include <OgreTextAreaOverlayElement.h>
#include <OgreTextureManager.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace tl::render {
namespace {

/// One window's `UnifiedPosition` and `UnifiedSize`, as the layout writes them:
/// `{{rel,off},{rel,off}}` for the position and a pixel pair for the size.
struct Authored {
    float rel_x, off_x, rel_y, off_y, wide, tall;
};

// Quoted from charactercreate.layout, window by window.
constexpr Authored kHeader{0.5f, -300.0f, 0.0f, 110.0f, 600.0f, 32.0f};
constexpr Authored kClassName{0.5f, -300.0f, 1.0f, -175.0f, 600.0f, 60.0f};
constexpr Authored kDescription{0.5f, -150.0f, 1.0f, -140.0f, 300.0f, 64.0f};
constexpr Authored kBack{0.5f, -400.0f, 1.0f, -64.0f, 246.0f, 44.0f};
constexpr Authored kOk{0.5f, 154.0f, 1.0f, -64.0f, 246.0f, 44.0f};

/// `PortraitBox` sits at `{{0,30},{.5,-200}}` and holds the three radio buttons
/// at `(5,0)`, `(5,135)` and `(5,270)`, each 128x128. The offsets are folded
/// together here because nothing else needs the box.
constexpr float kIconLeft = 30.0f + 5.0f;
constexpr float kIconTop = -200.0f;
constexpr float kIconPitch = 135.0f;
constexpr float kIconSize = 128.0f;

/// `PetBox` at `{{1,-170},{.5,-20}}`, its `PetTitle` at `(0,5)` and the `Dog`
/// and `Cat` buttons at `(-5,45)` and `(-5,75)`, 152x28 each.
constexpr float kPetLeft = -170.0f;
constexpr float kPetTop = -20.0f;
constexpr Authored kPetTitle{1.0f, kPetLeft, 0.5f, kPetTop + 5.0f, 140.0f, 24.0f};
constexpr Authored kDog{1.0f, kPetLeft - 5.0f, 0.5f, kPetTop + 45.0f, 152.0f, 28.0f};
constexpr Authored kCat{1.0f, kPetLeft - 5.0f, 0.5f, kPetTop + 75.0f, 152.0f, 28.0f};

/// Which button in `buttons_` is which on the PC screen. The order they are
/// added in.
enum ButtonIndex { kBackButton = 0, kOkButton, kDogButton, kCatButton, kButtonCount };

/// The console screen's own files.
constexpr const char* kConsoleCreateFile = "media/UI/charactercreate.uilayout";
constexpr const char* kWidgetsImageset = "media/UI/icons2/widgets.imageset";
constexpr const char* kWidgets1Imageset = "media/UI/icons2/widgets1.imageset";
constexpr const char* kWidgets2Imageset = "media/UI/icons2/widgets2.imageset";

/// Where the console's left-hand panel sits. As on the main menu, the panel's container (`i_432`
/// here, `i_402` there) carries a -0.45 horizontal scale that cannot be applied as authored, and
/// nothing in the Xbox image moves it: both panels land about 66 authored units from the left (a
/// title-safe inset, measured). Only the container's horizontal offset is replaced.
constexpr const char* kConsolePanelWindow = "i_432";
constexpr float kConsolePanelLeft = 77.0f;

/// `Size` in each of the five `.font` files under `media/UI`, all of them
/// `BRLNSDB.TTF` autoscaled against 1024x768. A window's `Font` names one.
float font_points(const std::string& font) {
    if (font == "SerifSmall") return 8.0f;
    if (font == "Serif14") return 14.0f;
    if (font == "SerifBig") return 16.0f;
    if (font == "SerifHuge") return 32.0f;
    return 11.0f;  // `Serif`, which is also every look's default
}

/// One `{{scale,offset},...}` CEGUI rectangle, both halves of each of its four
/// numbers.
struct Area {
    float sx = 0.0f, x = 0.0f, sy = 0.0f, y = 0.0f, sx2 = 0.0f, x2 = 0.0f, sy2 = 0.0f, y2 = 0.0f;
    float wide() const { return x2 - x; }
    float tall() const { return y2 - y; }
    /// A window that spans its parent: a full-screen container.
    bool fills_parent() const { return sx2 >= 1.0f && sy2 >= 1.0f; }
};

bool parse_area(const std::string& text, Area& out) {
    // Four `{scale,offset}` pairs inside one outer pair of braces.
    float values[8];
    size_t at = 0;
    for (int i = 0; i < 4; ++i) {
        size_t open = text.find('{', at);
        while (open != std::string::npos && open + 1 < text.size() && text[open + 1] == '{') {
            ++open;
        }
        if (open == std::string::npos) return false;
        const size_t close = text.find('}', open);
        if (close == std::string::npos) return false;
        const std::string pair = text.substr(open + 1, close - open - 1);
        const size_t comma = pair.find(',');
        if (comma == std::string::npos) return false;
        values[i * 2] = std::strtof(pair.substr(0, comma).c_str(), nullptr);
        values[i * 2 + 1] = std::strtof(pair.substr(comma + 1).c_str(), nullptr);
        at = close + 1;
    }
    out.sx = values[0];
    out.x = values[1];
    out.sy = values[2];
    out.y = values[3];
    out.sx2 = values[4];
    out.x2 = values[5];
    out.sy2 = values[6];
    out.y2 = values[7];
    return true;
}

bool window_area(const tl::formats::GuiWindow& window, Area& out) {
    return parse_area(window.property("UnifiedAreaRect", std::string()), out);
}

/// The chain of windows from the layout's root down to `name`, inclusive.
bool path_to(const tl::formats::GuiWindow& from, const std::string& name,
             std::vector<const tl::formats::GuiWindow*>& path) {
    path.push_back(&from);
    if (from.name == name) return true;
    for (const tl::formats::GuiWindow& child : from.children) {
        if (path_to(child, name, path)) return true;
    }
    path.pop_back();
    return false;
}

/// Section `itemframe`, which `ceguiWidget/ItemTooltip` draws: the bordered box
/// round the classes and round each name. Its nine images are spread over two
/// imagesets, exactly as the look names them.
constexpr FrameLook kItemFrame{
    {"itemtooltiptopleft", "itemtooltiptopright", "itemtooltipbottomleft", "itemtooltipbottomright",
     "itemtooltipleftedge", "itemtooltiprightedge", "itemtooltiptopedge", "itemtooltipbottomedge",
     "itemtooltipmiddle"},
    {"widgets1", "widgets1", "widgets2", "widgets2", "widgets2", "widgets1", "widgets1", "widgets2",
     "widgets2"},
    -26.0f,
    -13.0f,
    52.0f,
    26.0f};

/// A window's `Alpha` while it is not the focused name box
/// (sets "0.5"; sets "1.0" on focus).
constexpr float kUnfocusedAlpha = 0.5f;

/// The UTF-8 character at `at` in `text`, and `at` moved past it.
std::string next_utf8_char(const std::string& text, size_t& at) {
    const unsigned char lead = static_cast<unsigned char>(text[at]);
    const size_t width = lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : lead >= 0xc0 ? 2 : 1;
    std::string piece = text.substr(at, width);
    at += width;
    return piece;
}

size_t utf8_chars(const std::string& text) {
    size_t chars = 0;
    for (char c : text) chars += (c & 0xc0) != 0x80 ? 1 : 0;
    return chars;
}

void erase_last_utf8_char(std::string& text) {
    while (!text.empty() && (text.back() & 0xc0) == 0x80) text.pop_back();
    if (!text.empty()) text.pop_back();
}

}  // namespace

CharacterCreate::~CharacterCreate() { destroy(); }

// PORT: the creation screen's chosen class set (the renderer's interface).
void CharacterCreate::select(const std::string& name) {
    for (size_t i = 0; i < choices_.size(); ++i) {
        if (choices_[i].name != name) continue;
        if (selected_ != static_cast<int>(i)) selection_changed_ = true;
        selected_ = static_cast<int>(i);
        apply_look();
        return;
    }
}

const std::string& CharacterCreate::chosen() const {
    static const std::string kNone;
    if (selected_ < 0 || static_cast<size_t>(selected_) >= choices_.size()) return kNone;
    return choices_[static_cast<size_t>(selected_)].name;
}

// PORT: the renderer's interface: whether the pet changed since it last asked.
bool CharacterCreate::take_pet_changed() {
    const bool was = pet_changed_;
    pet_changed_ = false;
    return was;
}

// INVENTED: the pets the creation screen offers, `Dog` and `Cat` and, on the
// console's screen, `Chakawary`; the console menu's own list is not transcribed.
void CharacterCreate::select_pet(const std::string& name) {
    const bool offered = name == "Dog" || name == "Cat" || (console_mode_ && name == "Chakawary");
    if (!offered || name == pet_) return;
    pet_ = name;
    pet_changed_ = true;
    apply_look();
}

// PORT: the renderer's interface: whether the class changed since it last asked.
bool CharacterCreate::take_selection_changed() {
    const bool was = selection_changed_;
    selection_changed_ = false;
    return was;
}

Ogre::PanelOverlayElement* CharacterCreate::make_panel(const std::string& name,
                                                       Ogre::Overlay* overlay,
                                                       const Ogre::MaterialPtr& material) {
    auto& overlays = Ogre::OverlayManager::getSingleton();
    auto* panel =
        static_cast<Ogre::PanelOverlayElement*>(overlays.createOverlayElement("Panel", name));
    panel->setMetricsMode(Ogre::GMM_RELATIVE);
    panel->setMaterial(material);
    overlay->add2D(panel);
    return panel;
}

Ogre::TextAreaOverlayElement* CharacterCreate::make_text(const std::string& name, float points,
                                                         const Ogre::ColourValue& colour,
                                                         bool centred) {
    auto& overlays = Ogre::OverlayManager::getSingleton();
    auto* text =
        static_cast<Ogre::TextAreaOverlayElement*>(overlays.createOverlayElement("TextArea", name));
    text->setMetricsMode(Ogre::GMM_RELATIVE);
    text->setFontName(kFontName, Ogre::RGN_INTERNAL);
    text->setColour(colour);
    set_char_height(text, points * points_to_char_pixels() / kNativeHeight);
    if (centred) text->setAlignment(Ogre::TextAreaOverlayElement::Center);
    // Inside the container, not the overlay: `Overlay::add2D` takes an
    // `OverlayContainer` and a `TextArea` is not one. A child's position is
    // *added* to its parent's derived position rather than scaled by it, and
    // the container sits at the origin at full size, so these stay absolute.
    text_root_->addChild(text);
    return text;
}

void CharacterCreate::set_uv(Ogre::PanelOverlayElement* panel, const tl::formats::Imageset& set,
                             const Ogre::TexturePtr& texture, const std::string& image) const {
    if (panel == nullptr || !texture) return;
    const tl::formats::ImagesetImage* found = set.find(image);
    if (found == nullptr) return;
    const float tw = static_cast<float>(texture->getWidth());
    const float th = static_cast<float>(texture->getHeight());
    panel->setUV(static_cast<float>(found->x) / tw, static_cast<float>(found->y) / th,
                 static_cast<float>(found->x + found->width) / tw,
                 static_cast<float>(found->y + found->height) / th);
}

bool CharacterCreate::load_console_screen(const std::string& group) {
    std::vector<uint8_t> bytes;
    if (!read_menu_resource(kConsoleCreateFile, group, bytes)) return false;
    console_ = tl::formats::parse_gui_layout(bytes.data(), bytes.size());
    if (!console_.ok) {
        TL_LOG_WARN("character creation: %s: %s", kConsoleCreateFile, console_.error.c_str());
        return false;
    }
    const struct {
        const char* file;
        MenuArtSet* art;
    } sets[] = {{kWidgetsImageset, &widgets_art_},
                {kWidgets1Imageset, &widgets1_art_},
                {kWidgets2Imageset, &widgets2_art_}};
    for (const auto& want : sets) {
        if (!read_menu_resource(want.file, group, bytes)) return false;
        want.art->set = tl::formats::parse_imageset(bytes.data(), bytes.size());
        if (!want.art->set.ok) return false;
        try {
            want.art->texture =
                Ogre::TextureManager::getSingleton().load(want.art->set.image_file, group);
            want.art->material =
                build_menu_material("tl/create/" + want.art->set.name, want.art->texture);
            want.art->dim_material = build_menu_material("tl/create/" + want.art->set.name + "/dim",
                                                         want.art->texture, kUnfocusedAlpha);
        } catch (const Ogre::Exception&) {
            return false;
        }
        want.art->ok = static_cast<bool>(want.art->material);
        if (!want.art->ok) return false;
    }
    TL_LOG_INFO("character creation: the console screen, from %s", kConsoleCreateFile);
    return true;
}

MenuArtSet* CharacterCreate::art_named(const std::string& set) {
    if (set == "widgets") return &widgets_art_;
    if (set == "widgets1") return &widgets1_art_;
    if (set == "widgets2") return &widgets2_art_;
    if (logo_art_.ok && set == logo_art_.set.name) return &logo_art_;
    if (gui_art_.ok && set == gui_art_.set.name) return &gui_art_;
    return nullptr;
}

// PORT: the set an image of the console screen is drawn from.
MenuArtSet* CharacterCreate::art_for(const std::string& set, const std::string& image) {
    MenuArtSet* named = art_named(set);
    if (named != nullptr && named->ok && named->set.find(image) != nullptr) return named;
    for (MenuArtSet* art : {&widgets_art_, &widgets1_art_, &widgets2_art_, &gui_art_, &logo_art_}) {
        if (art->ok && art->set.find(image) != nullptr) return art;
    }
    return nullptr;
}

// PORT: the PC's `Logo` and `GuiLook` as two more sets of the console screen, on a PC pak (the
// Xbox's carries neither): the port's pack draws the console screen and leaves the logo bars,
// the buttons, the class portraits and the panel looks to them.
bool CharacterCreate::load_pc_art(const std::string& group) {
    if (console_game_data() || !load_pc_imagesets(group)) return false;
    try {
        if (!load_pc_textures(group)) return false;
        const struct {
            const tl::formats::Imageset* set;
            const Ogre::TexturePtr* texture;
            MenuArtSet* art;
        } pcs[] = {{&logo_set_, &logo_texture_, &logo_art_}, {&gui_set_, &gui_texture_, &gui_art_}};
        for (const auto& pc : pcs) {
            pc.art->set = *pc.set;
            pc.art->texture = *pc.texture;
            pc.art->material = build_menu_material("tl/create/pc/" + pc.set->name, *pc.texture);
            pc.art->dim_material = build_menu_material("tl/create/pc/" + pc.set->name + "/dim",
                                                       *pc.texture, kUnfocusedAlpha);
            pc.art->ok = static_cast<bool>(pc.art->material);
        }
    } catch (const Ogre::Exception&) {
        return false;
    }
    return logo_art_.ok && gui_art_.ok;
}

namespace {

/// Where a window's parent puts it, in authored units from the screen's left
/// and top: the sum of its ancestors' offsets. `bar` comes back true for a
/// window inside the bottom border strip (`i_421`), whose offsets are then
/// from that strip's own corner rather than the screen's.
struct Origin {
    float x = 0.0f, y = 0.0f;
    /// The parent's size, and whether the parent is the whole screen.
    float parent_wide = 0.0f, parent_tall = 0.0f;
    bool parent_is_screen = true;
    bool bar = false;
};

bool origin_of(const tl::formats::GuiLayout& layout, const std::string& name, Origin& out) {
    std::vector<const tl::formats::GuiWindow*> path;
    if (!path_to(layout.root, name, path)) return false;
    out = Origin{};
    for (size_t i = 0; i + 1 < path.size(); ++i) {
        const tl::formats::GuiWindow& ancestor = *path[i];
        Area area;
        if (!window_area(ancestor, area)) continue;
        if (ancestor.name == "i_421") {
            // The bottom border strip. Its children are placed from its own
            // corner, which the layout pass anchors to the bottom of the screen.
            out = Origin{};
            out.bar = true;
            out.parent_wide = area.wide();
            out.parent_tall = area.tall();
            out.parent_is_screen = false;
            continue;
        }
        if (area.fills_parent()) continue;
        out.x += ancestor.name == kConsolePanelWindow ? kConsolePanelLeft : area.x;
        out.y += area.y;
        out.parent_wide = area.wide();
        out.parent_tall = area.tall();
        out.parent_is_screen = false;
    }
    return true;
}

}  // namespace

Ogre::TextAreaOverlayElement* CharacterCreate::console_label_text(const char* window_name) {
    for (ConsoleLabel& label : console_labels_) {
        if (label.window == window_name) return label.text;
    }
    return nullptr;
}

void CharacterCreate::add_console_label(const char* window_name, float dx, float dy,
                                        float parent_wide) {
    const tl::formats::GuiWindow* window = console_.find(window_name);
    if (window == nullptr) return;
    Origin origin;
    if (!origin_of(console_, window_name, origin)) return;
    Area area;
    if (!window_area(*window, area)) return;

    ConsoleLabel label;
    label.window = window_name;
    label.in_bottom_bar = origin.bar;
    label.x = origin.x + dx + area.x;
    label.y = origin.y + dy + area.y;
    label.wide = area.wide();
    label.tall = area.tall();

    // `HorizontalAlignment` and `VerticalAlignment` place the *box* in its
    // parent; `HorzTextFormatting` and `VertFormatting` place the text in the
    // box.
    const float parent = parent_wide >= 0.0f ? parent_wide : origin.parent_wide;
    if (window->property("HorizontalAlignment", std::string()) == "Centre") {
        if (origin.parent_is_screen && parent_wide < 0.0f) {
            label.centred_on_screen = true;
            label.x = area.x;
        } else {
            label.x += (parent - label.wide) * 0.5f;
        }
    }
    if (window->property("VerticalAlignment", std::string()) == "Centre") {
        label.y += (origin.parent_tall - label.tall) * 0.5f;
    }
    const std::string horz = window->property("HorzTextFormatting", std::string());
    label.text_centred = horz == "CentreAligned" || horz == "WordWrapCentreAligned";
    label.wraps = horz.rfind("WordWrap", 0) == 0;
    label.v_centred = window->property("VertFormatting", std::string()) == "CentreAligned";
    label.points = font_points(window->property("Font", std::string()));
    float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
    if (tl::formats::parse_colour(window->property("TextColour", std::string()), r, g, b, a)) {
        label.colour = Ogre::ColourValue(r, g, b, a);
    }

    // The look's shadow: `StaticTextOutline` draws the text four times in
    // `DropTextColour` (black) at (+1,+1), (-1,-1), (+1,-1) and (-1,+1);
    // `StaticText` once at (+2,+2).
    if (window->type == "ceguiWidget/StaticTextOutline") {
        label.drop_offsets = {{1.0f, 1.0f}, {-1.0f, -1.0f}, {1.0f, -1.0f}, {-1.0f, 1.0f}};
    } else if (window->type == "ceguiWidget/StaticText") {
        label.drop_offsets = {{2.0f, 2.0f}};
    }

    // Drawn in name order within the container, so the drops sort first.
    const std::string name = "tl/create/console/text" + std::to_string(console_labels_.size());
    for (size_t i = 0; i < label.drop_offsets.size(); ++i) {
        label.drops.push_back(make_text(name + "/a" + std::to_string(i), label.points,
                                        Ogre::ColourValue::Black, label.text_centred));
    }
    label.text = make_text(name + "/b", label.points, label.colour, label.text_centred);
    const std::string caption = window->property("Text", std::string());
    label.text->setCaption(caption);
    for (Ogre::TextAreaOverlayElement* drop : label.drops) drop->setCaption(caption);
    console_labels_.push_back(std::move(label));
}

void CharacterCreate::add_console_widget(const char* window_name, std::vector<Widget>& into,
                                         float dx, float dy) {
    const tl::formats::GuiWindow* window = console_.find(window_name);
    if (window == nullptr) return;
    Origin origin;
    Area area;
    if (!origin_of(console_, window_name, origin) || !window_area(*window, area)) return;

    // An `ImageButton` names three images and a `StaticImage` one, each as
    // `set:<imageset> image:<name>`.
    std::string normal_set, normal, hover_set, hover;
    const bool button = window->has_property("NormalImage");
    if (!tl::formats::parse_image_reference(
            window->property(button ? "NormalImage" : "Image", std::string()), normal_set,
            normal)) {
        return;
    }
    if (!button || !tl::formats::parse_image_reference(
                       window->property("HoverImage", std::string()), hover_set, hover)) {
        hover_set = normal_set;
        hover = normal;
    }
    MenuArtSet* normal_art = art_for(normal_set, normal);
    MenuArtSet* hover_art = art_for(hover_set, hover);
    if (normal_art == nullptr || !normal_art->ok || hover_art == nullptr || !hover_art->ok) return;

    Widget widget;
    widget.window = window_name;
    widget.off_x = origin.x + dx + area.x;
    widget.off_y = origin.y + dy + area.y;
    widget.wide = area.wide();
    widget.tall = area.tall();
    widget.normal = normal;
    widget.hover = hover;
    widget.selected = normal;
    widget.normal_art = normal_art;
    widget.hover_art = hover_art;
    widget.set = &normal_art->set;
    widget.texture = normal_art->texture;
    const std::string name =
        "tl/create/console/" + std::string(window_name) + "/" + std::to_string(into.size());
    // The pets' boxes go under everything else, with the frames; the rest
    // over them.
    widget.panel =
        make_panel(name, &into == &console_frame_ ? frame_ : widgets_, normal_art->material);
    if (origin.bar) {
        // Placed from the bottom strip's corner by `layout_console()`.
        console_bar_.push_back(widget);
        return;
    }
    into.push_back(widget);
}

void CharacterCreate::take_choices(const std::string& group, std::vector<Choice> choices) {
    group_ = group;
    choices_ = std::move(choices);
    std::vector<std::string> mod_classes;
    for (const Choice& choice : choices_) {
        if (!choice.portrait_imageset.empty()) mod_classes.push_back(choice.name);
    }
    grid_ = tl::core::new_game_grid(mod_classes);
}

void CharacterCreate::build_class_row() {
    // The three classes, **built in `choices_` order**, which is the order the
    // rest of this class indexes them in. Each is found by its window name,
    // which is the class's own.
    for (const Choice& choice : choices_) {
        add_console_widget(choice.name.c_str(), icons_, 0.0f, 0.0f);
    }
    const size_t built_in = icons_.size();
    for (const Choice& choice : choices_) add_mod_portrait(choice);
    fit_class_row(built_in);
}

// `CNewGameMenu`'s step on this screen's grid.
void CharacterCreate::step_focus(int dx, int dy) {
    focus_ = tl::core::step_new_game_focus(grid_, focus_, dx, dy, [this](const std::string& name) {
        return console_window_visible(name);
    });
}

// DIVERGENCE: a mod's class beside the console's three, drawn from the images its own
// `charactercreate.layout` gives its radio button, in a cell made like the three's.
void CharacterCreate::add_mod_portrait(const Choice& choice) {
    if (choice.portrait_imageset.empty() || icons_.empty()) return;
    std::vector<uint8_t> bytes;
    if (!read_menu_resource(choice.portrait_imageset, group_, bytes)) return;
    MenuArtSet art;
    art.set = tl::formats::parse_imageset(bytes.data(), bytes.size());
    if (!art.set.ok) {
        TL_LOG_WARN("character creation: %s: %s", choice.portrait_imageset.c_str(),
                    art.set.error.c_str());
        return;
    }
    try {
        art.texture = Ogre::TextureManager::getSingleton().load(art.set.image_file, group_);
        art.material = build_menu_material("tl/create/mod/" + art.set.name, art.texture);
    } catch (const Ogre::Exception& e) {
        TL_LOG_WARN("character creation: no portrait for %s: %s", choice.name.c_str(),
                    e.getDescription().c_str());
        return;
    }
    art.ok = static_cast<bool>(art.material);
    if (!art.ok) return;
    portrait_art_.push_back(std::move(art));
    MenuArtSet* stored = &portrait_art_.back();
    Widget widget = icons_.back();
    widget.window = choice.name;
    widget.normal = choice.portrait[0];
    widget.hover = choice.portrait[1];
    widget.selected = choice.portrait[2];
    widget.normal_art = stored;
    widget.hover_art = stored;
    widget.set = &stored->set;
    widget.texture = stored->texture;
    widget.panel = make_panel("tl/create/console/mod/" + std::to_string(icons_.size()), widgets_,
                              stored->material);
    icons_.push_back(widget);
}

// A on a mod's class, which the layout has no window for: `GUISELECT1`, as its own
// `charactercreate.layout` authors it.
void CharacterCreate::choose_mod_class(const std::string& name) {
    for (const Choice& choice : choices_) {
        if (choice.name != name || choice.portrait_imageset.empty()) continue;
        select(name);
        TL_LOG_INFO("character creation: class %s (a mod's)", name.c_str());
    }
}

// DIVERGENCE: with a mod's class the row holds more than the box's three cells; all of them
// share the three's width, in the grid's order (the three as the layout places them, then
// the mods').
void CharacterCreate::fit_class_row(size_t built_in) {
    if (icons_.size() <= built_in || built_in == 0) return;
    std::vector<size_t> order(built_in);
    for (size_t i = 0; i < built_in; ++i) order[i] = i;
    std::sort(order.begin(), order.end(),
              [&](size_t a, size_t b) { return icons_[a].off_x < icons_[b].off_x; });
    for (size_t i = built_in; i < icons_.size(); ++i) order.push_back(i);
    float left = icons_[0].off_x;
    float right = left;
    float top = icons_[0].off_y;
    float tall = 0.0f;
    for (size_t i = 0; i < built_in; ++i) {
        left = std::min(left, icons_[i].off_x);
        right = std::max(right, icons_[i].off_x + icons_[i].wide);
        top = std::min(top, icons_[i].off_y);
        tall = std::max(tall, icons_[i].tall);
    }
    const float cell = (right - left) / static_cast<float>(icons_.size());
    for (size_t at = 0; at < order.size(); ++at) {
        Widget& icon = icons_[order[at]];
        icon.off_x = left + cell * static_cast<float>(at);
        icon.off_y = top + (tall - cell) * 0.5f;
        icon.wide = cell;
        icon.tall = cell;
    }
}

void CharacterCreate::add_console_frame(const char* window_name, bool item_frame, float dx,
                                        float dy, const std::string& dims_with) {
    const tl::formats::GuiWindow* window = console_.find(window_name);
    if (window == nullptr) return;
    Origin origin;
    Area area;
    if (!origin_of(console_, window_name, origin) || !window_area(*window, area)) return;
    const FrameLook& look = item_frame ? kItemFrame : kPanelFrame;

    MenuArtSet* corner_art = art_for(look.set[0], look.image[0]);
    const tl::formats::ImagesetImage* corner =
        corner_art != nullptr ? corner_art->set.find(look.image[0]) : nullptr;
    if (corner == nullptr) return;

    for (int slice = 0; slice < 9; ++slice) {
        MenuArtSet* art = art_for(look.set[slice], look.image[slice]);
        if (art == nullptr) continue;
        Widget piece;
        piece.window = window_name;
        piece.dims_with = dims_with;
        piece.slice = slice;
        piece.off_x = origin.x + dx + area.x + look.left;
        piece.off_y = origin.y + dy + area.y + look.top;
        piece.wide = area.wide() + look.grow_w;
        piece.tall = area.tall() + look.grow_h;
        piece.corner_w = static_cast<float>(corner->width);
        piece.corner_h = static_cast<float>(corner->height);
        piece.normal = look.image[slice];
        piece.hover = piece.normal;
        piece.normal_art = art;
        piece.hover_art = art;
        piece.set = &art->set;
        piece.texture = art->texture;
        piece.panel = make_panel(
            "tl/create/console/frame/" + std::string(window_name) + "/" + std::to_string(slice),
            frame_, art->material);
        console_frame_.push_back(piece);
    }
}

// The PC screen's two imagesets, `logo.imageset` and `hud.imageset`, which the
// console screen does not draw (and the Xbox 360 pak does not carry).
bool CharacterCreate::load_pc_imagesets(const std::string& group) {
    std::vector<uint8_t> bytes;
    if (!read_menu_resource(kLogoImageset, group, bytes)) return false;
    logo_set_ = tl::formats::parse_imageset(bytes.data(), bytes.size());
    if (!read_menu_resource(kGuiImageset, group, bytes)) return false;
    gui_set_ = tl::formats::parse_imageset(bytes.data(), bytes.size());
    if (logo_set_.ok && gui_set_.ok) return true;
    TL_LOG_WARN("character creation: %s",
                logo_set_.ok ? gui_set_.error.c_str() : logo_set_.error.c_str());
    return false;
}

// Their textures and materials. Throws as OGRE does; create() catches it.
bool CharacterCreate::load_pc_textures(const std::string& group) {
    auto& textures = Ogre::TextureManager::getSingleton();
    logo_texture_ = textures.load(logo_set_.image_file, group);
    gui_texture_ = textures.load(gui_set_.image_file, group);
    if (!logo_texture_ || !gui_texture_) return false;
    logo_material_ = build_menu_material("tl/create/" + logo_set_.name, logo_texture_);
    gui_material_ = build_menu_material("tl/create/" + gui_set_.name, gui_texture_);
    return true;
}

bool CharacterCreate::create(const std::string& group, unsigned width, unsigned height,
                             std::vector<Choice> choices) {
    destroy();
    width_ = width;
    height_ = height;
    take_choices(group, std::move(choices));
    // Every showing opens on the Destroyer and the dog (`CNewGameMenu::vfunc_7`,
    // sets `L"Destroyer"` and `L"Dog"` each time). The two
    // names and the focus are the screen's own and are kept, as the console
    // builds them once and keeps them.
    selected_ = 0;
    for (size_t i = 0; i < choices_.size(); ++i) {
        if (choices_[i].name == tl::core::kDefaultClass) selected_ = static_cast<int>(i);
    }
    pet_ = tl::core::kDefaultPet;
    // A button held as the screen opens - the A that chose New Character - is
    // not a press on it.
    confirm_held_ = skip_held_ = move_held_ = turn_held_ = true;
    if (choices_.empty()) {
        TL_LOG_WARN("character creation: no playable classes to offer");
        return false;
    }

    // The console screen when its layout and art are there (the Xbox pak's own, or a PC pak's
    // `console_ui`); only the PC screen needs `logo.imageset` and `hud.imageset`.
    const bool console_screen = load_console_screen(group);
    if (!console_screen && !load_pc_imagesets(group)) return false;
    if (console_screen) load_pc_art(group);
    if (!ensure_menu_font()) return false;

    try {
        if (!console_screen && !load_pc_textures(group)) return false;

        auto& overlays = Ogre::OverlayManager::getSingleton();
        // Three overlays with ascending z-orders, above the main menu's three: within one
        // overlay OGRE sorts transparent renderables by pass (see menu.h).
        frame_ = overlays.create("tl/create/frame");
        frame_->setZOrder(460);
        widgets_ = overlays.create("tl/create/widgets");
        widgets_->setZOrder(461);
        text_overlay_ = overlays.create("tl/create/text");
        text_overlay_->setZOrder(462);

        text_root_ = static_cast<Ogre::OverlayContainer*>(
            overlays.createOverlayElement("Panel", "tl/create/textroot"));
        text_root_->setMetricsMode(Ogre::GMM_RELATIVE);
        text_root_->setPosition(0.0f, 0.0f);
        text_root_->setDimensions(1.0f, 1.0f);
        text_overlay_->add2D(text_root_);

        console_mode_ = console_screen;
        if (console_mode_) {
            build_console_screen();
            // `select()` writes the class's display name and `DESCRIPTION` into two of the
            // layout's own windows (`CNewGameMenu::update`,
            // reads `DISPLAYNAME` and `DESCRIPTION` off the unit on the backdrop).
            class_name_ = console_label_text("CharacterClass");
            description_ = console_label_text("CharacterClassDescription");
            header_ = nullptr;
            pet_title_ = nullptr;
            // The focus opens where it last was - the Destroyer, on a new
            // screen - through a (0, 0) step (`vfunc_7`).
            focus_ = tl::core::NewGameFocus{};
            step_focus(0, 0);
            created_ = true;
            layout();
            apply_look();
            for (Ogre::Overlay* overlay : {frame_, widgets_, text_overlay_}) overlay->show();
            TL_LOG_INFO("character creation: the console screen, %zu class(es) at %u x %u",
                        choices_.size(), width_, height_);
            return true;
        }

        size_t serial = 0;
        for (const MenuBar& bar : kMenuBars) {
            Ogre::PanelOverlayElement* panel =
                make_panel("tl/create/bar/" + std::to_string(serial++), frame_, logo_material_);
            set_uv(panel, logo_set_, logo_texture_, bar.image);
            bars_.push_back(panel);
        }

        for (size_t i = 0; i < choices_.size(); ++i) {
            Widget icon;
            icon.rel_x = 0.0f;
            icon.off_x = kIconLeft;
            icon.rel_y = 0.5f;
            icon.off_y = kIconTop + kIconPitch * static_cast<float>(i);
            icon.wide = kIconSize;
            icon.tall = kIconSize;
            // `destroyericon`, and its `lit` and `selected` twins - the three
            // images the layout gives each `RadioButton` as `UnselectedImage`,
            // `HoverImage` and `SelectedImage`.
            icon.normal = choices_[i].icon;
            icon.hover = choices_[i].icon + "lit";
            icon.selected = choices_[i].icon + "selected";
            icon.panel =
                make_panel("tl/create/icon/" + std::to_string(i), widgets_, logo_material_);
            icons_.push_back(icon);
        }

        auto add_button = [&](const char* text, const Authored& at) {
            Widget button;
            button.rel_x = at.rel_x;
            button.off_x = at.off_x;
            button.rel_y = at.rel_y;
            button.off_y = at.off_y;
            button.wide = at.wide;
            button.tall = at.tall;
            button.normal = kButtonNormal;
            button.hover = kButtonHover;
            button.selected = kButtonHover;
            const std::string name = "tl/create/button/" + std::to_string(buttons_.size());
            button.panel = make_panel(name, widgets_, gui_material_);
            // The drop first, so the white sits over it. **The suffixes are
            // ordered on purpose**: an `OverlayContainer` keeps its children in
            // a map keyed by name, so among siblings the draw order is
            // alphabetical rather than the order they were added.
            button.shadow = make_text(name + "/a-drop", kFontSize,
                                      Ogre::ColourValue(0.0f, 0.0f, 0.0f, 1.0f), true);
            button.label = make_text(name + "/b-text", kFontSize, Ogre::ColourValue::White, true);
            button.shadow->setCaption(text);
            button.label->setCaption(text);
            buttons_.push_back(button);
        };
        add_button("Back", kBack);
        add_button("OK", kOk);
        add_button("Dog", kDog);
        add_button("Cat", kCat);

        header_ = make_text("tl/create/header", kBigFontSize, Ogre::ColourValue::White, true);
        header_->setCaption("Select Hero Class");
        class_name_ = make_text("tl/create/name", kHugeFontSize, Ogre::ColourValue::White, true);
        description_ =
            make_text("tl/create/description", kSmallFontSize, Ogre::ColourValue::White, true);
        pet_title_ = make_text("tl/create/pettitle", kBigFontSize, Ogre::ColourValue::White, true);
        pet_title_->setCaption("Pet");

        created_ = true;
        layout();
        apply_look();
        for (Ogre::Overlay* overlay : {frame_, widgets_, text_overlay_}) overlay->show();
        TL_LOG_INFO("character creation: %zu class(es) at %u x %u", choices_.size(), width_,
                    height_);
        return true;
    } catch (const Ogre::Exception& e) {
        TL_LOG_WARN("no character creation screen: %s", e.getDescription().c_str());
        destroy();
        return false;
    }
}

// The console screen, every window of it placed by the layout: the border, the
// panel and its frames, the classes, the names, the pets, Done, the two
// headings over the character with the line under them, and the prompt bar.
void CharacterCreate::build_console_screen() {
    // The frame, the same six border pieces the main menu draws. The layout
    // authors them all at the origin and CEGUI's alignment properties put them
    // at the edges; these are the edges they end up on.
    struct FramePiece {
        const char* window;
        int corner;  ///< 0 centred, 1 left, 2 right
        bool bottom;
    };
    const FramePiece frame[] = {
        {"i_404", 0, false}, {"i_418", 1, false}, {"i_405", 2, false},
        {"i_421", 0, true},  {"i_403", 1, true},  {"i_419", 2, true},
    };
    for (const FramePiece& want : frame) {
        const tl::formats::GuiWindow* window = console_.find(want.window);
        Area area;
        std::string set, image;
        if (window == nullptr || !window_area(*window, area) ||
            !tl::formats::parse_image_reference(window->property("Image", std::string()), set,
                                                image)) {
            continue;
        }
        MenuArtSet* art = art_for(set, image);
        if (art == nullptr) continue;
        Widget piece;
        piece.window = want.window;
        piece.wide = area.wide();
        piece.tall = area.tall();
        piece.normal = image;
        piece.hover = image;
        piece.normal_art = art;
        piece.hover_art = art;
        piece.set = &art->set;
        piece.texture = art->texture;
        if (want.corner == 0) {
            piece.rel_x = 0.5f;
            piece.off_x = -piece.wide * 0.5f;
        } else if (want.corner == 2) {
            piece.rel_x = 1.0f;
            piece.off_x = -piece.wide;
        }
        if (want.bottom) {
            piece.rel_y = 1.0f;
            piece.off_y = -piece.tall;
        }
        piece.panel = make_panel("tl/create/console/border/" + std::string(want.window), frame_,
                                 art->material);
        console_frame_.push_back(piece);
    }

    // The panel and the three boxes on it, drawn by their looks: `i_434` is an
    // `ItemText`, whose look draws the soft dark panel, and `i_450`, `i_443`
    // and `i_438` are `ItemTooltip`s, whose look draws the bordered box. The
    // two name boxes' frames are their name box's parents, so they fade with
    // it (sets `Alpha` on the window and on its parent).
    add_console_frame("i_434", /*item_frame=*/false, 0.0f, 0.0f, std::string());
    add_console_frame("i_450", true, 0.0f, 0.0f, std::string());
    add_console_frame("i_443", true, 0.0f, 0.0f, "EditBox");
    add_console_frame("i_438", true, 0.0f, 0.0f, "EditBoxPet");

    build_class_row();

    // The three pets, each in its own `spells_window` box. The boxes first, so
    // the animals sit inside them.
    for (const char* box : {"i_435", "i_445", "i_458"}) {
        add_console_widget(box, console_frame_, 0.0f, 0.0f);
    }
    for (const char* pet : {"Dog", "Cat", "Chakawary"}) {
        add_console_widget(pet, buttons_, 0.0f, 0.0f);
    }
    add_console_widget("DoneButton", buttons_, 0.0f, 0.0f);

    // The captions: the panel's four headings, the two names, Done's word.
    for (const char* caption :
         {"i_454", "i_448", "i_441", "i_462", "EditBox", "EditBoxPet", "i_457"}) {
        add_console_label(caption, 0.0f, 0.0f, -1.0f);
    }
    for (ConsoleLabel& label : console_labels_) {
        if (label.window == "EditBox" || label.window == "EditBoxPet")
            label.dims_with = label.window;
        if (label.window == "i_457") label.shown_with = "DoneButton";
    }

    // The two headings over the character and the line under them.
    for (const char* caption : {"i_420", "CharacterClass", "CharacterClassDescription"}) {
        add_console_label(caption, 0.0f, 0.0f, -1.0f);
    }

    // The prompt bar, in the bottom border strip: three groups of a glyph and
    // a word each.
    for (const char* glyph : {"i_430", "i_428", "i_425"}) {
        add_console_widget(glyph, console_bar_, 0.0f, 0.0f);
    }
    for (const char* word : {"i_431", "i_427", "i_424"}) {
        add_console_label(word, 0.0f, 0.0f, -1.0f);
    }
}

void CharacterCreate::destroy() {
    typing_ = false;
    tl::core::set_typing(false);
    if (frame_ != nullptr || widgets_ != nullptr || text_overlay_ != nullptr) {
        auto& overlays = Ogre::OverlayManager::getSingleton();
        for (Ogre::Overlay* overlay : {frame_, widgets_, text_overlay_}) {
            if (overlay != nullptr) overlay->hide();
        }
        auto drop_text = [&](Ogre::TextAreaOverlayElement* text) {
            if (text == nullptr) return;
            if (text_root_ != nullptr) text_root_->removeChild(text->getName());
            overlays.destroyOverlayElement(text);
        };
        for (Widget& button : buttons_) {
            drop_text(button.shadow);
            drop_text(button.label);
        }
        // The console screen's captions own `class_name_` and `description_`:
        // those two are *aliases* into this list, not elements of their own, so
        // they are dropped here once and not again below.
        for (ConsoleLabel& label : console_labels_) {
            if (label.text == class_name_) class_name_ = nullptr;
            if (label.text == description_) description_ = nullptr;
            for (Ogre::TextAreaOverlayElement* drop : label.drops) drop_text(drop);
            drop_text(label.text);
        }
        drop_text(header_);
        drop_text(class_name_);
        drop_text(description_);
        drop_text(pet_title_);
        if (text_root_ != nullptr && text_overlay_ != nullptr) {
            text_overlay_->remove2D(text_root_);
            overlays.destroyOverlayElement(text_root_);
        }
        auto drop_panel = [&](Ogre::PanelOverlayElement* panel, Ogre::Overlay* overlay) {
            if (panel == nullptr || overlay == nullptr) return;
            overlay->remove2D(panel);
            overlays.destroyOverlayElement(panel);
        };
        for (Ogre::PanelOverlayElement* panel : bars_) drop_panel(panel, frame_);
        for (Widget& icon : icons_) drop_panel(icon.panel, widgets_);
        for (Widget& button : buttons_) drop_panel(button.panel, widgets_);
        for (Widget& piece : console_frame_) drop_panel(piece.panel, frame_);
        for (Widget& glyph : console_bar_) drop_panel(glyph.panel, widgets_);
        for (Ogre::Overlay* overlay : {frame_, widgets_, text_overlay_}) {
            if (overlay != nullptr) overlays.destroy(overlay);
        }
    }
    bars_.clear();
    icons_.clear();
    portrait_art_.clear();
    buttons_.clear();
    console_labels_.clear();
    console_frame_.clear();
    console_bar_.clear();
    console_mode_ = false;
    widgets_art_ = MenuArtSet{};
    widgets1_art_ = MenuArtSet{};
    widgets2_art_ = MenuArtSet{};
    logo_art_ = MenuArtSet{};
    gui_art_ = MenuArtSet{};
    header_ = nullptr;
    class_name_ = nullptr;
    description_ = nullptr;
    pet_title_ = nullptr;
    text_root_ = nullptr;
    frame_ = nullptr;
    widgets_ = nullptr;
    text_overlay_ = nullptr;
    logo_texture_.reset();
    gui_texture_.reset();
    logo_material_.reset();
    gui_material_.reset();
    created_ = false;
    selection_changed_ = false;
}

void CharacterCreate::layout() {
    if (!created_) return;
    const float across = static_cast<float>(width_ > 0 ? width_ : 1);
    const float down = static_cast<float>(height_ > 0 ? height_ : 1);
    // The screen's authored height: the console art's on the console screen, the
    // PC logo set's on the PC one (see MainMenu::layout()).
    scale_ = down / authored_height(console_mode_ ? widgets1_art_.set : logo_set_, kNativeHeight);

    if (console_mode_) {
        layout_console(across, down);
        return;
    }

    for (size_t i = 0; i < bars_.size(); ++i) {
        const MenuBar& bar = kMenuBars[i];
        if (bars_[i] == nullptr) continue;
        bars_[i]->setPosition((bar.rel_x * across + bar.off_x * scale_) / across,
                              (bar.rel_y * down + bar.off_y * scale_) / down);
        bars_[i]->setDimensions(bar.wide * scale_ / across, bar.tall * scale_ / down);
    }

    auto place = [&](Widget& widget) {
        if (widget.panel == nullptr) return;
        const float left = widget.rel_x * across + widget.off_x * scale_;
        const float top = widget.rel_y * down + widget.off_y * scale_;
        const float wide = widget.wide * scale_;
        const float tall = widget.tall * scale_;
        widget.panel->setPosition(left / across, top / down);
        widget.panel->setDimensions(wide / across, tall / down);
        if (widget.label == nullptr) return;
        // The caption sits on the button's middle rather than hanging from it,
        // so the baseline is dropped by half a line.
        const float line = kFontSize * points_to_char_pixels() * scale_;
        const float centre_x = (left + wide * 0.5f) / across;
        const float baseline = (top + (tall - line) * 0.5f) / down;
        widget.label->setPosition(centre_x, baseline);
        if (widget.shadow != nullptr) {
            widget.shadow->setPosition(centre_x + kLabelShadow * scale_ / across,
                                       baseline + kLabelShadow * scale_ / down);
        }
    };
    for (Widget& icon : icons_) place(icon);
    for (Widget& button : buttons_) place(button);

    // Each of these is `CentreAligned` inside its own window, so the element
    // goes at that window's horizontal middle and the alignment does the rest.
    auto put = [&](Ogre::TextAreaOverlayElement* text, const Authored& at, float points) {
        if (text == nullptr) return;
        set_char_height(text, points * points_to_char_pixels() * scale_ / down);
        const float left = at.rel_x * across + at.off_x * scale_;
        const float top = at.rel_y * down + at.off_y * scale_;
        text->setPosition((left + at.wide * scale_ * 0.5f) / across, top / down);
    };
    put(header_, kHeader, kBigFontSize);
    put(class_name_, kClassName, kHugeFontSize);
    put(pet_title_, kPetTitle, kBigFontSize);
    put(description_, kDescription, kSmallFontSize);
}

// Everything on the console screen, in viewport pixels. The authored units are
// against 1024x768 and scale by `height / 768`, the same rule the whole front
// end uses.
void CharacterCreate::layout_console(float across, float down) {
    auto put = [&](Widget& widget, float left, float top, float wide, float tall) {
        widget.panel->setPosition(left / across, top / down);
        widget.panel->setDimensions(std::max(wide, 0.0f) / across, std::max(tall, 0.0f) / down);
    };
    auto place = [&](Widget& widget) {
        if (widget.panel == nullptr) return;
        const float left = widget.rel_x * across + widget.off_x * scale_;
        const float top = widget.rel_y * down + widget.off_y * scale_;
        const float wide = widget.wide * scale_;
        const float tall = widget.tall * scale_;
        if (widget.slice >= 0) {
            // A `FrameComponent`: corners at their own size, edges stretched
            // between them, the middle filling what is left.
            const float cw = widget.corner_w * scale_;
            const float ch = widget.corner_h * scale_;
            const float right = left + wide - cw;
            const float bottom = top + tall - ch;
            switch (widget.slice) {
                case 0: put(widget, left, top, cw, ch); break;
                case 1: put(widget, right, top, cw, ch); break;
                case 2: put(widget, left, bottom, cw, ch); break;
                case 3: put(widget, right, bottom, cw, ch); break;
                case 4: put(widget, left, top + ch, cw, tall - 2.0f * ch); break;
                case 5: put(widget, right, top + ch, cw, tall - 2.0f * ch); break;
                case 6: put(widget, left + cw, top, wide - 2.0f * cw, ch); break;
                case 7: put(widget, left + cw, bottom, wide - 2.0f * cw, ch); break;
                default:
                    put(widget, left + cw, top + ch, wide - 2.0f * cw, tall - 2.0f * ch);
                    break;
            }
        } else {
            put(widget, left, top, wide, tall);
        }
        if (widget.set != nullptr) set_uv(widget.panel, *widget.set, widget.texture, widget.normal);
        widget.panel->show();
    };
    for (Widget& piece : console_frame_) place(piece);
    for (Widget& icon : icons_) place(icon);
    for (Widget& button : buttons_) place(button);

    // The prompt bar rides with the bottom strip: 1024 units wide, centred, and
    // against the bottom edge.
    const float strip_left = (across - 1024.0f * scale_) * 0.5f;
    const float strip_top = down - 93.0f * scale_;
    for (Widget& glyph : console_bar_) {
        if (glyph.panel == nullptr) continue;
        put(glyph, strip_left + glyph.off_x * scale_, strip_top + glyph.off_y * scale_,
            glyph.wide * scale_, glyph.tall * scale_);
        if (glyph.set != nullptr) set_uv(glyph.panel, *glyph.set, glyph.texture, glyph.normal);
        glyph.panel->show();
    }

    // A look's `AbsoluteDim` is in display pixels, which on the console's own
    // 720-line output are these.
    const float pixel = down / 720.0f;
    for (ConsoleLabel& label : console_labels_) {
        if (label.text == nullptr) continue;
        const float line = label.points * points_to_char_pixels() * scale_;
        const float box_left = label.centred_on_screen
                                   ? (across - label.wide * scale_) * 0.5f + label.x * scale_
                               : label.in_bottom_bar ? strip_left + label.x * scale_
                                                     : label.x * scale_;
        float top = label.in_bottom_bar ? strip_top + label.y * scale_ : label.y * scale_;
        const float left = label.text_centred ? box_left + label.wide * scale_ * 0.5f : box_left;
        if (label.v_centred) {
            // One line, centred in the box: every centred window here holds
            // a single word.
            top += (label.tall * scale_ - line) * 0.5f;
        }
        set_char_height(label.text, line / down);
        label.text->setPosition(left / across, top / down);
        label.text->show();
        for (size_t i = 0; i < label.drops.size(); ++i) {
            set_char_height(label.drops[i], line / down);
            label.drops[i]->setPosition((left + label.drop_offsets[i].first * pixel) / across,
                                        (top + label.drop_offsets[i].second * pixel) / down);
            label.drops[i]->show();
        }
    }
}

bool CharacterCreate::console_window_visible(const std::string& window) const {
    // `DoneButton` is shown only while both names have something in them;
    // everything else on the grid is always shown, if the
    // screen built it.
    if (window == "DoneButton") {
        return tl::core::new_game_done_visible(player_name_, pet_name_);
    }
    for (const Widget& icon : icons_) {
        if (icon.window == window) return true;
    }
    for (const Widget& button : buttons_) {
        if (button.window == window) return true;
    }
    for (const ConsoleLabel& label : console_labels_) {
        if (label.window == window) return true;
    }
    return false;
}

MenuAction CharacterCreate::fire_console(const std::string& window_name) {
    const tl::formats::GuiWindow* window = console_.find(window_name);
    if (window == nullptr) {
        choose_mod_class(window_name);
        return MenuAction::None;
    }
    // `CNewGameMenu::vfunc_12` hands A to the focused
    // window's `onClick`, and `vfunc_9` answers it.
    const tl::core::NewGameCommand command =
        tl::core::new_game_command(window->property("onClick", std::string()));
    switch (command) {
        case tl::core::NewGameCommand::ChooseClass:
            select(window_name);
            TL_LOG_INFO("character creation: class %s", window_name.c_str());
            return MenuAction::None;
        case tl::core::NewGameCommand::ChoosePet:
            select_pet(window_name);
            TL_LOG_INFO("character creation: pet %s", window_name.c_str());
            return MenuAction::None;
        case tl::core::NewGameCommand::EditPlayerName:
        case tl::core::NewGameCommand::EditPetName: {
            // The keyboard, and what comes back replaces the
            // name as typed - even an empty one, which then hides Done. The
            // answer is taken as `update` takes it (`new_game_keyboard_name`),
            // and after the pet's `handle_SubmitPet` hands both names on.
            const bool player = command == tl::core::NewGameCommand::EditPlayerName;
            std::string text = player ? player_name_ : pet_name_;
            if (tl::core::text_entry(player ? tl::core::kPlayerNameTitle : tl::core::kPetNameTitle,
                                     player ? tl::core::kPlayerNameGuide : tl::core::kPetNameGuide,
                                     text, tl::core::kNewGameNameChars)) {
                take_name(player, text);
            } else if (tl::core::typing_in_window()) {
                // DIVERGENCE (platform): no system keyboard to open, so the name is typed
                // into its own box, from empty; leaving keeps the name it had.
                typing_ = true;
                typing_player_ = player;
                typed_name_.clear();
                (void)tl::core::take_typed_text();
                tl::core::set_typing(true);
                TL_LOG_INFO("character creation: typing the %s name", player ? "player" : "pet");
            }
            return MenuAction::None;
        }
        case tl::core::NewGameCommand::Done:
            if (!console_window_visible("DoneButton")) return MenuAction::None;
            return MenuAction::NewGame;
        case tl::core::NewGameCommand::None: break;
    }
    return MenuAction::None;
}

void CharacterCreate::take_name(bool player, const std::string& text) {
    std::string& name = player ? player_name_ : pet_name_;
    name = tl::core::new_game_keyboard_name(text);
    TL_LOG_INFO("character creation: %s name '%s'", player ? "player" : "pet", name.c_str());
    if (player) return;
    const tl::core::NewGameSubmission submitted =
        tl::core::new_game_submit_pet(player_name_, pet_name_);
    if (submitted.submitted) {
        TL_LOG_INFO("character creation: '%s' and '%s' handed on", submitted.player_name.c_str(),
                    submitted.pet_name.c_str());
    }
}

void CharacterCreate::step_typing(bool confirm_edge, bool back_edge) {
    bool keep = confirm_edge;
    bool leave = back_edge;
    const std::string typed = tl::core::take_typed_text();
    size_t i = 0;
    while (i < typed.size() && !keep && !leave) {
        const std::string piece = next_utf8_char(typed, i);
        if (piece == "\r") {
            keep = true;
        } else if (piece == "\x1b") {
            leave = true;
        } else if (piece == "\b") {
            erase_last_utf8_char(typed_name_);
        } else if (utf8_chars(typed_name_) < tl::core::kNewGameNameChars) {
            typed_name_ += piece;
        }
    }
    if (!keep && !leave) return;
    typing_ = false;
    tl::core::set_typing(false);
    if (keep) {
        take_name(typing_player_, typed_name_);
    } else {
        TL_LOG_INFO("character creation: typing left, the name as it was");
    }
}

std::string CharacterCreate::shown_name(bool player) const {
    // A box being typed into shows the text so far and a caret.
    if (typing_ && typing_player_ == player) return typed_name_ + "_";
    return player ? player_name_ : pet_name_;
}

void CharacterCreate::apply_look() {
    if (!created_) return;

    if (console_mode_) {
        // The focused window is lit: the console parks its invisible pointer
        // on the focused window's centre,
        // so CEGUI draws that button's `HoverImage`. Nothing marks the class
        // or pet already chosen - the model on the backdrop does that.
        const std::string focused = tl::core::new_game_window(grid_, focus_);
        auto look = [&](Widget& widget) {
            if (widget.panel == nullptr || widget.normal_art == nullptr) return;
            const bool lit = widget.window == focused;
            MenuArtSet* art = lit ? widget.hover_art : widget.normal_art;
            widget.set = &art->set;
            widget.texture = art->texture;
            widget.panel->setMaterial(art->material);
            set_uv(widget.panel, art->set, art->texture, lit ? widget.hover : widget.normal);
            if (console_window_visible(widget.window)) {
                widget.panel->show();
            } else {
                widget.panel->hide();
            }
        };
        for (Widget& icon : icons_) look(icon);
        for (Widget& button : buttons_) look(button);
        // A name box and its frame at full strength only while it has the
        // focus.
        for (Widget& piece : console_frame_) {
            if (piece.dims_with.empty() || piece.panel == nullptr || piece.normal_art == nullptr) {
                continue;
            }
            piece.panel->setMaterial(piece.dims_with == focused ? piece.normal_art->material
                                                                : piece.normal_art->dim_material);
        }
        for (ConsoleLabel& label : console_labels_) {
            if (label.text == nullptr) continue;
            const float alpha =
                label.dims_with.empty() || label.dims_with == focused ? 1.0f : kUnfocusedAlpha;
            // Premultiplied: OGRE's font material blends `ONE,
            // ONE_MINUS_SOURCE_ALPHA` (`Font::loadResource`, antialiased
            // colour), so alpha alone would only thin the background under the
            // glyph and never dim the glyph itself.
            Ogre::ColourValue colour = label.colour;
            colour.r *= alpha;
            colour.g *= alpha;
            colour.b *= alpha;
            colour.a *= alpha;
            label.text->setColour(colour);
            for (Ogre::TextAreaOverlayElement* drop : label.drops) {
                drop->setColour(Ogre::ColourValue(0.0f, 0.0f, 0.0f, alpha));
            }
            const bool shown = label.shown_with.empty() || console_window_visible(label.shown_with);
            for (Ogre::TextAreaOverlayElement* text : label.drops) {
                if (shown) text->show();
                else text->hide();
            }
            if (shown) label.text->show();
            else label.text->hide();
        }
        auto set_caption = [&](const char* window, const std::string& text) {
            for (ConsoleLabel& label : console_labels_) {
                if (label.window != window || label.text == nullptr) continue;
                label.text->setCaption(text);
                for (Ogre::TextAreaOverlayElement* drop : label.drops) drop->setCaption(text);
            }
        };
        set_caption("EditBox", shown_name(true));
        set_caption("EditBoxPet", shown_name(false));

        if (selected_ < 0 || static_cast<size_t>(selected_) >= choices_.size()) return;
        const Choice& choice = choices_[static_cast<size_t>(selected_)];
        set_caption("CharacterClass", choice.display_name);
        // `WordWrapCentreAligned` in its own 448-unit window.
        const float across = static_cast<float>(width_ > 0 ? width_ : 1);
        const float down = static_cast<float>(height_ > 0 ? height_ : 1);
        for (ConsoleLabel& label : console_labels_) {
            if (label.window != "CharacterClassDescription" || label.text == nullptr) continue;
            const std::vector<std::string> lines =
                wrap_text(label.text->getFont(), choice.description, label.wide * scale_ / across,
                          label.points * points_to_char_pixels() * scale_ / down, across, down);
            std::string body;
            for (size_t i = 0; i < lines.size(); ++i) {
                if (i > 0) body += "\n";
                body += lines[i];
            }
            set_caption("CharacterClassDescription", body);
        }
        return;
    }

    for (size_t i = 0; i < icons_.size(); ++i) {
        const bool is_selected = static_cast<int>(i) == selected_;
        set_uv(icons_[i].panel, logo_set_, logo_texture_,
               is_selected ? icons_[i].selected : icons_[i].normal);
    }
    for (size_t i = 0; i < buttons_.size(); ++i) {
        // The two pet buttons stay lit while they are the choice.
        const bool chosen_pet =
            (i == kDogButton && pet_ == "Dog") || (i == kCatButton && pet_ == "Cat");
        set_uv(buttons_[i].panel, gui_set_, gui_texture_,
               chosen_pet ? buttons_[i].hover : buttons_[i].normal);
    }

    if (selected_ < 0 || static_cast<size_t>(selected_) >= choices_.size()) return;
    const Choice& choice = choices_[static_cast<size_t>(selected_)];
    if (class_name_ != nullptr) class_name_->setCaption(choice.display_name);
    if (description_ == nullptr) return;

    // `CharacterClassDescription` is `WordWrapCentreAligned` in a 300-pixel
    // window, so the sentence is wrapped to that and not to the screen.
    const float across = static_cast<float>(width_ > 0 ? width_ : 1);
    const float down = static_cast<float>(height_ > 0 ? height_ : 1);
    const std::vector<std::string> lines =
        wrap_text(description_->getFont(), choice.description, kDescription.wide * scale_ / across,
                  kSmallFontSize * points_to_char_pixels() * scale_ / down, across, down);
    std::string body;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) body += "\n";
        body += lines[i];
    }
    description_->setCaption(body);
}

MenuAction CharacterCreate::update_console(const ConsolePress& press) {
    // `CNewGameMenu::vfunc_12`: the D-pad and the left
    // stick move the focus (buttons 1-4 and 0x11-0x14), A fires the focused
    // window (5), B leaves the screen (6).
    MenuAction action = MenuAction::None;
    if (press.dx != 0) step_focus(press.dx, 0);
    if (press.dy != 0) step_focus(0, press.dy);
    if (press.confirm) action = fire_console(tl::core::new_game_window(grid_, focus_));
    if (press.back) action = MenuAction::Quit;
    apply_look();
    return action;
}

MenuAction CharacterCreate::update(const tl::core::InputState& input) {
    if (!created_) return MenuAction::None;

    const bool left = input.move_x < -0.5f;
    const bool right = input.move_x > 0.5f;
    const bool up = input.move_y > 0.5f;
    const bool down = input.move_y < -0.5f;
    const bool turn_edge = (left || right) && !turn_held_;
    const bool move_edge = (up || down) && !move_held_;
    const bool confirm_edge = input.confirm && !confirm_held_;
    const bool back_edge = input.skip && !skip_held_;
    turn_held_ = left || right;
    move_held_ = up || down;
    confirm_held_ = input.confirm;
    skip_held_ = input.skip;

    MenuAction action = MenuAction::None;

    if (typing_) {
        step_typing(confirm_edge, back_edge);
        apply_look();
        return MenuAction::None;
    }

    if (console_mode_) {
        return update_console({turn_edge ? (right ? 1 : -1) : 0, move_edge ? (down ? 1 : -1) : 0,
                               confirm_edge, back_edge});
    }

    // The PC screen, on a pad: up and down walk the classes, left and right
    // choose the pet, A is OK and B is Back.
    if (move_edge && !icons_.empty()) {
        int next = selected_ + (up ? -1 : 1);
        if (next < 0) next = static_cast<int>(icons_.size()) - 1;
        if (next >= static_cast<int>(icons_.size())) next = 0;
        if (next != selected_) {
            selected_ = next;
            selection_changed_ = true;
        }
    }
    if (turn_edge) select_pet(pet_ == "Dog" ? "Cat" : "Dog");
    if (confirm_edge) action = MenuAction::NewGame;
    if (back_edge) action = MenuAction::Quit;

    apply_look();
    return action;
}

}  // namespace tl::render
