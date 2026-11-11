#include "render/pc_pet.h"

#include <vector>

namespace tl::render {

namespace {

/// The layout carries three sets of 21 slots, one a tab; one set is drawn.
constexpr int kLayoutSlots = 63;

constexpr const char* kEquipWindows[PcPet::kEquip] = {"EquipRing1", "EquipRing2", "EquipAmulet"};
constexpr const char* kTabs[3] = {"TabBackpack", "TabSpell", "TabFish"};

std::string slot_window(int number) { return "Slot" + std::to_string(number); }

}  // namespace

const char* PcPet::equip_window(int trinket) {
    return trinket >= 0 && trinket < kEquip ? kEquipWindows[trinket] : "";
}

bool PcPet::create(const std::string& group, unsigned width, unsigned height) {
    pane_.create(group, "pet", /*right=*/false, width, height, "tl/pcpetpane", 547);
    std::vector<std::string> sets = {"media/UI/hud.imageset", "media/UI/ui2.imageset",
                                     "media/UI/uiicons.imageset",
                                     "media/UI/itemicons/itemicons.imageset"};
    for (int sheet = 1; sheet <= 16; ++sheet) {
        sets.push_back("media/UI/itemicons/itemicons" + std::to_string(sheet) + ".imageset");
    }
    screen_.use_imagesets(std::move(sets));
    if (!screen_.create_pc(group, width, height, "media/UI/petmenu.layout", "tl/pcpetpane", 548)) {
        pane_.destroy();
        return false;
    }
    for (int trinket = 0; trinket < kEquip; ++trinket) {
        empty_[static_cast<size_t>(trinket)] = screen_.image(kEquipWindows[trinket]);
    }
    for (int number = kCells + 1; number <= kLayoutSlots; ++number) {
        screen_.set_hidden(slot_window(number), true);
    }
    screen_.set_hidden("SlotGlow", true);
    // INVENTED: the experience, the mana and the four figures under the spells are not
    // the pet's here; their windows are left empty.
    for (const char* blank :
         {"XP", "Mana", "MeleeDamage", "RangedDamage", "MagicDamage", "Defense"}) {
        screen_.set_caption(blank, "");
    }
    screen_.refresh();
    counts_.create("tl/pcpetpane", 549, kCells);
    width_ = width;
    height_ = height;
    return true;
}

void PcPet::destroy() {
    doll_.destroy();
    counts_.destroy();
    screen_.destroy();
    pane_.destroy();
}

void PcPet::set_visible(bool on) {
    if (!screen_.ready() || on == screen_.visible()) return;
    if (on) {
        pane_.show();
        screen_.show();
        counts_.show();
        doll_.show();
    } else {
        doll_.hide();
        counts_.hide();
        screen_.hide();
        pane_.hide();
    }
}

void PcPet::resize(unsigned width, unsigned height) {
    pane_.resize(width, height);
    screen_.resize(width, height);
    width_ = width;
    height_ = height;
}

void PcPet::set(const View& view) {
    if (!screen_.ready()) return;
    screen_.set_caption("CharacterName", view.name);
    screen_.set_caption("Level", view.level);
    screen_.set_caption("HP", view.health);
    for (int trinket = 0; trinket < kEquip; ++trinket) {
        const std::string& worn = view.worn[static_cast<size_t>(trinket)];
        screen_.set_image(kEquipWindows[trinket],
                          worn.empty() ? empty_[static_cast<size_t>(trinket)] : worn);
    }
    screen_.set_image("Spell1", view.spells[0]);
    screen_.set_image("Spell2", view.spells[1]);
    for (int cell = 0; cell < kCells; ++cell) {
        screen_.set_image(slot_window(cell + 1), view.cells[static_cast<size_t>(cell)].icon);
    }
    for (int tab = 0; tab < 3; ++tab) screen_.set_hover(kTabs[tab], tab == view.tab);
    screen_.refresh();
    float left = 0.0f, top = 0.0f, wide = 0.0f, tall = 0.0f;
    for (int cell = 0; cell < kCells; ++cell) {
        if (!screen_.window_box(slot_window(cell + 1), left, top, wide, tall)) continue;
        counts_.set(cell, left, top, view.cells[static_cast<size_t>(cell)].count,
                    static_cast<float>(height_));
    }
}

void PcPet::set_doll(const std::string& material, float aspect, bool live) {
    if (!screen_.ready()) return;
    if (!doll_.made()) {
        doll_.create("tl/pcpetpane", 544, material);
        if (screen_.visible()) doll_.show();
    }
    PcDoll::Box box;
    if (!screen_.window_box("PaperdollEquip", box.left, box.top, box.wide, box.tall)) return;
    doll_.place(box, static_cast<float>(width_), static_cast<float>(height_), aspect, live);
}

bool PcPet::blocks(float x, float y) const {
    return screen_.visible() && screen_.window_contains("Blocker", x, y);
}

}  // namespace tl::render
