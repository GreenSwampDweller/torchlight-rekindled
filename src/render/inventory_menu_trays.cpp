// The inventory menu's trays: which one has the focus, and their tabs. Split from
// inventory_menu.cpp.
#include "render/inventory_menu.h"

#include "core/inventory_screen.h"
#include "core/skills.h"
#include "render/menu_pointer.h"

#include <OgrePanelOverlayElement.h>

#include <algorithm>
#include <cstdio>

namespace tl::render {

void InventoryMenu::focus_next_tray(int direction) {
    if (vendor_ == VendorScreen::None || direction == 0) return;
#if defined(TL_PLATFORM_IOS) || defined(TL_PLATFORM_MACOS)
    // DIVERGENCE (platform): the touch pad's LB (and the Mac's) rotates over the vendor's
    // tray, the bag and the pet's, and on the enchanter swaps the bag and the pet's.
    if (enchanter_screen()) {
        if (!pet_tray_available_) return;
        focus_ = focus_ == Tray::Pet ? Tray::Bag : Tray::Pet;
        return;
    }
    if (direction < 0) {
        if (focus_ == Tray::Vendor) focus_ = Tray::Bag;
        else if (focus_ == Tray::Bag && pet_tray_available_) focus_ = Tray::Pet;
        else focus_ = Tray::Vendor;
    } else {
        if (focus_ == Tray::Vendor && pet_tray_available_) focus_ = Tray::Pet;
        else if (focus_ == Tray::Pet || focus_ == Tray::Vendor) focus_ = Tray::Bag;
        else focus_ = Tray::Vendor;
    }
#else
    // `CEnchantMenuXenon`'s RB/LB (codes 10 and 12) swap the
    // bag and the pet's tray; the pet's takes no focus here (below), so the
    // bag keeps it.
    if (enchanter_screen()) return;
    // codes 10/12 -> (direction, false): the focus
    // rotates over the three trays and wraps. INVENTED: the pet's tray takes
    // no focus here yet (it has no selection of its own), so the rotation is
    // over the vendor's and the bag.
    focus_ = focus_ == Tray::Vendor ? Tray::Bag : Tray::Vendor;
#endif
}

void InventoryMenu::swap_bag_focus(int landing) {
    if (vendor_ != VendorScreen::None || menu_mode_ != MenuMode::Inventory) return;
    if (!pet_tray_available_) return;
    // the other tray takes the focus (on both
    // trays, on both panels); with a direction, the tray it lands on
    // gets its tab first - 5 for -1, 0 for +1 - and with none keeps its own.
    focus_ = focus_ == Tray::Pet ? Tray::Bag : Tray::Pet;
    if (landing != 0) {
        int& tab = focus_ == Tray::Pet ? pet_tab_ : current_tab_;
        tab = landing < 0 ? tl::core::kInventoryFilterCount - 1 : 0;
    }
}

void InventoryMenu::step_bag_tab(int direction) {
    if (vendor_ != VendorScreen::None || menu_mode_ != MenuMode::Inventory || direction == 0) {
        return;
    }
    // Handler: code 0x13 is the tab - 1 and below 0 the swap with
    // -1; code 0x14 (tray, 1) and a step off the end the swap with
    // +1. Each step refreshes the tray, which starts it at its
    // first row.
    const bool on_pet = focus_ == Tray::Pet;
    int& tab = on_pet ? pet_tab_ : current_tab_;
    const int leave = tl::core::step_tray_tab(tab, direction);
    if (on_pet) {
        pet_selected_ = 0;
    } else {
        selected_index_ = 0;
    }
    if (leave == 0) return;
    swap_bag_focus(leave);
    if (focus_ == Tray::Pet) {
        pet_selected_ = 0;
    } else {
        selected_index_ = 0;
    }
}

// PORT: the pet tray's slot under the cursor.
int InventoryMenu::selected_pet_slot() const {
    if (pet_rows_.empty() || pet_selected_ < 0 ||
        pet_selected_ >= static_cast<int>(pet_rows_.size())) {
        return -1;
    }
    return static_cast<int>(pet_rows_[static_cast<size_t>(pet_selected_)]);
}

void InventoryMenu::step_tray_tab(int direction) {
    if (vendor_ == VendorScreen::None || direction == 0) return;
    const auto tab_of = [this](Tray tray) -> int& {
        return tray == Tray::Vendor ? vendor_tab_ : tray == Tray::Pet ? pet_tab_ : current_tab_;
    };
    const auto start = [this](Tray tray) {
        if (tray == Tray::Vendor) vendor_selected_ = 0;
        else if (tray == Tray::Pet) pet_selected_ = 0;
        else selected_index_ = 0;
    };
    const int leave = tl::core::step_tray_tab(
        tab_of(focus_), direction, focus_ == Tray::Vendor && vendor_ == VendorScreen::Combine);
    start(focus_);
    if (leave == 0) return;
    // (leave, true)`: the tray it lands on starts at the edge it
    // came in from - tab 0 moving right, tab 5 moving left. INVENTED (DIVERGENCE, platform:
    // the Mac and the touch pad): the trays in the order they stand on the screen - the
    // vendor's, the bag, the pet's when it is here - wrapping, so none is passed over.
    std::vector<Tray> order{Tray::Vendor, Tray::Bag};
#if defined(TL_PLATFORM_IOS) || defined(TL_PLATFORM_MACOS)
    if (pet_tray_available_ && !enchanter_screen()) order.push_back(Tray::Pet);
#endif
    const auto at = std::find(order.begin(), order.end(), focus_);
    const int here = at == order.end() ? 0 : static_cast<int>(at - order.begin());
    const int count = static_cast<int>(order.size());
    focus_ = order[static_cast<size_t>(((here + leave) % count + count) % count)];
    tab_of(focus_) = leave > 0 ? 0 : tl::core::kInventoryFilterCount - 1;
    start(focus_);
}

void InventoryMenu::cycle_vendor_tab(int direction) {
    if (vendor_ == VendorScreen::None || direction == 0) return;
    vendor_tab_ = (vendor_tab_ + (direction > 0 ? 1 : -1) + tl::core::kInventoryFilterCount) %
                  tl::core::kInventoryFilterCount;
    vendor_selected_ = 0;
}

// PORT: the vendor tray's slot under the cursor.
int InventoryMenu::selected_vendor_slot() const {
    if (vendor_rows_.empty() || vendor_selected_ < 0 ||
        vendor_selected_ >= static_cast<int>(vendor_rows_.size())) {
        return -1;
    }
    return static_cast<int>(vendor_rows_[static_cast<size_t>(vendor_selected_)]);
}

// DIVERGENCE (desktop mouse): the pointer's hit tests, over the windows `update` placed.
InventoryMenu::PointerHit InventoryMenu::pointer_hit(const tl::core::InputState& in) {
    PointerHit hit;
    if (!visible_ || !in.pointer) return hit;
    // By the rectangle `layout()` gave the window, a little grown: the tabs are small.
    const auto over = [&](const std::string& widget) {
        const int index = find_node_index_by_widget(widget);
        if (index < 0 || is_node_hidden(index)) return false;
        const Node& node = nodes_[static_cast<size_t>(index)];
        constexpr float kSlack = 0.004f;
        return in.pointer_x >= node.left - kSlack && in.pointer_x < node.left + node.width + kSlack &&
               in.pointer_y >= node.top - kSlack && in.pointer_y < node.top + node.height + kSlack;
    };
    if (over("master/lefttrigger") || over("master/lefttriggerbutton")) {
        hit.kind = PointerHit::Kind::Ring;
        hit.tab = -1;
        return hit;
    }
    if (over("master/righttrigger") || over("master/righttriggerbutton")) {
        hit.kind = PointerHit::Kind::Ring;
        hit.tab = 1;
        return hit;
    }
    if (menu_mode_ == MenuMode::Quest) {
        for (int i = 0; i < tl::core::kQuestLogSize; ++i) {
            if (!over("quest/ItemTray" + std::to_string(i))) continue;
            hit.kind = PointerHit::Kind::Quest;
            hit.row = i;
            return hit;
        }
        return hit;
    }
    if (menu_mode_ != MenuMode::Inventory) return hit;

    static constexpr const char* kFilters[] = {"weapons", "armor",  "jewelry",
                                               "potions", "spells", "fish"};
    struct Shown {
        Tray tray;
        const char* prefix;
        int tab;
        size_t rows;
    };
    std::vector<Shown> trays;
    trays.push_back({Tray::Bag, "inv/", current_tab_, filtered_indices_.size()});
    if (vendor_ != VendorScreen::None) {
        trays.push_back({Tray::Vendor, "vendor/", vendor_tab_, vendor_rows_.size()});
    }
    if (pet_tray_available_) trays.push_back({Tray::Pet, "pet/", pet_tab_, pet_rows_.size()});
    for (const Shown& tray : trays) {
        for (int tab = 0; tab < tl::core::kInventoryFilterCount; ++tab) {
            if (!over(std::string(tray.prefix) + "inventory_filter_" + kFilters[tab])) continue;
            hit.kind = PointerHit::Kind::Tab;
            hit.tray = tray.tray;
            hit.tab = tab;
            return hit;
        }
        const auto scrolled = tray_scrolls_.find(tray.prefix + std::to_string(tray.tab));
        const int scroll = tray.rows > 5 && scrolled != tray_scrolls_.end() ? scrolled->second : 0;
        for (int n = 0; n < 5; ++n) {
            if (!over(std::string(tray.prefix) + "ItemTray" + std::to_string(n))) continue;
            if (static_cast<size_t>(scroll + n) >= tray.rows) return hit;
            hit.kind = PointerHit::Kind::Row;
            hit.tray = tray.tray;
            hit.tab = tray.tab;
            hit.row = scroll + n;
            return hit;
        }
    }
    return hit;
}

bool InventoryMenu::pointer_on_menu(const tl::core::InputState& in) const {
    if (!visible_ || !in.pointer) return false;
    for (const Node& node : nodes_) {
        if (node.panel == nullptr || (node.width > 0.95f && node.height > 0.95f)) continue;
        if (pointer_over(in, node.panel)) return true;
    }
    return false;
}

void InventoryMenu::point_at(Tray tray, int tab, int row) {
    if (menu_mode_ != MenuMode::Inventory) return;
    if (vendor_ == VendorScreen::None) {
        if (tray == Tray::Vendor) return;
        if ((tray == Tray::Pet) != (focus_ == Tray::Pet)) swap_bag_focus(0);
        if ((tray == Tray::Pet) != (focus_ == Tray::Pet)) return;
    } else {
        if (tray == Tray::Pet && !pet_tray_available_) return;
        focus_ = tray;
    }
    int& shown_tab =
        tray == Tray::Vendor ? vendor_tab_ : tray == Tray::Pet ? pet_tab_ : current_tab_;
    int& selected = tray == Tray::Vendor ? vendor_selected_
                    : tray == Tray::Pet  ? pet_selected_
                                         : selected_index_;
    if (shown_tab != tab) shown_tab = tab;
    selected = std::max(row, 0);
}

}  // namespace tl::render

