// The mouse layer's panes: the item picked up onto the pointer, and where it is put down.
//
// CGameUI::menuItemClick is the PC's: a left click on an item with
// nothing dragged takes it onto the pointer (`CGameUI +0xb8`), the next click puts it into
// the slot under it - worn, sold, bought, stored, handed to the pet - or, outside every
// pane, onto the ground (CLevel::addItem); CGameUI::returnDraggedItem puts it back.
// INVENTED: the PC moves the item between numbered slots; the inventories here are the
// console's and have none, so the item stays where it is until it is put down, and what a
// put-down does is decided by the pane it lands in, not the slot.
#include "core/log.h"
#include "render/mouse_panes.h"

namespace tl::render {

tl::core::Inventory* Renderer::Impl::MousePanes::held_inventory() {
    switch (held.owner) {
        case 'p': return inventory.visible() ? &g.player_inventory : nullptr;
        case 'a': return g.pet.alive() ? &g.pet.bag() : nullptr;
        case 's': return storage_inventory();
        default: return nullptr;
    }
}

/// The item of `owner`'s `slot` onto the pointer.
void Renderer::Impl::MousePanes::pick_up(char owner, int slot) {
    held.owner = owner;
    held.slot = slot;
    const tl::core::Inventory* from = held_inventory();
    if (from == nullptr || !from->occupied(static_cast<size_t>(slot))) return release();
    const tl::core::ItemStats& item = from->slots()[static_cast<size_t>(slot)].item;
    held.name = item.name;
    held.icon = item.icon;
    g.play_ui(tl::core::InterfaceSound::ItemTake);
    TL_LOG_INFO("mouse: '%s' is on the pointer", held.name.c_str());
}

void Renderer::Impl::MousePanes::release() {
    held = Held{};
    cursor.hide();
}

/// Every frame: the held item still where it was taken from, its picture at the pointer.
void Renderer::Impl::MousePanes::sync_held() {
    if (held.owner == 0) return;
    const tl::core::Inventory* from = held_inventory();
    const bool there = from != nullptr && from->occupied(static_cast<size_t>(held.slot)) &&
                       from->slots()[static_cast<size_t>(held.slot)].item.name == held.name;
    if (!there) return release();
    cursor.show(held.icon, PcCursorItem::Place{over_x, over_y, g.config.width, g.config.height},
                inventory.screen());
}

/// The held item put down in the pane the pointer is in. The right button puts it back.
void Renderer::Impl::MousePanes::place_held(bool right) {
    const Held was = held;
    release();
    if (right) return;
    const bool trading = storage_open == VendorScreen::Merchant;
    // The lower half of the stock's pane, and the pet's own pane, are the pet's bag.
    const bool pets =
        over_pet || (over_storage && (over.rfind("PetSlot", 0) == 0 || over_y > 0.6f));
    if (was.owner == 'p') {
        if (pets) return g.move_bag_and_pet(/*from_pet=*/false, was.slot);
        if (over_storage && trading) return g.trade_item(/*buying=*/false, was.slot);
        if (over_storage)
            return g.move_housing_item(storage_open, /*from_housing=*/false, was.slot);
        // On an equipment window: worn, when it is something to wear.
        g.load_unit_types();
        const tl::core::ItemStats& item =
            g.player_inventory.slots()[static_cast<size_t>(was.slot)].item;
        const bool wearable =
            tl::core::item_isa(item, tl::core::unit_type::kWeapon, g.unit_types) ||
            tl::core::item_isa(item, tl::core::unit_type::kArmor, g.unit_types) ||
            tl::core::item_isa(item, tl::core::unit_type::kTrinket, g.unit_types);
        if (over_inventory && over.rfind("Equip", 0) == 0 && wearable) {
            g.bag_default_action(was.slot);
        }
        return;
    }
    if (was.owner == 'a') {
        if (over_inventory) g.move_bag_and_pet(/*from_pet=*/true, was.slot);
        return;
    }
    // From the stock: bought, or taken out, when it lands in the player's pane.
    if (!over_inventory) return;
    if (trading) return g.trade_item(/*buying=*/true, was.slot);
    g.move_housing_item(storage_open, /*from_housing=*/true, was.slot);
}

/// The held item put down outside every pane: the player's goes onto the ground at the
/// player's feet, any other back where it was. True when an item was held.
bool Renderer::Impl::MousePanes::drop_held() {
    if (held.owner == 0) return false;
    const Held was = held;
    release();
    if (was.owner != 'p') return true;
    const tl::core::InventorySlot item = g.player_inventory.slots()[static_cast<size_t>(was.slot)];
    g.player_inventory.remove_item(static_cast<size_t>(was.slot), item.count);
    g.drop_item_near_player(item.item, item.count);
    g.update_menu();
    g.refresh_game_hud();
    TL_LOG_INFO("mouse: '%s' put down on the ground", item.item.name.c_str());
    return true;
}

}  // namespace tl::render
