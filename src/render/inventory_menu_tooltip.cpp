// The item details windows over the inventory screen: `CGameUI`'s three `CEquipmentTooltip`s
// (X360 `+0x374`, `+0x378`, `+0x37c`), from `media/UI/tooltip.layout` and
// `media/UI/tooltipcompare.layout` (`CGameUI::create`). What each says and where they
// go is `core/equipment_tooltip.h` (`place_equipment_tooltips`); this stacks the
// text as `CGameUI::showEquipmentTooltip` sizes its windows.
#include "render/inventory_menu.h"

#include "core/text.h"
#include "render/menu_chrome.h"
#include "render/overlay_set.h"
#include "render/text_layout.h"

#include <OgreFontManager.h>
#include <OgreOverlayContainer.h>
#include <OgreOverlayManager.h>
#include <OgrePanelOverlayElement.h>
#include <OgreTextAreaOverlayElement.h>

#include <algorithm>
#include <cstdio>

namespace tl::render {

namespace {

/// `ceguiWidget/StaticShared`'s `itemframe` section (`ceguiWidget/ItemTooltip`) and `itemframe2`
/// (`ceguiWidget/ItemTooltip2`): top-left, top-right, bottom-left, bottom-right, left, right,
/// top, bottom, middle. Each image is 26 x 26.
constexpr const char* kItemFrame[2][9] = {
    {"itemtooltiptopleft", "itemtooltiptopright", "itemtooltipbottomleft", "itemtooltipbottomright",
     "itemtooltipleftedge", "itemtooltiprightedge", "itemtooltiptopedge", "itemtooltipbottomedge",
     "itemtooltipmiddle"},
    {"itemtooltip2topleft", "itemtooltip2topright", "itemtooltip2bottomleft",
     "itemtooltip2bottomright", "itemtooltip2leftedge", "itemtooltip2rightedge",
     "itemtooltip2topedge", "itemtooltip2bottomedge", "itemtooltip2middle"}};
/// The section's `Area`: the window's rectangle moved by (-26, -13) and grown
/// by (52, 26), in pixels.
constexpr float kFrameLeft = -26.0f, kFrameTop = -13.0f, kFrameGrowW = 52.0f, kFrameGrowH = 26.0f;
constexpr float kFramePiece = 26.0f;

/// The point sizes the two layouts name: `tooltip.layout` uses `SerifBig`
/// (16), `Serif` (11) and `FrizQuadrataBig` (15); `tooltipcompare.layout`
/// `Serif14`, `SerifSmall` (8) and `FrizQuadrata` (11).
struct TipFonts {
    float name, body, big;
};
constexpr TipFonts kMainFonts{16.0f, 11.0f, 15.0f};
constexpr TipFonts kCompareFonts{14.0f, 8.0f, 11.0f};

/// The face `FrizQuadrata*.font` name, `media/ui/Torchlight Regular.TTF`: the
/// font the dialogue HUD builds (`render/hud.cpp`). `BigDPS` is drawn in it.
constexpr const char* kDisplayFontName = "tl/font/torchlight";

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

}  // namespace

// PORT: the item tooltips taken and laid out (drawing).
void InventoryMenu::set_item_tooltips(std::vector<tl::core::EquipmentTooltip> tips) {
    tooltips_ = std::move(tips);
    lay_out_tooltips();
}

void InventoryMenu::destroy_tooltips() {
    auto& overlays = Ogre::OverlayManager::getSingleton();
    for (TipWindow& window : tip_windows_) {
        for (Ogre::PanelOverlayElement*& panel : window.frame) {
            if (panel == nullptr) continue;
            if (top_panel_root_ != nullptr) top_panel_root_->removeChild(panel->getName());
            overlays.destroyOverlayElement(panel);
            panel = nullptr;
        }
        for (Ogre::TextAreaOverlayElement* area : window.lines) {
            if (top_text_root_ != nullptr) top_text_root_->removeChild(area->getName());
            overlays.destroyOverlayElement(area);
        }
        window.lines.clear();
    }
    tooltips_.clear();
}

const InventoryMenu::Node* InventoryMenu::tooltip_anchor_row() {
    std::string prefix = "inv/";
    int selected = selected_index_;
    int tab = current_tab_;
    size_t count = filtered_indices_.size();
    if (vendor_ != VendorScreen::None && focus_ == Tray::Vendor) {
        prefix = "vendor/";
        selected = vendor_selected_;
        tab = vendor_tab_;
        count = vendor_rows_.size();
    } else if (focus_ == Tray::Pet) {
        prefix = "pet/";
        selected = pet_selected_;
        tab = pet_tab_;
        count = pet_rows_.size();
    }
    if (selected < 0 || static_cast<size_t>(selected) >= count) return nullptr;
    const auto scroll = tray_scrolls_.find(prefix + std::to_string(tab));
    const int row = selected - (scroll != tray_scrolls_.end() ? scroll->second : 0);
    if (row < 0 || row > 4) return nullptr;
    return find_node_by_widget(prefix + "ItemTray" + std::to_string(row));
}

void InventoryMenu::lay_out_tooltips() {
    if (!built_ || top_panel_root_ == nullptr || top_text_root_ == nullptr) return;
    auto& overlays = Ogre::OverlayManager::getSingleton();
    auto& fonts = Ogre::FontManager::getSingleton();
    Ogre::FontPtr font = fonts.getByName(kFontName, Ogre::RGN_INTERNAL);
    if (font && !font->isLoaded()) font->load();
    // The display face is the dialogue HUD's to build; without it the big
    // figure falls back to the menu's.
    Ogre::FontPtr display_font = fonts.getByName(kDisplayFontName, Ogre::RGN_INTERNAL);
    if (display_font && !display_font->isLoaded()) display_font->load();
    const bool have_display = static_cast<bool>(display_font);
    const float across = static_cast<float>(width_ > 0 ? width_ : 1);
    const float down = static_cast<float>(height_ > 0 ? height_ : 1);
    const bool shown = visible_ && menu_mode_ == MenuMode::Inventory && !select_open_ &&
                       !pet_commands_open_ && !spell_remove_open_;
    const Node* anchor = shown ? tooltip_anchor_row() : nullptr;

    for (TipWindow& window : tip_windows_) {
        for (Ogre::PanelOverlayElement* panel : window.frame) {
            if (panel != nullptr) panel->hide();
        }
        for (Ogre::TextAreaOverlayElement* area : window.lines) area->hide();
    }
    if (anchor == nullptr || tooltips_.empty()) return;

    // A line is the font's own height (CEGUI's ascender less descender, which
    // `showEquipmentTooltip` reads at the font's `+0x278`/`+0x27c`): the glyph
    // cell OGRE sizes a line by, not the em.
    const auto line_height = [&](float points) {
        return points * points_to_char_pixels() * scale_;
    };
    // Keep long item names inside a screen-sized tooltip frame. The authored
    // layout grows to the full name width and can push the frame off-screen
    // when the iOS large-text setting is active.
    const float max_frame_wide = across * 0.84f;
    const auto colour_of = [](const tl::core::LabelColour& c) {
        return Ogre::ColourValue(c.r, c.g, c.b, c.a);
    };

    // Each window's text and size. The spacing is `showEquipmentTooltip`'s: a
    // window is its line count times the font's line height plus 2, with 4
    // between windows, 6 more under the effects, and the `Effects` window 4
    // lower than the running total.
    std::array<TipBuilt, 3> built;
    for (size_t index = 0; index < built.size() && index < tooltips_.size(); ++index) {
        const tl::core::EquipmentTooltip& tip = tooltips_[index];
        if (!tip.name.shown) continue;
        const TipFonts sizes = index == 0 ? kMainFonts : kCompareFonts;
        TipBuilt& out = built[index];
        float widest = 160.0f * scale_;  // `scaledY(160.0)`, the least width
        const float grow = std::max(0.0f, 52.0f * scale_ - 52.0f);
        const float max_window_wide = std::max(120.0f * scale_, max_frame_wide - 52.0f);
        const float max_text_wide = std::max(
            80.0f * scale_, max_window_wide - grow - 64.0f * scale_);

        // One window's text, a span per colour run of each line; returns its
        // height. `wrap` is the width a `WordWrapLeftAligned` window breaks at.
        const auto add_block = [&](const tl::core::TooltipText& block, float x, float y,
                                   float points, float spacing, bool right, bool display,
                                   float wrap) {
            const Ogre::FontPtr& face = display && have_display ? display_font : font;
            const float char_height = line_height(points) / down;
            const std::vector<std::vector<TextSpan>> lines =
                wrap_runs(face, tl::core::markup_runs(block.text),
                          wrap > 0.0f ? wrap / across : 100.0f, char_height, across, down);
            float used = 0.0f;
            for (const std::vector<TextSpan>& line : lines) {
                float line_wide = 0.0f;
                for (const TextSpan& span : line) {
                    TipSpan made;
                    made.text = span.text;
                    made.colour = span.coloured ? Ogre::ColourValue(span.r, span.g, span.b, span.a)
                                                : colour_of(block.colour);
                    made.x = x + span.x * across;
                    made.y = y + used;
                    made.points = points;
                    made.right = right;
                    made.display = display && have_display;
                    line_wide = std::max(
                        line_wide,
                        span.x * across +
                            measure_text(face, span.text, char_height, across, down) * across);
                    out.spans.push_back(made);
                }
                widest = std::max(widest, line_wide + 4.0f * scale_ + 40.0f * scale_);
                used += line_height(points) + spacing;
            }
            return used;
        };

        // `ItemType` at 10 and `ItemName` at 26, `BigDPS` at 50, as the layout
        // authors them; the rest runs down from there.
        const float left = 20.0f * scale_;
        add_block(tip.type, 0.0f, 10.0f * scale_, sizes.body, 2.0f, /*right=*/true, false,
                  max_text_wide + 20.0f * scale_);
        add_block(tip.name, left, 26.0f * scale_, sizes.name, 2.0f, false, false, max_text_wide);
        float y = 50.0f * scale_;
        if (tip.big.shown) {
            const Ogre::FontPtr& face = have_display ? display_font : font;
            const float big_wide =
                measure_text(face, tip.big.text, line_height(sizes.big) / down, across, down) *
                across;
            const float tall = add_block(tip.big, left, y, sizes.big, 2.0f, false, true, 0.0f);
            if (tip.big_label.shown) {
                add_block(tip.big_label, left + big_wide + 4.0f * scale_,
                          y + (line_height(sizes.big) - line_height(sizes.body)), sizes.body, 2.0f,
                          false, false, max_text_wide);
            }
            y += tall + 4.0f * scale_;
        }
        if (tip.stats.shown && !tip.stats.text.empty()) {
            y +=
                add_block(tip.stats, left, y, sizes.body, 2.0f, false, false, max_text_wide) +
                4.0f * scale_;
        }
        if (tip.effects.shown) {
            y += 4.0f * scale_;
            y += add_block(tip.effects, left, y, sizes.body, 0.0f, false, false, max_text_wide) +
                 4.0f * scale_;
        }
        y += 6.0f * scale_;
        for (const tl::core::TooltipText& requirement : tip.requirements) {
            if (!requirement.shown) continue;
            y += add_block(requirement, left, y, sizes.body, 2.0f, false, false, max_text_wide) +
                 4.0f * scale_;
        }
        if (tip.description.shown) {
            // `WordWrapLeftAligned`, to the width the lines above settled.
            y += add_block(tip.description, left, y, sizes.body, 2.0f, false, false,
                           std::min(widest - 64.0f * scale_, max_text_wide)) +
                 4.0f * scale_;
        }
        if (tip.price.shown) {
            // `Price` hangs from the window's bottom right.
            y += 4.0f * scale_;
            add_block(tip.price, 0.0f, y, sizes.body, 2.0f, /*right=*/true, false, 0.0f);
            y += line_height(sizes.body) + 4.0f * scale_;
        }
        // The last `setSize`: the widest line
        // and the running height, at least `scaledY(100.0)`, each grown by
        // `scaledY(52.0) - 52` when that is above 0.
        const float natural_height = std::max(100.0f * scale_, y);
        const float height_room = std::max(
            100.0f * scale_, down * 0.84f - grow - 52.0f);
        const float height_scale = std::min(1.0f, height_room / natural_height);
        if (height_scale < 1.0f) {
            for (TipSpan& span : out.spans) {
                span.y *= height_scale;
                span.points *= height_scale;
            }
        }
        out.wide = std::min(widest + grow, max_window_wide);
        out.tall = natural_height * height_scale + grow;
    }

    // from the selected row's left and top.
    std::array<tl::core::TooltipBox, 3> boxes;
    for (size_t index = 0; index < boxes.size(); ++index) {
        boxes[index].shown = !built[index].spans.empty();
        boxes[index].width = built[index].wide;
        boxes[index].height = built[index].tall;
    }
    if (!boxes[0].shown) return;
    tl::core::place_equipment_tooltips(boxes[0], boxes[1], boxes[2],
                                       static_cast<unsigned>(std::max(0.0f, anchor->left * across)),
                                       static_cast<unsigned>(std::max(0.0f, anchor->top * down)),
                                       static_cast<int>(width_), static_cast<int>(height_), scale_);

    for (size_t index = 0; index < tip_windows_.size(); ++index) {
        if (!boxes[index].shown) continue;
        TipWindow& window = tip_windows_[index];
        const TipBuilt& made = built[index];
        const float x = boxes[index].x;
        const float top = boxes[index].y;
        const float wide = made.wide;
        const float tall = made.tall;

        // The frame over its `Area`, as a `FrameComponent` draws one: the
        // corners at their own size, the edges between, the middle behind.
        const float fx = x + kFrameLeft;
        const float fy = top + kFrameTop;
        const float fw = wide + kFrameGrowW;
        const float fh = tall + kFrameGrowH;
        const float p = kFramePiece;
        struct Slice {
            float x, y, w, h;
        };
        const Slice slices[9] = {
            {fx, fy, p, p},                           // top left
            {fx + fw - p, fy, p, p},                  // top right
            {fx, fy + fh - p, p, p},                  // bottom left
            {fx + fw - p, fy + fh - p, p, p},         // bottom right
            {fx, fy + p, p, fh - 2.0f * p},           // left
            {fx + fw - p, fy + p, p, fh - 2.0f * p},  // right
            {fx + p, fy, fw - 2.0f * p, p},           // top
            {fx + p, fy + fh - p, fw - 2.0f * p, p},  // bottom
            {fx + p, fy + p, fw - 2.0f * p, fh - 2.0f * p},
        };
        const char* const* images = kItemFrame[index == 0 ? 0 : 1];
        for (size_t s = 0; s < 9; ++s) {
            const Sprite* art = sprite(images[s]);
            if (art == nullptr || !art->texture) continue;
            if (window.frame[s] == nullptr) {
                char name[64];
                std::snprintf(name, sizeof(name), "tl/inv/tip_%zu_%zu", index, s);
                window.frame[s] = static_cast<Ogre::PanelOverlayElement*>(
                    overlays.createOverlayElement("Panel", name));
                window.frame[s]->setMetricsMode(Ogre::GMM_RELATIVE);
                top_panel_root_->addChild(window.frame[s]);
            }
            Ogre::PanelOverlayElement* panel = window.frame[s];
            panel->setMaterialName(art->material->getName());
            const float tw = static_cast<float>(art->texture->getWidth());
            const float th = static_cast<float>(art->texture->getHeight());
            panel->setUV(static_cast<float>(art->rect.x) / tw, static_cast<float>(art->rect.y) / th,
                         static_cast<float>(art->rect.x + art->rect.width) / tw,
                         static_cast<float>(art->rect.y + art->rect.height) / th);
            panel->setPosition(slices[s].x / across, slices[s].y / down);
            panel->setDimensions(slices[s].w / across, slices[s].h / down);
            panel->show();
        }

        for (size_t l = 0; l < made.spans.size(); ++l) {
            if (l == window.lines.size()) {
                char name[64];
                std::snprintf(name, sizeof(name), "tl/inv/tip_%zu_line_%zu", index, l);
                auto* area = static_cast<Ogre::TextAreaOverlayElement*>(
                    overlays.createOverlayElement("TextArea", name));
                area->setMetricsMode(Ogre::GMM_RELATIVE);
                top_text_root_->addChild(area);
                window.lines.push_back(area);
            }
            Ogre::TextAreaOverlayElement* area = window.lines[l];
            const TipSpan& span = made.spans[l];
            if (span.text.empty()) continue;
            const Ogre::FontPtr& face = span.display ? display_font : font;
            const float char_height = line_height(span.points) / down;
            float at = x + span.x;
            if (span.right) {
                at = x + wide - measure_text(face, span.text, char_height, across, down) * across -
                     20.0f * scale_;
            }
            area->setFontName(span.display ? kDisplayFontName : kFontName, Ogre::RGN_INTERNAL);
            area->setCaption(span.text);
            set_char_height(area, char_height);
            area->setColour(span.colour);
            area->setPosition(at / across, (top + span.y) / down);
            area->show();
        }
    }
}

}  // namespace tl::render
