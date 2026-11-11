// The renderer: the merchants - their characters, stock, restocking and trading.
#include "core/item_data.h"
#include "core/unit_resource.h"
#include "formats/unit_data.h"
#include "render/renderer_impl.h"

namespace tl::render {

/// The merchant `unit` as the one traded with, its stock on the vendor's tray. True when
/// the PC's merchant pane took it (the mouse controls), so the console's menu stays shut.
bool Renderer::Impl::open_merchant_stock(const std::string& unit) {
    MerchantState* merchant = merchant_named(unit);
    if (merchant == nullptr) {
        TL_LOG_WARN("npc: '%s' has no stock", unit.c_str());
        return false;
    }
    for (const auto& entry : merchants) {
        if (&entry.second == merchant) trading_with = entry.first;
    }
    if (mouse_opens_storage(VendorScreen::Merchant)) return true;
    inventory_menu.set_vendor_stock(&merchant->inventory, merchant->gambler);
    return false;
}

/// `CLevel::addItem` at the merchant, for what a trade could not hold.
void Renderer::Impl::drop_at_merchant(const tl::core::TradeOutcome& out) {
    if (!out.dropped) return;
    for (const auto& entry : scene_units) {
        if (entry.second.node != trading_with || entry.second.entity == nullptr) continue;
        const Ogre::SceneNode* at = entry.second.entity->getParentSceneNode();
        if (at != nullptr) drop_item(out.dropped_item, out.count, at->_getDerivedPosition());
        break;
    }
}

void Renderer::Impl::trade_selected() {
    const bool buying = inventory_menu.focused_tray() == InventoryMenu::Tray::Vendor;
    trade_item(buying, buying ? inventory_menu.selected_vendor_slot()
                              : inventory_menu.selected_slot_index());
}

/// One item bought from the merchant's slot or sold from the player's.
void Renderer::Impl::trade_item(bool buying, int slot) {
    const auto found = merchants.find(trading_with);
    if (found == merchants.end() || slot < 0) return;
    MerchantState& merchant = found->second;
    load_unit_types();
    load_stat_graphs();
    ensure_item_index();
    const float bonus = player_total().effect_value(tl::core::item_effect::kPriceBonus);
    tl::core::TradeContext context;
    context.unit_types = &unit_types;
    context.graphs = &stat_graphs;
    context.player_price_bonus = &bonus;
    // `CResourceManager::createUnit(data, 0, false, false)` for an item:
    // `createItem(guid, false, true, false)`, from the item's own data.
    context.create_copy = [this](const tl::core::ItemStats& held) {
        const tl::core::ItemStats* data = items.find(held.name);
        return create_item(data != nullptr ? *data : held, false, true, false);
    };

    tl::core::TradeOutcome out;
    std::string name;
    if (buying) {
        if (!merchant.inventory.occupied(static_cast<size_t>(slot))) return;
        name = merchant.inventory.slots()[static_cast<size_t>(slot)].item.name;
        out = tl::core::buy_from_merchant(merchant.inventory, merchant.gambler,
                                          static_cast<size_t>(slot), player_inventory, context);
    } else {
        if (!player_inventory.occupied(static_cast<size_t>(slot))) return;
        name = player_inventory.slots()[static_cast<size_t>(slot)].item.name;
        out = tl::core::sell_to_merchant(player_inventory, static_cast<size_t>(slot),
                                         merchant.inventory, merchant.gambler, player_inventory,
                                         context);
    }
    switch (out.result) {
        case tl::core::TradeResult::None: return;
        case tl::core::TradeResult::Refused:
            // `Error` at the player, `VOICEIMPOSSIBLE` 0.1 s on.
            play_ui(tl::core::InterfaceSound::Error);
            play_voice(tl::core::SoundSlot::VoiceImpossible, 0.1f);
            TL_LOG_INFO("merchant: '%s' refused (%d gold)", name.c_str(), player_inventory.gold());
            return;
        case tl::core::TradeResult::InventoryFull:
            play_voice(tl::core::SoundSlot::VoiceInventoryFull, 0.1f);
            TL_LOG_INFO("merchant: no room for '%s'", name.c_str());
            return;
        case tl::core::TradeResult::Bought:
        case tl::core::TradeResult::Sold: break;
    }
    for (int i = 0; i < out.gold_buy_sounds; ++i) play_ui(tl::core::InterfaceSound::GoldBuy);
    // CGameUI::menuItemClick: incrementJournalStatistic(15, 1)
    if (out.gambled) count_journal(tl::core::JournalStatistic::TimesGambled);
    if (out.result == tl::core::TradeResult::Sold && out.price > 0) {
        // CCharacter::giveGold: incrementJournalStatistic(1, amount)
        count_journal(tl::core::JournalStatistic::GoldGathered, out.price);
    }
    drop_at_merchant(out);
    TL_LOG_INFO("merchant: %s %d x '%s' for %d gold%s%s%s; %d left", buying ? "bought" : "sold",
                out.count, name.c_str(), out.price, out.copied ? " (a fresh copy)" : "",
                out.gambled ? " (a gamble)" : "", out.dropped ? " (dropped at the merchant)" : "",
                player_inventory.gold());
    sync_equipped_weapon();
    update_menu();
    update_character_preview(0.0f);
    refresh_game_hud();
}

}  // namespace tl::render
