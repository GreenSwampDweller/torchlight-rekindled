#include "render/item_tip.h"

#include "core/text.h"
#include "render/menu_chrome.h"
#include "render/overlay_set.h"
#include "render/text_layout.h"

#include <OgreFontManager.h>
#include <OgreOverlayContainer.h>
#include <OgreOverlayManager.h>
#include <OgreTextAreaOverlayElement.h>

#include <algorithm>

namespace tl::render {

namespace {

/// The face `FrizQuadrata*.font` name, `media/ui/Torchlight Regular.TTF`: the
/// font the dialogue HUD builds (`render/hud.cpp`). `BigDPS` is drawn in it.
constexpr const char* kDisplayFontName = "tl/font/torchlight";

constexpr float kFrameLeft = -26.0f, kFrameTop = -13.0f, kFrameGrowW = 52.0f, kFrameGrowH = 26.0f;
constexpr float kFramePiece = 26.0f;

Ogre::FontPtr loaded_font(const char* name) {
    Ogre::FontPtr font = Ogre::FontManager::getSingleton().getByName(name, Ogre::RGN_INTERNAL);
    if (font && !font->isLoaded()) font->load();
    return font;
}

/// A line is the font's own height (CEGUI's ascender less descender, which
/// `showEquipmentTooltip` reads at the font's `+0x278`/`+0x27c`): the glyph
/// cell OGRE sizes a line by, not the em.
float line_height(float points, float scale) { return points * points_to_char_pixels() * scale; }

/// How a block of text is set: its size, the gap under each line, whether it hangs from
/// the window's right edge, whether it is in the display face, and the width a
/// `WordWrapLeftAligned` window breaks at (0 for none).
struct BlockStyle {
    float points = 0.0f;
    float spacing = 2.0f;
    bool right = false;
    bool display = false;
    float wrap = 0.0f;
};

/// The fonts, the view and the window being built: what a block of text is added with.
struct TipBuilder {
    Ogre::FontPtr font;
    Ogre::FontPtr display_font;
    float scale = 1.0f, across = 1.0f, down = 1.0f;
    float widest = 0.0f;
    TipBuilt out;

