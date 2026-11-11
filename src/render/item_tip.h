// An item's details window, drawn: the text of a `tl::core::EquipmentTooltip` stacked and
// sized as `CGameUI::showEquipmentTooltip` sizes the
// windows of `media/UI/tooltip.layout`, and the nine pieces of its frame. Shared by the
// console's inventory screen and the PC's panes; where a window goes is each one's own.
#pragma once

#include "core/equipment_tooltip.h"

#include <OgreColourValue.h>
#include <OgrePrerequisites.h>

#include <array>
#include <string>
#include <vector>

namespace Ogre {
class OverlayContainer;
class TextAreaOverlayElement;
}  // namespace Ogre

namespace tl::render {

/// The point sizes the two layouts name: `tooltip.layout` uses `SerifBig`
/// (16), `Serif` (11) and `FrizQuadrataBig` (15); `tooltipcompare.layout`
/// `Serif14`, `SerifSmall` (8) and `FrizQuadrata` (11).
struct TipFonts {
    float name, body, big;
};
inline constexpr TipFonts kMainTipFonts{16.0f, 11.0f, 15.0f};
inline constexpr TipFonts kCompareTipFonts{14.0f, 8.0f, 11.0f};

/// One stretch of a window's text: where it starts, in pixels from the
/// window's top left, and how it is drawn.
struct TipSpan {
    std::string text;
    Ogre::ColourValue colour;
    float x = 0.0f, y = 0.0f, points = 0.0f;
    bool right = false;    ///< hangs from the window's right edge
    bool display = false;  ///< in the display face
};

struct TipBuilt {
    std::vector<TipSpan> spans;
    float wide = 0.0f, tall = 0.0f;
};

/// The view a window is drawn in: the interface ratio and the view's size in pixels.
struct TipView {
    float scale = 1.0f;
    float across = 1.0f;
    float down = 1.0f;
    /// The least a window is wide, in authored pixels (`scaledY(160.0)`).
    float least_wide = 160.0f;
};

/// One window's text and size, in pixels of the view.
TipBuilt build_item_tip(const tl::core::EquipmentTooltip& tip, const TipFonts& sizes,
                        const TipView& view);

/// Where a window's text elements live: under `root`, named `prefix` and a number, made
/// as they are needed and kept in `lines`.
struct TipTextHome {
    Ogre::OverlayContainer* root = nullptr;
    std::string prefix;
    std::vector<Ogre::TextAreaOverlayElement*>* lines = nullptr;
};

/// The window's text drawn with its top left at (`x`, `top`) pixels, one text element a
/// span. The caller hides the home's `lines` first; the ones used are shown.
void draw_item_tip_text(const TipBuilt& made, float x, float top, const TipView& view,
                        const TipTextHome& home);

/// `ceguiWidget/StaticShared`'s `itemframe` section (`ceguiWidget/ItemTooltip`) and `itemframe2`
/// (`ceguiWidget/ItemTooltip2`): top-left, top-right, bottom-left, bottom-right, left, right,
/// top, bottom, middle. Each image is 26 x 26.
inline constexpr const char* kItemTipFrame[2][9] = {
    {"itemtooltiptopleft", "itemtooltiptopright", "itemtooltipbottomleft", "itemtooltipbottomright",
     "itemtooltipleftedge", "itemtooltiprightedge", "itemtooltiptopedge", "itemtooltipbottomedge",
     "itemtooltipmiddle"},
    {"itemtooltip2topleft", "itemtooltip2topright", "itemtooltip2bottomleft",
     "itemtooltip2bottomright", "itemtooltip2leftedge", "itemtooltip2rightedge",
     "itemtooltip2topedge", "itemtooltip2bottomedge", "itemtooltip2middle"}};

/// Where a piece of the frame goes, in pixels.
struct TipSlice {
    float x, y, w, h;
};
/// The frame over its `Area` - the window's rectangle moved by (-26, -13) and grown by
/// (52, 26) - as a `FrameComponent` draws one: the corners at their own size, the edges
/// between, the middle behind. In `kItemTipFrame`'s order.
std::array<TipSlice, 9> item_tip_slices(float x, float top, float wide, float tall);

}  // namespace tl::render
