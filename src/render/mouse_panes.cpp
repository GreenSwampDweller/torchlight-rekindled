#include "render/mouse_panes.h"

#include "core/combat.h"
#include "core/log.h"
#include "core/text.h"

#include <algorithm>
#include <cstdlib>
#include <vector>

namespace tl::render {

namespace {

constexpr const char* kSpend[4] = {"SpendStrength", "SpendDexterity", "SpendMagic", "SpendDefense"};
constexpr const char* kReclaim[4] = {"ReclaimStrength", "ReclaimDexterity", "ReclaimMagic",
                                     "ReclaimDefense"};

}  // namespace

/// The character sheet up or down.
bool Renderer::Impl::MousePanes::toggle_stats() {
    if (!stats_tried) {
        stats_tried = true;
        stats.create(kGameGroup, g.config.width, g.config.height);
    }
    if (!stats.ready()) return false;
    if (stats_open) {
        close_stats();
        return true;
    }
    stats_open = true;
    // One pane a side: the sheet takes the stock's and the pet's.
    if (storage_open != VendorScreen::None) close_storage();
    if (pet_open) toggle_pet();
    tl::core::open_stats_page(g.stats_page, g.player_character.unspent_stat_points());
    g.play_ui(tl::core::InterfaceSound::MenuOpen);
    TL_LOG_INFO("mouse: the character sheet opens");
    return true;
}

void Renderer::Impl::MousePanes::close_stats() {
    stats_open = false;
    over.clear();
    stats.set_visible(false);
    // The points placed are the character's from here (`close_stats_page`).
    if (g.stats_page.open) {
        tl::core::close_stats_page(g.stats_page, g.player_character);
        g.refresh_game_hud();
    }
    g.play_ui(tl::core::InterfaceSound::MenuClose);
    TL_LOG_INFO("mouse: the character sheet closes");
}

/// The quest log up or down.
bool Renderer::Impl::MousePanes::toggle_quests() {
    if (!quests_tried) {
        quests_tried = true;
        quests.create(kGameGroup, g.config.width, g.config.height);
    }
    if (!quests.ready()) return false;
    quests_open = !quests_open;
    if (!quests_open) quests.set_visible(false);
    // One pane a side: the log takes the inventory's and the skills'.
    if (quests_open && inventory_open) toggle_inventory();
    if (quests_open && skills_open) toggle_skills();
    g.play_ui(quests_open ? tl::core::InterfaceSound::MenuOpen
                          : tl::core::InterfaceSound::MenuClose);
    TL_LOG_INFO("mouse: the quest log %s", quests_open ? "opens" : "closes");
    return true;
}

/// The log's contents: the quests that hold a tray, the chosen one's text and rewards.
void Renderer::Impl::MousePanes::sync_quests() {
    if (!quests.ready()) return;
    const bool show = quests_open && !g.hud.letterboxed() && !g.inventory_menu.visible();
    quests.set_visible(show);
    if (!show) return;
    const std::vector<const tl::core::QuestEntry*> entries = g.quest_log.journal_entries();
    PcQuests::View view;
    for (const tl::core::QuestEntry* entry : entries) {
        if (view.names.size() == static_cast<size_t>(PcQuests::kRows)) break;
        view.names.push_back(tl::core::strip_markup(entry->display_name()));
    }
    if (quest_selected >= static_cast<int>(view.names.size())) quest_selected = 0;
    view.selected = view.names.empty() ? -1 : quest_selected;
    if (view.selected >= 0) {
        const tl::core::QuestEntry& chosen = *entries[static_cast<size_t>(view.selected)];
        view.details = tl::core::strip_markup(chosen.details());
        view.experience = chosen.reward.xp;
        view.gold = chosen.reward.gold;
        view.fame = chosen.reward.fame;
        view.reward_icon = chosen.reward_icon;
        view.can_abandon = chosen.definition != nullptr && chosen.definition->can_abandon;
    }
    quests.set(view);
}

/// A click in the log: a quest's row chooses it, `Abandon` gives the chosen one up as
/// the console's log does, `Close` closes.
void Renderer::Impl::MousePanes::click_quests() {
    if (over == "Close") {
        toggle_quests();
        return;
    }
    for (int row = 0; row < PcQuests::kRows; ++row) {
        const std::string number = std::to_string(row + 1);
        if (over == "Quest" + number || over == "QuestText" + number) quest_selected = row;
    }
    if (over != "Abandon") return;
    const std::vector<const tl::core::QuestEntry*> entries = g.quest_log.journal_entries();
    if (quest_selected < 0 || static_cast<size_t>(quest_selected) >= entries.size()) return;
    const std::string name = entries[static_cast<size_t>(quest_selected)]->name();
    if (!g.quest_log.abandon(name)) return;
    // `CQuestMenu::abandonQuest` and `CQuestManager::removeQuest`:
    // the sound, the quest's items taken back, the icons.
    g.play_ui(tl::core::InterfaceSound::QuestAbandoned);
    TL_LOG_INFO("quest abandoned: %s", name.c_str());
    g.apply_quest_item_removals();
    g.update_automap_icons();
    g.update_npc_icons();
    g.update_menu();
}

/// The inventory up or down.
bool Renderer::Impl::MousePanes::toggle_inventory() {
    if (!inventory_tried) {
        inventory_tried = true;
        inventory.create(kGameGroup, g.config.width, g.config.height);
    }
    if (!inventory.ready()) return false;
    inventory_open = !inventory_open;
    if (!inventory_open) inventory.set_visible(false);
    if (!inventory_open && storage_open != VendorScreen::None) close_storage();
    if (inventory_open && quests_open) toggle_quests();
    if (inventory_open && skills_open) toggle_skills();
    g.play_ui(inventory_open ? tl::core::InterfaceSound::MenuOpen
                             : tl::core::InterfaceSound::MenuClose);
    TL_LOG_INFO("mouse: the inventory %s", inventory_open ? "opens" : "closes");
    return true;
}

// DIVERGENCE: the PC's three tabs hold 21 slots each; the bag here is the console's
// one of 50, so a tab is a filter over it (spells, fish, everything else) and the wheel
// turns its rows when it holds more than 21.
void Renderer::Impl::MousePanes::sync_inventory(float delta) {
    if (!inventory.ready()) return;
    const bool show = inventory_open && !g.hud.letterboxed() && !g.inventory_menu.visible();
    inventory.set_visible(show);
    if (!show) return;
    g.load_unit_types();
    const std::vector<tl::core::InventorySlot>& slots = g.player_inventory.slots();
    PcInventory::View view;
    for (int location = 0; location < PcInventory::kEquip; ++location) {
        const tl::core::InventorySlot& worn = slots[static_cast<size_t>(location)];
        if (worn.count > 0) view.worn[static_cast<size_t>(location)] = worn.item.icon;
    }
    // The bag's items of this tab, in the bag's order.
    std::vector<int> listed;
    for (size_t slot = tl::core::Inventory::kFixedSlotCount; slot < slots.size(); ++slot) {
        if (slots[slot].count <= 0) continue;
        const uint32_t type = g.unit_types.id_of(slots[slot].item.unittype);
        const int tab = g.unit_types.isa(type, tl::core::unit_type::kFish)    ? 2
                        : g.unit_types.isa(type, tl::core::unit_type::kSpell) ? 1
                                                                              : 0;
        if (tab == inventory_tab) listed.push_back(static_cast<int>(slot));
    }
    const int rows =
        (static_cast<int>(listed.size()) + PcInventory::kColumns - 1) / PcInventory::kColumns;
    inventory_scroll = std::max(0, std::min(inventory_scroll, rows - 3));
    inventory_cells.fill(-1);
    for (int cell = 0; cell < PcInventory::kCells; ++cell) {
        const size_t at = static_cast<size_t>(inventory_scroll) * PcInventory::kColumns +
                          static_cast<size_t>(cell);
        if (at >= listed.size()) break;
        inventory_cells[static_cast<size_t>(cell)] = listed[at];
        view.cells[static_cast<size_t>(cell)] = slots[static_cast<size_t>(listed[at])].item.icon;
        view.counts[static_cast<size_t>(cell)] = slots[static_cast<size_t>(listed[at])].count;
    }
    view.tab = inventory_tab;
    view.gold = std::to_string(g.player_inventory.gold());
    inventory.set(view);
    // The paperdoll is the console menu's preview of the character; with the pet's pane
    // up the one preview is the pet's (`sync_pet`), and this window keeps its backdrop.
    const bool own = !pet_pane.visible();
    if (own) g.update_character_preview(delta);
    if (g.preview_texture) inventory.set_doll("tl/inv/preview_material", 266.0f / 412.0f, own);
}

/// A click in the inventory. A tab chooses its filter; the left button takes an item of the
/// bag onto the pointer, the right takes the console's own action for it (used, worn, fed
/// to the pet, learnt); a worn item is taken off.
// INVENTED: Ctrl with a click drops it at the player's feet, Shift hands it to the pet.
void Renderer::Impl::MousePanes::click_inventory(bool right, bool control, bool shift) {
    if (over == "Close") {
        toggle_inventory();
        return;
    }
    static constexpr const char* kTabs[3] = {"TabBackpack", "TabSpell", "TabFish"};
    for (int tab = 0; tab < 3; ++tab) {
        if (over != kTabs[tab]) continue;
        inventory_tab = tab;
        inventory_scroll = 0;
        return;
    }
    const int slot = inventory_slot_over();
    if (slot < 0 || !g.player_inventory.occupied(static_cast<size_t>(slot))) return;
    const bool worn = tl::core::Inventory::is_equip_location(static_cast<size_t>(slot));
    if (control && !worn) {
        const tl::core::InventorySlot held = g.player_inventory.slots()[static_cast<size_t>(slot)];
        g.player_inventory.remove_item(static_cast<size_t>(slot), held.count);
        g.drop_item_near_player(held.item, held.count);
        g.update_menu();
        g.refresh_game_hud();
        TL_LOG_INFO("mouse: %s dropped from the inventory", held.item.name.c_str());
        return;
    }
    TL_LOG_INFO("mouse: the inventory's %s (slot %d)", over.c_str(), slot);
    // Shift sends an item of the bag to the pet's. The left button takes it onto the
    // pointer; the right acts on it: with a merchant's pane up it is sold, with a stash's
    // put away, else used or worn. A worn item comes off on either.
    if (shift && !worn) return g.move_bag_and_pet(/*from_pet=*/false, slot);
    if (!right && !worn) return pick_up('p', slot);
    if (storage_open == VendorScreen::Merchant && !worn) {
        return g.trade_item(/*buying=*/false, slot);
    }
    if (storage_open != VendorScreen::None && !worn) {
        return g.move_housing_item(storage_open, /*from_housing=*/false, slot);
    }
    g.bag_default_action(slot);
}

int Renderer::Impl::MousePanes::inventory_slot_over() const {
    for (int location = 0; location < PcInventory::kEquip; ++location) {
        if (over == PcInventory::equip_window(location)) return location;
    }
    if (over.rfind("Slot", 0) != 0) return -1;
    const long cell = std::strtol(over.c_str() + 4, nullptr, 10) - 1;
    if (cell < 0 || cell >= PcInventory::kCells) return -1;
    return inventory_cells[static_cast<size_t>(cell)];
}

bool Renderer::Impl::MousePanes::scroll(int wheel) {
    if ((!over_inventory && !over_storage && !over_pet) || wheel == 0) return false;
    // Over the stock's pane the wheel turns the half the pointer is in.
    const bool pets = over_pet || (over_storage && over_y > 0.6f);
    int& rows = over_inventory ? inventory_scroll : pets ? pet_scroll : storage_scroll;
    rows += wheel > 0 ? -1 : 1;
    return true;
}

void Renderer::Impl::MousePanes::close_all() {
    if (stats_open) close_stats();
    if (quests_open) toggle_quests();
    if (inventory_open) toggle_inventory();
    if (storage_open != VendorScreen::None) close_storage();
    if (pet_open) toggle_pet();
    if (skills_open) toggle_skills();
}

void Renderer::Impl::MousePanes::follow_view() {
    const unsigned width = g.config.width;
    const unsigned height = g.config.height;
    if (width == view_width && height == view_height) return;
    const bool first = view_width == 0;
    view_width = width;
    view_height = height;
    if (first) return;
    if (stats.ready()) stats.resize(width, height);
    if (quests.ready()) quests.resize(width, height);
    if (inventory.ready()) inventory.resize(width, height);
    if (merchant.ready()) merchant.resize(width, height);
    if (stash.ready()) stash.resize(width, height);
    if (pet_pane.ready()) pet_pane.resize(width, height);
    if (skills.ready()) skills.resize(width, height);
    tip.hide();
    tip_key.clear();
    TL_LOG_INFO("mouse: the panes follow the view to %u x %u", width, height);
}

void Renderer::Impl::MousePanes::sync(float delta) {
    follow_view();
    sync_quests();
    sync_inventory(delta);
    sync_storage();
    sync_pet(delta);
    sync_skills();
    sync_held();
    // The port's map stands where the right pane does (the PC's is covered by its pane
    // too): down while a pane is up.
    const bool right = quests.visible() || inventory.visible() || skills.visible();
    if (right && g.automap.visible()) {
        g.automap.hide();
        map_under_pane = true;
    } else if (!right && map_under_pane) {
        map_under_pane = false;
        if (!g.hud.letterboxed()) g.automap.show();
    }
    if (!stats.ready()) return;
    const bool show = stats_open && !g.hud.letterboxed() && !g.inventory_menu.visible();
    stats.set_visible(show);
    if (!show) return;
    // The console's own bookkeeping closes a page its Stats tab is not showing (placing
    // what was pending); the page is opened again under the pane.
    if (!g.stats_page.open) {
        tl::core::open_stats_page(g.stats_page, g.player_character.unspent_stat_points());
    }
    g.load_stat_graphs();
    g.load_unit_types();
    const tl::core::AttackDescription own_attack = tl::core::own_attack(
        g.player_stats, g.player_stats.level, g.config.difficulty, /*player=*/true, g.stat_graphs);
    const tl::core::StatsPageView view =
        tl::core::refresh_stats_page(g.stats_page, g.player_character, g.player_total(),
                                     g.player_inventory, g.unit_types, &g.stat_graphs, &own_attack);
    PcStats::Figures figures;
    figures.name = g.player_character.name();
    figures.level = g.player_character.level();
    figures.experience = "XP: " + std::to_string(g.player_character.experience());
    figures.experience_fill = g.game_hud.state().experience;
    figures.health = "HP: " + std::to_string(static_cast<int>(g.player_character.max_hp()));
    figures.mana = "Mana: " + std::to_string(static_cast<int>(g.player_character.max_mp()));
    stats.set(view, figures);
}

bool Renderer::Impl::MousePanes::point(float x, float y) {
    over_x = x;
    over_y = y;
    over.clear();
    over_quests = false;
    over_inventory = false;
    over_storage = false;
    over_pet = false;
    over_skills = false;
    if (skills.visible() && skills.blocks(x, y)) {
        over = skills.point(x, y);
        over_skills = true;
        return true;
    }
    if (pet_pane.visible() && pet_pane.blocks(x, y)) {
        over = pet_pane.point(x, y);
        over_pet = true;
        return true;
    }
    if (const PcStorage* pane = storage_pane(); pane != nullptr && pane->blocks(x, y)) {
        over = pane->point(x, y);
        over_storage = true;
        return true;
    }
    if (inventory.visible() && inventory.blocks(x, y)) {
        over = inventory.point(x, y);
        over_inventory = true;
        return true;
    }
    if (quests.visible() && quests.blocks(x, y)) {
        over = quests.point(x, y);
        over_quests = true;
        return true;
    }
    if (!stats.visible()) return false;
    over = stats.point(x, y);
    return stats.blocks(x, y);
}

// The pane's buttons: `Close`, and a plus and a minus for each attribute, which are the
// stats page's add and remove on that attribute (`stats_page_add_point`,
// `stats_page_remove_point`).
void Renderer::Impl::MousePanes::click(bool right, bool control, bool shift) {
    if (held.owner != 0) return place_held(right);
    if (over_inventory) return click_inventory(right, control, shift);
    if (over_storage) return click_storage(right);
    if (over_pet) return click_pet(right);
    if (over_skills) return click_skills();
    // The sheet and the log answer the left button alone.
    if (right) return;
    if (over_quests) return click_quests();
    if (over == "Close") return close_stats();
    for (int i = 0; i < 4; ++i) {
        const bool spend = over == kSpend[i];
        if (!spend && over != kReclaim[i]) continue;
        g.stats_page.selected = i;
        const int unspent = g.player_character.unspent_stat_points();
        const bool moved = spend ? tl::core::stats_page_add_point(g.stats_page, unspent)
                                 : tl::core::stats_page_remove_point(g.stats_page, unspent);
        if (moved) g.play_ui(tl::core::InterfaceSound::PointAssign);
        TL_LOG_INFO("mouse: %s%s", over.c_str(), moved ? "" : " (nothing to move)");
        return;
    }
}

}  // namespace tl::render