    /// One window's text, a span per colour run of each line; returns its
    /// height. `wrap` is the width a `WordWrapLeftAligned` window breaks at.
    float add_block(const tl::core::TooltipText& block, float x, float y, const BlockStyle& style) {
        const bool in_display = style.display && static_cast<bool>(display_font);
        const Ogre::FontPtr& face = in_display ? display_font : font;
        const float char_height = line_height(style.points, scale) / down;
        const std::vector<std::vector<TextSpan>> lines =
            wrap_runs(face, tl::core::markup_runs(block.text),
                      style.wrap > 0.0f ? style.wrap / across : 100.0f, char_height, across, down);
        float used = 0.0f;
        for (const std::vector<TextSpan>& line : lines) {
            float line_wide = 0.0f;
            for (const TextSpan& span : line) {
                TipSpan made;
                made.text = span.text;
                made.colour = span.coloured ? Ogre::ColourValue(span.r, span.g, span.b, span.a)
                                            : Ogre::ColourValue(block.colour.r, block.colour.g,
                                                                block.colour.b, block.colour.a);
                made.x = x + span.x * across;
                made.y = y + used;
                made.points = style.points;
                made.right = style.right;
                made.display = in_display;
                line_wide =
                    std::max(line_wide,
                             span.x * across +
                                 measure_text(face, span.text, char_height, across, down) * across);
                out.spans.push_back(made);
            }
            widest = std::max(widest, line_wide + 4.0f * scale + 40.0f * scale);
            used += line_height(style.points, scale) + style.spacing;
        }
        return used;
    }
};

}  // namespace

// The spacing is `showEquipmentTooltip`'s: a window is its line count times the font's
// line height plus 2, with 4 between windows, 6 more under the effects, and the `Effects`
// window 4 lower than the running total.
TipBuilt build_item_tip(const tl::core::EquipmentTooltip& tip, const TipFonts& sizes,
                        const TipView& view) {
    const float scale = view.scale;
    const float across = view.across;
    const float down = view.down;
    TipBuilder b;
    b.font = loaded_font(kFontName);
    // The display face is the dialogue HUD's to build; without it the big
    // figure falls back to the menu's.
    b.display_font = loaded_font(kDisplayFontName);
    b.scale = scale;
    b.across = across;
    b.down = down;
    b.widest = view.least_wide * scale;  // `scaledY(160.0)`, the least width

    // `ItemType` at 10 and `ItemName` at 26, `BigDPS` at 50, as the layout
    // authors them; the rest runs down from there.
    const float left = 20.0f * scale;
    const BlockStyle body{sizes.body};
    BlockStyle hung = body;
    hung.right = true;
    b.add_block(tip.type, 0.0f, 10.0f * scale, hung);
    b.add_block(tip.name, left, 26.0f * scale, BlockStyle{sizes.name});
    float y = 50.0f * scale;
    if (tip.big.shown) {
        const Ogre::FontPtr& face = b.display_font ? b.display_font : b.font;
        const float big_wide =
            measure_text(face, tip.big.text, line_height(sizes.big, scale) / down, across, down) *
            across;
        BlockStyle big{sizes.big};
        big.display = true;
        const float tall = b.add_block(tip.big, left, y, big);
        if (tip.big_label.shown) {
            b.add_block(tip.big_label, left + big_wide + 4.0f * scale,
                        y + (line_height(sizes.big, scale) - line_height(sizes.body, scale)), body);
        }
        y += tall + 4.0f * scale;
    }
    if (tip.stats.shown && !tip.stats.text.empty()) {
        y += b.add_block(tip.stats, left, y, body) + 4.0f * scale;
    }
    if (tip.effects.shown) {
        BlockStyle tight = body;
        tight.spacing = 0.0f;
        y += 4.0f * scale;
        y += b.add_block(tip.effects, left, y, tight) + 4.0f * scale;
    }
    y += 6.0f * scale;
    for (const tl::core::TooltipText& requirement : tip.requirements) {
        if (!requirement.shown) continue;
        y += b.add_block(requirement, left, y, body) + 4.0f * scale;
    }
    if (tip.description.shown) {
        // `WordWrapLeftAligned`, to the width the lines above settled.
        BlockStyle wrapped = body;
        wrapped.wrap = b.widest - 40.0f * scale;
        y += b.add_block(tip.description, left, y, wrapped) + 4.0f * scale;
    }
    if (tip.price.shown) {
        // `Price` hangs from the window's bottom right.
        y += 4.0f * scale;
        b.add_block(tip.price, 0.0f, y, hung);
        y += line_height(sizes.body, scale) + 4.0f * scale;
    }
    // The last `setSize`: the widest line and the running height, at least
    // `scaledY(100.0)`, each grown by `scaledY(52.0) - 52` when that is above 0.
    const float grow = std::max(0.0f, 52.0f * scale - 52.0f);
    b.out.wide = b.widest + grow;
    b.out.tall = std::max(100.0f * scale, y) + grow;
    return b.out;
}

void draw_item_tip_text(const TipBuilt& made, float x, float top, const TipView& view,
                        const TipTextHome& home) {
    const float scale = view.scale;
    const float across = view.across;
    const float down = view.down;
    std::vector<Ogre::TextAreaOverlayElement*>& lines = *home.lines;
    const Ogre::FontPtr font = loaded_font(kFontName);
    const Ogre::FontPtr display_font = loaded_font(kDisplayFontName);
    auto& overlays = Ogre::OverlayManager::getSingleton();
    for (size_t l = 0; l < made.spans.size(); ++l) {
        if (l == lines.size()) {
            auto* area = static_cast<Ogre::TextAreaOverlayElement*>(
                overlays.createOverlayElement("TextArea", home.prefix + std::to_string(l)));
            area->setMetricsMode(Ogre::GMM_RELATIVE);
            home.root->addChild(area);
            lines.push_back(area);
        }
        Ogre::TextAreaOverlayElement* area = lines[l];
        const TipSpan& span = made.spans[l];
        if (span.text.empty()) continue;
        const Ogre::FontPtr& face = span.display ? display_font : font;
        const float char_height = line_height(span.points, scale) / down;
        float at = x + span.x;
        if (span.right) {
            at = x + made.wide - measure_text(face, span.text, char_height, across, down) * across -
                 20.0f * scale;
        }
        area->setFontName(span.display ? kDisplayFontName : kFontName, Ogre::RGN_INTERNAL);
        area->setCaption(span.text);
        set_char_height(area, char_height);
        area->setColour(span.colour);
        area->setPosition(at / across, (top + span.y) / down);
        area->show();
    }
}

std::array<TipSlice, 9> item_tip_slices(float x, float top, float wide, float tall) {
    const float fx = x + kFrameLeft;
    const float fy = top + kFrameTop;
    const float fw = wide + kFrameGrowW;
    const float fh = tall + kFrameGrowH;
    const float p = kFramePiece;
    return {{
        {fx, fy, p, p},                           // top left
        {fx + fw - p, fy, p, p},                  // top right
        {fx, fy + fh - p, p, p},                  // bottom left
        {fx + fw - p, fy + fh - p, p, p},         // bottom right
        {fx, fy + p, p, fh - 2.0f * p},           // left
        {fx + fw - p, fy + p, p, fh - 2.0f * p},  // right
        {fx + p, fy, fw - 2.0f * p, p},           // top
        {fx + p, fy + fh - p, fw - 2.0f * p, p},  // bottom
        {fx + p, fy + p, fw - 2.0f * p, fh - 2.0f * p},
    }};
}

}  // namespace tl::render
