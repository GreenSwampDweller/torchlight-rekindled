// The PC's inventory (`media/UI/inventorymenu.layout`), the right pane: the paperdoll's
// equipment windows, three tabs over 21 slots, and the gold. It is a view over the
// console's inventory (`core/inventory.h`: 19 fixed locations and one bag), which is what
// a save holds: the tabs are filters over the bag.
#pragma once

#include "render/layout_screen.h"
#include "render/pc_counts.h"
#include "render/pc_doll.h"
#include "render/pc_pane.h"

#include <OgrePrerequisites.h>

#include <array>
#include <string>

namespace tl::render {

class PcInventory {
public:
    /// The slots a tab shows at once (`Slot1` .. `Slot21`, seven by three).
    static constexpr int kCells = 21;
    static constexpr int kColumns = 7;
    /// The equipment windows, in the order of the equip locations 0 to 10.
    static constexpr int kEquip = 11;
    static const char* equip_window(int location);

    /// What the pane shows.
    struct View {
        std::array<std::string, kEquip> worn;   ///< icons; empty for nothing worn
        std::array<std::string, kCells> cells;  ///< icons; empty for an empty slot
        std::array<int, kCells> counts{};       ///< the stack in each cell
        int tab = 0;                            ///< 0 backpack, 1 spells, 2 fish
        std::string gold;
    };

    /// Build it against the mounted data. False when `inventorymenu.layout` is not there.
    bool create(const std::string& group, unsigned width, unsigned height);
    void destroy();
    bool ready() const { return screen_.ready(); }
    bool visible() const { return screen_.visible(); }
    void set_visible(bool on);
    void resize(unsigned width, unsigned height);

    void set(const View& view);
    /// The character in the paperdoll's window: `material` draws the preview, whose
    /// picture is `aspect` wide for one tall. `live` false leaves the window to the
    /// preview's backdrop.
    void set_doll(const std::string& material, float aspect, bool live);

    /// The window under the pointer; empty over none.
    std::string point(float x, float y) const { return screen_.window_at(x, y); }
    /// The pane's `Blocker`: the pointer inside it is the pane's.
    bool blocks(float x, float y) const;
    /// The pane's windows, for the art of its imagesets.
    const LayoutScreen& screen() const { return screen_; }

private:
    PcPane pane_;
    PcCounts counts_;
    PcDoll doll_;
    unsigned width_ = 0;
    unsigned height_ = 0;
    LayoutScreen screen_;
    /// The picture each equipment window authors for an empty location.
    std::array<std::string, kEquip> empty_;
};

}  // namespace tl::render
