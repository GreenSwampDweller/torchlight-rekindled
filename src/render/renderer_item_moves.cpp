// The renderer: one item moved between the player's bag, the pet's, and a stash or the
// Transmuter.
#include "render/renderer_impl.h"

namespace tl::render {

void Renderer::Impl::swap_bag_and_pet() {
    const bool from_pet = inventory_menu.pet_tray_focused();
    move_bag_and_pet(from_pet, from_pet ? inventory_menu.selected_pet_slot()
                                        : inventory_menu.selected_slot_index());
}

/// One item from the pet's bag to the player's, or the other way.
void Renderer::Impl::move_bag_and_pet(bool from_pet, int slot) {
    // from the player's tray (tray, pet), then into
    // the pet's inventory; from the pet's (ISA 0x57) into the player's.
    if (!pet.alive()) return;
    load_unit_types();
    tl::core::Inventory& from = from_pet ? pet.bag() : player_inventory;
    tl::core::Inventory& to = from_pet ? player_inventory : pet.bag();
    if (slot < 0 || !from.occupied(static_cast<size_t>(slot))) return;
    if (!from_pet &&
        !tl::core::pet_takes_items(/*tray_owner_is_player=*/true, pet.alive(), pet.state())) {
        return;
    }
    // DIVERGENCE: X refuses a worn item, as Drop does; takes it off and moves it.
    if (tl::core::Inventory::is_equip_location(static_cast<size_t>(slot))) {
        TL_LOG_INFO("%s: '%s' is equipped; unequip it first", from_pet ? "pet tray" : "pet bag",
                    from.slots()[static_cast<size_t>(slot)].item.name.c_str());
        return;
    }
    const tl::core::InventorySlot moved = from.slots()[static_cast<size_t>(slot)];
    switch (tl::core::move_to_inventory(from, static_cast<size_t>(slot), to,
                                        /*target_is_combiner=*/false, unit_types)) {
        case tl::core::TrayMove::Nothing: return;
        case tl::core::TrayMove::Refused:
            play_ui(tl::core::InterfaceSound::Error);
            TL_LOG_INFO("%s: '%s' does not fit", from_pet ? "bag" : "pet bag",
                        moved.item.name.c_str());
            break;
        case tl::core::TrayMove::Moved:
            play_ui(tl::core::InterfaceSound::ItemTake);
            load_stat_graphs();
            TL_LOG_INFO("%s: %d x %s, worth %d gold", from_pet ? "bag (from the pet)" : "pet bag",
                        moved.count, moved.item.name.c_str(),
                        tl::core::item_sell_price(moved.item, stat_graphs, moved.count));
            break;
    }
    player_inventory.calculate_effect_values();
    sync_equipped_weapon();
    update_menu();
    refresh_game_hud();
}

void Renderer::Impl::swap_selected() {
    const bool from_housing = inventory_menu.focused_tray() == InventoryMenu::Tray::Vendor;
    move_housing_item(inventory_menu.vendor(), from_housing,
                      from_housing ? inventory_menu.selected_vendor_slot()
                                   : inventory_menu.selected_slot_index());
}

/// An item of the stash's or the Transmuter's back to the player. False for nothing moved.
bool Renderer::Impl::take_from_housing(tl::core::Inventory& housing, int slot, const char* what) {
    // on the combiner's tray (ISA 0x86) or a stash's:
    //
    const std::string name = housing.slots()[static_cast<size_t>(slot)].item.name;
    tl::core::InventorySlot dropped;
    const tl::core::TrayReturn where =
        tl::core::return_to_player(housing, static_cast<size_t>(slot), player_inventory,
                                   pet.alive() ? &pet.bag() : nullptr, unit_types, dropped);
    switch (where) {
        case tl::core::TrayReturn::None: return false;
        case tl::core::TrayReturn::Bag:
        case tl::core::TrayReturn::PetBag: play_ui(tl::core::InterfaceSound::ItemTake); break;
        case tl::core::TrayReturn::Dropped:
            drop_item_near_player(dropped.item, dropped.count);
            break;
    }
    TL_LOG_INFO("%s: '%s' back to the %s", what, name.c_str(),
                where == tl::core::TrayReturn::Bag      ? "bag"
                : where == tl::core::TrayReturn::PetBag ? "pet's bag"
                                                        : "floor");
    return true;
}

/// An item of the player's bag into the stash or the Transmuter. False for nothing moved.
bool Renderer::Impl::put_in_housing(tl::core::Inventory& housing, int slot, bool combine) {
    const char* const what = combine ? "transmute" : "stash";
    // on the bag: into the combiner's or the stash's.
    if (!player_inventory.occupied(static_cast<size_t>(slot))) return false;
    const std::string name = player_inventory.slots()[static_cast<size_t>(slot)].item.name;
    const tl::core::TrayMove moved = tl::core::move_to_inventory(
        player_inventory, static_cast<size_t>(slot), housing, combine, unit_types);
    if (moved == tl::core::TrayMove::Nothing) return false;
    play_ui(moved == tl::core::TrayMove::Moved ? tl::core::InterfaceSound::ItemTake
                                               : tl::core::InterfaceSound::Error);
    const char* const target = combine ? "transmuter" : "stash";
    TL_LOG_INFO("%s: '%s' %s the %s", what, name.c_str(),
                moved == tl::core::TrayMove::Moved ? "put in" : "refused by", target);
    return true;
}

/// One item between the player's bag and `screen`'s stash or Transmuter.
void Renderer::Impl::move_housing_item(VendorScreen screen, bool from_housing, int slot) {
    tl::core::Inventory* housing = housing_inventory_for(screen);
    if (housing == nullptr || slot < 0) return;
    load_unit_types();
    const bool combine = screen == VendorScreen::Combine;
    const bool moved = from_housing
                           ? take_from_housing(*housing, slot, combine ? "transmute" : "stash")
                           : put_in_housing(*housing, slot, combine);
    if (!moved) return;
    sync_equipped_weapon();
    update_menu();
    update_character_preview(0.0f);
    refresh_game_hud();
}

}  // namespace tl::render
