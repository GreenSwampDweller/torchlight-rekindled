// The mouse layer's panes: a merchant's stock or a stash on the left, the pet's bag under
// it, and the details window of the item under the pointer.
#include "core/log.h"
#include "render/mouse_panes.h"

#include <algorithm>
#include <cstdlib>
#include <vector>

namespace tl::render {

namespace {

/// The tab an item of a merchant's stock is listed under: 1 weapons, 2 armour, 0 the rest.
int stock_tab(const tl::core::UnitTypeHierarchy& types, const tl::core::ItemStats& item) {
    const uint32_t type = types.id_of(item.unittype);
    if (types.isa(type, tl::core::unit_type::kWeapon)) return 1;
    return types.isa(type, tl::core::unit_type::kArmor) ? 2 : 0;
}

/// The tab an item of a bag is listed under: 2 fish, 1 spells, 0 the rest.
int bag_tab(const tl::core::UnitTypeHierarchy& types, const tl::core::ItemStats& item) {
    const uint32_t type = types.id_of(item.unittype);
    if (types.isa(type, tl::core::unit_type::kFish)) return 2;
    return types.isa(type, tl::core::unit_type::kSpell) ? 1 : 0;
}

/// The number a window `<stem><N>` carries, from 0; -1 when `name` is not one.
long numbered(const std::string& name, const char* stem, size_t length) {
    if (name.compare(0, length, stem) != 0) return -1;
    return std::strtol(name.c_str() + length, nullptr, 10) - 1;
}

/// The cells of one grid: the slots of `slots` that `listed` keeps, `columns` a row, from
/// row `scroll` (kept inside the rows there are). `cells` takes the slot of each cell.
template <size_t N, typename Listed>
void fill_cells(const std::vector<tl::core::InventorySlot>& slots, const Listed& listed,
                int columns, int& scroll, std::array<int, N>& cells,
                std::array<PcStorage::Cell, N>& shown, size_t first = 0) {
    std::vector<int> kept;
    for (size_t slot = first; slot < slots.size(); ++slot) {
        if (slots[slot].count > 0 && listed(slots[slot].item))
            kept.push_back(static_cast<int>(slot));
    }
    const int rows = (static_cast<int>(kept.size()) + columns - 1) / columns;
    scroll = std::max(0, std::min(scroll, rows - static_cast<int>(N) / columns));
    cells.fill(-1);
    for (size_t cell = 0; cell < N; ++cell) {
        const size_t at = static_cast<size_t>(scroll) * static_cast<size_t>(columns) + cell;
        if (at >= kept.size()) break;
        const tl::core::InventorySlot& held = slots[static_cast<size_t>(kept[at])];
        cells[cell] = kept[at];
        shown[cell].icon = held.item.icon;
        shown[cell].count = held.count;
    }
}

}  // namespace

PcStorage* Renderer::Impl::MousePanes::storage_pane() {
    if (storage_open == VendorScreen::Merchant) return &merchant;
    return storage_open == VendorScreen::None ? nullptr : &stash;
}

tl::core::Inventory* Renderer::Impl::MousePanes::storage_inventory() {
    if (storage_open == VendorScreen::None) return nullptr;
    if (storage_open != VendorScreen::Merchant) return g.housing_inventory_for(storage_open);
    const auto found = g.merchants.find(g.trading_with);
    return found == g.merchants.end() ? nullptr : &found->second.inventory;
}

bool Renderer::Impl::MousePanes::open_storage(VendorScreen screen) {
    const bool for_merchant = screen == VendorScreen::Merchant;
    PcStorage& pane = for_merchant ? merchant : stash;
    bool& tried = for_merchant ? merchant_tried : stash_tried;
    if (!tried) {
        tried = true;
        pane.create(kGameGroup, g.config.width, g.config.height,
                    for_merchant ? PcStorage::Kind::Merchant : PcStorage::Kind::Stash);
    }
    if (!pane.ready()) return false;
    // A held button asks again while it is up.
    if (storage_open == screen) return true;
    if (!inventory_open && !toggle_inventory()) return false;
    if (stats_open) close_stats();
    if (pet_open) toggle_pet();
    if (storage_open != VendorScreen::None && storage_open != screen) close_storage();
    storage_open = screen;
    storage_tab = for_merchant ? -1 : 0;
    storage_scroll = 0;
    TL_LOG_INFO("mouse: the %s pane opens", for_merchant ? "merchant's" : "stash's");
    return true;
}

void Renderer::Impl::MousePanes::close_storage() {
    TL_LOG_INFO("mouse: the %s pane closes",
                storage_open == VendorScreen::Merchant ? "merchant's" : "stash's");
    if (PcStorage* pane = storage_pane()) pane->set_visible(false);
    storage_open = VendorScreen::None;
    g.trading_with = 0;
    g.play_ui(tl::core::InterfaceSound::MenuClose);
}

// DIVERGENCE: the PC's merchant has three tabs of 42 slots and its stash 42; the stock
// here is the console's one inventory, so a tab is a filter over it and the wheel turns
// its rows past 42. The pet's bag under it likewise.
void Renderer::Impl::MousePanes::sync_storage() {
    PcStorage* pane = storage_pane();
    tl::core::Inventory* stock = storage_inventory();
    if (pane != nullptr && stock == nullptr) {
        pane->set_visible(false);
        storage_open = VendorScreen::None;
        return;
    }
    if (pane == nullptr) return;
    const bool show = !g.hud.letterboxed() && !g.inventory_menu.visible();
    pane->set_visible(show);
    if (!show) return;
    g.load_unit_types();
    const tl::core::UnitTypeHierarchy& types = g.unit_types;
    const bool tabbed = storage_open == VendorScreen::Merchant;
    if (storage_tab < 0) {
        // The first tab that lists anything.
        storage_tab = 0;
        for (int tab = 2; tab >= 0; --tab) {
            for (const tl::core::InventorySlot& held : stock->slots()) {
                if (held.count > 0 && stock_tab(types, held.item) == tab) storage_tab = tab;
            }
        }
    }
    PcStorage::View view;
    fill_cells(
        stock->slots(),
        [&](const tl::core::ItemStats& item) {
            return !tabbed || stock_tab(types, item) == storage_tab;
        },
        PcStorage::kColumns, storage_scroll, storage_cells, view.cells);
    view.tab = tabbed ? storage_tab : -1;
    view.pet = g.pet.alive();
    pet_cells.fill(-1);
    if (view.pet) {
        fill_cells(
            g.pet.bag().slots(),
            [&](const tl::core::ItemStats& item) { return bag_tab(types, item) == pet_tab; },
            PcStorage::kColumns, pet_scroll, pet_cells, view.pet_cells,
            tl::core::Inventory::kFixedSlotCount);
    }
    view.pet_tab = pet_tab;
    pane->set(view);
}

int Renderer::Impl::MousePanes::storage_slot_over() const {
    const long cell = numbered(over, "Slot", 4);
    if (cell < 0 || cell >= PcStorage::kCells) return -1;
    return storage_cells[static_cast<size_t>(cell)];
}

int Renderer::Impl::MousePanes::pet_slot_over() const {
    const long cell = numbered(over, "PetSlot", 7);
    if (cell < 0 || cell >= PcStorage::kPetCells) return -1;
    return pet_cells[static_cast<size_t>(cell)];
}

/// A click in the stock's pane: a tab's filter. The left button takes an item onto the
/// pointer; the right buys a merchant's, takes a stash's out, brings the pet's to the
/// player's bag.
void Renderer::Impl::MousePanes::click_storage(bool right) {
    if (over == "Close") return close_storage();
    static constexpr const char* kTabs[3] = {"TabMisc", "TabWeapon", "TabArmor"};
    static constexpr const char* kPetTabs[3] = {"TabBackpack", "TabSpell", "TabFish"};
    for (int tab = 0; tab < 3; ++tab) {
        if (over == kTabs[tab]) {
            storage_tab = tab;
            storage_scroll = 0;
            return;
        }
        if (over == kPetTabs[tab]) {
            pet_tab = tab;
            pet_scroll = 0;
            return;
        }
    }
    if (const int slot = pet_slot_over(); slot >= 0) {
        TL_LOG_INFO("mouse: the pet's %s (slot %d)", over.c_str(), slot);
        if (!right) return pick_up('a', slot);
        return g.move_bag_and_pet(/*from_pet=*/true, slot);
    }
    const int slot = storage_slot_over();
    if (slot < 0) return;
    TL_LOG_INFO("mouse: the stock's %s (slot %d)", over.c_str(), slot);
    if (!right) return pick_up('s', slot);
    if (storage_open == VendorScreen::Merchant) return g.trade_item(/*buying=*/true, slot);
    g.move_housing_item(storage_open, /*from_housing=*/true, slot);
}

/// The pet's pane up or down.
bool Renderer::Impl::MousePanes::toggle_pet() {
    if (!pet_tried) {
        pet_tried = true;
        pet_pane.create(kGameGroup, g.config.width, g.config.height);
    }
    if (!pet_pane.ready() || (!pet_open && !g.pet.alive())) return false;
    pet_open = !pet_open;
    if (pet_open) {
        if (stats_open) close_stats();
        if (storage_open != VendorScreen::None) close_storage();
    } else {
        pet_pane.set_visible(false);
        g.preview_shows_pet = false;
    }
    g.play_ui(pet_open ? tl::core::InterfaceSound::MenuOpen : tl::core::InterfaceSound::MenuClose);
    TL_LOG_INFO("mouse: the pet's pane %s", pet_open ? "opens" : "closes");
    return true;
}

void Renderer::Impl::MousePanes::sync_pet(float delta) {
    if (!pet_pane.ready()) return;
    if (pet_open && !g.pet.alive()) pet_open = false;
    const bool show = pet_open && !g.hud.letterboxed() && !g.inventory_menu.visible();
    pet_pane.set_visible(show);
    g.preview_shows_pet = show;
    if (!show) return;
    g.load_unit_types();
    const tl::core::UnitTypeHierarchy& types = g.unit_types;
    const std::vector<tl::core::InventorySlot>& slots = g.pet.bag().slots();
    PcPet::View view;
    view.name = g.pet.stats().display_name;
    view.level = std::to_string(g.pet.level());
    view.health = "HP: " + std::to_string(static_cast<int>(g.pet.max_health()));
    for (int trinket = 0; trinket < PcPet::kEquip; ++trinket) {
        const tl::core::InventorySlot& worn =
            slots[static_cast<size_t>(tl::core::equip_location::kRing1) +
                  static_cast<size_t>(trinket)];
        if (worn.count > 0) view.worn[static_cast<size_t>(trinket)] = worn.item.icon;
    }
    for (size_t spell = 0; spell < view.spells.size(); ++spell) {
        const tl::core::KnownSkill* known = g.pet.skills().known_spell(spell);
        if (known != nullptr && known->def != nullptr) view.spells[spell] = known->def->icon;
    }
    // The bag alone: what the pet wears has its own windows.
    std::array<PcStorage::Cell, PcPet::kCells> cells;
    fill_cells(
        slots, [&](const tl::core::ItemStats& item) { return bag_tab(types, item) == pet_tab; },
        PcPet::kColumns, pet_scroll, pet_cells, cells, tl::core::Inventory::kFixedSlotCount);
    for (size_t cell = 0; cell < cells.size(); ++cell) {
        view.cells[cell].icon = cells[cell].icon;
        view.cells[cell].count = cells[cell].count;
    }
    view.tab = pet_tab;
    pet_pane.set(view);
    g.update_character_preview(delta);
    if (g.preview_texture) pet_pane.set_doll("tl/inv/preview_material", 266.0f / 412.0f, true);
}

int Renderer::Impl::MousePanes::pet_pane_slot_over() const {
    for (int trinket = 0; trinket < PcPet::kEquip; ++trinket) {
        if (over == PcPet::equip_window(trinket)) return tl::core::equip_location::kRing1 + trinket;
    }
    const long cell = numbered(over, "Slot", 4);
    if (cell < 0 || cell >= PcPet::kCells) return -1;
    return pet_cells[static_cast<size_t>(cell)];
}

/// A click in the pet's pane: a tab's filter; the left button takes an item of its bag
/// onto the pointer, the right has the pet wear it or take it off.
void Renderer::Impl::MousePanes::click_pet(bool right) {
    if (over == "Close") {
        toggle_pet();
        return;
    }
    static constexpr const char* kPetTabs[3] = {"TabBackpack", "TabSpell", "TabFish"};
    for (int tab = 0; tab < 3; ++tab) {
        if (over != kPetTabs[tab]) continue;
        pet_tab = tab;
        pet_scroll = 0;
        return;
    }
    const int slot = pet_pane_slot_over();
    if (slot < 0 || !g.pet.bag().occupied(static_cast<size_t>(slot))) return;
    const bool worn = tl::core::Inventory::is_equip_location(static_cast<size_t>(slot));
    TL_LOG_INFO("mouse: the pet's %s (slot %d)", over.c_str(), slot);
    if (right || worn) {
        if (g.wear_pet_item(static_cast<size_t>(slot))) g.update_menu();
        return;
    }
    pick_up('a', slot);
}

// CGameUI::showEquipmentTooltip, for the item of a pane the pointer is
// over: the player's, the pet's, a stash's, or the merchant's with its price.
void Renderer::Impl::MousePanes::sync_tooltip(bool pointer, const HudOver& hud) {
    if (skill_tooltip(pointer, hud.skill, hud.art)) return;
    const tl::core::Inventory* held_in = nullptr;
    // The frame's art comes from the pane the pointer is in: any of them holds it.
    const LayoutScreen* art = &inventory.screen();
    int slot = -1;
    char owner = 'p';
    tl::core::EquipmentTooltipContext context;
    const bool trading = storage_open == VendorScreen::Merchant;
    if (pointer && over_inventory && inventory.visible()) {
        held_in = &g.player_inventory;
        slot = inventory_slot_over();
    } else if (pointer && over_pet) {
        held_in = &g.pet.bag();
        slot = pet_pane_slot_over();
        art = &pet_pane.screen();
        owner = 'a';
        context.owner_is_player = false;
    } else if (pointer && over_storage && pet_slot_over() >= 0) {
        held_in = &g.pet.bag();
        slot = pet_slot_over();
        owner = 'a';
        context.owner_is_player = false;
    } else if (pointer && over_storage) {
        art = &storage_pane()->screen();
        held_in = storage_inventory();
        slot = storage_slot_over();
        owner = 's';
        context.owner_is_player = !trading;
        context.owner_is_merchant = trading;
        const auto found = g.merchants.find(g.trading_with);
        context.owner_is_gambler = trading && found != g.merchants.end() && found->second.gambler;
    }
    // An item bound to a numbered slot of the HUD: the bag's copy of it.
    if (held_in == nullptr && pointer && hud.item_slot >= 0 && hud.art != nullptr) {
        held_in = &g.player_inventory;
        slot = hud.item_slot;
        art = hud.art;
        owner = 'h';
    }
    if (held_in == nullptr || slot < 0 || !held_in->occupied(static_cast<size_t>(slot))) {
        tip_key.clear();
        tip.hide();
        return;
    }
    const tl::core::InventorySlot& held = held_in->slots()[static_cast<size_t>(slot)];
    const std::string key = owner + std::to_string(slot) + ":" + std::to_string(held.count) + ":" +
                            held.item.name + (trading ? "$" : "");
    if (key == tip_key && over_x == tip_x && over_y == tip_y) return;
    tip_key = key;
    tip_x = over_x;
    tip_y = over_y;
    context.merchant_open = trading;
    const std::vector<tl::core::EquipmentTooltip> tips =
        g.item_tooltips(*held_in, slot, context, /*compare=*/false);
    if (tips.empty()) return tip.hide();
    tip.show(tips.front(), over_x, over_y, g.config.width, g.config.height, *art);
}

}  // namespace tl::render