namespace tl::render {

// DIVERGENCE (desktop mouse): the cell under the pointer takes the skills tab's cursor.
bool InventoryMenu::point_skill(const tl::core::InputState& in) {
    if (!visible_ || menu_mode_ != MenuMode::Skills || skill_map_open_ || !in.pointer) return false;
    const auto over = [&](const char* name) {
        const int index = find_node_index_by_widget(name);
        if (index < 0 || is_node_hidden(index)) return false;
        const Node& node = nodes_[static_cast<size_t>(index)];
        return in.pointer_x >= node.left && in.pointer_x < node.left + node.width &&
               in.pointer_y >= node.top && in.pointer_y < node.top + node.height;
    };
    char name[48];
    for (int pane = 0; pane < tl::core::kSkillPanes; ++pane) {
        for (int row = 0; row < tl::core::kSkillRows; ++row) {
            for (int column = 0; column < tl::core::kSkillColumns; ++column) {
                std::snprintf(name, sizeof(name), "skills/skillbg%d_%d_%d", pane, row, column);
                if (!over(name)) continue;
                skill_cursor_.in_spells = false;
                skill_cursor_.pane = pane;
                skill_cursor_.row = row;
                skill_cursor_.column = column;
                return true;
            }
        }
    }
    for (int i = 0; i < tl::core::kSkillSpellSlots; ++i) {
        std::snprintf(name, sizeof(name), "skills/spellbg%d", i);
        if (!over(name)) continue;
        skill_cursor_.in_spells = true;
        skill_cursor_.spell = i;
        return true;
    }
    return false;
}

int InventoryMenu::skill_slot_at(const tl::core::InputState& in) const {
    if (!visible_ || menu_mode_ != MenuMode::Skills || !in.pointer) return -1;
    for (int slot = 2; slot < tl::core::kSkillFunctionSlots; ++slot) {
        for (const char* row : {"skills/SkillSlot", "skillmap/MapSkillSlot"}) {
            const int index = find_node_index_by_widget(row + std::to_string(slot + 1));
            if (index < 0 || is_node_hidden(index)) continue;
            const Node& node = nodes_[static_cast<size_t>(index)];
            if (in.pointer_x >= node.left && in.pointer_x < node.left + node.width &&
                in.pointer_y >= node.top && in.pointer_y < node.top + node.height) {
                return slot;
            }
        }
    }
    return -1;
}

}  // namespace tl::render
