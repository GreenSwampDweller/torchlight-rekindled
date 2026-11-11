#include "render/menu.h"

#include "core/log.h"
#include "core/run_config.h"
#include "core/text.h"
#include "formats/adm.h"
#include "render/layout_screen.h"
#include "render/menu_chrome.h"
#include "render/text_layout.h"

#include <OgreFont.h>
#include <OgreFontManager.h>
#include <OgreMaterialManager.h>
#include <OgreOverlay.h>
#include <OgreOverlayManager.h>
#include <OgrePanelOverlayElement.h>
#include <OgrePass.h>
#include <OgreResourceGroupManager.h>
#include <OgreTechnique.h>
#include <OgreTextAreaOverlayElement.h>
#include <OgreTexture.h>
#include <OgreTextureManager.h>
#include <OgreTextureUnitState.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace tl::render {
namespace {

/// The main menu's own layout file (the shared imagesets, typeface and button skin are in
/// render/menu_chrome.h). A full archive path: several of these basenames occur twice.
constexpr const char* kMenuLayoutFile = "media/UI/mainmenuframe.layout";

/// The console's own main menu: Xbox-only, and only as the CEGUI export.
constexpr const char* kConsoleMenuFile = "media/UI/mainmenu.uilayout";

/// Its art, all three of them: the frame's borders are in `widgets`, the gem
/// buttons and the A/START glyphs in `widgets1`, the stick glyph in `widgets2`.
constexpr const char* kWidgetsImageset = "media/UI/icons2/widgets.imageset";
constexpr const char* kWidgets1Imageset = "media/UI/icons2/widgets1.imageset";
constexpr const char* kWidgets2Imageset = "media/UI/icons2/widgets2.imageset";

/// The console screen's authored size. Every offset in `mainmenu.uilayout` is
/// in these units, and the scale rule in `layout()` is `height / 768` - the
/// same reading that places the PC row.
constexpr float kConsoleWidth = 1024.0f;

/// Where the button column sits, in authored units: `i_402`'s rectangle is
/// `{{-0.45,138},{0,138},...}`, `Centre`-aligned (see resolve_path). Measured at about 68 units
/// from the left on the console; the vertical placement is the layout's.
constexpr float kConsoleColumnLeft = 68.0f;
constexpr float kConsoleColumnTop = 138.0f;

/// `SerifBig` for the captions and `Serif14` for the copyright line, in points,
/// as the layout's `Font` properties name them.
constexpr float kConsoleCaptionPoints = 16.0f;
constexpr float kConsoleSmallPoints = 14.0f;

/// The bottom border strip, and where `CopyrightInfo` sits inside the screen:
/// the strip is 93 units tall and the text is authored at y 653 of 768.
constexpr float kConsoleBottomBar = 93.0f;
[[maybe_unused]] constexpr float kConsoleBottomBarTop = 675.0f;

/// The debug wardrobe editor, this engine's own screen: its text block's size
/// in points, its top-left corner in authored units from the screen's top right,
/// and its rows' column on the PC screen (which has no authored column).
constexpr float kEditorInfoPoints = 11.0f;
constexpr float kEditorInfoFromRight = 470.0f;
constexpr float kEditorInfoTop = 150.0f;
constexpr float kEditorColumnLeft = 40.0f;
constexpr float kEditorColumnTop = 150.0f;
constexpr float kEditorColumnGap = 6.0f;

/// Left or right held on a wardrobe row repeats after this long, this often.
constexpr double kStepRepeatDelay = 0.35;
constexpr double kStepRepeatEvery = 0.06;

double steady_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

/// The console's copyright line (`L"(v1.15) Torchlight (C)
/// 2009 Runic Games Inc."`).
constexpr const char* kConsoleCopyright = "(v1.15) Torchlight (C) 2009 Runic Games Inc.";

MainMenu::~MainMenu() { destroy(); }

bool MainMenu::load_art_set(const char* file, const std::string& group, ArtSet& art) {
    std::vector<uint8_t> bytes;
    if (!read_menu_resource(file, group, bytes)) return false;
    art.set = tl::formats::parse_imageset(bytes.data(), bytes.size());
    if (!art.set.ok) {
        TL_LOG_WARN("menu: %s: %s", file, art.set.error.c_str());
        return false;
    }
    art.material = material_for(art.set, group, art.texture);
    art.ok = static_cast<bool>(art.material);
    return art.ok;
}

// The console main menu: `media/UI/mainmenu.uilayout` and the three imagesets
// it draws from. Absent without the Xbox data, and then the PC row is drawn.
bool MainMenu::load_console_menu(const std::string& group) {
    std::vector<uint8_t> bytes;
    if (!read_menu_resource(kConsoleMenuFile, group, bytes)) return false;
    console_ = tl::formats::parse_gui_layout(bytes.data(), bytes.size());
    if (!console_.ok) {
        TL_LOG_WARN("menu: %s: %s", kConsoleMenuFile, console_.error.c_str());
        return false;
    }
    if (!load_art_set(kWidgetsImageset, group, widgets_art_) ||
        !load_art_set(kWidgets1Imageset, group, widgets1_art_) ||
        !load_art_set(kWidgets2Imageset, group, widgets2_art_)) {
        TL_LOG_WARN("menu: %s is mounted but its art is not; drawing the PC row", kConsoleMenuFile);
        return false;
    }
    TL_LOG_INFO("menu: the console screen, from %s", kConsoleMenuFile);
    return true;
}

// PORT: an image of the console screen, from the sheet its reference names or, where that sheet
// leaves it out, the PC's own sets.
MainMenu::ArtRef MainMenu::resolve_art(const std::string& set_name,
                                       const std::string& image) const {
    const ArtSet* sheets[] = {&widgets_art_, &widgets1_art_, &widgets2_art_};
    for (const ArtSet* art : sheets) {
        if (art->ok && art->set.name == set_name && art->set.find(image) != nullptr) {
            return {&art->set, art->material};
        }
    }
    if (pc_art_) {
        if (logo_set_.name == set_name && logo_set_.find(image) != nullptr) {
            return {&logo_set_, logo_material_};
        }
        if (gui_set_.name == set_name && gui_set_.find(image) != nullptr) {
            return {&gui_set_, gui_material_};
        }
    }
    // Not where the reference says: a look's fixed image (`tooltiptopleft`), which the pack
    // leaves to the PC's `GuiLook` (`TooltipTopLeft`).
    for (const ArtSet* art : sheets) {
        if (art->ok && art->set.find(image) != nullptr) return {&art->set, art->material};
    }
    if (pc_art_) {
        if (gui_set_.find(image) != nullptr) return {&gui_set_, gui_material_};
        if (logo_set_.find(image) != nullptr) return {&logo_set_, logo_material_};
    }
    return {};
}

// PORT: the console column's button images, as its first button names them.
void MainMenu::resolve_console_buttons() {
    console_normal_ = "gem_button";
    console_hover_ = "gem_button_rollover";
    std::string set_name = widgets1_art_.set.name;
    if (const tl::formats::GuiWindow* first = console_.find("ContinueLast")) {
        std::string normal_set, normal, hover_set, hover;
        if (tl::formats::parse_image_reference(first->property("NormalImage", std::string()),
                                               normal_set, normal) &&
            tl::formats::parse_image_reference(first->property("HoverImage", std::string()),
                                               hover_set, hover)) {
            set_name = normal_set;
            console_normal_ = normal;
            console_hover_ = hover;
        }
    }
    console_button_art_ = resolve_art(set_name, console_normal_);
    if (console_button_art_.set == nullptr) {
        console_button_art_ = {&widgets1_art_.set, widgets1_art_.material};
    }
    // The difficulty screen's `ceguiWidget/Checkbox`, whose look names `widgets2`.
    checkbox_art_ = {&widgets2_art_.set, widgets2_art_.material};
}

bool MainMenu::load_imagesets(const std::string& group) {
    std::vector<uint8_t> bytes;

    if (!read_menu_resource(kLogoImageset, group, bytes)) return false;
    logo_set_ = tl::formats::parse_imageset(bytes.data(), bytes.size());
    if (!logo_set_.ok) {
        TL_LOG_WARN("menu: %s: %s", kLogoImageset, logo_set_.error.c_str());
        return false;
    }

    if (!read_menu_resource(kGuiImageset, group, bytes)) return false;
    gui_set_ = tl::formats::parse_imageset(bytes.data(), bytes.size());
    if (!gui_set_.ok) {
        TL_LOG_WARN("menu: %s: %s", kGuiImageset, gui_set_.error.c_str());
        return false;
    }

    TL_LOG_INFO("menu: '%s' (%zu images from %s), '%s' (%zu images from %s)",
                logo_set_.name.c_str(), logo_set_.images.size(), logo_set_.image_file.c_str(),
                gui_set_.name.c_str(), gui_set_.images.size(), gui_set_.image_file.c_str());
    return true;
}

Ogre::MaterialPtr MainMenu::material_for(const tl::formats::Imageset& set, const std::string& group,
                                         Ogre::TexturePtr& texture) {
    try {
        texture = Ogre::TextureManager::getSingleton().load(set.image_file, group);
        if (!texture) return {};
        return build_menu_material("tl/menu/" + set.name, texture);
    } catch (const Ogre::Exception& e) {
        TL_LOG_WARN("menu: could not load '%s': %s", set.image_file.c_str(),
                    e.getDescription().c_str());
        return {};
    }
}

bool MainMenu::load_font(const std::string& group) {
    auto& fonts = Ogre::FontManager::getSingleton();
    font_ = kFontName;
    if (Ogre::FontPtr existing = fonts.getByName(font_, Ogre::RGN_INTERNAL)) return true;
    try {
        Ogre::FontPtr font = fonts.create(font_, Ogre::RGN_INTERNAL);
        font->setType(Ogre::FT_TRUETYPE);
        font->setSource(kFontFile);
        // Rasterised well above its drawn size, which depends on the viewport.
        font->setTrueTypeSize(48);
        font->setTrueTypeResolution(96);
        font->addCodePointRange(Ogre::Font::CodePointRange(32, 255));
        font->load();
        (void)group;
        return true;
    } catch (const Ogre::Exception& e) {
        TL_LOG_WARN("menu: could not build a font from '%s': %s", kFontFile,
                    e.getDescription().c_str());
        font_.clear();
        return false;
    }
}

Ogre::PanelOverlayElement* MainMenu::make_panel(const std::string& name, Ogre::Overlay* overlay) {
    auto& overlays = Ogre::OverlayManager::getSingleton();
    auto* panel =
        static_cast<Ogre::PanelOverlayElement*>(overlays.createOverlayElement("Panel", name));
    panel->setMetricsMode(Ogre::GMM_RELATIVE);
    overlay->add2D(panel);
    return panel;
}

void MainMenu::add_piece(const std::string& window_name, const char* fallback_image, bool centred) {
    const tl::formats::GuiWindow* window = menu_.find(window_name);
    if (window == nullptr) {
        TL_LOG_WARN("menu: no window '%s' in %s", window_name.c_str(), kMenuLayoutFile);
        return;
    }
    const std::string empty;
    Piece piece;
    if (!tl::formats::parse_uvector2(window->property("UnifiedPosition", empty), piece.position) ||
        !tl::formats::parse_uvector2(window->property("UnifiedSize", empty), piece.size)) {
        TL_LOG_WARN("menu: '%s' has no usable geometry", window_name.c_str());
        return;
    }

    std::string set_name;
    std::string image_name;
    if (!tl::formats::parse_image_reference(window->property("Image", empty), set_name,
                                            image_name)) {
        // A container - `TopBarB` holds `TopBar` and draws nothing itself - so
        // the image comes from the child the caller named instead.
        if (fallback_image == nullptr) return;
        set_name = logo_set_.name;
        image_name = fallback_image;
    }

    piece.set = (set_name == gui_set_.name) ? &gui_set_ : &logo_set_;
    piece.image = image_name;
    if (piece.set->find(piece.image) == nullptr) {
        TL_LOG_WARN("menu: '%s' names image '%s', which set '%s' has not", window_name.c_str(),
                    piece.image.c_str(), piece.set->name.c_str());
        return;
    }
    piece.centred = centred;
    piece.panel = make_panel("tl/menu/" + window_name, frame_);
    piece.panel->setMaterial(piece.set == &gui_set_ ? gui_material_ : logo_material_);
    pieces_.push_back(piece);
}

namespace {

/// One `{{scale,offset},{scale,offset},...}` corner out of a CEGUI
/// `UnifiedAreaRect`, which is what the console layout uses instead of the
/// `UnifiedPosition`/`UnifiedSize` pair the PC screens are authored with.
bool parse_area(const std::string& text, tl::formats::UVector2& top_left,
                tl::formats::UVector2& bottom_right) {
    tl::formats::UDim dims[4];
    size_t at = 0;
    for (int i = 0; i < 4; ++i) {
        const size_t open = text.find('{', at);
        if (open == std::string::npos) return false;
        const size_t close = text.find('}', open);
        if (close == std::string::npos) return false;
        std::string pair = text.substr(open + 1, close - open - 1);
        // The first pair opens with the rectangle's own brace as well; left in,
        // it reads the first scale as zero.
        while (!pair.empty() && pair.front() == '{') pair.erase(pair.begin());
        const size_t comma = pair.find(',');
        if (comma == std::string::npos) return false;
        dims[i].scale = std::strtof(pair.substr(0, comma).c_str(), nullptr);
        dims[i].offset = std::strtof(pair.substr(comma + 1).c_str(), nullptr);
        at = close + 1;
    }
    top_left.x = dims[0];
    top_left.y = dims[1];
    bottom_right.x = dims[2];
    bottom_right.y = dims[3];
    return true;
}

}  // namespace

// The console frame: the two 1024-wide border strips and the four corner
// pieces that cover what they do not reach, exactly as `mainmenu.uilayout`
// places them (`i_425`, `i_429` and `i_421`/`i_423`/`i_424`/`i_426`).
void MainMenu::build_console_frame() {
    struct FramePiece {
        const char* image;
        ArtSet* art;
        float width, height;
        int corner;  ///< 0 centred strip, 1 top-left, 2 top-right, 3 bottom-left, 4 bottom-right
        bool bottom;
        /// The PC frame's piece in the same place (`logo.imageset`, drawn at its own size) when
        /// the console sheet leaves this one out.
        const char* pc_image;
    };
    const FramePiece frame[] = {
        {"main_topborder", &widgets_art_, 1024.0f, 141.0f, 0, false, "topbar"},
        {"main_topborderleft", &widgets1_art_, 215.0f, 87.0f, 1, false, "topbarleft"},
        {"main_topborderright", &widgets1_art_, 215.0f, 87.0f, 2, false, "topbarright"},
        {"main_bottomborder", &widgets_art_, 1024.0f, 93.0f, 0, true, "bottombar"},
        {"main_bottomborderleft", &widgets1_art_, 215.0f, 93.0f, 3, true, "bottombarleft"},
        {"main_bottomborderright", &widgets1_art_, 215.0f, 93.0f, 4, true, "bottombarright"},
    };
    int serial = 0;
    for (const FramePiece& want : frame) {
        Piece piece;
        float width = want.width;
        float height = want.height;
        if (want.art->ok && want.art->set.find(want.image) != nullptr) {
            piece.set = &want.art->set;
            piece.material = want.art->material;
            piece.image = want.image;
        } else if (const tl::formats::ImagesetImage* pc =
                       pc_art_ ? logo_set_.find(want.pc_image) : nullptr) {
            piece.set = &logo_set_;
            piece.material = logo_material_;
            piece.image = want.pc_image;
            width = static_cast<float>(pc->width);
            height = static_cast<float>(pc->height);
        } else {
            continue;
        }
        piece.centred = want.corner == 0;
        piece.size.x.offset = width;
        piece.size.y.offset = height;
        // The corners are anchored to their own edge; the strips are centred
        // and sit against the top or the bottom.
        if (want.corner == 2 || want.corner == 4) {
            piece.position.x.scale = 1.0f;
            piece.position.x.offset = -width;
        }
        if (want.bottom) {
            piece.position.y.scale = 1.0f;
            piece.position.y.offset = -height;
        }
        piece.panel = make_panel("tl/menu/console/" + std::to_string(serial++), frame_);
        piece.panel->setMaterial(piece.material);
        pieces_.push_back(piece);
    }
}

// The bar of button prompts along the bottom: a glyph and the word beside it, three times.
// `mainmenu.uilayout` authors four groups inside `i_430` (in the bottom strip): `i_440` Select,
// `i_431` Confirm, `invitefriend` Gift a Friend (Xbox Live, left out) and `credits` Credits.
void MainMenu::build_console_bar() {
    build_bar(
        console_, "i_430",
        {{"i_440", "i_442", "i_441"}, {"i_431", "i_432", "i_433"}, {"credits", "i_435", "i_436"}},
        /*authored_colour=*/false);
}

void MainMenu::build_bar(const tl::formats::GuiLayout& layout, const std::string& bar_window,
                         const std::vector<BarGroup>& groups, bool authored_colour) {
    const tl::formats::GuiWindow* bar = layout.find(bar_window);
    tl::formats::UVector2 bar_top_left{};
    tl::formats::UVector2 bar_bottom_right{};
    if (bar == nullptr || !parse_area(bar->property("UnifiedAreaRect", std::string()), bar_top_left,
                                      bar_bottom_right)) {
        return;
    }

    for (const BarGroup& group : groups) {
        // Unique for the menu's life, as a button's name: the difficulty screen rebuilds the bar.
        const size_t serial = button_serial_++;
        const tl::formats::GuiWindow* window = layout.find(group.window);
        if (window == nullptr) continue;
        tl::formats::UVector2 top_left{};
        tl::formats::UVector2 bottom_right{};
        if (!parse_area(window->property("UnifiedAreaRect", std::string()), top_left,
                        bottom_right)) {
            continue;
        }

        BarItem item;
        item.window = group.window;
        item.x = bar_top_left.x.offset + top_left.x.offset;
        item.y = bar_top_left.y.offset + top_left.y.offset;

        for (const tl::formats::GuiWindow& child : window->children) {
            tl::formats::UVector2 child_top_left{};
            tl::formats::UVector2 child_bottom_right{};
            if (!parse_area(child.property("UnifiedAreaRect", std::string()), child_top_left,
                            child_bottom_right)) {
                continue;
            }
            std::string set_name;
            std::string image_name;
            if (child.name == group.glyph &&
                tl::formats::parse_image_reference(child.property("Image", std::string()), set_name,
                                                   image_name)) {
                const ArtRef art = resolve_art(set_name, image_name);
                if (art.set == nullptr) continue;
                item.glyph.set = art.set;
                item.glyph.material = art.material;
                item.glyph.image = image_name;
                item.glyph_x = child_top_left.x.offset;
                item.glyph_y = child_top_left.y.offset;
                item.glyph_w = child_bottom_right.x.offset - child_top_left.x.offset;
                item.glyph_h = child_bottom_right.y.offset - child_top_left.y.offset;
                // Above the border strip, not in it: the strip shares the overlay and would
                // cover the glyph.
                item.glyph.panel =
                    make_panel("tl/menu/bar/glyph" + std::to_string(serial), buttons_overlay_);
                item.glyph.panel->setMaterial(art.material);
            } else if (child.name == group.caption) {
                item.text_x = child_top_left.x.offset;
                item.text_y = child_top_left.y.offset;
                item.text_h = child_bottom_right.y.offset - child_top_left.y.offset;
                auto& overlays = Ogre::OverlayManager::getSingleton();
                const std::string name = "tl/menu/bar/text" + std::to_string(serial);
                item.label = static_cast<Ogre::TextAreaOverlayElement*>(
                    overlays.createOverlayElement("TextArea", name));
                item.label->setMetricsMode(Ogre::GMM_RELATIVE);
                item.label->setFontName(font_, Ogre::RGN_INTERNAL);
                item.label->setCaption(child.property("Text", std::string()));
                float r = 1.0f, g = 0.85f, b = 0.45f, a = 1.0f;
                if (authored_colour) {
                    tl::formats::parse_colour(child.property("TextColour", std::string()), r, g, b,
                                              a);
                }
                item.label->setColour(Ogre::ColourValue(r, g, b, a));
                text_root_->addChild(item.label);
            }
        }
        if (item.glyph.panel != nullptr || item.label != nullptr) bar_items_.push_back(item);
    }
}

void MainMenu::clear_bar() {
    auto& overlays = Ogre::OverlayManager::getSingleton();
    for (BarItem& item : bar_items_) {
        if (item.label != nullptr) {
            if (text_root_ != nullptr) text_root_->removeChild(item.label->getName());
            overlays.destroyOverlayElement(item.label);
        }
        if (item.glyph.panel != nullptr && buttons_overlay_ != nullptr) {
            buttons_overlay_->remove2D(item.glyph.panel);
            overlays.destroyOverlayElement(item.glyph.panel);
        }
    }
    bar_items_.clear();
}

void MainMenu::add_button(const std::string& text, MenuAction action, int slot,
                          const std::string& data, int floor, float row_offset) {
    auto& overlays = Ogre::OverlayManager::getSingleton();
    Button button;
    button.text = text;
    button.action = action;
    button.slot = slot;
    button.row_offset = row_offset;
    button.data = data;
    button.floor = floor;

    // Unique across the menu's lifetime, not reused after `clear_buttons()`, so a rebuilt button
    // never collides in OGRE's overlay-element namespace.
    const std::string name = "tl/menu/button/" + std::to_string(button_serial_++);
    button.piece.set = &gui_set_;
    button.piece.image = button_normal_;
    button.piece.panel = make_panel(name, buttons_overlay_);
    button.piece.panel->setMaterial(gui_material_);

    auto make_label = [&](const char* suffix, const Ogre::ColourValue& colour) {
        auto* label = static_cast<Ogre::TextAreaOverlayElement*>(
            overlays.createOverlayElement("TextArea", name + suffix));
        label->setMetricsMode(Ogre::GMM_RELATIVE);
        label->setFontName(font_, Ogre::RGN_INTERNAL);
        label->setAlignment(Ogre::TextAreaOverlayElement::Center);
        label->setColour(colour);
        label->setCaption(text);
        // Inside the text container, not the overlay: `add2D` takes a
        // container and a `TextArea` is not one. A child's position is *added*
        // to its parent's derived position rather than scaled by it, and the
        // container sits at the origin, so these stay absolute.
        text_root_->addChild(label);
        return label;
    };
    // The drop first, so the white sits over it. The suffixes are ordered on purpose: an
    // `OverlayContainer` keeps its children in a `std::map` keyed by name, so siblings draw in
    // name order.
    button.shadow = make_label("/a-drop", Ogre::ColourValue(0.0f, 0.0f, 0.0f, 1.0f));
    button.label = make_label("/b-text", Ogre::ColourValue::White);
    buttons_.push_back(button);
}

// One console button, at the rectangle `mainmenu.uilayout` gives the window of
// that name and with the caption its `StaticTextOutline` child carries.
//
// The column's own container (`i_402`) is flattened into these coordinates by
// `layout()`; the rectangles here are the button's within it.
void MainMenu::add_console_button(const char* window_name, const std::string& text,
                                  MenuAction action) {
    const tl::formats::GuiWindow* window = console_.find(window_name);
    if (window == nullptr) {
        TL_LOG_WARN("menu: no window '%s' in %s", window_name, kConsoleMenuFile);
        return;
    }
    // The caption is the button's `StaticTextOutline` child, read from the layout so the
    // original's wording stays; the buttons this engine replaces pass their own text.
    std::string caption = text;
    if (caption.empty()) {
        for (const tl::formats::GuiWindow& child : window->children) {
            const std::string& authored = child.property("Text", std::string());
            if (!authored.empty()) {
                caption = authored;
                break;
            }
        }
    }
    tl::formats::UVector2 top_left{};
    tl::formats::UVector2 bottom_right{};
    if (!parse_area(window->property("UnifiedAreaRect", std::string()), top_left, bottom_right)) {
        TL_LOG_WARN("menu: '%s' has no usable rectangle", window_name);
        return;
    }

    add_button(caption, action, /*slot=*/0);
    Button& button = buttons_.back();
    button.col_x = top_left.x.offset;
    button.col_y = top_left.y.offset;
    button.col_w = bottom_right.x.offset - top_left.x.offset;
    button.col_h = bottom_right.y.offset - top_left.y.offset;
    // The column's Normal and Hover images, as the layout names them on every one of these
    // windows (`resolve_console_buttons`).
    button.piece.set = console_button_art_.set;
    button.piece.material = console_button_art_.material;
    button.piece.image = console_normal_;
    if (button.piece.panel != nullptr) button.piece.panel->setMaterial(button.piece.material);
}

// The Choose Level lists and the wardrobe editor are this engine's own screens, with no window
// in the console layout, so their buttons are stacked down the console column on the authored
// slots: the first slot's top (`ContinueLast`), the second's for the pitch (`NewCharacter`), the
// last slot's bottom (`ExitGame`) as the floor. Nine fit; a longer list shares the span.
void MainMenu::stack_console_list() {
    if (!console_mode_ || buttons_.empty()) return;

    float x = 2.0f, top = -1.0f, w = 269.0f, h = 57.0f, pitch = 60.0f, bottom = 537.0f;
    tl::formats::UVector2 a{}, b{};
    const std::string empty;
    if (const tl::formats::GuiWindow* first = console_.find("ContinueLast");
        first != nullptr && parse_area(first->property("UnifiedAreaRect", empty), a, b)) {
        x = a.x.offset;
        top = a.y.offset;
        w = b.x.offset - a.x.offset;
        h = b.y.offset - a.y.offset;
        if (const tl::formats::GuiWindow* second = console_.find("NewCharacter");
            second != nullptr && parse_area(second->property("UnifiedAreaRect", empty), a, b)) {
            pitch = a.y.offset - top;
        }
    }
    if (const tl::formats::GuiWindow* last = console_.find("ExitGame");
        last != nullptr && parse_area(last->property("UnifiedAreaRect", empty), a, b)) {
        bottom = b.y.offset;
    }

    const float authored_h = h;
    const float gap = pitch - h;
    const float n = static_cast<float>(buttons_.size());
    if (top + pitch * (n - 1.0f) + h > bottom) {
        pitch = (bottom - top + gap) / n;
        h = pitch - gap;
    }

    for (size_t i = 0; i < buttons_.size(); ++i) {
        Button& button = buttons_[i];
        button.col_x = x;
        button.col_y = top + pitch * static_cast<float>(i);
        button.col_w = w;
        button.col_h = h;
        button.caption_scale = authored_h > 0.0f ? std::min(1.0f, h / authored_h) : 1.0f;
        button.piece.set = console_button_art_.set;
        button.piece.material = console_button_art_.material;
        button.piece.image = console_normal_;
        if (button.piece.panel != nullptr) button.piece.panel->setMaterial(button.piece.material);
    }
}

void MainMenu::clear_buttons() {
    clear_editor_info();
    clear_extras();
    difficulty_mode_ = false;
    difficulty_state_ = nullptr;
    editor_mode_ = false;
    step_held_ = 0;
    if (buttons_.empty()) return;
    auto& overlays = Ogre::OverlayManager::getSingleton();
    for (Button& button : buttons_) {
        for (Ogre::OverlayElement* label : {static_cast<Ogre::OverlayElement*>(button.shadow),
                                            static_cast<Ogre::OverlayElement*>(button.label)}) {
            if (label == nullptr) continue;
            if (text_root_ != nullptr) text_root_->removeChild(label->getName());
            overlays.destroyOverlayElement(label);
        }
        if (button.piece.panel != nullptr && buttons_overlay_ != nullptr) {
            buttons_overlay_->remove2D(button.piece.panel);
            overlays.destroyOverlayElement(button.piece.panel);
        }
    }
    buttons_.clear();
}

void MainMenu::clear_editor_info() {
    auto& overlays = Ogre::OverlayManager::getSingleton();
    for (Ogre::TextAreaOverlayElement** text : {&editor_info_shadow_, &editor_info_}) {
        if (*text == nullptr) continue;
        if (text_root_ != nullptr) text_root_->removeChild((*text)->getName());
        overlays.destroyOverlayElement(*text);
        *text = nullptr;
    }
}

// A debug editor (`Renderer::open_wardrobe`, `Renderer::open_bestiary`): one
// row per thing it steps, then Back. A screen of this engine's own, like the
// Choose Level list, so its look is borrowed from the buttons around it.
void MainMenu::show_editor(const std::vector<std::string>& rows, const std::string& back) {
    if (buttons_overlay_ == nullptr || text_root_ == nullptr) return;
    clear_buttons();
    list_mode_ = true;
    editor_mode_ = true;

    int slot = 0;
    for (const std::string& caption : rows) {
        add_button(caption, MenuAction::EditorStep, slot++);
    }
    if (!back.empty()) add_button(back, MenuAction::Back, slot);
    stack_console_list();

    // The text block beside the column, white over a black drop as every
    // caption here is. Named so the drop sorts first (see `add_button`).
    auto& overlays = Ogre::OverlayManager::getSingleton();
    const std::string name = "tl/menu/editor/" + std::to_string(button_serial_++);
    auto make_text = [&](const char* suffix, const Ogre::ColourValue& colour) {
        auto* text = static_cast<Ogre::TextAreaOverlayElement*>(
            overlays.createOverlayElement("TextArea", name + suffix));
        text->setMetricsMode(Ogre::GMM_RELATIVE);
        text->setFontName(font_, Ogre::RGN_INTERNAL);
        text->setAlignment(Ogre::TextAreaOverlayElement::Left);
        text->setColour(colour);
        text_root_->addChild(text);
        return text;
    };
    editor_info_shadow_ = make_text("/a-drop", Ogre::ColourValue(0.0f, 0.0f, 0.0f, 1.0f));
    editor_info_ = make_text("/b-text", Ogre::ColourValue::White);

    selected_ = 0;
    apply_button_look();
    layout();
}

void MainMenu::set_row_caption(size_t row, const std::string& text) {
    if (row >= buttons_.size()) return;
    Button& button = buttons_[row];
    button.text = text;
    for (Ogre::TextAreaOverlayElement* label : {button.shadow, button.label}) {
        if (label != nullptr) label->setCaption(text);
    }
}

void MainMenu::set_editor_info(const std::string& text) {
    for (Ogre::TextAreaOverlayElement* label : {editor_info_shadow_, editor_info_}) {
        if (label != nullptr) label->setCaption(text);
    }
}

void MainMenu::layout_editor_info(float across, float down) {
    if (editor_info_ == nullptr) return;
    const float height = kEditorInfoPoints * points_to_char_pixels() * scale_;
    const float left = across - kEditorInfoFromRight * scale_;
    const float top = kEditorInfoTop * scale_;
    for (Ogre::TextAreaOverlayElement* label : {editor_info_shadow_, editor_info_}) {
        if (label == nullptr) continue;
        const bool is_shadow = (label == editor_info_shadow_);
        const float dx = is_shadow ? kLabelShadow * scale_ : 0.0f;
        label->setCharHeight(height / down);
        label->setPosition((left + dx) / across, (top + dx) / down);
        label->show();
    }
}

void MainMenu::show_biome_list(const std::vector<std::pair<std::string, std::string>>& biomes) {
    if (buttons_overlay_ == nullptr) return;
    clear_buttons();
    list_mode_ = true;
    chosen_biome_.clear();

    int slot = 0;
    for (const auto& [key, label] : biomes) {
        add_button(label, MenuAction::ChooseLevelBiome, slot++, key);
    }
    add_button("Back", MenuAction::Back, slot);
    stack_console_list();

    selected_ = 0;
    apply_button_look();
    layout();
}

// PORT: the port's own Choose Level list drawn as buttons.
void MainMenu::show_floor_list(const std::vector<std::pair<std::string, int>>& entries) {
    if (buttons_overlay_ == nullptr) return;
    clear_buttons();
    list_mode_ = true;
    chosen_dungeon_.clear();
    chosen_dungeon_floor_ = 1;

    int slot = 0;
    for (const auto& [label, floor] : entries) {
        add_button(label, MenuAction::LoadDungeon, slot++, "Main", floor);
    }
    add_button("Back", MenuAction::Back, slot);
    stack_console_list();

    selected_ = 0;
    apply_button_look();
    layout();
}

// The console's column (show_main_buttons()). The port's debug tools - the
// wardrobe editor, Choose Level and the bestiary, in the Achievements,
// Leaderboards and Unlock Full Game slots - are offered only with the debug
// tools on (config.txt); without them the buttons below move up into the slots
// they leave, so the column has no gaps. With them on it is the layout's own.
void MainMenu::add_console_column() {
    struct Row {
        const char* window;
        const char* text;
        MenuAction action;
        bool offered;
    };
    const bool debug = tl::core::debug_tools();
    const Row column[] = {
        {"ContinueLast", "", MenuAction::Continue, continue_available_},
        {"NewCharacter", "", MenuAction::NewGame, true},
        {"LoadCharacter", "", MenuAction::LoadCharacter, true},
        {"Achievements", "Wardrobe", MenuAction::Wardrobe, debug},
        {"Leaderboards", "Choose Level", MenuAction::ChooseLevel, debug},
        {"Help", "", MenuAction::Help, true},
        {"i_415", "", MenuAction::Options, true},
        {"UnlockFullGame", "Bestiary", MenuAction::Bestiary, debug},
        {"ExitGame", "Quit", MenuAction::Quit, true},
    };
    // The slots: the tops of the rows the column has with the tools on.
    std::vector<float> tops;
    for (const Row& row : column) {
        if (row.action == MenuAction::Continue && !continue_available_) continue;
        const tl::formats::GuiWindow* window = console_.find(row.window);
        tl::formats::UVector2 top_left{};
        tl::formats::UVector2 bottom_right{};
        if (window != nullptr && parse_area(window->property("UnifiedAreaRect", std::string()),
                                            top_left, bottom_right)) {
            tops.push_back(top_left.y.offset);
        }
    }
    size_t next = 0;
    for (const Row& row : column) {
        if (!row.offered) continue;
        const size_t before = buttons_.size();
        add_console_button(row.window, row.text, row.action);
        if (buttons_.size() == before) continue;
        if (next < tops.size()) buttons_.back().col_y = tops[next];
        ++next;
    }
}

void MainMenu::show_main_buttons() {
    if (buttons_overlay_ == nullptr) return;
    clear_buttons();
    list_mode_ = false;

    if (console_mode_) {
        // The console's own column, in the layout's order and slots. `UnlockFullGame` is hidden
        // (`CMainMenu`'s builder does in the full game) and `ContinueLast` shows only with
        // something to continue. `Wardrobe` and `Choose Level` stand in the Xbox Live slots
        // (Achievements, Leaderboards) and `Bestiary` in `UnlockFullGame`'s. `LoadCharacter` is
        // shown whenever there is somewhere to save (`CMainMenu::update`).
        // `ExitGame` keeps its slot with the port's own wording.
        add_console_column();

        selected_ = 0;
        apply_button_look();
        layout();
        return;
    }

    // The same two the original screen has anything behind - see the note on
    // where they sit, in layout().
    add_button(menu_.find("NewGame") != nullptr
                   ? menu_.find("NewGame")->property("Text", std::string("New Character"))
                   : std::string("New Character"),
               MenuAction::NewGame, 1);
    add_button(menu_.find("ExitGame") != nullptr
                   ? menu_.find("ExitGame")->property("Text", std::string("Quit Game"))
                   : std::string("Quit Game"),
               MenuAction::Quit, 2);
    // Slot 0, the outer slot the pair leaves empty: the debug wardrobe editor.
    // Slot 3, the other outer slot: the real game's floors (Renderer::list_main_floors()).
    // Both only with the debug tools on (config.txt).
    const bool debug = tl::core::debug_tools();
    if (debug) {
        add_button("Wardrobe", MenuAction::Wardrobe, 0);
        add_button("Choose Level", MenuAction::ChooseLevel, 3);
    }
    // `ContinueLast`, authored above the row's right end (`{{.5,251},{1,-140}}`),
    // shown only while there is a character to continue.
    const tl::formats::GuiWindow* continue_window = menu_.find("ContinueLast");
    float raised = -140.0f;
    {
        const std::string empty;
        tl::formats::UVector2 at{};
        if (continue_window != nullptr &&
            tl::formats::parse_uvector2(continue_window->property("UnifiedPosition", empty), at)) {
            raised = at.y.offset;
        }
    }
    // The debug bestiary, this engine's own: above Wardrobe, at the height the
    // layout authors for Continue over the other end of the row.
    if (debug) add_button("Bestiary", MenuAction::Bestiary, 0, {}, 0, raised);
    if (continue_available_) {
        add_button(continue_window != nullptr
                       ? continue_window->property("Text", std::string("Continue"))
                       : std::string("Continue"),
                   MenuAction::Continue, 3, {}, 0, raised);
    }

    selected_ = 0;
    apply_button_look();
    layout();
}

// The copyright line, bottom right, where the layout places it: the
// console screen's own, else the PC row's. The console's text is not
// the layout's placeholder ("Copyright 2010"): `CMainMenu::createMenus`
// sets it to the string a static
// initialiser built.
void MainMenu::build_copyright() {
    const tl::formats::GuiWindow* copyright =
        console_mode_ ? console_.find("CopyrightInfo") : nullptr;
    const bool console_copyright = copyright != nullptr;
    if (copyright == nullptr) copyright = menu_.find("CopyrightInfo");
    if (const tl::formats::GuiWindow* window = copyright) {
        copyright_text_ = console_copyright ? std::string(kConsoleCopyright)
                                            : window->property("Text", std::string());
        copyright_ = static_cast<Ogre::TextAreaOverlayElement*>(
            Ogre::OverlayManager::getSingleton().createOverlayElement("TextArea",
                                                                      "tl/menu/copyright"));
        copyright_->setMetricsMode(Ogre::GMM_RELATIVE);
        copyright_->setFontName(font_, Ogre::RGN_INTERNAL);
        copyright_->setAlignment(Ogre::TextAreaOverlayElement::Right);
        copyright_->setCaption(copyright_text_);
        float r = 1.0f, g = 1.0f, b = 1.0f, a = 0.76f;
        tl::formats::parse_colour(window->property("TextColour", std::string("c2FFFFFF")), r, g, b,
                                  a);
        copyright_->setColour(Ogre::ColourValue(r, g, b, a));
        text_root_->addChild(copyright_);
    }
}

bool MainMenu::create(const std::string& group, unsigned width, unsigned height) {
    destroy();
    width_ = width;
    height_ = height;
    group_ = group;

    // The console screen when its layout and art are there (the Xbox pak's own, or a PC pak's
    // `console_ui`); only the PC row needs `logo.imageset`, `hud.imageset` and
    // `mainmenuframe.layout`.
    console_mode_ = load_console_menu(group);
    // A PC pak drawing the console screen from the port's pack: the PC's own sets are mounted
    // too, and supply what the pack leaves out (the logo bars, the buttons, the panel looks).
    pc_art_ = false;
    if (console_mode_ && !console_game_data() && load_imagesets(group)) {
        logo_material_ = material_for(logo_set_, group, logo_texture_);
        gui_material_ = material_for(gui_set_, group, gui_texture_);
        pc_art_ = logo_material_ && gui_material_;
    }
    if (console_mode_) resolve_console_buttons();
    std::vector<uint8_t> bytes;
    if (read_menu_resource(kMenuLayoutFile, group, bytes)) {
        menu_ = tl::formats::parse_gui_layout(bytes.data(), bytes.size());
    }
    if (!console_mode_) {
        if (!menu_.ok) {
            TL_LOG_WARN("menu: no usable %s in the archive", kMenuLayoutFile);
            return false;
        }
        if (!load_imagesets(group)) return false;
        logo_material_ = material_for(logo_set_, group, logo_texture_);
        gui_material_ = material_for(gui_set_, group, gui_texture_);
        if (!logo_material_ || !gui_material_) return false;
    }
    if (!load_font(group)) return false;

    button_normal_ = kButtonNormal;
    button_hover_ = kButtonHover;

    try {
        auto& overlays = Ogre::OverlayManager::getSingleton();
        // Three layers, bottom to top, all below the HUD's 500. See the note in
        // menu.h.
        frame_ = overlays.create("tl/menu/frame");
        frame_->setZOrder(400);
        buttons_overlay_ = overlays.create("tl/menu/buttons");
        buttons_overlay_->setZOrder(401);
        text_overlay_ = overlays.create("tl/menu/labels");
        text_overlay_->setZOrder(402);

        text_root_ = static_cast<Ogre::PanelOverlayElement*>(
            overlays.createOverlayElement("Panel", "tl/menu/text"));
        text_root_->setMetricsMode(Ogre::GMM_RELATIVE);
        text_root_->setPosition(0.0f, 0.0f);
        text_root_->setDimensions(1.0f, 1.0f);
        // No material, and none wanted: a transparent panel draws nothing of
        // its own and still updates its children.
        text_root_->setTransparent(true);
        text_overlay_->add2D(text_root_);

        // The frame, in the order the layout declares it. `TopBarB` and
        // `BottomBarB` are containers whose only job is to centre the 1024-wide
        // strip inside them, so the image they draw is their child's; the four
        // corner pieces cover what the strip does not reach on a wide display.
        if (console_mode_) {
            build_console_frame();
            build_console_bar();
        } else {
            add_piece("TopBarB", "topbar", /*centred=*/true);
            add_piece("TopBarLeft", nullptr, false);
            add_piece("TopBarRight", nullptr, false);
            add_piece("BottomBarB", "bottombar", /*centred=*/true);
            add_piece("BottomBarLeft", nullptr, false);
            add_piece("BottomBarRight", nullptr, false);
        }

        // New Character and Quit Game take the middle two of the layout's four bottom-row slots
        // (`ContinueGame` at -251, `Settings` at +5), keeping the authored row, size and pitch
        // while the pair stays centred; Wardrobe takes a spare outer slot (show_main_buttons()).
        show_main_buttons();

        build_copyright();

        created_ = true;
        // Whatever is held as the menu opens - the A that chose Back on the
        // screen before - is not a press on this one. Every button has to be
        // let go first.
        confirm_held_ = quit_held_ = skip_held_ = nav_held_ = true;
        // The first button has the focus from the start: `CMainMenu::vfunc_7`
        // focuses the head of its tab list as the menu shows.
        selected_ = 0;
        layout();
        show();
        TL_LOG_INFO("menu: %zu frame pieces, %zu buttons, at %u x %u", pieces_.size(),
                    buttons_.size(), width_, height_);
        return true;
    } catch (const Ogre::Exception& e) {
        TL_LOG_WARN("menu: %s", e.getDescription().c_str());
        destroy();
        return false;
    }
}

// The console column, and the bar of button prompts under it. The column's container (`i_402`,
// `{{-0.45,138},{0,138},...}`) cannot take its horizontal scale as authored (it would be off the
// left edge); everything else on the screen is the layout's.
void MainMenu::layout_console_buttons(float across, float down) {
    float column_left = 0.0f;
    float column_top = 0.0f;
    console_column_origin(across, down, column_left, column_top);

    for (Button& button : buttons_) {
        const float left = column_left + button.col_x * scale_;
        const float top = column_top + button.col_y * scale_;
        const float w = button.col_w * scale_;
        const float h = button.col_h * scale_;

        if (button.piece.panel != nullptr) {
            button.piece.panel->setPosition(left / across, top / down);
            button.piece.panel->setDimensions(w / across, h / down);
            set_uv(button.piece.panel,
                   button.piece.set != nullptr ? *button.piece.set : widgets1_art_.set,
                   button.piece.image);
            button.piece.panel->show();
        }

        // `SerifBig`, centred in the button - the caption's own window is
        // `{{0,4},{0,15},{0,259},{0,48}}` inside it, which is the middle band.
        font_height_ =
            kConsoleCaptionPoints * points_to_char_pixels() * scale_ * button.caption_scale;
        const float relative_height = font_height_ / down;
        const float centre_x = (left + w * 0.5f) / across;
        const float label_top = (top + h * 0.5f) / down - relative_height * 0.62f;
        for (Ogre::TextAreaOverlayElement* label : {button.shadow, button.label}) {
            if (label == nullptr) continue;
            const bool is_shadow = (label == button.shadow);
            const float dx = is_shadow ? kLabelShadow * scale_ / across : 0.0f;
            const float dy = is_shadow ? kLabelShadow * scale_ / down : 0.0f;
            label->setCharHeight(relative_height);
            label->setPosition(centre_x + dx, label_top + dy);
            label->show();
        }
    }
}

// `CopyrightInfo`, which the console layout puts at {{0,240},{0,653}} - inside
// the bottom bar rather than against the screen's corner, where the PC screen
// has it.
// The prompt bar's glyphs and words, against the bottom of the screen: the
// strip they sit in is 93 authored units tall and anchored there.
void MainMenu::layout_console_bar(float across, float down) {
    const float strip_top = down - kConsoleBottomBar * scale_;
    // The strip is 1024 authored units wide and centred, the way every one of
    // these borders is; its contents ride with it rather than with the screen's
    // own left edge.
    const float strip_left = (across - kConsoleWidth * scale_) * 0.5f;
    for (BarItem& item : bar_items_) {
        if (item.glyph.panel != nullptr) {
            const float left = strip_left + (item.x + item.glyph_x) * scale_;
            const float top = strip_top + (item.y + item.glyph_y) * scale_;
            item.glyph.panel->setPosition(left / across, top / down);
            item.glyph.panel->setDimensions(item.glyph_w * scale_ / across,
                                            item.glyph_h * scale_ / down);
            set_uv(item.glyph.panel, *item.glyph.set, item.glyph.image);
            item.glyph.panel->show();
        }
        if (item.label != nullptr) {
            // Centred in its own authored box, which is what the layout asks
            // for (`VertFormatting: CentreAligned` on the Select label, and the
            // other two are boxes of the line's own height anyway).
            const float height = kConsoleSmallPoints * points_to_char_pixels() * scale_;
            const float box_top = strip_top + (item.y + item.text_y) * scale_;
            const float centred = box_top + (item.text_h * scale_ - height) * 0.5f;
            item.label->setCharHeight(height / down);
            item.label->setPosition((strip_left + (item.x + item.text_x) * scale_) / across,
                                    centred / down);
            item.label->show();
        }
    }
}

void MainMenu::layout_console_copyright(float across, float down) {
    if (copyright_ == nullptr) return;
    const tl::formats::GuiWindow* window = console_.find("CopyrightInfo");
    if (window == nullptr) return;
    tl::formats::UVector2 top_left{};
    tl::formats::UVector2 bottom_right{};
    if (!parse_area(window->property("UnifiedAreaRect", std::string()), top_left, bottom_right)) {
        return;
    }
    // Straight off the layout: the whole screen is 768 authored units tall and
    // scaled by `height / 768`, so an authored y is just `y * scale_`.
    const float height = kConsoleSmallPoints * points_to_char_pixels() * scale_;
    copyright_->setAlignment(Ogre::TextAreaOverlayElement::Left);
    copyright_->setCharHeight(height / down);
    // Left-aligned at the authored rectangle's right offset (704), where the console draws the
    // text's left edge; its authored left (240) would sit under the button column.
    copyright_->setPosition(bottom_right.x.offset * scale_ / across,
                            top_left.y.offset * scale_ / down);
    copyright_->show();
}

void MainMenu::destroy() {
    if (frame_ != nullptr || buttons_overlay_ != nullptr || text_overlay_ != nullptr) {
        auto& overlays = Ogre::OverlayManager::getSingleton();
        hide();

        // Text first, out of its container: an element that is still parented
        // cannot be destroyed.
        auto drop_text = [&](Ogre::OverlayElement* element) {
            if (element == nullptr) return;
            if (text_root_ != nullptr) text_root_->removeChild(element->getName());
            overlays.destroyOverlayElement(element);
        };
        for (Button& button : buttons_) {
            drop_text(button.shadow);
            drop_text(button.label);
        }
        for (BarItem& item : bar_items_) drop_text(item.label);
        clear_extras();
        drop_text(editor_info_shadow_);
        drop_text(editor_info_);
        drop_text(copyright_);
        if (text_root_ != nullptr && text_overlay_ != nullptr) {
            text_overlay_->remove2D(text_root_);
            overlays.destroyOverlayElement(text_root_);
        }
        text_root_ = nullptr;

        auto drop = [&](Ogre::Overlay* overlay, Ogre::OverlayElement* element) {
            if (element == nullptr || overlay == nullptr) return;
            overlay->remove2D(static_cast<Ogre::OverlayContainer*>(element));
            overlays.destroyOverlayElement(element);
        };
        for (Piece& piece : pieces_) drop(frame_, piece.panel);
        for (Button& button : buttons_) drop(buttons_overlay_, button.piece.panel);
        // The bar's glyphs live in the button overlay, above the border strip.
        for (BarItem& item : bar_items_) drop(buttons_overlay_, item.glyph.panel);

        for (Ogre::Overlay* overlay : {frame_, buttons_overlay_, text_overlay_}) {
            if (overlay != nullptr) overlays.destroy(overlay);
        }
    }
    pieces_.clear();
    buttons_.clear();
    bar_items_.clear();
    console_mode_ = false;
    difficulty_mode_ = false;
    difficulty_state_ = nullptr;
    difficulty_ = tl::formats::GuiLayout{};
    difficulty_tabs_.clear();
    difficulty_button_.clear();
    difficulty_column_path_.clear();
    difficulty_title_path_.clear();
    widgets_art_ = ArtSet{};
    widgets1_art_ = ArtSet{};
    widgets2_art_ = ArtSet{};
    pc_art_ = false;
    console_button_art_ = ArtRef{};
    checkbox_art_ = ArtRef{};
    copyright_ = nullptr;
    editor_info_ = nullptr;
    editor_info_shadow_ = nullptr;
    editor_mode_ = false;
    frame_ = nullptr;
    buttons_overlay_ = nullptr;
    text_overlay_ = nullptr;
    logo_material_.reset();
    gui_material_.reset();
    logo_texture_.reset();
    gui_texture_.reset();
    created_ = false;
}

void MainMenu::resize(unsigned width, unsigned height) {
    width_ = width;
    height_ = height;
    layout();
}

void MainMenu::show() {
    for (Ogre::Overlay* overlay : {frame_, buttons_overlay_, text_overlay_}) {
        if (overlay != nullptr) overlay->show();
    }
}

void MainMenu::hide() {
    for (Ogre::Overlay* overlay : {frame_, buttons_overlay_, text_overlay_}) {
        if (overlay != nullptr) overlay->hide();
    }
}

void MainMenu::layout() {
    if (!created_ || frame_ == nullptr) return;

    const float across = static_cast<float>(width_ > 0 ? width_ : 1);
    const float down = static_cast<float>(height_ > 0 ? height_ : 1);

    // Every authored pixel offset is scaled by the viewport's height over the interface's native
    // height, uniformly in both axes: `CGame::frameEnded` stores `XRATIO = width / 1024` and
    // `YRATIO = height / 768`, and
    // `CGameUI::convertToScreenScale(CEGUI::Window*, bool useX)` multiplies each window's UDim
    // offsets (not the relative scales) by one of them; `CMainMenu::createMenus()` passes false.
    // `UDim::resolve()` in formats/cegui.h is the same arithmetic.
    // The console art's height (`widgets1`, 768) on the console screen.
    scale_ = down / authored_height(console_mode_ ? widgets1_art_.set : logo_set_, kNativeHeight);

    for (Piece& piece : pieces_) {
        if (piece.panel == nullptr || piece.set == nullptr) continue;
        const tl::formats::ImagesetImage* image = piece.set->find(piece.image);
        if (image == nullptr) continue;

        const float w = piece.size.x.resolve(across, scale_);
        const float h = piece.size.y.resolve(down, scale_);
        (void)image;
        // `TopBarB` at {{.5,0},{0,0}} holding `TopBar` at {{-.5,0},...} centres a 1024-wide
        // strip; collapsed into one placement here.
        const float left =
            piece.centred ? (across - w) * 0.5f : piece.position.x.resolve(across, scale_);
        const float top = piece.position.y.resolve(down, scale_);

        piece.panel->setPosition(left / across, top / down);
        piece.panel->setDimensions(w / across, h / down);

        set_uv(piece.panel, *piece.set, piece.image);
        piece.panel->show();
    }

    layout_editor_info(across, down);
    if (console_mode_) {
        layout_console_buttons(across, down);
        layout_extras(across, down);
        apply_button_look();
        // `difficulty.uilayout` has no `CopyrightInfo`.
        if (difficulty_mode_) {
            if (copyright_ != nullptr) copyright_->hide();
        } else {
            layout_console_copyright(across, down);
        }
        layout_console_bar(across, down);
        return;
    }

    // The button row, from the layout's own bottom-row geometry.
    const std::string empty;
    tl::formats::UVector2 slot_position{};
    tl::formats::UVector2 slot_size{};
    bool have_row = false;
    if (const tl::formats::GuiWindow* window = menu_.find("NewGame")) {
        have_row = tl::formats::parse_uvector2(window->property("UnifiedPosition", empty),
                                               slot_position) &&
                   tl::formats::parse_uvector2(window->property("UnifiedSize", empty), slot_size);
    }
    if (!have_row) {
        slot_position.x.scale = 0.5f;
        slot_position.x.offset = -507.0f;
        slot_position.y.scale = 1.0f;
        slot_position.y.offset = -64.0f;
        slot_size.x.offset = 246.0f;
        slot_size.y.offset = 44.0f;
    }
    // The pitch between slots, taken from the two the layout spaces evenly:
    // `NewGame` at -507 and `ContinueGame` at -251.
    float pitch = 256.0f;
    if (const tl::formats::GuiWindow* window = menu_.find("ContinueGame")) {
        tl::formats::UVector2 second{};
        if (tl::formats::parse_uvector2(window->property("UnifiedPosition", empty), second)) {
            pitch = second.x.offset - slot_position.x.offset;
        }
    }

    float button_w = slot_size.x.resolve(across, scale_);
    const float button_h = slot_size.y.resolve(down, scale_);
    const float row_top = slot_position.y.resolve(down, scale_);
    float first_left = slot_position.x.resolve(across, scale_);

    // The Choose Level lists hold more buttons than any authored row, so they are spaced evenly,
    // centred and narrow enough to fit (gated on `list_mode_`, not the count). The wardrobe
    // editor is a column instead (below).
    if (list_mode_ && !editor_mode_) {
        const float margin = across * 0.03f;
        const float available = across - margin * 2.0f;
        const float gap = 8.0f * scale_;
        const size_t n = buttons_.size();
        const float fitted = (available - gap * static_cast<float>(n - 1)) / static_cast<float>(n);
        button_w = std::max(60.0f * scale_, fitted);
        pitch = (button_w + gap) / scale_;
        first_left = margin;
    }

    for (Button& button : buttons_) {
        float left = first_left + pitch * static_cast<float>(button.slot) * scale_;
        float top = button.row_offset != 0.0f ? down + button.row_offset * scale_ : row_top;
        if (editor_mode_) {
            left = kEditorColumnLeft * scale_;
            top = kEditorColumnTop * scale_ +
                  (button_h + kEditorColumnGap * scale_) * static_cast<float>(button.slot);
        }

        if (button.piece.panel != nullptr) {
            button.piece.panel->setPosition(left / across, top / down);
            button.piece.panel->setDimensions(button_w / across, button_h / down);
            set_uv(button.piece.panel, gui_set_, button.piece.image);
            button.piece.panel->show();
        }

        // The label, centred in the button, at the authored 11 points converted
        // to pixels and scaled the same way everything else is.
        font_height_ = kFontSize * points_to_char_pixels() * scale_;
        const float relative_height = font_height_ / down;
        const float centre_x = (left + button_w * 0.5f) / across;
        // OGRE lays a TextArea out from the top of its first line; 0.62 rather than 0.5 because
        // OGRE's char height is the full em box and the cap height sits above the baseline.
        const float label_top = (top + button_h * 0.5f) / down - relative_height * 0.62f;
        for (Ogre::TextAreaOverlayElement* label : {button.shadow, button.label}) {
            if (label == nullptr) continue;
            const bool is_shadow = (label == button.shadow);
            const float dx = is_shadow ? kLabelShadow * scale_ / across : 0.0f;
            const float dy = is_shadow ? kLabelShadow * scale_ / down : 0.0f;
            label->setCharHeight(relative_height);
            label->setPosition(centre_x + dx, label_top + dy);
            label->show();
        }
    }
    apply_button_look();

    if (copyright_ != nullptr) {
        // `{{1,-460},{1,-20}}`, size 400x24, right aligned - so the text's
        // right edge is 60 authored pixels in from the corner.
        const float right = across - 60.0f * scale_;
        const float top = down - 20.0f * scale_;
        const float height = kSmallFontSize * points_to_char_pixels() * scale_;  // SerifSmall
        copyright_->setCharHeight(height / down);
        copyright_->setPosition(right / across, top / down);
        copyright_->show();
    }
}

void MainMenu::set_uv(Ogre::PanelOverlayElement* panel, const tl::formats::Imageset& set,
                      const std::string& image_name) const {
    if (panel == nullptr) return;
    const tl::formats::ImagesetImage* image = set.find(image_name);
    if (image == nullptr) return;
    // The texture's size is taken from what was loaded, so a modded set of another size lands
    // right.
    Ogre::TexturePtr texture = (&set == &gui_set_) ? gui_texture_ : logo_texture_;
    // The console screen's three sets are not the PC sheets' size; the wrong one stretches every
    // button.
    if (&set == &widgets_art_.set) texture = widgets_art_.texture;
    else if (&set == &widgets1_art_.set) texture = widgets1_art_.texture;
    else if (&set == &widgets2_art_.set) texture = widgets2_art_.texture;
    const float tw = texture ? static_cast<float>(texture->getWidth()) : 1024.0f;
    const float th = texture ? static_cast<float>(texture->getHeight()) : 1024.0f;
    panel->setUV(static_cast<float>(image->x) / tw, static_cast<float>(image->y) / th,
                 static_cast<float>(image->x + image->width) / tw,
                 static_cast<float>(image->y + image->height) / th);
}

void MainMenu::apply_button_look() {
    for (size_t i = 0; i < buttons_.size(); ++i) {
        Button& button = buttons_[i];
        if (button.piece.panel == nullptr) continue;
        if (console_mode_ && button.checkbox && button.piece.set == &widgets2_art_.set) {
            // `ceguiWidget/Checkbox`: `NormalImage` `checkbox_off`, `HoverImage`
            // `checkbox_on` (the look's own names: the lit box is the hovered
            // one, ticked or not); the tick is its `select_mark`, in `extras_`.
            button.piece.image =
                (static_cast<int>(i) == selected_) ? "checkbox_on" : "checkbox_off";
            set_uv(button.piece.panel, widgets2_art_.set, button.piece.image);
            continue;
        }
        if (console_mode_) {
            // The column's Normal and Hover, as the console layout's buttons name them.
            button.piece.image =
                (static_cast<int>(i) == selected_) ? console_hover_ : console_normal_;
            set_uv(button.piece.panel, *console_button_art_.set, button.piece.image);
            continue;
        }
        button.piece.image = (static_cast<int>(i) == selected_) ? button_hover_ : button_normal_;
        set_uv(button.piece.panel, gui_set_, button.piece.image);
    }
    // The tick is drawn only while the box is ticked (`SelectedNormal` /
    // `SelectedHover`).
    for (Extra& extra : extras_) {
        if (!extra.tick || extra.piece.panel == nullptr) continue;
        if (difficulty_state_ != nullptr && difficulty_state_->hardcore) {
            extra.piece.panel->show();
        } else {
            extra.piece.panel->hide();
        }
    }
}

void MainMenu::hold(const tl::core::InputState& input) {
    const bool column = console_mode_ || editor_mode_;
    nav_held_ = column ? std::abs(input.move_y) > 0.5f : std::abs(input.move_x) > 0.5f;
    quit_held_ = input.quit;
    skip_held_ = input.skip;
    confirm_held_ = input.confirm;
}

MenuAction MainMenu::update(const tl::core::InputState& input) {
    if (!created_ || buttons_.empty()) return MenuAction::None;
    if (difficulty_mode_ && difficulty_state_ != nullptr) return update_difficulty(input);

    const int previous = selected_;

    // The D-pad or the left stick, edge-triggered, wrapping at both ends: up and down on the
    // console's column (and the wardrobe editor), left and right on the PC row. `move_y` is
    // positive away from the viewer, so up steps towards the top.
    const bool column = console_mode_ || editor_mode_;
    const bool back = column ? input.move_y > 0.5f : input.move_x < -0.5f;
    const bool forward = column ? input.move_y < -0.5f : input.move_x > 0.5f;
    if (back || forward) {
        if (!nav_held_) {
            selected_ += forward ? 1 : -1;
            if (selected_ < 0) selected_ = static_cast<int>(buttons_.size()) - 1;
            if (selected_ >= static_cast<int>(buttons_.size())) selected_ = 0;
        }
        nav_held_ = true;
    } else {
        nav_held_ = false;
    }

    if (selected_ != previous) apply_button_look();

    // START (Escape on a keyboard) leaves; the same thing the Quit button does.
    // On the Choose Level lists and the wardrobe editor it backs out instead - the list is a
    // menu of its own, not a second way to quit - and so does B there.
    MenuAction action = MenuAction::None;
    if (input.quit && !quit_held_) action = list_mode_ ? MenuAction::Back : MenuAction::Quit;
    quit_held_ = input.quit;
    if (input.skip && !skip_held_ && list_mode_) action = MenuAction::Back;
    skip_held_ = input.skip;

    // A acts on the focused button. A press rather than a release, and only on
    // a transition: `confirm` is held for as long as the button is down and
    // this loop runs at 60 Hz.
    if (input.confirm && !confirm_held_ && selected_ >= 0 &&
        selected_ < static_cast<int>(buttons_.size())) {
        const Button& button = buttons_[static_cast<size_t>(selected_)];
        action = button.action;
        if (action == MenuAction::LoadDungeon) {
            chosen_dungeon_ = button.data;
            chosen_dungeon_floor_ = button.floor > 0 ? button.floor : 1;
        } else if (action == MenuAction::ChooseLevelBiome) {
            chosen_biome_ = button.data;
        } else if (action == MenuAction::EditorStep) {
            editor_row_ = button.slot;
            editor_step_ = 1;
        }
    }
    confirm_held_ = input.confirm;

    // A debug editor's sideways: left and right step the focused row by one,
    // repeating while held, and the bumpers by ten. Not on Back.
    if (editor_mode_ && action == MenuAction::None && selected_ >= 0 &&
        selected_ < static_cast<int>(buttons_.size()) &&
        buttons_[static_cast<size_t>(selected_)].action == MenuAction::EditorStep) {
        const int row = buttons_[static_cast<size_t>(selected_)].slot;
        const int side = input.move_x > 0.5f ? 1 : input.move_x < -0.5f ? -1 : 0;
        const double now = steady_seconds();
        int step = 0;
        if (side != 0 && side != step_held_) {
            step = side;
            step_repeat_at_ = now + kStepRepeatDelay;
        } else if (side != 0 && now >= step_repeat_at_) {
            step = side;
            step_repeat_at_ = now + kStepRepeatEvery;
        }
        step_held_ = side;
        const bool bumper = input.tab_prev || input.tab_next;
        if (bumper && !bumper_held_) step = input.tab_next ? 10 : -10;
        bumper_held_ = bumper;
        if (step != 0) {
            editor_row_ = row;
            editor_step_ = step;
            action = MenuAction::EditorStep;
        }
    }
    return action;
}

// ---------------------------------------------------------------------------
// The difficulty screen
// ---------------------------------------------------------------------------

namespace {

/// The file the console's builder names.
constexpr const char* kDifficultyFile = "media/UI/difficulty.uilayout";

/// The five windows of `difficulty.uilayout` that take the focus, top to
/// bottom, with their captions - for a run without the console data mounted.
struct DifficultyFallback {
    const char* window;
    int tab_order;
    const char* on_click;
    const char* caption;
};
constexpr DifficultyFallback kDifficultyFallback[] = {
    {"ContinueLast", 5, "GUISELECT1", "Easy"},
    {"NewCharacter", 1, "GUISELECT2", "Normal"},
    {"LoadCharacter", 2, "GUISELECT3", "Hard"},
    {"Achievements", 3, "GUISELECT4", "Very Hard"},
    {"Hardcore", 4, "", "Hardcore"},
};

/// The windows from `from` down to the one named `name`, both included.
bool window_path(const tl::formats::GuiWindow& from, const std::string& name,
                 std::vector<const tl::formats::GuiWindow*>& path) {
    path.push_back(&from);
    if (from.name == name) return true;
    for (const tl::formats::GuiWindow& child : from.children) {
        if (window_path(child, name, path)) return true;
    }
    path.pop_back();
    return false;
}

struct PixelRect {
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
};

/// Where the last window of `path` lies in the viewport: each `UnifiedAreaRect` resolved against
/// its parent, offsets scaled as `layout()` describes, and `HorizontalAlignment` /
/// `VerticalAlignment` applied as CEGUI does (centred: half of what the parent has to spare;
/// right or bottom: all of it). So the column's `Centre` container `{{-0.45,150},...}` sits at
/// `(W - w) / 2 - 0.45 W + 150`.
PixelRect resolve_path(const std::vector<const tl::formats::GuiWindow*>& path, float across,
                       float down, float scale) {
    PixelRect at;
    at.w = across;
    at.h = down;
    const std::string empty;
    for (const tl::formats::GuiWindow* window : path) {
        tl::formats::UVector2 top_left{};
        tl::formats::UVector2 bottom_right{};
        if (!parse_area(window->property("UnifiedAreaRect", empty), top_left, bottom_right)) {
            continue;
        }
        const float w = (bottom_right.x.scale - top_left.x.scale) * at.w +
                        (bottom_right.x.offset - top_left.x.offset) * scale;
        const float h = (bottom_right.y.scale - top_left.y.scale) * at.h +
                        (bottom_right.y.offset - top_left.y.offset) * scale;
        float x = top_left.x.scale * at.w + top_left.x.offset * scale;
        float y = top_left.y.scale * at.h + top_left.y.offset * scale;
        const std::string horizontal = window->property("HorizontalAlignment", empty);
        if (horizontal == "Centre") x += (at.w - w) * 0.5f;
        if (horizontal == "Right") x += at.w - w;
        const std::string vertical = window->property("VerticalAlignment", empty);
        if (vertical == "Centre") y += (at.h - h) * 0.5f;
        if (vertical == "Bottom") y += at.h - h;
        at.x += x;
        at.y += y;
        at.w = w;
        at.h = h;
    }
    return at;
}

/// The point size a `Font` property names: the four `Serif*.font` files and
/// the console's `Serif14`.
float font_points(const std::string& font) {
    if (font == "SerifBig") return kBigFontSize;
    if (font == "SerifHuge") return kHugeFontSize;
    if (font == "SerifSmall") return kSmallFontSize;
    if (font == "Serif14") return kConsoleSmallPoints;
    return kFontSize;
}

void gather_tabs(const tl::formats::GuiWindow& window, std::vector<tl::core::DifficultyTab>& into) {
    if (window.has_property("tabOrder")) {
        tl::core::DifficultyTab tab;
        tab.window = window.name;
        tab.tab_order = std::atoi(window.property("tabOrder", std::string()).c_str());
        tab.on_click = window.property("onClick", std::string());
        into.push_back(tab);
    }
    for (const tl::formats::GuiWindow& child : window.children) gather_tabs(child, into);
}

const tl::formats::GuiWindow* find_image_window(const tl::formats::GuiWindow& window,
                                                const std::string& image) {
    std::string set_name;
    std::string image_name;
    if (tl::formats::parse_image_reference(window.property("Image", std::string()), set_name,
                                           image_name) &&
        image_name == image) {
        return &window;
    }
    for (const tl::formats::GuiWindow& child : window.children) {
        if (const tl::formats::GuiWindow* found = find_image_window(child, image)) return found;
    }
    return nullptr;
}

/// The bottom strip a screen's prompt bar sits in: the console's `main_bottomborder`, or the PC's
/// `bottombar` where the port's interface pack draws the console screen in the PC's art.
const tl::formats::GuiWindow* find_bar_strip(const tl::formats::GuiWindow& root) {
    const tl::formats::GuiWindow* strip = find_image_window(root, "main_bottomborder");
    return strip != nullptr ? strip : find_image_window(root, "bottombar");
}

}  // namespace

bool MainMenu::load_difficulty_layout() {
    if (difficulty_.ok) return true;
    if (!console_mode_) return false;
    std::vector<uint8_t> bytes;
    if (!read_menu_resource(kDifficultyFile, group_, bytes)) return false;
    difficulty_ = tl::formats::parse_gui_layout(bytes.data(), bytes.size());
    if (!difficulty_.ok) {
        TL_LOG_WARN("menu: %s: %s", kDifficultyFile, difficulty_.error.c_str());
    }
    return difficulty_.ok;
}

void MainMenu::console_column_origin(float across, float down, float& left, float& top) const {
    left = kConsoleColumnLeft * scale_;
    top = kConsoleColumnTop * scale_;
    if (!difficulty_mode_ || difficulty_column_path_.empty()) return;
    const PixelRect column = resolve_path(difficulty_column_path_, across, down, scale_);
    left = column.x;
    top = column.y;
}

void MainMenu::clear_extras() {
    auto& overlays = Ogre::OverlayManager::getSingleton();
    for (Extra& extra : extras_) {
        if (extra.piece.panel == nullptr) continue;
        Ogre::Overlay* overlay = extra.tick ? text_overlay_ : frame_;
        if (overlay != nullptr) overlay->remove2D(extra.piece.panel);
        overlays.destroyOverlayElement(extra.piece.panel);
    }
    extras_.clear();
    for (ExtraLabel& label : extra_labels_) {
        for (Ogre::TextAreaOverlayElement* text : {label.shadow, label.text}) {
            if (text == nullptr) continue;
            if (text_root_ != nullptr) text_root_->removeChild(text->getName());
            overlays.destroyOverlayElement(text);
        }
    }
    extra_labels_.clear();
}

// What `difficulty.uilayout` draws besides its five focus windows: the
// `ceguiWidget/ItemText` panel they sit in, the checkbox's tick, the two lines
// beside it and the title.
void MainMenu::build_difficulty_extras() {
    if (difficulty_column_path_.empty()) return;
    auto& overlays = Ogre::OverlayManager::getSingleton();
    const std::string empty;
    const tl::formats::GuiWindow* frame = difficulty_column_path_.back();

    // The panel: section `frame` of the look, nine images round the window.
    tl::formats::UVector2 top_left{};
    tl::formats::UVector2 bottom_right{};
    if (frame->type.find("ItemText") != std::string::npos &&
        parse_area(frame->property("UnifiedAreaRect", empty), top_left, bottom_right)) {
        const FrameLook& look = kPanelFrame;
        const ArtRef corner_art = resolve_art(look.set[0], look.image[0]);
        const tl::formats::ImagesetImage* corner =
            corner_art.set != nullptr ? corner_art.set->find(look.image[0]) : nullptr;
        for (int slice = 0; corner != nullptr && slice < 9; ++slice) {
            const ArtRef art = resolve_art(look.set[slice], look.image[slice]);
            if (art.set == nullptr) continue;
            Extra extra;
            extra.slice = slice;
            extra.x = look.left;
            extra.y = look.top;
            extra.w = bottom_right.x.offset - top_left.x.offset + look.grow_w;
            extra.h = bottom_right.y.offset - top_left.y.offset + look.grow_h;
            extra.corner_w = static_cast<float>(corner->width);
            extra.corner_h = static_cast<float>(corner->height);
            extra.piece.set = art.set;
            extra.piece.material = art.material;
            extra.piece.image = look.image[slice];
            extra.piece.panel =
                make_panel("tl/menu/extra/" + std::to_string(button_serial_++), frame_);
            extra.piece.panel->setMaterial(art.material);
            extras_.push_back(extra);
        }
    }

    auto add_label = [&](const tl::formats::GuiWindow& window, bool title) {
        tl::formats::UVector2 a{};
        tl::formats::UVector2 b{};
        if (!parse_area(window.property("UnifiedAreaRect", empty), a, b)) return;
        ExtraLabel label;
        label.title = title;
        label.x = a.x.offset;
        label.y = a.y.offset;
        label.w = b.x.offset - a.x.offset;
        label.h = b.y.offset - a.y.offset;
        label.points = font_points(window.property("Font", empty));
        const std::string name = "tl/menu/extra/" + std::to_string(button_serial_++);
        auto make = [&](const char* suffix, const Ogre::ColourValue& colour) {
            auto* text = static_cast<Ogre::TextAreaOverlayElement*>(
                overlays.createOverlayElement("TextArea", name + suffix));
            text->setMetricsMode(Ogre::GMM_RELATIVE);
            text->setFontName(font_, Ogre::RGN_INTERNAL);
            text->setAlignment(title ? Ogre::TextAreaOverlayElement::Center
                                     : Ogre::TextAreaOverlayElement::Left);
            text->setColour(colour);
            text->setCaption(window.property("Text", empty));
            text_root_->addChild(text);
            return text;
        };
        // The drop sorts first; see `add_button`.
        label.shadow = make("/a-drop", Ogre::ColourValue(0.0f, 0.0f, 0.0f, 1.0f));
        label.text = make("/b-text", Ogre::ColourValue::White);
        extra_labels_.push_back(label);
    };

    for (const tl::formats::GuiWindow& child : frame->children) {
        if (child.has_property("tabOrder")) {
            // The checkbox's tick: `select_mark`, the `PushedImage` over the
            // whole window.
            if (child.type.find("Checkbox") == std::string::npos) continue;
            tl::formats::UVector2 a{};
            tl::formats::UVector2 b{};
            if (!widgets2_art_.ok || widgets2_art_.set.find("check") == nullptr ||
                !parse_area(child.property("UnifiedAreaRect", empty), a, b)) {
                continue;
            }
            Extra tick;
            tick.tick = true;
            tick.x = a.x.offset;
            tick.y = a.y.offset;
            tick.w = b.x.offset - a.x.offset;
            tick.h = b.y.offset - a.y.offset;
            tick.piece.set = &widgets2_art_.set;
            tick.piece.material = widgets2_art_.material;
            tick.piece.image = "check";
            tick.piece.panel =
                make_panel("tl/menu/extra/" + std::to_string(button_serial_++), text_overlay_);
            tick.piece.panel->setMaterial(widgets2_art_.material);
            extras_.push_back(tick);
            continue;
        }
        if (!child.property("Text", empty).empty()) add_label(child, /*title=*/false);
    }

    // `DemoVersion`: the main menu's window of that name, which this layout
    // keeps and words "Select Difficulty".
    difficulty_title_path_.clear();
    if (window_path(difficulty_.root, "DemoVersion", difficulty_title_path_)) {
        add_label(*difficulty_title_path_.back(), /*title=*/true);
    }
}

void MainMenu::layout_extras(float across, float down) {
    if (extras_.empty() && extra_labels_.empty()) return;
    float column_left = 0.0f;
    float column_top = 0.0f;
    console_column_origin(across, down, column_left, column_top);

    for (Extra& extra : extras_) {
        if (extra.piece.panel == nullptr || extra.piece.set == nullptr) continue;
        const float left = column_left + extra.x * scale_;
        const float top = column_top + extra.y * scale_;
        const float wide = extra.w * scale_;
        const float tall = extra.h * scale_;
        float x = left, y = top, w = wide, h = tall;
        if (extra.slice >= 0) {
            // A `FrameComponent`: corners at their own size, edges stretched
            // between them, the middle filling what is left.
            const float cw = extra.corner_w * scale_;
            const float ch = extra.corner_h * scale_;
            const float right = left + wide - cw;
            const float bottom = top + tall - ch;
            switch (extra.slice) {
                case 0:
                    w = cw;
                    h = ch;
                    break;
                case 1:
                    x = right;
                    w = cw;
                    h = ch;
                    break;
                case 2:
                    y = bottom;
                    w = cw;
                    h = ch;
                    break;
                case 3:
                    x = right;
                    y = bottom;
                    w = cw;
                    h = ch;
                    break;
                case 4:
                    y = top + ch;
                    w = cw;
                    h = tall - 2.0f * ch;
                    break;
                case 5:
                    x = right;
                    y = top + ch;
                    w = cw;
                    h = tall - 2.0f * ch;
                    break;
                case 6:
                    x = left + cw;
                    w = wide - 2.0f * cw;
                    h = ch;
                    break;
                case 7:
                    x = left + cw;
                    y = bottom;
                    w = wide - 2.0f * cw;
                    h = ch;
                    break;
                default:
                    x = left + cw;
                    y = top + ch;
                    w = wide - 2.0f * cw;
                    h = tall - 2.0f * ch;
                    break;
            }
        }
        extra.piece.panel->setPosition(x / across, y / down);
        extra.piece.panel->setDimensions(w / across, h / down);
        set_uv(extra.piece.panel, *extra.piece.set, extra.piece.image);
        if (!extra.tick) extra.piece.panel->show();
    }

    for (ExtraLabel& label : extra_labels_) {
        float left = column_left + label.x * scale_;
        float top = column_top + label.y * scale_;
        float wide = label.w * scale_;
        float tall = label.h * scale_;
        if (label.title) {
            const PixelRect box = resolve_path(difficulty_title_path_, across, down, scale_);
            left = box.x;
            top = box.y;
            wide = box.w;
            tall = box.h;
        }
        // A `StaticText` centres its line in its box unless told otherwise
        // (`FalagardStaticText`'s `VertCentred` default); the title is
        // `HorzTextFormatting` `CentreAligned` as well.
        const float height = label.points * points_to_char_pixels() * scale_;
        const float x = label.title ? left + wide * 0.5f : left;
        const float y = top + (tall - height) * 0.5f;
        for (Ogre::TextAreaOverlayElement* text : {label.shadow, label.text}) {
            if (text == nullptr) continue;
            const float drop = text == label.shadow ? kLabelShadow * scale_ : 0.0f;
            text->setCharHeight(height / down);
            text->setPosition((x + drop) / across, (y + drop) / down);
            text->show();
        }
    }
}

void MainMenu::show_difficulty(tl::core::DifficultyMenu& state) {
    if (buttons_overlay_ == nullptr || text_root_ == nullptr) return;
    clear_buttons();
    list_mode_ = true;
    difficulty_mode_ = true;
    difficulty_state_ = &state;
    difficulty_click_ = tl::core::DifficultyClick{};
    difficulty_button_.clear();
    difficulty_column_path_.clear();
    difficulty_title_path_.clear();

    const std::string empty;
    const bool authored = load_difficulty_layout();
    std::vector<tl::core::DifficultyTab> windows;
    if (authored) {
        gather_tabs(difficulty_.root, windows);
    } else {
        for (const DifficultyFallback& entry : kDifficultyFallback) {
            windows.push_back({entry.window, entry.tab_order, entry.on_click});
        }
    }
    difficulty_tabs_ = tl::core::difficulty_tab_list(windows);

    if (authored) {
        // One button per window of the tab list, at its own rectangle.
        for (const tl::core::DifficultyTab& tab : difficulty_tabs_) {
            const tl::formats::GuiWindow* window = difficulty_.find(tab.window);
            tl::formats::UVector2 top_left{};
            tl::formats::UVector2 bottom_right{};
            if (window == nullptr ||
                !parse_area(window->property("UnifiedAreaRect", empty), top_left, bottom_right)) {
                continue;
            }
            std::string caption;
            for (const tl::formats::GuiWindow& child : window->children) {
                caption = child.property("Text", empty);
                if (!caption.empty()) break;
            }
            add_button(caption, MenuAction::NewGame, /*slot=*/0, tab.on_click);
            Button& button = buttons_.back();
            button.col_x = top_left.x.offset;
            button.col_y = top_left.y.offset;
            button.col_w = bottom_right.x.offset - top_left.x.offset;
            button.col_h = bottom_right.y.offset - top_left.y.offset;
            button.checkbox = window->type.find("Checkbox") != std::string::npos;
            const ArtRef& art = button.checkbox ? checkbox_art_ : console_button_art_;
            button.piece.set = art.set;
            button.piece.material = art.material;
            button.piece.image = button.checkbox ? "checkbox_off" : console_normal_;
            if (button.piece.panel != nullptr) button.piece.panel->setMaterial(art.material);
            difficulty_button_.push_back(buttons_.size() - 1);
        }
        // The windows above the buttons: everything down to their parent.
        if (!difficulty_tabs_.empty() &&
            window_path(difficulty_.root, difficulty_tabs_.front().window,
                        difficulty_column_path_)) {
            difficulty_column_path_.pop_back();
        }
        build_difficulty_extras();

        // The bar: Select, Confirm and Back, in the container the bottom
        // strip holds.
        if (const tl::formats::GuiWindow* strip = find_bar_strip(difficulty_.root);
            strip != nullptr && !strip->children.empty()) {
            const tl::formats::GuiWindow& bar = strip->children.front();
            std::vector<BarGroup> groups;
            for (const tl::formats::GuiWindow& group : bar.children) {
                BarGroup entry;
                entry.window = group.name;
                for (const tl::formats::GuiWindow& child : group.children) {
                    if (entry.glyph.empty() && child.has_property("Image")) {
                        entry.glyph = child.name;
                    } else if (entry.caption.empty() && child.has_property("Text")) {
                        entry.caption = child.name;
                    }
                }
                groups.push_back(entry);
            }
            clear_bar();
            build_bar(difficulty_, bar.name, groups, /*authored_colour=*/true);
        }
    } else {
        // DIVERGENCE: without the console data the screen is this menu's own
        // list - the same five windows, top to bottom, in whatever row or
        // column the lists use. The PC's `difficulty.layout` (a `Back` button
        // and a pointer) is not drawn.
        difficulty_button_.assign(difficulty_tabs_.size(), 0);
        int slot = 0;
        for (const DifficultyFallback& entry : kDifficultyFallback) {
            const bool checkbox = entry.on_click[0] == 0;
            add_button(checkbox ? std::string("Hardcore: Off") : std::string(entry.caption),
                       MenuAction::NewGame, slot++, entry.on_click);
            buttons_.back().checkbox = checkbox;
            for (size_t tab = 0; tab < difficulty_tabs_.size(); ++tab) {
                if (difficulty_tabs_[tab].window == entry.window) {
                    difficulty_button_[tab] = buttons_.size() - 1;
                }
            }
        }
        stack_console_list();
    }

    // The list is built once and the focus put on its first window;
    // every later opening finds it where it was left.
    const size_t count = difficulty_button_.size();
    if (state.focus < 0 || state.focus >= static_cast<int>(count)) {
        state.focus = tl::core::difficulty_first_focus(count, [](size_t) { return true; });
    }
    state.set_open(true);
    selected_ = state.focus >= 0 ? static_cast<int>(difficulty_button_[state.focus]) : -1;
    // The A that pressed Done is not a press on this screen.
    confirm_held_ = quit_held_ = skip_held_ = nav_held_ = true;
    apply_button_look();
    layout();
}

MenuAction MainMenu::update_difficulty(const tl::core::InputState& input) {
    tl::core::DifficultyMenu& state = *difficulty_state_;
    const size_t count = difficulty_button_.size();
    // Every window of the list is shown and enabled.
    const auto usable = [](size_t) { return true; };

    // Up and down on the console's column, left and right on the PC row.
    const bool back = console_mode_ ? input.move_y > 0.5f : input.move_x < -0.5f;
    const bool forward = console_mode_ ? input.move_y < -0.5f : input.move_x > 0.5f;
    if (back || forward) {
        if (!nav_held_ && count > 0) {
            state.focus = tl::core::step_difficulty_focus(state.focus, forward, count, usable);
            if (state.focus >= 0) selected_ = static_cast<int>(difficulty_button_[state.focus]);
            apply_button_look();
        }
        nav_held_ = true;
    } else {
        nav_held_ = false;
    }

    MenuAction action = MenuAction::None;
    // B: back to character creation.
    if (input.skip && !skip_held_) {
        difficulty_click_ = state.back();
        action = MenuAction::Back;
    }
    skip_held_ = input.skip;
    quit_held_ = input.quit;

    if (action == MenuAction::None && input.confirm && !confirm_held_ && state.focus >= 0 &&
        state.focus < static_cast<int>(count)) {
        const size_t index = difficulty_button_[static_cast<size_t>(state.focus)];
        if (buttons_[index].checkbox) {
            state.toggle_hardcore();
            if (buttons_[index].piece.set != &widgets2_art_.set) {
                set_row_caption(index, state.hardcore ? "Hardcore: On" : "Hardcore: Off");
            }
            apply_button_look();
        } else {
            const tl::core::DifficultyClick click =
                state.on_click(tl::core::difficulty_layout_function(buttons_[index].data));
            if (click.handled) {
                difficulty_click_ = click;
                action = click.chosen ? MenuAction::NewGame : MenuAction::Back;
            }
        }
    }
    confirm_held_ = input.confirm;
    return action;
}

}  // namespace tl::render
