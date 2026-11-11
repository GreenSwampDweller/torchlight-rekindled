// The inventory menu's trays: which one has the focus, and their tabs. Split from
// inventory_menu.cpp.
#include "render/inventory_menu.h"

#include "core/inventory_screen.h"
#include "render/menu_chrome.h"
#include "render/text_layout.h"

#include <OgreFontManager.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace tl::render {

void InventoryMenu::focus_next_tray(int direction) {
    if (vendor_ == VendorScreen::None || direction == 0) return;
#if defined(TL_PLATFORM_IOS)
    // DIVERGENCE (platform): the touch pad's LB rotates over the vendor's tray, the
    // bag and the pet's, and on the enchanter swaps the bag and the pet's.
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
    const bool on_vendor = focus_ == Tray::Vendor;
    int& tab = on_vendor ? vendor_tab_ : current_tab_;
    const int leave =
        tl::core::step_tray_tab(tab, direction, on_vendor && vendor_ == VendorScreen::Combine);
    if (on_vendor) {
        vendor_selected_ = 0;
    } else {
        selected_index_ = 0;
    }
    if (leave == 0) return;
#if defined(TL_PLATFORM_IOS) || defined(TL_PLATFORM_MACOS)
    // INVENTED (DIVERGENCE, platform: the Mac and the touch pad): the trays in the order they
    // stand on the screen - the vendor's, the bag, the pet's when it is here - wrapping, so
    // none is passed over.
    std::vector<Tray> order{Tray::Vendor, Tray::Bag};
    if (pet_tray_available_ && !enchanter_screen()) order.push_back(Tray::Pet);
    const auto at = std::find(order.begin(), order.end(), focus_);
    const int here = at == order.end() ? 0 : static_cast<int>(at - order.begin());
    const int count = static_cast<int>(order.size());
    focus_ = order[static_cast<size_t>(((here + leave) % count + count) % count)];
    int& landed = focus_ == Tray::Vendor ? vendor_tab_
                  : focus_ == Tray::Pet  ? pet_tab_
                                         : current_tab_;
    landed = leave > 0 ? 0 : tl::core::kInventoryFilterCount - 1;
    if (focus_ == Tray::Vendor) {
        vendor_selected_ = 0;
    } else if (focus_ == Tray::Pet) {
        pet_selected_ = 0;
    } else {
        selected_index_ = 0;
    }
#else
    // (leave, true)`: the tray it lands on starts at the edge it
    // came in from - tab 0 moving right, tab 5 moving left.
    focus_next_tray(leave);
    int& landed = focus_ == Tray::Vendor ? vendor_tab_ : current_tab_;
    landed = leave > 0 ? 0 : tl::core::kInventoryFilterCount - 1;
    if (focus_ == Tray::Vendor) {
        vendor_selected_ = 0;
    } else {
        selected_index_ = 0;
    }
#endif
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

void InventoryMenu::set_tray_legend(const std::string& prefix, const std::string& a,
                                    const std::string& x, const std::string& y) {
    set_widget_visible(prefix + "buttonB", false);
    const std::string captions[3][2] = {{"buttonA", a}, {"buttonX", x}, {"buttonY", y}};
    for (const auto& button : captions) {
        set_widget_text(prefix + button[0] + "text", button[1]);
        set_widget_visible(prefix + button[0], !button[1].empty());
    }

    // the row centred in `buttontray` by the captions' widths, in pixels.
    // Each button is `buttonA`'s width plus its caption's extent and a pad (2.5
    // for A and X, none for Y, nothing for an empty caption); a shown one gets a share of
    // what is left of 0.9 of the tray. The order across is A, Y, X.
    Node* button_a = find_node_by_widget(prefix + "buttonA");
    Node* tray = find_node_by_widget(prefix + "buttontray");
    Ogre::FontPtr font = Ogre::FontManager::getSingleton().getByName(kFontName);
    if (button_a == nullptr || tray == nullptr || !font || scale_ <= 0.0f) return;
    if (!font->isLoaded()) font->load();
    const float across = static_cast<float>(width_ > 0 ? width_ : 1);
    const float down = static_cast<float>(height_ > 0 ? height_ : 1);
    const float icon = button_a->wide * scale_;
    const auto extent = [&](const char* button, const std::string& caption, float pad) {
        const Node* text = find_node_by_widget(prefix + button + "text");
        if (caption.empty() || text == nullptr) return 0.0f;
        const float line = text->points * ui_points_to_pixels() * scale_ / down;
        return measure_text(font, caption, line, across, down) * across + pad;
    };
    const float wa = extent("buttonA", a, 2.5f) + icon;
    const float wx = extent("buttonX", x, 2.5f) + icon;
    const float wy = extent("buttonY", y, 0.0f) + icon;
    const float spare = std::fma(tray->wide * scale_, 0.9f, -((wy + wx) + wa)) * 0.15f;
    const float gap = spare >= 0.0f ? spare : 0.0f;
    const float ga = (a.empty() ? 0.0f : gap) + wa;
    const float gy = (y.empty() ? 0.0f : gap) + wy;
    const float gx = (x.empty() ? 0.0f : gap) + wx;
    const float half_gap = gap * 0.5f;
    const float half_row = ((gy + gx) + ga) * 0.5f;
    const auto place = [&](const char* button, float px) {
        if (Node* node = find_node_by_widget(prefix + button)) node->off_x = px / scale_;
    };
    place("buttonA", (half_gap - half_row) + 5.0f);
    place("buttonB", ((((half_gap + gy) + gx) + ga) - half_row) + 5.0f);
    place("buttonX", (((half_gap + gy) + ga) - half_row) + 5.0f);
    place("buttonY", ((half_gap + ga) - half_row) + 5.0f);
}

}  // namespace tl::render
