// The pieces every one of Torchlight's full-screen menus is built from: the imageset, typeface,
// button skin and scaling rule the main menu, loading screen and character creation share.
#pragma once

#include "formats/cegui.h"
#include "render/text_scale.h"

#include <OgreFont.h>
#include <OgreMaterial.h>
#include <OgreTexture.h>

#include <cstdint>
#include <string>
#include <vector>

namespace tl::render {

/// Full archive paths: `hud.imageset` exists under `media/UI/` and `media/UI/editimages/`, and a
/// bare basename opens neither with `OGRE_RESOURCEMANAGER_STRICT=0`.
inline constexpr const char* kLogoImageset = "media/UI/logo.imageset";
inline constexpr const char* kGuiImageset = "media/UI/hud.imageset";

/// The typeface CEGUI's `Serif` names (`media/UI/Serif.font`: FreeType, `Size="11"`, autoscaled
/// against 1024x768), and the one a menu window that names no font gets. Berlin Sans Demi Bold,
/// not the HUD dialogue's `Torchlight Regular.ttf`.
inline constexpr const char* kFontFile = "media/UI/BRLNSDB.TTF";
inline constexpr const char* kFontName = "tl/menu/serif";

/// The authored point sizes of the four `Serif*.font` files: 8, 11, 16 and 32.
/// All four name the same TTF, so one OGRE font serves them all and only the
/// drawn size differs.
inline constexpr float kSmallFontSize = 8.0f;
inline constexpr float kFontSize = 11.0f;
inline constexpr float kBigFontSize = 16.0f;
inline constexpr float kHugeFontSize = 32.0f;

/// Points to pixels, at the 96 dpi CEGUI's FreeType font rasterises at: a `Size="11"` is a point
/// size, an em of 14.67 pixels before autoscaling.
///
/// CEGUI autoscales a font's width by `displayWidth / 1024` and its height by
/// `displayHeight / 768`; OGRE's `TextAreaOverlayElement` has no separate horizontal scale, so
/// this matches the height and leaves the glyphs unstretched at 16:9.
inline constexpr float kPointsToPixels = 96.0f / 72.0f;
inline float ui_points_to_pixels() { return kPointsToPixels * ui_text_scale(); }

/// The two images `GuiLook.looknfeel` gives a `GuiLook/StandardButton` (`NormalImage`
/// `ButtonStandardRed`; `HoverImage` and `PushedImage` `ButtonStandardRedHighlight`).
inline constexpr const char* kButtonNormal = "ButtonStandardRed";
inline constexpr const char* kButtonHover = "ButtonStandardRedHighlight";

/// The label's drop shadow, from the same WidgetLook: a second TextComponent at
/// `AbsoluteDim value="2"` on both edges, coloured by `DropTextColour`, whose
/// default is `FF000000`.
inline constexpr float kLabelShadow = 2.0f;

/// The interface's own resolution, as every one of Torchlight's imagesets and
/// fonts declares it: `NativeHorzRes="1024" NativeVertRes="768"`. Read from the
/// imageset when there is one; this is only the fallback.
inline constexpr float kNativeHeight = 768.0f;

/// The top and bottom bars, as `mainmenuframe.layout`, `loading.layout` and
/// `charactercreate.layout` all place them. `rel_*` is the fraction of the viewport the offset
/// hangs from, `off_*` the authored pixel offset, scaled by `height / native`.
struct MenuBar {
    const char* image;
    float rel_x, off_x, rel_y, off_y, wide, tall;
};
inline constexpr MenuBar kMenuBars[] = {
    {"topbar", 0.5f, -512.0f, 0.0f, 0.0f, 1024.0f, 140.0f},
    {"topbarleft", 0.0f, 0.0f, 0.0f, 0.0f, 225.0f, 88.0f},
    {"topbarright", 1.0f, -225.0f, 0.0f, 0.0f, 225.0f, 88.0f},
    {"bottombar", 0.5f, -512.0f, 1.0f, -89.0f, 1024.0f, 89.0f},
    {"bottombarleft", 0.0f, 0.0f, 1.0f, -89.0f, 225.0f, 89.0f},
    {"bottombarright", 1.0f, -225.0f, 1.0f, -89.0f, 225.0f, 89.0f},
};

/// A `FrameComponent` of `ceguiWidget/StaticShared`, from
/// `media/UI/hud/schemes/ceguiWidgets.looknfeel`: its nine images (top-left,
/// top-right, bottom-left, bottom-right, left, right, top, bottom, middle) with
/// the imageset each comes from, and the `Area` it is drawn over - the window's
/// own rectangle moved by (`left`, `top`) and grown by (`grow_w`, `grow_h`).
struct FrameLook {
    const char* image[9];
    const char* set[9];
    float left, top, grow_w, grow_h;
};

/// Section `frame`, which `ceguiWidget/ItemText` draws: the soft dark panel.
inline constexpr FrameLook kPanelFrame{
    {"tooltiptopleft", "tooltiptopright", "tooltipbottomleft", "tooltipbottomright",
     "tooltipleftedge", "tooltiprightedge", "tooltiptopedge", "tooltipbottomedge", "tooltipmiddle"},
    {"widgets1", "widgets1", "widgets1", "widgets1", "widgets1", "widgets1", "widgets1", "widgets1",
     "widgets1"},
    -24.0f,
    -9.0f,
    48.0f,
    18.0f};

/// A material that samples one of the interface textures as it is: `LBX_SOURCE1` from
/// `LBS_TEXTURE` on both channels (see hud.cpp).
Ogre::MaterialPtr build_menu_material(const std::string& name, const Ogre::TexturePtr& texture,
                                      float alpha = 1.0f);

/// A screen's authored height: its reference imageset's `NativeVertRes`, or `fallback` when the
/// set was never read or authors none (an unread set keeps CEGUI's default of 480).
float authored_height(const tl::formats::Imageset& reference, float fallback);

/// Whether the mounted archive is the Xbox 360's (set by Renderer::Impl's mount): its own
/// `itemicons13` is then the console's sheet, and no renamed `xbox_itemicons13` is needed.
void set_console_game_data(bool console);
bool console_game_data();

inline constexpr const char* kUi2Imageset = "media/UI/ui2.imageset";
inline constexpr const char* kUiIconsImageset = "media/UI/uiicons.imageset";

/// The PC's own `Logo`, `GuiLook`, `ui2` and `UIIcons`, which a console-style screen loads after
/// its own sheets on a PC pak: the art the port's interface pack leaves out (the logo bars, the
/// buttons, the panel looks, the HUD's slots and bars), found by name where a sheet lacks it.
/// Empty on the Xbox 360 pak, which carries none of them. Then the mods' own imagesets
/// (set_mod_imagesets), on either pak.
std::vector<const char*> pc_imagesets();

/// The imagesets the mounted mods bring under `media/UI/` (a class's portrait, its skill icons):
/// the PC's interface loads every imageset it is given, a mod's among them.
void set_mod_imagesets(std::vector<std::string> paths);

/// The PC image that stands for a console one the mounted sheets lack (`skills_window_active` is
/// `ActiveSkillHousing`), or empty when the PC has none like it. Looked up only after the console
/// name is not found, so the console's own art always wins.
std::string pc_image_for(const std::string& console_image);

/// Read a whole file out of the mounted game archive. False, with a logged reason, when it is
/// not there.
bool read_menu_resource(const std::string& path, const std::string& group,
                        std::vector<uint8_t>& bytes);

/// Build the menus' typeface if nothing has yet. False when it cannot be built (a screen with
/// pictures and no words). Rasterised well above its drawn size, which depends on the viewport.
bool ensure_menu_font();

/// How many OGRE char-heights make one em of the interface font. CEGUI sizes a FreeType font by
/// its em, OGRE's `TextAreaOverlayElement` a line by its glyph cell (`Font::_prepareFont`), which
/// for `BRLNSDB.TTF` is taller. Read off the loaded font (a glyph's texture-coordinate height
/// times the atlas height, against `size * dpi / 72`); 1 when the font is not loaded yet.
float font_cell_per_em();

/// Points to an OGRE char height in pixels, before autoscaling:
/// `kPointsToPixels` times `font_cell_per_em()`. What every front-end screen
/// sets its text with.
inline float points_to_char_pixels() { return ui_points_to_pixels() * font_cell_per_em(); }

}  // namespace tl::render
