#include "render/pc_inventory.h"

#include <cstdio>
#include <vector>

namespace tl::render {

namespace {

constexpr const char* kLayout = "media/UI/inventorymenu.layout";

/// The layout carries three sets of 21 slots, one a tab; one set is drawn.
constexpr int kLayoutSlots = 63;

constexpr const char* kEquipWindows[PcInventory::kEquip] = {
    "EquipRightHand", "EquipLeftHand", "EquipGloves", "EquipHelm",  "EquipChest", "EquipShoulder",
    "EquipBoots",     "EquipBelt",     "EquipRing1",  "EquipRing2", "EquipAmulet"};

constexpr const char* kTabs[3] = {"TabBackpack", "TabSpell", "TabFish"};

std::string slot_window(int number) {
    char name[16];
    std::snprintf(name, sizeof name, "Slot%d", number);
    return name;
}

}  // namespace

const char* PcInventory::equip_window(int location) {
    return location >= 0 && location < kEquip ? kEquipWindows[location] : "";
}

bool PcInventory::create(const std::string& group, unsigned width, unsigned height) {
    pane_.create(group, "inventory", /*right=*/true, width, height, "tl/pcinventory", 545);
    std::vector<std::string> sets = {"media/UI/hud.imageset", "media/UI/ui2.imageset",
                                     "media/UI/uiicons.imageset",
                                     "media/UI/itemicons/itemicons.imageset"};
    for (int sheet = 1; sheet <= 16; ++sheet) {
        sets.push_back("media/UI/itemicons/itemicons" + std::to_string(sheet) + ".imageset");
    }
    screen_.use_imagesets(std::move(sets));
    if (!screen_.create_pc(group, width, height, kLayout, "tl/pcinventory", 546)) {
        pane_.destroy();
        return false;
    }
    for (int location = 0; location < kEquip; ++location) {
        empty_[static_cast<size_t>(location)] = screen_.image(kEquipWindows[location]);
    }
    for (int number = kCells + 1; number <= kLayoutSlots; ++number) {
        screen_.set_hidden(slot_window(number), true);
    }
    // The slot's glow follows a dragged item on the PC; nothing is dragged here.
    screen_.set_hidden("SlotGlow", true);
    screen_.refresh();
    counts_.create("tl/pcinventory", 549, kCells);
    width_ = width;
    height_ = height;
    return true;
}

void PcInventory::set_doll(const std::string& material, float aspect, bool live) {
    if (!screen_.ready()) return;
    if (!doll_.made()) {
        doll_.create("tl/pcinventory", 544, material);
        if (screen_.visible()) doll_.show();
    }
    PcDoll::Box box;
    if (!screen_.window_box("PaperdollEquip", box.left, box.top, box.wide, box.tall)) return;
    doll_.place(box, static_cast<float>(width_), static_cast<float>(height_), aspect, live);
}

void PcInventory::destroy() {
    doll_.destroy();
    counts_.destroy();
    screen_.destroy();
    pane_.destroy();
}

void PcInventory::set_visible(bool on) {
    if (!screen_.ready() || on == screen_.visible()) return;
    if (on) {
        pane_.show();
        screen_.show();
        counts_.show();
        doll_.show();
    } else {
        screen_.hide();
        pane_.hide();
        counts_.hide();
        doll_.hide();
    }
}

void PcInventory::resize(unsigned width, unsigned height) {
    pane_.resize(width, height);
    screen_.resize(width, height);
    width_ = width;
    height_ = height;
}

// The pane's windows, by their names in the layout.
void PcInventory::set(const View& view) {
    if (!screen_.ready()) return;
    for (int location = 0; location < kEquip; ++location) {
        const std::string& worn = view.worn[static_cast<size_t>(location)];
        screen_.set_image(kEquipWindows[location],
                          worn.empty() ? empty_[static_cast<size_t>(location)] : worn);
    }
    for (int cell = 0; cell < kCells; ++cell) {
        screen_.set_image(slot_window(cell + 1), view.cells[static_cast<size_t>(cell)]);
    }
    for (int tab = 0; tab < 3; ++tab) screen_.set_hover(kTabs[tab], tab == view.tab);
    screen_.set_caption("Money", view.gold);
    screen_.refresh();
    for (int cell = 0; cell < kCells; ++cell) {
        float left = 0.0f, top = 0.0f, wide = 0.0f, tall = 0.0f;
        if (!screen_.window_box(slot_window(cell + 1), left, top, wide, tall)) continue;
        counts_.set(cell, left, top, view.counts[static_cast<size_t>(cell)],
                    static_cast<float>(height_));
    }
}

bool PcInventory::blocks(float x, float y) const {
    return screen_.visible() && screen_.window_contains("Blocker", x, y);
}

}  // namespace tl::render
