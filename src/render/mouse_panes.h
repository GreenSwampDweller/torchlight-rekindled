// The PC's side panes under the mouse controls: which are open, what they show, and what
// a click on them does. Held by the mouse layer (render/mouse_game.cpp).
#pragma once

#include "render/pc_cursor_item.h"
#include "render/pc_inventory.h"
#include "render/pc_pet.h"
#include "render/pc_quests.h"
#include "render/pc_skills.h"
#include "render/pc_stats.h"
#include "render/pc_storage.h"
#include "render/pc_tooltip.h"
#include "render/renderer_impl.h"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace tl::render {

struct Renderer::Impl::MousePanes {
    Impl& g;
    PcStats stats;
    bool stats_tried = false;
    bool stats_open = false;
    PcQuests quests;
    bool quests_tried = false;
    bool quests_open = false;
    /// The map was up when the right pane came over its corner, and is put back after.
    bool map_under_pane = false;
    /// The quest chosen in the log, by its place in the list.
    int quest_selected = 0;
    PcInventory inventory;
    bool inventory_tried = false;
    bool inventory_open = false;
    /// The inventory's tab (0 backpack, 1 spells, 2 fish), how many rows it is scrolled,
    /// and the bag slot each of its cells shows (-1 for an empty cell).
    int inventory_tab = 0;
    int inventory_scroll = 0;
    std::array<int, PcInventory::kCells> inventory_cells{};
    /// The left pane over a merchant's stock or a stash, whichever `storage_open` names
    /// (`None` for neither), with the pet's bag under it.
    PcStorage merchant;
    PcStorage stash;
    bool merchant_tried = false;
    bool stash_tried = false;
    VendorScreen storage_open = VendorScreen::None;
    /// The stock's tab (a merchant's: 0 misc, 1 weapons, 2 armour; -1 until the stock
    /// chooses it), its rows scrolled, and the slot each cell shows (-1 for an empty cell).
    int storage_tab = 0;
    int storage_scroll = 0;
    std::array<int, PcStorage::kCells> storage_cells{};
    /// The same for the pet's bag (0 equipment, 1 spells, 2 fish).
    int pet_tab = 0;
    int pet_scroll = 0;
    std::array<int, PcStorage::kPetCells> pet_cells{};
    /// The skills pane, on the right: its tab, and the skill each of its cells shows.
    PcSkills skills;
    bool skills_tried = false;
    bool skills_open = false;
    int skills_tab = 0;
    std::vector<std::int64_t> skill_cells;
    /// Where the PC's own class file puts each skill of `skill_tree_class`, by its name,
    /// and that class's three tab names.
    std::string skill_tree_class;
    std::map<std::string, tl::core::ClassSkill> skill_tree;
    std::array<std::string, 3> skill_tabs;
    /// The pet's own pane, on the left; its bag's tab, rows and cells are `pet_*` above.
    PcPet pet_pane;
    bool pet_tried = false;
    bool pet_open = false;
    /// The item on the pointer: whose it is (`p` the player's, `a` the pet's, `s` the
    /// stock's; 0 for none), its slot there, and what it was when it was taken.
    struct Held {
        char owner = 0;
        int slot = -1;
        std::string name;
        std::string icon;
    };
    Held held;
    PcCursorItem cursor;
    tl::core::Inventory* held_inventory();
    void pick_up(char owner, int slot);
    void release();
    void sync_held();
    void place_held(bool right);
    bool drop_held();
    /// Whether the pointer is in a pane, on a window of it or not.
    bool pointed() const {
        return over_inventory || over_storage || over_pet || over_skills || over_quests ||
               !over.empty();
    }
    /// The pane window the pointer is over, and whose it is.
    std::string over;
    bool over_quests = false;
    bool over_inventory = false;
    bool over_storage = false;
    bool over_pet = false;
    bool over_skills = false;
    /// The details window of the item under the pointer: what it shows (-1 for nothing)
    /// and where the pointer was when it was last placed.
    PcTooltip tip;
    std::string tip_key;
    float over_x = 0.0f;
    float over_y = 0.0f;
    float tip_x = -1.0f;
    float tip_y = -1.0f;

    explicit MousePanes(Impl& game) : g(game) {}

    /// The character sheet opened or closed. False when the PC's is not in the mounted
    /// data, so the caller opens the console's.
    bool toggle_stats();
    void close_stats();
    /// The quest log opened or closed; false when the PC's is not in the mounted data.
    bool toggle_quests();
    void sync_quests();
    void click_quests();
    /// The inventory opened or closed; false when the PC's is not in the mounted data.
    bool toggle_inventory();
    void sync_inventory(float delta);
    void click_inventory(bool right, bool control, bool shift);
    /// The inventory slot of the window `over` names, worn or in the bag; -1 for none.
    int inventory_slot_over() const;
    /// What the HUD has under the pointer, for the details window: a skill (null for none),
    /// the bag slot of an item bound to a numbered slot (-1 for none), and the HUD's own
    /// windows for the frame's art.
    struct HudOver {
        const tl::core::KnownSkill* skill = nullptr;
        int item_slot = -1;
        const LayoutScreen* art = nullptr;
    };
    /// The item or the skill under the pointer described beside it; `pointer` false takes
    /// it down.
    void sync_tooltip(bool pointer, const HudOver& hud);
    /// The pane of a merchant's stock or a stash and the inventory beside it; false when
    /// the PC's are not in the mounted data.
    bool open_storage(VendorScreen screen);
    void close_storage();
    void sync_storage();
    void click_storage(bool right);
    /// The pane a HUD button's or a key's function opens or closes; false when it names
    /// none, or the PC's is not in the mounted data.
    bool toggle_for(const std::string& function);
    /// The skills pane opened or closed; false when the PC's is not in the mounted data.
    bool toggle_skills();
    void load_skill_tree();
    void sync_skills();
    void click_skills();
    /// The skill or the spell the window `over` names in the skills pane; null for none.
    const tl::core::KnownSkill* skill_over() const;
    /// The skill under the pointer described beside it: the skills pane's, else `hud`, a
    /// skill of the HUD's the pointer is on (null for none) with `hud_art` for the frame.
    /// False when the pointer is on no skill.
    bool skill_tooltip(bool pointer, const tl::core::KnownSkill* hud, const LayoutScreen* hud_art);
    /// The pet's pane opened or closed; false when the PC's is not in the mounted data.
    bool toggle_pet();
    void sync_pet(float delta);
    void click_pet(bool right);
    /// The pet's bag slot the window `over` names in its own pane, worn or not; -1 for none.
    int pet_pane_slot_over() const;
    /// The pane and the inventory `storage_open` names; null for none.
    PcStorage* storage_pane();
    tl::core::Inventory* storage_inventory();
    /// The slot of the stock, or of the pet's bag, the window `over` names; -1 for none.
    int storage_slot_over() const;
    int pet_slot_over() const;
    /// The wheel over the inventory or the stock turns its rows; true when it took the wheel.
    bool scroll(int wheel);
    /// Every pane closed (`KEYMAP_CLOSEALL`).
    void close_all();
    /// The view the panes were last laid out for, and every pane made laid out again when
    /// it changes (a window resized).
    unsigned view_width = 0;
    unsigned view_height = 0;
    void follow_view();
    /// Every frame: what is shown, and its contents.
    void sync(float delta);
    /// The pointer at (x, y): `over` set; true when a pane has the pointer.
    bool point(float x, float y);
    /// A click on `over`: the right button's, or the left's, with Ctrl or Shift held or not.
    void click(bool right, bool control, bool shift);
};

}  // namespace tl::render
