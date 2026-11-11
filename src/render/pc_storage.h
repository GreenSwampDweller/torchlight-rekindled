// The PC's left pane over an inventory that is not the player's: a merchant's stock
// (`media/UI/merchantmenu.layout`, three tabs) or a stash (`media/UI/stashmenu.layout`),
// 42 slots, with the pet's bag of 21 under it. A view over the inventories as the console
// keeps them; the tabs are filters.
#pragma once

#include "render/layout_screen.h"
#include "render/pc_counts.h"
#include "render/pc_pane.h"

#include <array>
#include <string>

namespace tl::render {

class PcStorage {
public:
    enum class Kind { Merchant, Stash };

    /// The slots shown at once (`Slot1` .. `Slot42`, seven by six).
    static constexpr int kCells = 42;
    static constexpr int kColumns = 7;
    static constexpr int kRows = 6;
    /// The pet's (`PetSlot1` .. `PetSlot21`, seven by three).
    static constexpr int kPetCells = 21;
    static constexpr int kPetRows = 3;

    struct Cell {
        std::string icon;  ///< empty for an empty slot
        int count = 0;
    };

    /// What the pane shows.
    struct View {
        std::array<Cell, kCells> cells;
        int tab = 0;       ///< a merchant's: 0 misc, 1 weapons, 2 armour
        bool pet = false;  ///< the pet's bag is under the stock
        std::array<Cell, kPetCells> pet_cells;
        int pet_tab = 0;  ///< 0 equipment, 1 spells, 2 fish
    };

    /// Build it against the mounted data. False when the kind's layout is not there.
    bool create(const std::string& group, unsigned width, unsigned height, Kind kind);
    void destroy();
    bool ready() const { return screen_.ready(); }
    bool visible() const { return screen_.visible(); }
    void set_visible(bool on);
    void resize(unsigned width, unsigned height);

    void set(const View& view);

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
    unsigned height_ = 0;
};

}  // namespace tl::render
