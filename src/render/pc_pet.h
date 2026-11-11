// The PC's pet pane (`media/UI/petmenu.layout`), a left pane: the pet's name, level and
// health, the three trinkets it wears, its two spells, and its bag of 21 under three tabs.
#pragma once

#include "render/layout_screen.h"
#include "render/pc_counts.h"
#include "render/pc_doll.h"
#include "render/pc_pane.h"

#include <array>
#include <string>

namespace tl::render {

class PcPet {
public:
    /// The slots a tab shows at once (`Slot1` .. `Slot21`, seven by three).
    static constexpr int kCells = 21;
    static constexpr int kColumns = 7;
    /// The trinkets: `EquipRing1`, `EquipRing2`, `EquipAmulet`.
    static constexpr int kEquip = 3;
    static const char* equip_window(int trinket);

    struct Cell {
        std::string icon;  ///< empty for an empty slot
        int count = 0;
    };

    /// What the pane shows.
    struct View {
        std::string name;
        std::string level;
        std::string health;
        std::array<std::string, kEquip> worn;  ///< icons; empty for nothing worn
        std::array<std::string, 2> spells;     ///< icons; empty for none
        std::array<Cell, kCells> cells;
        int tab = 0;  ///< 0 equipment, 1 spells, 2 fish
    };

    /// Build it against the mounted data. False when `petmenu.layout` is not there.
    bool create(const std::string& group, unsigned width, unsigned height);
    void destroy();
    bool ready() const { return screen_.ready(); }
    bool visible() const { return screen_.visible(); }
    void set_visible(bool on);
    void resize(unsigned width, unsigned height);

    void set(const View& view);
    /// The pet in the paperdoll's window: `material` draws the preview, `aspect` wide for
    /// one tall. `live` false leaves the window to the preview's backdrop.
    void set_doll(const std::string& material, float aspect, bool live);

    /// The window under the pointer; empty over none.
    std::string point(float x, float y) const { return screen_.window_at(x, y); }
    /// The pane's `Blocker`: the pointer inside it is the pane's.
    bool blocks(float x, float y) const;
    /// The pane's windows, for the art of its imagesets.
    const LayoutScreen& screen() const { return screen_; }

private:
    PcPane pane_;
    LayoutScreen screen_;
    PcCounts counts_;
    PcDoll doll_;
    /// The picture each trinket window authors for an empty place.
    std::array<std::string, kEquip> empty_;
    unsigned width_ = 0;
    unsigned height_ = 0;
};

}  // namespace tl::render
