// The console's inventory screen under the mouse controls.
#include "render/inventory_menu.h"

#include <string>

namespace tl::render {

/// The row of the tray `prefix` whose window (`<prefix>ItemTray0` .. `4`) holds (x, y); -1
/// for none.
int InventoryMenu::tray_row_at(const std::string& prefix, float x, float y) {
    constexpr int kRows = 5;
    for (int row = 0; row < kRows; ++row) {
        const Node* node = find_node_by_widget(prefix + "ItemTray" + std::to_string(row));
        if (node == nullptr || is_node_hidden(static_cast<int>(node - nodes_.data()))) continue;
        const bool inside = x >= node->left && y >= node->top && x <= node->left + node->width &&
                            y <= node->top + node->height;
        if (inside) return row;
    }
    return -1;
}

// DIVERGENCE (mouse controls): the console's screen has no pointer. The tray row under
// (x, y) - 0 to 1 across and down the view - of the bag's, the vendor's or the pet's tray;
// with `take` that tray gets the focus and the row the selection.
InventoryMenu::Pointed InventoryMenu::point_row(float x, float y, bool take) {
    if (!built_ || !visible_ || menu_mode_ != MenuMode::Inventory || select_open_ ||
        pet_commands_open_ || spell_remove_open_) {
        return Pointed::Nothing;
    }
    struct TrayRows {
        const char* prefix;
        Tray tray;
        int* selected;
        int tab;
        size_t count;
    };
    const TrayRows trays[] = {
        {"inv/", Tray::Bag, &selected_index_, current_tab_, filtered_indices_.size()},
        {"vendor/", Tray::Vendor, &vendor_selected_, vendor_tab_, vendor_rows_.size()},
        {"pet/", Tray::Pet, &pet_selected_, pet_tab_, pet_rows_.size()}};
    for (const TrayRows& tray : trays) {
        const int row = tray.tray == Tray::Vendor && vendor_ == VendorScreen::None
                            ? -1
                            : tray_row_at(tray.prefix, x, y);
        if (row < 0) continue;
        // A tray shows its rows from its scroll on.
        const auto scroll = tray_scrolls_.find(tray.prefix + std::to_string(tray.tab));
        const int index = row + (scroll != tray_scrolls_.end() ? scroll->second : 0);
        if (index < 0 || static_cast<size_t>(index) >= tray.count) return Pointed::Nothing;
        if (!take || (focus_ == tray.tray && *tray.selected == index)) return Pointed::Row;
        focus_ = tray.tray;
        *tray.selected = index;
        return Pointed::Selected;
    }
    return Pointed::Nothing;
}

/// A click at (x, y) on one of a tray's six filter icons (`<prefix>inventory_filter_*`):
/// that tray gets the focus and that tab. True when it was one.
bool InventoryMenu::click_filter(float x, float y) {
    if (!built_ || !visible_ || menu_mode_ != MenuMode::Inventory || select_open_ ||
        pet_commands_open_ || spell_remove_open_) {
        return false;
    }
    static constexpr const char* kFilters[] = {"weapons", "armor",  "jewelry",
                                               "potions", "spells", "fish"};
    struct TrayTab {
        const char* prefix;
        Tray tray;
        int* tab;
        int* selected;
    };
    const TrayTab trays[] = {{"inv/", Tray::Bag, &current_tab_, &selected_index_},
                             {"vendor/", Tray::Vendor, &vendor_tab_, &vendor_selected_},
                             {"pet/", Tray::Pet, &pet_tab_, &pet_selected_}};
    for (const TrayTab& tray : trays) {
        if (tray.tray == Tray::Vendor && vendor_ == VendorScreen::None) continue;
        for (int filter = 0; filter < 6; ++filter) {
            const Node* node = find_node_by_widget(std::string(tray.prefix) + "inventory_filter_" +
                                                   kFilters[filter]);
            if (node == nullptr || is_node_hidden(static_cast<int>(node - nodes_.data()))) continue;
            const bool inside = x >= node->left && y >= node->top &&
                                x <= node->left + node->width && y <= node->top + node->height;
            if (!inside) continue;
            focus_ = tray.tray;
            *tray.tab = filter;
            *tray.selected = 0;
            return true;
        }
    }
    return false;
}

}  // namespace tl::render
