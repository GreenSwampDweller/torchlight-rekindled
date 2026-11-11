#include "render/pc_storage.h"

#include <vector>

namespace tl::render {

namespace {

/// The merchant's layout carries three sets of 42 stock slots, one a tab; both carry
/// three sets of 21 pet slots.
constexpr int kLayoutSlots = 126;
constexpr int kLayoutPetSlots = 63;

constexpr const char* kTabs[3] = {"TabMisc", "TabWeapon", "TabArmor"};
constexpr const char* kPetTabs[3] = {"TabBackpack", "TabSpell", "TabFish"};

std::string numbered(const char* stem, int number) { return stem + std::to_string(number); }

}  // namespace

bool PcStorage::create(const std::string& group, unsigned width, unsigned height, Kind kind) {
    const bool merchant = kind == Kind::Merchant;
    const std::string tag = merchant ? "tl/pcmerchant" : "tl/pcstash";
    // The stash has no model of its own: it stands in the merchant's.
    pane_.create(group, "merchant", /*right=*/false, width, height, tag, 547);
    std::vector<std::string> sets = {"media/UI/hud.imageset", "media/UI/ui2.imageset",
                                     "media/UI/uiicons.imageset",
                                     "media/UI/itemicons/itemicons.imageset"};
    for (int sheet = 1; sheet <= 16; ++sheet) {
        sets.push_back("media/UI/itemicons/itemicons" + std::to_string(sheet) + ".imageset");
    }
    screen_.use_imagesets(std::move(sets));
    const char* const layout =
        merchant ? "media/UI/merchantmenu.layout" : "media/UI/stashmenu.layout";
    if (!screen_.create_pc(group, width, height, layout, tag, 548)) {
        pane_.destroy();
        return false;
    }
    for (int number = kCells + 1; number <= kLayoutSlots; ++number) {
        screen_.set_hidden(numbered("Slot", number), true);
    }
    for (int number = kPetCells + 1; number <= kLayoutPetSlots; ++number) {
        screen_.set_hidden(numbered("PetSlot", number), true);
    }
    screen_.set_hidden("SlotGlow", true);
    // The layouts author `BottomFrame` at the right edge; the PC stands each frame on its
    // half of the pane's model, and this pane's lower half is on the left.
    screen_.set_relative("BottomFrame", 0.0f, 0.0f);
    screen_.set_offset("BottomFrame", 0.0f, 0.0f);
    screen_.refresh();
    counts_.create(tag, 549, kCells + kPetCells);
    height_ = height;
    return true;
}

void PcStorage::destroy() {
    counts_.destroy();
    screen_.destroy();
    pane_.destroy();
}

void PcStorage::set_visible(bool on) {
    if (!screen_.ready() || on == screen_.visible()) return;
    if (on) {
        pane_.show();
        screen_.show();
        counts_.show();
    } else {
        counts_.hide();
        screen_.hide();
        pane_.hide();
    }
}

void PcStorage::resize(unsigned width, unsigned height) {
    pane_.resize(width, height);
    screen_.resize(width, height);
    height_ = height;
}

void PcStorage::set(const View& view) {
    if (!screen_.ready()) return;
    for (int cell = 0; cell < kCells; ++cell) {
        screen_.set_image(numbered("Slot", cell + 1), view.cells[static_cast<size_t>(cell)].icon);
    }
    for (int tab = 0; tab < 3; ++tab) screen_.set_hover(kTabs[tab], tab == view.tab);
    // Without a pet the lower half keeps its frame and loses its tabs, caption and cells.
    screen_.set_hidden("BottomFrame", !view.pet);
    for (int cell = 0; cell < kPetCells; ++cell) {
        screen_.set_image(numbered("PetSlot", cell + 1),
                          view.pet_cells[static_cast<size_t>(cell)].icon);
    }
    for (int tab = 0; tab < 3; ++tab) screen_.set_hover(kPetTabs[tab], tab == view.pet_tab);
    screen_.refresh();

    const float down = static_cast<float>(height_);
    float left = 0.0f, top = 0.0f, wide = 0.0f, tall = 0.0f;
    for (int cell = 0; cell < kCells; ++cell) {
        if (!screen_.window_box(numbered("Slot", cell + 1), left, top, wide, tall)) continue;
        counts_.set(cell, left, top, view.cells[static_cast<size_t>(cell)].count, down);
    }
    for (int cell = 0; cell < kPetCells; ++cell) {
        if (!screen_.window_box(numbered("PetSlot", cell + 1), left, top, wide, tall)) continue;
        counts_.set(kCells + cell, left, top,
                    view.pet ? view.pet_cells[static_cast<size_t>(cell)].count : 0, down);
    }
}

bool PcStorage::blocks(float x, float y) const {
    return screen_.visible() && screen_.window_contains("Blocker", x, y);
}

}  // namespace tl::render
