// The item details windows over the inventory screen: `CGameUI`'s three `CEquipmentTooltip`s
// (X360 `+0x374`, `+0x378`, `+0x37c`), from `media/UI/tooltip.layout` and
// `media/UI/tooltipcompare.layout` (`CGameUI::create`). What each says and where they
// go is `core/equipment_tooltip.h` (`place_equipment_tooltips`); this stacks the
// text as `CGameUI::showEquipmentTooltip` sizes its windows.
#include "render/inventory_menu.h"

#include "core/text.h"
#include "render/item_tip.h"
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

    const TipView view{scale_, across, down};
    std::array<TipBuilt, 3> built;
    for (size_t index = 0; index < built.size() && index < tooltips_.size(); ++index) {
        if (!tooltips_[index].name.shown) continue;
        built[index] =
            build_item_tip(tooltips_[index], index == 0 ? kMainTipFonts : kCompareTipFonts, view);
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

        const std::array<TipSlice, 9> slices = item_tip_slices(x, top, wide, tall);
        const char* const* images = kItemTipFrame[index == 0 ? 0 : 1];
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

        char prefix[64];
        std::snprintf(prefix, sizeof(prefix), "tl/inv/tip_%zu_line_", index);
        draw_item_tip_text(made, x, top, view, TipTextHome{top_text_root_, prefix, &window.lines});
    }
}

}  // namespace tl::render
